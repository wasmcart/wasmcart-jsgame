/*
 * webgpu_shim.c - navigator.gpu for JavaScript running in the cart's QuickJS,
 * on top of webgpu.h (Dawn's emdawnwebgpu port, linked into the cart).
 *
 * Built only into the WebGPU runtime (build_webgpu.sh -> cart-webgpu.wasm).
 *
 * Most of the binding is GENERATED: src/webgpu_bindings.gen.c, made by
 * tools/gen_webgpu_bindings.mjs from the pinned emdawnwebgpu release's
 * webgpu.h and its JS enum tables (every GPU* class, every descriptor
 * dictionary, every method whose arguments are plain values or dictionaries).
 * This file holds what a generator cannot know:
 *
 *   - the helpers the generated code calls (arena, wrap/unwrap, enums);
 *   - dictionaries whose JS shape differs from the C struct (GPUExtent3D as an
 *     array, GPUBindGroupEntry.resource, GPUShaderModuleDescriptor.code, ...);
 *   - every Promise: requestAdapter, requestDevice, mapAsync,
 *     onSubmittedWorkDone, popErrorScope, create*PipelineAsync,
 *     getCompilationInfo. The C callback only RECORDS the result; the Promise
 *     settles at the start of the next wc_render (webgpu_frame_begin), and
 *     the cart's normal microtask pump then runs the JS continuations. The
 *     host fires these callbacks between frames, on its event loop, so no JS
 *     ever runs from inside a host callback;
 *   - raw-memory calls: writeBuffer, writeTexture, getMappedRange (an
 *     ArrayBuffer over the mapped range in wasm memory, detached on unmap);
 *   - adapter/device features, limits and info;
 *   - the canvas: getContext('webgpu') on the main canvas returns a
 *     GPUCanvasContext over the "#canvas" surface. It never presents; the
 *     host does that between frames. getCurrentTexture() returns the same
 *     texture for the whole frame, like a browser.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <webgpu/webgpu.h>

#include "wasmcart.h"
#include "quickjs.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ── Arena: per-call scratch for converted descriptors ──────────────
 * A converter may run JS (a getter on a descriptor), which may call back
 * into another WebGPU method, so a call frees only what it allocated:
 * mark on entry, release to the mark on exit. */
typedef struct { void *p; void (*release)(void *); } arena_entry;
static arena_entry *arena;
static size_t arena_len, arena_cap;

static void arena_push(void *p, void (*release)(void *)) {
    if (arena_len == arena_cap) {
        arena_cap = arena_cap ? arena_cap * 2 : 256;
        arena = realloc(arena, arena_cap * sizeof *arena);
    }
    arena[arena_len].p = p;
    arena[arena_len].release = release;
    arena_len++;
}
static void *wgj_alloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    arena_push(p, free);
    return p;
}
static size_t wgj_arena_mark(void) { return arena_len; }
static void wgj_arena_release(size_t mark) {
    while (arena_len > mark) { arena_len--; arena[arena_len].release(arena[arena_len].p); }
}

static int64_t wgj_d2i(double d) {
    if (d != d) return 0;
    if (d >= 18446744073709551615.0) return (int64_t)UINT64_MAX;
    if (d >= 9223372036854775807.0) return (int64_t)(uint64_t)d;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}

static int wgj_to_sv(JSContext *ctx, JSValueConst v, WGPUStringView *out) {
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, v);
    if (!s) return -1;
    char *copy = wgj_alloc(len + 1);
    memcpy(copy, s, len);
    JS_FreeCString(ctx, s);
    out->data = copy;
    out->length = len;
    return 0;
}

typedef struct { const char *name; uint32_t value; } wgj_enum_entry;

static int wgj_enum_lookup(JSContext *ctx, JSValueConst v, const wgj_enum_entry *tab,
                           const char *type, uint32_t *out) {
    const char *s = JS_ToCString(ctx, v);
    if (!s) return -1;
    for (; tab->name; tab++) {
        if (strcmp(tab->name, s) == 0) { *out = tab->value; JS_FreeCString(ctx, s); return 0; }
    }
    JS_ThrowTypeError(ctx, "'%s' is not a valid %s value", s, type);
    JS_FreeCString(ctx, s);
    return -1;
}

static JSValue wgj_enum_name(JSContext *ctx, const wgj_enum_entry *tab, uint32_t v) {
    for (; tab->name; tab++) if (tab->value == v) return JS_NewString(ctx, tab->name);
    return JS_UNDEFINED;
}

static int wgj_array_len(JSContext *ctx, JSValueConst v, uint32_t *n, const char *what) {
    JSValue len = JS_GetPropertyStr(ctx, v, "length");
    if (JS_IsException(len)) return -1;
    if (JS_IsUndefined(len)) { JS_ThrowTypeError(ctx, "%s: expected a sequence", what); return -1; }
    int r = JS_ToUint32(ctx, n, len);
    JS_FreeValue(ctx, len);
    return r;
}

/* ── Objects ──────────────────────────────────────────────────────── */

#define WGJ_SLOTS 6
typedef struct {
    void *h;
    int type;
    JSValue label;
    JSValue slots[WGJ_SLOTS];  /* per-type cached values, see SLOT_* */
    /* GPUBuffer mapping state */
    int map_mode;              /* WGPUMapMode of the current mapping, 0 = none */
    uint64_t map_offset, map_size;
} wgj_obj;

/* Device slots */
#define SLOT_QUEUE    0
#define SLOT_LOST     1
#define SLOT_FEATURES 2
#define SLOT_LIMITS   3
#define SLOT_INFO     4
#define SLOT_LOST_RESOLVE 5
/* Buffer slot: an array of the ArrayBuffers handed out by getMappedRange */
#define SLOT_MAPPED   0

static void wgj_unwrap_fail(JSContext *ctx, const char *what, const char *want) {
    JS_ThrowTypeError(ctx, "%s: expected a %s", what, want);
}

/* Generated code needs these before its own body. */
static JSClassID wgj_class_ids[64];
static JSValue wgj_protos[64];

static int wgj_unwrap(JSContext *ctx, JSValueConst v, int type, void **out, const char *what);
static int wgj_unwrap_this(JSContext *ctx, JSValueConst v, int type, void **out);
static JSValue wgj_wrap(JSContext *ctx, int type, void *h);
static JSValue wgj_label_get(JSContext *ctx, JSValueConst this_val, int magic);
static JSValue wgj_label_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic);

static int wgj_conv_constants(JSContext *ctx, JSValueConst v, WGPUConstantEntry const **out, size_t *count);

#include "webgpu_bindings.gen.c"

static wgj_obj *wgj_get(JSValueConst v, int type) {
    return (wgj_obj *)JS_GetOpaque(v, wgj_class_ids[type]);
}

static int wgj_unwrap(JSContext *ctx, JSValueConst v, int type, void **out, const char *what) {
    if (JS_IsNull(v) || JS_IsUndefined(v)) { *out = NULL; return 0; }
    /* layout: 'auto' */
    if (type == WGJ_T_PipelineLayout && JS_IsString(v)) { *out = NULL; return 0; }
    wgj_obj *o = wgj_get(v, type);
    if (!o) { wgj_unwrap_fail(ctx, what, wgj_class_names[type]); return -1; }
    *out = o->h;
    return 0;
}

static int wgj_unwrap_this(JSContext *ctx, JSValueConst v, int type, void **out) {
    wgj_obj *o = wgj_get(v, type);
    if (!o) { JS_ThrowTypeError(ctx, "not a %s", wgj_class_names[type]); return -1; }
    *out = o->h;
    return 0;
}

/* Wrap a handle the caller owns one reference to; the JS object takes it. */
static JSValue wgj_wrap(JSContext *ctx, int type, void *h) {
    if (!h) return JS_NULL;
    JSValue obj = JS_NewObjectProtoClass(ctx, wgj_protos[type], wgj_class_ids[type]);
    if (JS_IsException(obj)) { wgj_release_fns[type](h); return obj; }
    wgj_obj *o = calloc(1, sizeof *o);
    o->h = h;
    o->type = type;
    o->label = JS_NewString(ctx, "");
    for (int i = 0; i < WGJ_SLOTS; i++) o->slots[i] = JS_UNDEFINED;
    JS_SetOpaque(obj, o);
    return obj;
}

static void wgj_finalizer(JSRuntime *rt, JSValue val) {
    JSClassID cid = JS_GetClassID(val);
    wgj_obj *o = (wgj_obj *)JS_GetOpaque(val, cid);
    if (!o) return;
    JS_FreeValueRT(rt, o->label);
    for (int i = 0; i < WGJ_SLOTS; i++) JS_FreeValueRT(rt, o->slots[i]);
    if (o->h) wgj_release_fns[o->type](o->h);
    free(o);
}

static void wgj_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
    JSClassID cid = JS_GetClassID(val);
    wgj_obj *o = (wgj_obj *)JS_GetOpaque(val, cid);
    if (!o) return;
    JS_MarkValue(rt, o->label, mark_func);
    for (int i = 0; i < WGJ_SLOTS; i++) JS_MarkValue(rt, o->slots[i], mark_func);
}

static JSValue wgj_label_get(JSContext *ctx, JSValueConst this_val, int magic) {
    wgj_obj *o = wgj_get(this_val, magic);
    if (!o) return JS_UNDEFINED;
    return JS_DupValue(ctx, o->label);
}

static JSValue wgj_label_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic) {
    wgj_obj *o = wgj_get(this_val, magic);
    if (!o) return JS_UNDEFINED;
    JSValue s = JS_ToString(ctx, val);
    if (JS_IsException(s)) return s;
    JS_FreeValue(ctx, o->label);
    o->label = s;
    /* Labels only show up in Dawn's error messages; set it on the handle
     * for the types that matter most there. */
    size_t mark = wgj_arena_mark();
    WGPUStringView sv;
    if (wgj_to_sv(ctx, s, &sv) == 0) {
        switch (magic) {
        case WGJ_T_Buffer: wgpuBufferSetLabel((WGPUBuffer)o->h, sv); break;
        case WGJ_T_Texture: wgpuTextureSetLabel((WGPUTexture)o->h, sv); break;
        case WGJ_T_RenderPipeline: wgpuRenderPipelineSetLabel((WGPURenderPipeline)o->h, sv); break;
        case WGJ_T_ShaderModule: wgpuShaderModuleSetLabel((WGPUShaderModule)o->h, sv); break;
        default: break;
        }
    }
    wgj_arena_release(mark);
    return JS_UNDEFINED;
}

/* ── Dictionaries with a JS shape of their own ───────────────────── */

/* GPUExtent3D: [width, height = 1, depth = 1] or {width, height, depthOrArrayLayers} */
static int wgj_conv_Extent3D(JSContext *ctx, JSValueConst o, WGPUExtent3D *out) {
    out->width = 0; out->height = 1; out->depthOrArrayLayers = 1;
    if (JS_IsUndefined(o) || JS_IsNull(o)) return 0;
    uint32_t *f[3] = { &out->width, &out->height, &out->depthOrArrayLayers };
    if (JS_IsArray(o)) {
        uint32_t n; if (wgj_array_len(ctx, o, &n, "GPUExtent3D")) return -1;
        for (uint32_t i = 0; i < n && i < 3; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, o, i);
            int r = JS_ToUint32(ctx, f[i], e); JS_FreeValue(ctx, e);
            if (r) return -1;
        }
        return 0;
    }
    static const char *names[3] = { "width", "height", "depthOrArrayLayers" };
    for (int i = 0; i < 3; i++) {
        JSValue e = JS_GetPropertyStr(ctx, o, names[i]);
        if (JS_IsException(e)) return -1;
        if (!JS_IsUndefined(e) && JS_ToUint32(ctx, f[i], e)) { JS_FreeValue(ctx, e); return -1; }
        JS_FreeValue(ctx, e);
    }
    return 0;
}

/* GPUOrigin3D: [x, y, z] or {x, y, z}, all default 0 */
static int wgj_conv_Origin3D(JSContext *ctx, JSValueConst o, WGPUOrigin3D *out) {
    out->x = out->y = out->z = 0;
    if (JS_IsUndefined(o) || JS_IsNull(o)) return 0;
    uint32_t *f[3] = { &out->x, &out->y, &out->z };
    if (JS_IsArray(o)) {
        uint32_t n; if (wgj_array_len(ctx, o, &n, "GPUOrigin3D")) return -1;
        for (uint32_t i = 0; i < n && i < 3; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, o, i);
            int r = JS_ToUint32(ctx, f[i], e); JS_FreeValue(ctx, e);
            if (r) return -1;
        }
        return 0;
    }
    static const char *names[3] = { "x", "y", "z" };
    for (int i = 0; i < 3; i++) {
        JSValue e = JS_GetPropertyStr(ctx, o, names[i]);
        if (JS_IsException(e)) return -1;
        if (!JS_IsUndefined(e) && JS_ToUint32(ctx, f[i], e)) { JS_FreeValue(ctx, e); return -1; }
        JS_FreeValue(ctx, e);
    }
    return 0;
}

/* GPUColor: [r, g, b, a] or {r, g, b, a} */
static int wgj_conv_Color(JSContext *ctx, JSValueConst o, WGPUColor *out) {
    out->r = out->g = out->b = out->a = 0;
    if (JS_IsUndefined(o) || JS_IsNull(o)) return 0;
    double *f[4] = { &out->r, &out->g, &out->b, &out->a };
    if (JS_IsArray(o)) {
        for (uint32_t i = 0; i < 4; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, o, i);
            int r = JS_IsUndefined(e) ? 0 : JS_ToFloat64(ctx, f[i], e); JS_FreeValue(ctx, e);
            if (r) return -1;
        }
        return 0;
    }
    static const char *names[4] = { "r", "g", "b", "a" };
    for (int i = 0; i < 4; i++) {
        JSValue e = JS_GetPropertyStr(ctx, o, names[i]);
        if (JS_IsException(e)) return -1;
        if (!JS_IsUndefined(e) && JS_ToFloat64(ctx, f[i], e)) { JS_FreeValue(ctx, e); return -1; }
        JS_FreeValue(ctx, e);
    }
    return 0;
}

/* GPUTexelCopyBufferInfo: {buffer, offset, bytesPerRow, rowsPerImage}; C
 * nests the last three in .layout. */
static int wgj_conv_TexelCopyBufferInfo(JSContext *ctx, JSValueConst o, WGPUTexelCopyBufferInfo *out) {
    *out = (WGPUTexelCopyBufferInfo)WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    if (wgj_conv_TexelCopyBufferLayout(ctx, o, &out->layout)) return -1;
    JSValue v = JS_GetPropertyStr(ctx, o, "buffer");
    if (JS_IsException(v)) return -1;
    int r = wgj_unwrap(ctx, v, WGJ_T_Buffer, (void **)&out->buffer, "GPUTexelCopyBufferInfo.buffer");
    JS_FreeValue(ctx, v);
    return r;
}

/* GPUBindGroupEntry: {binding, resource}, where resource is a GPUSampler, a
 * GPUTextureView, a GPUBuffer, a GPUBufferBinding {buffer, offset, size}, or
 * a GPUTexture (its default view, per the current spec). */
static int wgj_conv_BindGroupEntry(JSContext *ctx, JSValueConst o, WGPUBindGroupEntry *out) {
    *out = (WGPUBindGroupEntry)WGPU_BIND_GROUP_ENTRY_INIT;
    JSValue v = JS_GetPropertyStr(ctx, o, "binding");
    if (JS_IsException(v)) return -1;
    int r = JS_ToUint32(ctx, &out->binding, v);
    JS_FreeValue(ctx, v);
    if (r) return -1;
    JSValue res = JS_GetPropertyStr(ctx, o, "resource");
    if (JS_IsException(res)) return -1;
    wgj_obj *w;
    if ((w = wgj_get(res, WGJ_T_Sampler))) out->sampler = w->h;
    else if ((w = wgj_get(res, WGJ_T_TextureView))) out->textureView = w->h;
    else if ((w = wgj_get(res, WGJ_T_Buffer))) out->buffer = w->h;
    else if ((w = wgj_get(res, WGJ_T_Texture))) {
        /* The bind group takes its own reference; ours goes with the call. */
        WGPUTextureView view = wgpuTextureCreateView((WGPUTexture)w->h, NULL);
        out->textureView = view;
        arena_push(view, wgj_release_TextureView);
    } else if (JS_IsObject(res)) {
        JSValue b = JS_GetPropertyStr(ctx, res, "buffer");
        if (wgj_unwrap(ctx, b, WGJ_T_Buffer, (void **)&out->buffer, "GPUBufferBinding.buffer")) { JS_FreeValue(ctx, b); JS_FreeValue(ctx, res); return -1; }
        JS_FreeValue(ctx, b);
        double d;
        JSValue x = JS_GetPropertyStr(ctx, res, "offset");
        if (!JS_IsUndefined(x)) { if (JS_ToFloat64(ctx, &d, x)) { JS_FreeValue(ctx, x); JS_FreeValue(ctx, res); return -1; } out->offset = (uint64_t)d; }
        JS_FreeValue(ctx, x);
        x = JS_GetPropertyStr(ctx, res, "size");
        if (!JS_IsUndefined(x)) { if (JS_ToFloat64(ctx, &d, x)) { JS_FreeValue(ctx, x); JS_FreeValue(ctx, res); return -1; } out->size = (uint64_t)d; }
        JS_FreeValue(ctx, x);
    } else {
        JS_FreeValue(ctx, res);
        JS_ThrowTypeError(ctx, "GPUBindGroupEntry.resource: expected a GPUSampler, GPUTextureView, GPUBuffer or GPUBufferBinding");
        return -1;
    }
    JS_FreeValue(ctx, res);
    return 0;
}

/* GPUShaderModuleDescriptor: {label, code}; C carries code in a chained
 * WGPUShaderSourceWGSL. */
static int wgj_conv_ShaderModuleDescriptor(JSContext *ctx, JSValueConst o, WGPUShaderModuleDescriptor *out) {
    *out = (WGPUShaderModuleDescriptor)WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    WGPUShaderSourceWGSL *src = wgj_alloc(sizeof *src);
    *src = (WGPUShaderSourceWGSL)WGPU_SHADER_SOURCE_WGSL_INIT;
    JSValue v = JS_GetPropertyStr(ctx, o, "code");
    if (JS_IsException(v)) return -1;
    int r = wgj_to_sv(ctx, v, &src->code);
    JS_FreeValue(ctx, v);
    if (r) return -1;
    v = JS_GetPropertyStr(ctx, o, "label");
    if (!JS_IsUndefined(v)) r = wgj_to_sv(ctx, v, &out->label);
    JS_FreeValue(ctx, v);
    out->nextInChain = &src->chain;
    return r;
}

/* constants: record<USVString, GPUPipelineConstantValue> */
static int wgj_conv_constants(JSContext *ctx, JSValueConst v, WGPUConstantEntry const **out, size_t *count) {
    JSPropertyEnum *props; uint32_t n;
    if (JS_GetOwnPropertyNames(ctx, &props, &n, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) return -1;
    WGPUConstantEntry *a = n ? wgj_alloc(sizeof *a * n) : NULL;
    int r = 0;
    for (uint32_t i = 0; i < n && !r; i++) {
        a[i] = (WGPUConstantEntry)WGPU_CONSTANT_ENTRY_INIT;
        const char *k = JS_AtomToCString(ctx, props[i].atom);
        size_t kl = strlen(k);
        char *kc = wgj_alloc(kl + 1); memcpy(kc, k, kl);
        JS_FreeCString(ctx, k);
        a[i].key.data = kc; a[i].key.length = kl;
        JSValue e = JS_GetProperty(ctx, v, props[i].atom);
        r = JS_ToFloat64(ctx, &a[i].value, e);
        JS_FreeValue(ctx, e);
    }
    JS_FreePropertyEnum(ctx, props, n);
    *out = a; *count = n;
    return r;
}

/* ── Raw bytes from a BufferSource ──────────────────────────────── */

static int wgj_bytes(JSContext *ctx, JSValueConst v, uint8_t **ptr, size_t *len, size_t *elem) {
    size_t off, blen, bpe = 1;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &bpe);
    if (!JS_IsException(ab)) {
        size_t total;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, ab);
        JS_FreeValue(ctx, ab);
        if (!base) return -1;
        *ptr = base + off; *len = blen; *elem = bpe ? bpe : 1;
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    uint8_t *p = JS_GetArrayBuffer(ctx, len, v);
    if (p) { *ptr = p; *elem = 1; return 0; }
    JS_FreeValue(ctx, JS_GetException(ctx));
    /* DataView */
    JSValue buf = JS_GetPropertyStr(ctx, v, "buffer");
    JSValue bo = JS_GetPropertyStr(ctx, v, "byteOffset");
    JSValue bl = JS_GetPropertyStr(ctx, v, "byteLength");
    uint32_t o32 = 0, l32 = 0;
    p = JS_IsObject(buf) ? JS_GetArrayBuffer(ctx, len, buf) : NULL;
    JS_ToUint32(ctx, &o32, bo); JS_ToUint32(ctx, &l32, bl);
    JS_FreeValue(ctx, buf); JS_FreeValue(ctx, bo); JS_FreeValue(ctx, bl);
    if (!p) { JS_FreeValue(ctx, JS_GetException(ctx)); JS_ThrowTypeError(ctx, "expected an ArrayBuffer or ArrayBufferView"); return -1; }
    *ptr = p + o32; *len = l32; *elem = 1;
    return 0;
}

/* ── Promises settled between frames ────────────────────────────── */

enum { PK_ADAPTER = 1, PK_DEVICE, PK_MAP, PK_WORKDONE, PK_POPERR, PK_RPIPE, PK_CPIPE, PK_COMPINFO };

typedef struct {
    int state;          /* 0 free, 1 waiting, 2 done */
    int kind;
    JSValue resolve, reject, keep;
    uint32_t status, etype;
    char *msg;
    void *handle;
    uint32_t map_mode;
    uint64_t map_offset, map_size;
    /* compilation info copy */
    uint32_t nmsg;
    struct { char *text; uint32_t type; uint64_t line, pos, off, len; } *msgs;
} wgj_pending;

static wgj_pending *pend;
static size_t pend_cap;
static int pend_done_count;
static JSContext *wgj_ctx;

static int pend_new(JSContext *ctx, int kind, JSValue *promise, JSValueConst keep) {
    size_t i;
    for (i = 0; i < pend_cap && pend[i].state; i++) {}
    if (i == pend_cap) {
        size_t ncap = pend_cap ? pend_cap * 2 : 64;
        pend = realloc(pend, ncap * sizeof *pend);
        memset(pend + pend_cap, 0, (ncap - pend_cap) * sizeof *pend);
        pend_cap = ncap;
    }
    JSValue funcs[2];
    *promise = JS_NewPromiseCapability(ctx, funcs);
    memset(&pend[i], 0, sizeof pend[i]);
    pend[i].state = 1;
    pend[i].kind = kind;
    pend[i].resolve = funcs[0];
    pend[i].reject = funcs[1];
    pend[i].keep = JS_DupValue(ctx, keep);
    return (int)i;
}

static char *sv_dup(WGPUStringView s) {
    if (!s.data) return NULL;
    size_t n = s.length == WGPU_STRLEN ? strlen(s.data) : s.length;
    char *c = malloc(n + 1);
    memcpy(c, s.data, n); c[n] = 0;
    return c;
}

static wgj_pending *pend_from(void *ud) {
    size_t i = (size_t)(uintptr_t)ud;
    if (i >= pend_cap || pend[i].state != 1) return NULL;
    return &pend[i];
}
static void pend_finish(wgj_pending *p) { p->state = 2; pend_done_count++; }

static void cb_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView msg, void *u1, void *u2) {
    wgj_pending *p = pend_from(u1); if (!p) return;
    p->status = status; p->handle = adapter; p->msg = sv_dup(msg);
    if (adapter) wgpuAdapterAddRef(adapter);
    pend_finish(p);
}
static void cb_device(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView msg, void *u1, void *u2) {
    wgj_pending *p = pend_from(u1); if (!p) return;
    p->status = status; p->handle = device; p->msg = sv_dup(msg);
    if (device) wgpuDeviceAddRef(device);
    pend_finish(p);
}
static void cb_map(WGPUMapAsyncStatus status, WGPUStringView msg, void *u1, void *u2) {
    wgj_pending *p = pend_from(u1); if (!p) return;
    p->status = status; p->msg = sv_dup(msg);
    pend_finish(p);
}
static void cb_workdone(WGPUQueueWorkDoneStatus status, WGPUStringView msg, void *u1, void *u2) {
    wgj_pending *p = pend_from(u1); if (!p) return;
    p->status = status; p->msg = sv_dup(msg);
    pend_finish(p);
}
static void cb_poperr(WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView msg, void *u1, void *u2) {
    wgj_pending *p = pend_from(u1); if (!p) return;
    p->status = status; p->etype = type; p->msg = sv_dup(msg);
    pend_finish(p);
}
static void cb_rpipe(WGPUCreatePipelineAsyncStatus status, WGPURenderPipeline pipe, WGPUStringView msg, void *u1, void *u2) {
    wgj_pending *p = pend_from(u1); if (!p) return;
    p->status = status; p->handle = pipe; p->msg = sv_dup(msg);
    pend_finish(p);
}
static void cb_cpipe(WGPUCreatePipelineAsyncStatus status, WGPUComputePipeline pipe, WGPUStringView msg, void *u1, void *u2) {
    wgj_pending *p = pend_from(u1); if (!p) return;
    p->status = status; p->handle = pipe; p->msg = sv_dup(msg);
    pend_finish(p);
}
static void cb_compinfo(WGPUCompilationInfoRequestStatus status, struct WGPUCompilationInfo const *info, void *u1, void *u2) {
    wgj_pending *p = pend_from(u1); if (!p) return;
    p->status = status;
    if (info && info->messageCount) {
        p->nmsg = (uint32_t)info->messageCount;
        p->msgs = calloc(p->nmsg, sizeof *p->msgs);
        for (uint32_t i = 0; i < p->nmsg; i++) {
            const WGPUCompilationMessage *m = &info->messages[i];
            p->msgs[i].text = sv_dup(m->message);
            p->msgs[i].type = m->type;
            p->msgs[i].line = m->lineNum; p->msgs[i].pos = m->linePos;
            p->msgs[i].off = m->offset; p->msgs[i].len = m->length;
        }
    }
    pend_finish(p);
}

static JSValue wgj_new_error(JSContext *ctx, const char *cls, const char *msg) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, cls);
    JSValue m = JS_NewString(ctx, msg ? msg : "");
    JSValue e = JS_CallConstructor(ctx, ctor, 1, (JSValueConst *)&m);
    JS_FreeValue(ctx, m); JS_FreeValue(ctx, ctor); JS_FreeValue(ctx, global);
    return e;
}

static void pend_settle(JSContext *ctx, wgj_pending *p) {
    JSValue val = JS_UNDEFINED;
    int ok = 1;
    switch (p->kind) {
    case PK_ADAPTER:
        /* A missing adapter resolves null (WebGPU spec), not a rejection. */
        val = (p->status == WGPURequestAdapterStatus_Success && p->handle) ? wgj_wrap(ctx, WGJ_T_Adapter, p->handle) : JS_NULL;
        break;
    case PK_DEVICE:
        if (p->status == WGPURequestDeviceStatus_Success && p->handle) val = wgj_wrap(ctx, WGJ_T_Device, p->handle);
        else { ok = 0; val = wgj_new_error(ctx, "Error", p->msg ? p->msg : "requestDevice failed"); }
        break;
    case PK_MAP: {
        wgj_obj *o = wgj_get(p->keep, WGJ_T_Buffer);
        if (p->status == WGPUMapAsyncStatus_Success) {
            if (o) { o->map_mode = p->map_mode; o->map_offset = p->map_offset; o->map_size = p->map_size; }
        } else {
            ok = 0;
            val = wgj_new_error(ctx, p->status == WGPUMapAsyncStatus_Aborted ? "AbortError" : "OperationError",
                                p->msg ? p->msg : "mapAsync failed");
        }
        break;
    }
    case PK_WORKDONE:
        break;
    case PK_POPERR:
        if (p->status != WGPUPopErrorScopeStatus_Success) { ok = 0; val = wgj_new_error(ctx, "OperationError", p->msg ? p->msg : "popErrorScope failed"); }
        else if (p->etype == WGPUErrorType_NoError) val = JS_NULL;
        else if (p->etype == WGPUErrorType_Validation) val = wgj_new_error(ctx, "GPUValidationError", p->msg);
        else if (p->etype == WGPUErrorType_OutOfMemory) val = wgj_new_error(ctx, "GPUOutOfMemoryError", p->msg);
        else val = wgj_new_error(ctx, "GPUInternalError", p->msg);
        break;
    case PK_RPIPE:
    case PK_CPIPE:
        if (p->status == WGPUCreatePipelineAsyncStatus_Success && p->handle)
            val = wgj_wrap(ctx, p->kind == PK_RPIPE ? WGJ_T_RenderPipeline : WGJ_T_ComputePipeline, p->handle);
        else { ok = 0; val = wgj_new_error(ctx, "GPUPipelineError", p->msg ? p->msg : "pipeline creation failed"); }
        break;
    case PK_COMPINFO: {
        JSValue info = JS_NewObject(ctx);
        JSValue arr = JS_NewArray(ctx);
        for (uint32_t i = 0; i < p->nmsg; i++) {
            JSValue m = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, m, "message", JS_NewString(ctx, p->msgs[i].text ? p->msgs[i].text : ""));
            JS_SetPropertyStr(ctx, m, "type", wgj_enum_CompilationMessageType_to_js(ctx, p->msgs[i].type));
            JS_SetPropertyStr(ctx, m, "lineNum", JS_NewFloat64(ctx, (double)p->msgs[i].line));
            JS_SetPropertyStr(ctx, m, "linePos", JS_NewFloat64(ctx, (double)p->msgs[i].pos));
            JS_SetPropertyStr(ctx, m, "offset", JS_NewFloat64(ctx, (double)p->msgs[i].off));
            JS_SetPropertyStr(ctx, m, "length", JS_NewFloat64(ctx, (double)p->msgs[i].len));
            JS_SetPropertyUint32(ctx, arr, i, m);
            free(p->msgs[i].text);
        }
        free(p->msgs); p->msgs = NULL;
        JS_SetPropertyStr(ctx, info, "messages", arr);
        val = info;
        break;
    }
    }
    JSValue r = JS_Call(ctx, ok ? p->resolve : p->reject, JS_UNDEFINED, 1, (JSValueConst *)&val);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    JS_FreeValue(ctx, p->keep);
    free(p->msg);
    memset(p, 0, sizeof *p);
}

/* Start of every frame: settle what completed since the last one. */
static uint32_t wgj_frame_serial;
void webgpu_frame_begin(void) {
    wgj_frame_serial++;
    if (!wgj_ctx || !pend_done_count) return;
    for (size_t i = 0; i < pend_cap; i++) {
        if (pend[i].state == 2) { pend_done_count--; pend_settle(wgj_ctx, &pend[i]); }
    }
}

#define UD(i) ((void *)(uintptr_t)(i))

/* ── navigator.gpu ─────────────────────────────────────────────── */

static WGPUInstance wgj_instance;
static WGPUSurface wgj_surface;

static WGPUSurface wgj_get_surface(void) {
    if (!wgj_surface) {
        if (!wgj_instance) wgj_instance = wgpuCreateInstance(NULL);
        WGPUEmscriptenSurfaceSourceCanvasHTMLSelector sel = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
        sel.selector.data = "#canvas";
        sel.selector.length = 7;
        WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
        sd.nextInChain = &sel.chain;
        wgj_surface = wgpuInstanceCreateSurface(wgj_instance, &sd);
    }
    return wgj_surface;
}

static JSValue js_gpu_request_adapter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!wgj_instance) wgj_instance = wgpuCreateInstance(NULL);
    size_t mark = wgj_arena_mark();
    WGPURequestAdapterOptions opts;
    if (wgj_conv_RequestAdapterOptions(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &opts)) { wgj_arena_release(mark); return JS_EXCEPTION; }
    JSValue promise;
    int id = pend_new(ctx, PK_ADAPTER, &promise, JS_UNDEFINED);
    WGPURequestAdapterCallbackInfo ci = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowSpontaneous;
    ci.callback = cb_adapter;
    ci.userdata1 = UD(id);
    wgpuInstanceRequestAdapter(wgj_instance, &opts, ci);
    wgj_arena_release(mark);
    return promise;
}

static JSValue js_gpu_preferred_format(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(wgj_get_surface(), NULL, &caps) != WGPUStatus_Success || !caps.formatCount)
        return JS_NewString(ctx, "rgba8unorm");
    JSValue r = wgj_enum_TextureFormat_to_js(ctx, caps.formats[0]);
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    return r;
}

/* ── GPUAdapter / GPUDevice attributes ───────────────────────────── */

static JSValue wgj_features_to_set(JSContext *ctx, WGPUSupportedFeatures *f) {
    JSValue arr = JS_NewArray(ctx);
    uint32_t n = 0;
    for (size_t i = 0; i < f->featureCount; i++) {
        JSValue s = wgj_enum_FeatureName_to_js(ctx, f->features[i]);
        if (!JS_IsUndefined(s)) JS_SetPropertyUint32(ctx, arr, n++, s);
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue set_ctor = JS_GetPropertyStr(ctx, global, "Set");
    JSValue set = JS_CallConstructor(ctx, set_ctor, 1, (JSValueConst *)&arr);
    JS_FreeValue(ctx, set_ctor); JS_FreeValue(ctx, global); JS_FreeValue(ctx, arr);
    return set;
}

static JSValue js_adapter_features(JSContext *ctx, JSValueConst this_val) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Adapter); if (!o) return JS_UNDEFINED;
    if (JS_IsUndefined(o->slots[SLOT_FEATURES])) {
        WGPUSupportedFeatures f = WGPU_SUPPORTED_FEATURES_INIT;
        wgpuAdapterGetFeatures((WGPUAdapter)o->h, &f);
        o->slots[SLOT_FEATURES] = wgj_features_to_set(ctx, &f);
        wgpuSupportedFeaturesFreeMembers(f);
    }
    return JS_DupValue(ctx, o->slots[SLOT_FEATURES]);
}
static JSValue js_adapter_limits(JSContext *ctx, JSValueConst this_val) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Adapter); if (!o) return JS_UNDEFINED;
    if (JS_IsUndefined(o->slots[SLOT_LIMITS])) {
        WGPULimits l = WGPU_LIMITS_INIT;
        wgpuAdapterGetLimits((WGPUAdapter)o->h, &l);
        o->slots[SLOT_LIMITS] = wgj_out_Limits(ctx, &l);
    }
    return JS_DupValue(ctx, o->slots[SLOT_LIMITS]);
}
static JSValue js_adapter_info(JSContext *ctx, JSValueConst this_val) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Adapter); if (!o) return JS_UNDEFINED;
    if (JS_IsUndefined(o->slots[SLOT_INFO])) {
        WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
        wgpuAdapterGetInfo((WGPUAdapter)o->h, &info);
        o->slots[SLOT_INFO] = wgj_out_AdapterInfo(ctx, &info);
        wgpuAdapterInfoFreeMembers(info);
    }
    return JS_DupValue(ctx, o->slots[SLOT_INFO]);
}
static JSValue js_false_getter(JSContext *ctx, JSValueConst this_val) { return JS_FALSE; }

static JSValue js_adapter_request_device(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    WGPUAdapter a; if (wgj_unwrap_this(ctx, this_val, WGJ_T_Adapter, (void **)&a)) return JS_EXCEPTION;
    size_t mark = wgj_arena_mark();
    WGPUDeviceDescriptor d;
    if (wgj_conv_DeviceDescriptor(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &d)) { wgj_arena_release(mark); return JS_EXCEPTION; }
    JSValue promise;
    int id = pend_new(ctx, PK_DEVICE, &promise, JS_UNDEFINED);
    WGPURequestDeviceCallbackInfo ci = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowSpontaneous;
    ci.callback = cb_device;
    ci.userdata1 = UD(id);
    wgpuAdapterRequestDevice(a, &d, ci);
    wgj_arena_release(mark);
    return promise;
}

static JSValue js_device_features(JSContext *ctx, JSValueConst this_val) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Device); if (!o) return JS_UNDEFINED;
    if (JS_IsUndefined(o->slots[SLOT_FEATURES])) {
        WGPUSupportedFeatures f = WGPU_SUPPORTED_FEATURES_INIT;
        wgpuDeviceGetFeatures((WGPUDevice)o->h, &f);
        o->slots[SLOT_FEATURES] = wgj_features_to_set(ctx, &f);
        wgpuSupportedFeaturesFreeMembers(f);
    }
    return JS_DupValue(ctx, o->slots[SLOT_FEATURES]);
}
static JSValue js_device_limits(JSContext *ctx, JSValueConst this_val) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Device); if (!o) return JS_UNDEFINED;
    if (JS_IsUndefined(o->slots[SLOT_LIMITS])) {
        WGPULimits l = WGPU_LIMITS_INIT;
        wgpuDeviceGetLimits((WGPUDevice)o->h, &l);
        o->slots[SLOT_LIMITS] = wgj_out_Limits(ctx, &l);
    }
    return JS_DupValue(ctx, o->slots[SLOT_LIMITS]);
}
static JSValue js_device_adapter_info(JSContext *ctx, JSValueConst this_val) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Device); if (!o) return JS_UNDEFINED;
    if (JS_IsUndefined(o->slots[SLOT_INFO])) {
        WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
        wgpuDeviceGetAdapterInfo((WGPUDevice)o->h, &info);
        o->slots[SLOT_INFO] = wgj_out_AdapterInfo(ctx, &info);
        wgpuAdapterInfoFreeMembers(info);
    }
    return JS_DupValue(ctx, o->slots[SLOT_INFO]);
}
static JSValue js_device_queue(JSContext *ctx, JSValueConst this_val) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Device); if (!o) return JS_UNDEFINED;
    if (JS_IsUndefined(o->slots[SLOT_QUEUE]))
        o->slots[SLOT_QUEUE] = wgj_wrap(ctx, WGJ_T_Queue, wgpuDeviceGetQueue((WGPUDevice)o->h));
    return JS_DupValue(ctx, o->slots[SLOT_QUEUE]);
}
/* The host owns the device and its lifetime; a cart cannot register a
 * device-lost callback on a device it did not create, so this promise only
 * settles if the cart's own destroy() is called. */
static JSValue js_device_lost(JSContext *ctx, JSValueConst this_val) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Device); if (!o) return JS_UNDEFINED;
    if (JS_IsUndefined(o->slots[SLOT_LOST])) {
        JSValue funcs[2];
        o->slots[SLOT_LOST] = JS_NewPromiseCapability(ctx, funcs);
        o->slots[SLOT_LOST_RESOLVE] = funcs[0];
        JS_FreeValue(ctx, funcs[1]);
    }
    return JS_DupValue(ctx, o->slots[SLOT_LOST]);
}
static JSValue js_device_destroy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Device); if (!o) return JS_UNDEFINED;
    /* Never destroy the HOST's device from the cart: settle lost and stop. */
    if (!JS_IsUndefined(o->slots[SLOT_LOST_RESOLVE])) {
        JSValue info = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, info, "reason", JS_NewString(ctx, "destroyed"));
        JS_SetPropertyStr(ctx, info, "message", JS_NewString(ctx, "device.destroy() called by the cart"));
        JSValue r = JS_Call(ctx, o->slots[SLOT_LOST_RESOLVE], JS_UNDEFINED, 1, (JSValueConst *)&info);
        JS_FreeValue(ctx, r); JS_FreeValue(ctx, info);
        JS_FreeValue(ctx, o->slots[SLOT_LOST_RESOLVE]); o->slots[SLOT_LOST_RESOLVE] = JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

static JSValue js_device_pop_error_scope(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    WGPUDevice d; if (wgj_unwrap_this(ctx, this_val, WGJ_T_Device, (void **)&d)) return JS_EXCEPTION;
    JSValue promise;
    int id = pend_new(ctx, PK_POPERR, &promise, JS_UNDEFINED);
    WGPUPopErrorScopeCallbackInfo ci = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowSpontaneous;
    ci.callback = cb_poperr;
    ci.userdata1 = UD(id);
    wgpuDevicePopErrorScope(d, ci);
    return promise;
}

static JSValue js_device_create_rpipe_async(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    WGPUDevice d; if (wgj_unwrap_this(ctx, this_val, WGJ_T_Device, (void **)&d)) return JS_EXCEPTION;
    size_t mark = wgj_arena_mark();
    WGPURenderPipelineDescriptor desc;
    if (wgj_conv_RenderPipelineDescriptor(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &desc)) { wgj_arena_release(mark); return JS_EXCEPTION; }
    JSValue promise;
    int id = pend_new(ctx, PK_RPIPE, &promise, JS_UNDEFINED);
    WGPUCreateRenderPipelineAsyncCallbackInfo ci = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowSpontaneous;
    ci.callback = cb_rpipe;
    ci.userdata1 = UD(id);
    wgpuDeviceCreateRenderPipelineAsync(d, &desc, ci);
    wgj_arena_release(mark);
    return promise;
}

static JSValue js_device_create_cpipe_async(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    WGPUDevice d; if (wgj_unwrap_this(ctx, this_val, WGJ_T_Device, (void **)&d)) return JS_EXCEPTION;
    size_t mark = wgj_arena_mark();
    WGPUComputePipelineDescriptor desc;
    if (wgj_conv_ComputePipelineDescriptor(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &desc)) { wgj_arena_release(mark); return JS_EXCEPTION; }
    JSValue promise;
    int id = pend_new(ctx, PK_CPIPE, &promise, JS_UNDEFINED);
    WGPUCreateComputePipelineAsyncCallbackInfo ci = WGPU_CREATE_COMPUTE_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowSpontaneous;
    ci.callback = cb_cpipe;
    ci.userdata1 = UD(id);
    wgpuDeviceCreateComputePipelineAsync(d, &desc, ci);
    wgj_arena_release(mark);
    return promise;
}

static JSValue js_noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return JS_UNDEFINED; }

/* ── GPUBuffer mapping ─────────────────────────────────────────── */

static void wgj_detach_mapped(JSContext *ctx, wgj_obj *o) {
    JSValue arr = o->slots[SLOT_MAPPED];
    if (JS_IsUndefined(arr)) return;
    uint32_t n = 0;
    wgj_array_len(ctx, arr, &n, "mapped");
    for (uint32_t i = 0; i < n; i++) {
        JSValue ab = JS_GetPropertyUint32(ctx, arr, i);
        JS_DetachArrayBuffer(ctx, ab);
        JS_FreeValue(ctx, ab);
    }
    JS_FreeValue(ctx, arr);
    o->slots[SLOT_MAPPED] = JS_UNDEFINED;
}

static JSValue js_buffer_map_async(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Buffer);
    if (!o) return JS_ThrowTypeError(ctx, "not a GPUBuffer");
    double mode = 0, off = 0, size = -1;
    if (argc > 0 && JS_ToFloat64(ctx, &mode, argv[0])) return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &off, argv[1])) return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &size, argv[2])) return JS_EXCEPTION;
    uint64_t total = wgpuBufferGetSize((WGPUBuffer)o->h);
    uint64_t sz = size < 0 ? (total > (uint64_t)off ? total - (uint64_t)off : 0) : (uint64_t)size;
    JSValue promise;
    int id = pend_new(ctx, PK_MAP, &promise, this_val);
    pend[id].map_mode = (uint32_t)mode;
    pend[id].map_offset = (uint64_t)off;
    pend[id].map_size = sz;
    WGPUBufferMapCallbackInfo ci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowSpontaneous;
    ci.callback = cb_map;
    ci.userdata1 = UD(id);
    wgpuBufferMapAsync((WGPUBuffer)o->h, (WGPUMapMode)mode, (size_t)off, (size_t)sz, ci);
    return promise;
}

static JSValue js_buffer_get_mapped_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Buffer);
    if (!o) return JS_ThrowTypeError(ctx, "not a GPUBuffer");
    double off = 0, size = -1;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToFloat64(ctx, &off, argv[0])) return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &size, argv[1])) return JS_EXCEPTION;
    uint64_t end;
    if (o->map_mode) end = o->map_offset + o->map_size;
    else end = wgpuBufferGetSize((WGPUBuffer)o->h);   /* mappedAtCreation */
    uint64_t sz = size < 0 ? (end > (uint64_t)off ? end - (uint64_t)off : 0) : (uint64_t)size;
    void *p = NULL;
    if (o->map_mode != WGPUMapMode_Read) p = wgpuBufferGetMappedRange((WGPUBuffer)o->h, (size_t)off, (size_t)sz);
    if (!p) p = (void *)wgpuBufferGetConstMappedRange((WGPUBuffer)o->h, (size_t)off, (size_t)sz);
    if (!p) return JS_Throw(ctx, wgj_new_error(ctx, "OperationError", "getMappedRange: the buffer is not mapped for that range"));
    JSValue ab = JS_NewArrayBuffer(ctx, (uint8_t *)p, (size_t)sz, NULL, NULL, false);
    if (JS_IsUndefined(o->slots[SLOT_MAPPED])) o->slots[SLOT_MAPPED] = JS_NewArray(ctx);
    uint32_t n = 0;
    wgj_array_len(ctx, o->slots[SLOT_MAPPED], &n, "mapped");
    JS_SetPropertyUint32(ctx, o->slots[SLOT_MAPPED], n, JS_DupValue(ctx, ab));
    return ab;
}

static JSValue js_buffer_unmap(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Buffer);
    if (!o) return JS_ThrowTypeError(ctx, "not a GPUBuffer");
    wgj_detach_mapped(ctx, o);
    wgpuBufferUnmap((WGPUBuffer)o->h);
    o->map_mode = 0;
    return JS_UNDEFINED;
}

static JSValue js_buffer_destroy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    wgj_obj *o = wgj_get(this_val, WGJ_T_Buffer);
    if (!o) return JS_ThrowTypeError(ctx, "not a GPUBuffer");
    wgj_detach_mapped(ctx, o);
    wgpuBufferDestroy((WGPUBuffer)o->h);
    o->map_mode = 0;
    return JS_UNDEFINED;
}

/* ── GPUQueue ──────────────────────────────────────────────────── */

static JSValue js_queue_write_buffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    WGPUQueue q; if (wgj_unwrap_this(ctx, this_val, WGJ_T_Queue, (void **)&q)) return JS_EXCEPTION;
    if (argc < 3) return JS_ThrowTypeError(ctx, "writeBuffer: expected (buffer, bufferOffset, data, dataOffset?, size?)");
    WGPUBuffer b;
    if (wgj_unwrap(ctx, argv[0], WGJ_T_Buffer, (void **)&b, "writeBuffer: buffer")) return JS_EXCEPTION;
    double boff; if (JS_ToFloat64(ctx, &boff, argv[1])) return JS_EXCEPTION;
    uint8_t *p; size_t len, elem;
    if (wgj_bytes(ctx, argv[2], &p, &len, &elem)) return JS_EXCEPTION;
    double doff = 0, dsize = -1;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && JS_ToFloat64(ctx, &doff, argv[3])) return JS_EXCEPTION;
    if (argc > 4 && !JS_IsUndefined(argv[4]) && JS_ToFloat64(ctx, &dsize, argv[4])) return JS_EXCEPTION;
    size_t start = (size_t)doff * elem;
    if (start > len) return JS_ThrowRangeError(ctx, "writeBuffer: dataOffset is past the end of data");
    size_t n = dsize < 0 ? len - start : (size_t)dsize * elem;
    if (start + n > len) return JS_ThrowRangeError(ctx, "writeBuffer: size is past the end of data");
    wgpuQueueWriteBuffer(q, b, (uint64_t)boff, p + start, n);
    return JS_UNDEFINED;
}

static JSValue js_queue_write_texture(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    WGPUQueue q; if (wgj_unwrap_this(ctx, this_val, WGJ_T_Queue, (void **)&q)) return JS_EXCEPTION;
    if (argc < 4) return JS_ThrowTypeError(ctx, "writeTexture: expected (destination, data, dataLayout, size)");
    size_t mark = wgj_arena_mark();
    WGPUTexelCopyTextureInfo dst; WGPUTexelCopyBufferLayout layout; WGPUExtent3D size;
    uint8_t *p; size_t len, elem;
    if (wgj_conv_TexelCopyTextureInfo(ctx, argv[0], &dst) || wgj_bytes(ctx, argv[1], &p, &len, &elem) ||
        wgj_conv_TexelCopyBufferLayout(ctx, argv[2], &layout) || wgj_conv_Extent3D(ctx, argv[3], &size)) {
        wgj_arena_release(mark);
        return JS_EXCEPTION;
    }
    wgpuQueueWriteTexture(q, &dst, p, len, &layout, &size);
    wgj_arena_release(mark);
    return JS_UNDEFINED;
}

/* copyExternalImageToTexture({source, origin?, flipY?}, destination, size):
 * the cart has no DOM images, but its Image/ImageData-like objects carry
 * decoded RGBA8 (img._rgba, or .data), which is written with writeTexture. */
static JSValue js_queue_copy_external_image(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    WGPUQueue q; if (wgj_unwrap_this(ctx, this_val, WGJ_T_Queue, (void **)&q)) return JS_EXCEPTION;
    if (argc < 3) return JS_ThrowTypeError(ctx, "copyExternalImageToTexture: expected (source, destination, copySize)");
    JSValue src = JS_GetPropertyStr(ctx, argv[0], "source");
    JSValue flipv = JS_GetPropertyStr(ctx, argv[0], "flipY");
    int flip = JS_ToBool(ctx, flipv);
    JS_FreeValue(ctx, flipv);
    JSValue px = JS_GetPropertyStr(ctx, src, "_rgba");
    if (JS_IsUndefined(px) || JS_IsNull(px)) { JS_FreeValue(ctx, px); px = JS_GetPropertyStr(ctx, src, "data"); }
    uint32_t sw = 0, sh = 0;
    JSValue t = JS_GetPropertyStr(ctx, src, "width"); JS_ToUint32(ctx, &sw, t); JS_FreeValue(ctx, t);
    t = JS_GetPropertyStr(ctx, src, "height"); JS_ToUint32(ctx, &sh, t); JS_FreeValue(ctx, t);
    JS_FreeValue(ctx, src);
    uint8_t *p; size_t len, elem;
    if (JS_IsUndefined(px) || wgj_bytes(ctx, px, &p, &len, &elem) || len < (size_t)sw * sh * 4) {
        JS_FreeValue(ctx, px);
        return JS_ThrowTypeError(ctx, "copyExternalImageToTexture: the source has no RGBA pixels (this runtime supports decoded Image and ImageData-like sources)");
    }
    size_t mark = wgj_arena_mark();
    WGPUTexelCopyTextureInfo dst; WGPUExtent3D size;
    if (wgj_conv_TexelCopyTextureInfo(ctx, argv[1], &dst) || wgj_conv_Extent3D(ctx, argv[2], &size)) {
        JS_FreeValue(ctx, px); wgj_arena_release(mark); return JS_EXCEPTION;
    }
    const uint8_t *data = p;
    if (flip) {
        uint8_t *f = wgj_alloc((size_t)sw * sh * 4);
        for (uint32_t y = 0; y < sh; y++) memcpy(f + (size_t)y * sw * 4, p + (size_t)(sh - 1 - y) * sw * 4, (size_t)sw * 4);
        data = f;
    }
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = sw * 4;
    layout.rowsPerImage = sh;
    wgpuQueueWriteTexture(q, &dst, data, (size_t)sw * sh * 4, &layout, &size);
    JS_FreeValue(ctx, px);
    wgj_arena_release(mark);
    return JS_UNDEFINED;
}

static JSValue js_queue_work_done(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    WGPUQueue q; if (wgj_unwrap_this(ctx, this_val, WGJ_T_Queue, (void **)&q)) return JS_EXCEPTION;
    JSValue promise;
    int id = pend_new(ctx, PK_WORKDONE, &promise, JS_UNDEFINED);
    WGPUQueueWorkDoneCallbackInfo ci = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowSpontaneous;
    ci.callback = cb_workdone;
    ci.userdata1 = UD(id);
    wgpuQueueOnSubmittedWorkDone(q, ci);
    return promise;
}

static JSValue js_shader_compilation_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    WGPUShaderModule m; if (wgj_unwrap_this(ctx, this_val, WGJ_T_ShaderModule, (void **)&m)) return JS_EXCEPTION;
    JSValue promise;
    int id = pend_new(ctx, PK_COMPINFO, &promise, JS_UNDEFINED);
    WGPUCompilationInfoCallbackInfo ci = WGPU_COMPILATION_INFO_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowSpontaneous;
    ci.callback = cb_compinfo;
    ci.userdata1 = UD(id);
    wgpuShaderModuleGetCompilationInfo(m, ci);
    return promise;
}

/* ── GPUCanvasContext over the "#canvas" surface ────────────────── */

static struct {
    int configured;
    JSValue canvas;          /* the JS canvas (for width/height) */
    JSValue device;          /* configured GPUDevice wrapper */
    JSValue config;          /* the dictionary passed to configure() */
    WGPUSurfaceConfiguration cfg;
    WGPUTextureFormat view_formats[8];
    JSValue current;         /* this frame's texture wrapper */
    uint32_t current_serial;
} cctx = { 0, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED };

static void canvas_size(JSContext *ctx, uint32_t *w, uint32_t *h) {
    JSValue v = JS_GetPropertyStr(ctx, cctx.canvas, "width"); JS_ToUint32(ctx, w, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, cctx.canvas, "height"); JS_ToUint32(ctx, h, v); JS_FreeValue(ctx, v);
}

static JSValue js_ctx_configure(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "configure: expected a GPUCanvasConfiguration");
    JSValueConst c = argv[0];
    WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
    JSValue dev = JS_GetPropertyStr(ctx, c, "device");
    if (wgj_unwrap(ctx, dev, WGJ_T_Device, (void **)&cfg.device, "configure: device") || !cfg.device) {
        JS_FreeValue(ctx, dev);
        if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "configure: device is required");
        return JS_EXCEPTION;
    }
    JSValue v = JS_GetPropertyStr(ctx, c, "format");
    int bad = wgj_enum_TextureFormat(ctx, v, &cfg.format);
    JS_FreeValue(ctx, v);
    if (bad) { JS_FreeValue(ctx, dev); return JS_EXCEPTION; }
    v = JS_GetPropertyStr(ctx, c, "usage");
    double usage = WGPUTextureUsage_RenderAttachment;
    if (!JS_IsUndefined(v)) JS_ToFloat64(ctx, &usage, v);
    JS_FreeValue(ctx, v);
    cfg.usage = (WGPUTextureUsage)usage;
    v = JS_GetPropertyStr(ctx, c, "alphaMode");
    cfg.alphaMode = WGPUCompositeAlphaMode_Opaque;
    if (!JS_IsUndefined(v) && wgj_enum_CompositeAlphaMode(ctx, v, &cfg.alphaMode)) { JS_FreeValue(ctx, v); JS_FreeValue(ctx, dev); return JS_EXCEPTION; }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, c, "viewFormats");
    cfg.viewFormatCount = 0;
    if (JS_IsObject(v)) {
        uint32_t n = 0; wgj_array_len(ctx, v, &n, "viewFormats");
        for (uint32_t i = 0; i < n && i < 8; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            if (wgj_enum_TextureFormat(ctx, e, &cctx.view_formats[i])) { JS_FreeValue(ctx, e); JS_FreeValue(ctx, v); JS_FreeValue(ctx, dev); return JS_EXCEPTION; }
            JS_FreeValue(ctx, e);
            cfg.viewFormatCount++;
        }
    }
    JS_FreeValue(ctx, v);
    cfg.viewFormats = cctx.view_formats;
    cfg.presentMode = WGPUPresentMode_Fifo;
    canvas_size(ctx, &cfg.width, &cfg.height);
    wgpuSurfaceConfigure(wgj_get_surface(), &cfg);
    cctx.cfg = cfg;
    JS_FreeValue(ctx, cctx.device); cctx.device = dev;
    JS_FreeValue(ctx, cctx.config); cctx.config = JS_DupValue(ctx, c);
    JS_FreeValue(ctx, cctx.current); cctx.current = JS_UNDEFINED;
    cctx.configured = 1;
    return JS_UNDEFINED;
}

static JSValue js_ctx_unconfigure(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (cctx.configured) wgpuSurfaceUnconfigure(wgj_get_surface());
    cctx.configured = 0;
    JS_FreeValue(ctx, cctx.current); cctx.current = JS_UNDEFINED;
    return JS_UNDEFINED;
}

static JSValue js_ctx_get_configuration(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return cctx.configured ? JS_DupValue(ctx, cctx.config) : JS_NULL;
}

static JSValue js_ctx_get_current_texture(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!cctx.configured) return JS_Throw(ctx, wgj_new_error(ctx, "InvalidStateError", "getCurrentTexture: the canvas context is not configured"));
    /* The same texture for the whole frame, as in a browser. */
    if (!JS_IsUndefined(cctx.current) && cctx.current_serial == wgj_frame_serial)
        return JS_DupValue(ctx, cctx.current);
    /* A canvas resize reconfigures, as a browser does implicitly. */
    uint32_t w, h;
    canvas_size(ctx, &w, &h);
    if (w != cctx.cfg.width || h != cctx.cfg.height) {
        cctx.cfg.width = w; cctx.cfg.height = h;
        wgpuSurfaceConfigure(wgj_get_surface(), &cctx.cfg);
    }
    WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(wgj_get_surface(), &st);
    if (!st.texture) return JS_Throw(ctx, wgj_new_error(ctx, "OperationError", "getCurrentTexture: the surface has no texture"));
    JS_FreeValue(ctx, cctx.current);
    cctx.current = wgj_wrap(ctx, WGJ_T_Texture, st.texture);
    cctx.current_serial = wgj_frame_serial;
    return JS_DupValue(ctx, cctx.current);
}

static JSValue js_ctx_canvas(JSContext *ctx, JSValueConst this_val) { return JS_DupValue(ctx, cctx.canvas); }

static const JSCFunctionListEntry ctx_proto[] = {
    JS_CFUNC_DEF("configure", 1, js_ctx_configure),
    JS_CFUNC_DEF("unconfigure", 0, js_ctx_unconfigure),
    JS_CFUNC_DEF("getConfiguration", 0, js_ctx_get_configuration),
    JS_CFUNC_DEF("getCurrentTexture", 0, js_ctx_get_current_texture),
    JS_CGETSET_DEF("canvas", js_ctx_canvas, NULL),
};

static JSValue wgj_canvas_ctx_obj = JS_UNDEFINED;

/* _wcGPUCanvasContext(canvas): the main canvas's WebGPU context (one per cart). */
static JSValue js_get_canvas_context(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (JS_IsUndefined(wgj_canvas_ctx_obj)) {
        wgj_canvas_ctx_obj = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, wgj_canvas_ctx_obj, ctx_proto, countof(ctx_proto));
        JS_FreeValue(ctx, cctx.canvas);
        cctx.canvas = JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    }
    return JS_DupValue(ctx, wgj_canvas_ctx_obj);
}

/* ── Extras per class ──────────────────────────────────────────── */

static const JSCFunctionListEntry extra_adapter[] = {
    JS_CGETSET_DEF("features", js_adapter_features, NULL),
    JS_CGETSET_DEF("limits", js_adapter_limits, NULL),
    JS_CGETSET_DEF("info", js_adapter_info, NULL),
    JS_CGETSET_DEF("isFallbackAdapter", js_false_getter, NULL),
    JS_CFUNC_DEF("requestDevice", 1, js_adapter_request_device),
};
static const JSCFunctionListEntry extra_device[] = {
    JS_CGETSET_DEF("features", js_device_features, NULL),
    JS_CGETSET_DEF("limits", js_device_limits, NULL),
    JS_CGETSET_DEF("adapterInfo", js_device_adapter_info, NULL),
    JS_CGETSET_DEF("queue", js_device_queue, NULL),
    JS_CGETSET_DEF("lost", js_device_lost, NULL),
    JS_CFUNC_DEF("destroy", 0, js_device_destroy),
    JS_CFUNC_DEF("popErrorScope", 0, js_device_pop_error_scope),
    JS_CFUNC_DEF("createRenderPipelineAsync", 1, js_device_create_rpipe_async),
    JS_CFUNC_DEF("createComputePipelineAsync", 1, js_device_create_cpipe_async),
    JS_CFUNC_DEF("addEventListener", 2, js_noop),
    JS_CFUNC_DEF("removeEventListener", 2, js_noop),
};
static const JSCFunctionListEntry extra_buffer[] = {
    JS_CFUNC_DEF("mapAsync", 3, js_buffer_map_async),
    JS_CFUNC_DEF("getMappedRange", 2, js_buffer_get_mapped_range),
    JS_CFUNC_DEF("unmap", 0, js_buffer_unmap),
    JS_CFUNC_DEF("destroy", 0, js_buffer_destroy),
};
static const JSCFunctionListEntry extra_queue[] = {
    JS_CFUNC_DEF("writeBuffer", 5, js_queue_write_buffer),
    JS_CFUNC_DEF("writeTexture", 4, js_queue_write_texture),
    JS_CFUNC_DEF("copyExternalImageToTexture", 3, js_queue_copy_external_image),
    JS_CFUNC_DEF("onSubmittedWorkDone", 0, js_queue_work_done),
};
static const JSCFunctionListEntry extra_shader[] = {
    JS_CFUNC_DEF("getCompilationInfo", 0, js_shader_compilation_info),
};
static const JSCFunctionListEntry gpu_funcs[] = {
    JS_CFUNC_DEF("requestAdapter", 1, js_gpu_request_adapter),
    JS_CFUNC_DEF("getPreferredCanvasFormat", 0, js_gpu_preferred_format),
};

/* JS half: constants, error classes, wiring into navigator and the canvas. */
static const char *webgpu_js =
    "globalThis.GPUBufferUsage = Object.freeze({ MAP_READ: 0x1, MAP_WRITE: 0x2, COPY_SRC: 0x4, COPY_DST: 0x8, INDEX: 0x10, VERTEX: 0x20, UNIFORM: 0x40, STORAGE: 0x80, INDIRECT: 0x100, QUERY_RESOLVE: 0x200 });\n"
    "globalThis.GPUTextureUsage = Object.freeze({ COPY_SRC: 0x1, COPY_DST: 0x2, TEXTURE_BINDING: 0x4, STORAGE_BINDING: 0x8, RENDER_ATTACHMENT: 0x10 });\n"
    "globalThis.GPUMapMode = Object.freeze({ READ: 0x1, WRITE: 0x2 });\n"
    "globalThis.GPUShaderStage = Object.freeze({ VERTEX: 0x1, FRAGMENT: 0x2, COMPUTE: 0x4 });\n"
    "globalThis.GPUColorWrite = Object.freeze({ RED: 0x1, GREEN: 0x2, BLUE: 0x4, ALPHA: 0x8, ALL: 0xF });\n"
    "class GPUError { constructor(message) { this.message = String(message); } }\n"
    "globalThis.GPUError = GPUError;\n"
    "globalThis.GPUValidationError = class GPUValidationError extends GPUError {};\n"
    "globalThis.GPUOutOfMemoryError = class GPUOutOfMemoryError extends GPUError {};\n"
    "globalThis.GPUInternalError = class GPUInternalError extends GPUError {};\n"
    "globalThis.GPUPipelineError = class GPUPipelineError extends Error { constructor(m, o) { super(m); this.name = 'GPUPipelineError'; this.reason = (o && o.reason) || 'validation'; } };\n"
    "for (const n of ['OperationError', 'AbortError', 'InvalidStateError']) {\n"
    "  if (!globalThis[n]) globalThis[n] = class extends Error { constructor(m) { super(m); this.name = n; } };\n"
    "}\n"
    "/* three.js probes these with instanceof; nothing here produces them */\n"
    "for (const n of ['VideoFrame', 'ImageBitmap', 'OffscreenCanvas', 'HTMLVideoElement', 'HTMLCanvasElement', 'GPUExternalTexture']) {\n"
    "  if (!globalThis[n]) globalThis[n] = class { constructor() { throw new TypeError(n + ' is not supported in this runtime'); } };\n"
    "}\n"
    "navigator.gpu = _wcGPU;\n"
    "/* getContext('webgpu') on the main canvas */\n"
    "{\n"
    "  const C = Object.getPrototypeOf(document.getElementById('canvas'));\n"
    "  const orig = C.getContext;\n"
    "  C.getContext = function (type, opts) {\n"
    "    if (type === 'webgpu') return this._isMain ? _wcGPUCanvasContext(this) : null;\n"
    "    /* Canvas 2D and WebGL render through GL, which a WebGPU cart has none of */\n"
    "    if (type === '2d' || type.startsWith('webgl') || type === 'experimental-webgl') return null;\n"
    "    return orig.call(this, type, opts);\n"
    "  };\n"
    "}\n";

void register_webgpu_api(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    wgj_ctx = ctx;
    for (int t = 0; t < WGJ_T_COUNT; t++) {
        JS_NewClassID(rt, &wgj_class_ids[t]);
        JSClassDef def = { .class_name = wgj_class_names[t], .finalizer = wgj_finalizer, .gc_mark = wgj_gc_mark };
        JS_NewClass(rt, wgj_class_ids[t], &def);
        JSValue proto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, proto, wgj_gen_protos[t], wgj_gen_proto_lens[t]);
        if (t == WGJ_T_Adapter) JS_SetPropertyFunctionList(ctx, proto, extra_adapter, countof(extra_adapter));
        if (t == WGJ_T_Device) JS_SetPropertyFunctionList(ctx, proto, extra_device, countof(extra_device));
        if (t == WGJ_T_Buffer) JS_SetPropertyFunctionList(ctx, proto, extra_buffer, countof(extra_buffer));
        if (t == WGJ_T_Queue) JS_SetPropertyFunctionList(ctx, proto, extra_queue, countof(extra_queue));
        if (t == WGJ_T_ShaderModule) JS_SetPropertyFunctionList(ctx, proto, extra_shader, countof(extra_shader));
        JS_SetClassProto(ctx, wgj_class_ids[t], JS_DupValue(ctx, proto));
        wgj_protos[t] = proto;
        /* A global constructor per class, so `x instanceof GPUTexture` works.
         * Like the browser's, it cannot be called. */
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ctor = JS_NewCFunction2(ctx, js_noop, wgj_class_names[t], 0, JS_CFUNC_constructor, 0);
        JS_SetConstructor(ctx, ctor, proto);
        JS_SetPropertyStr(ctx, global, wgj_class_names[t], ctor);
        JS_FreeValue(ctx, global);
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue gpu = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, gpu, gpu_funcs, countof(gpu_funcs));
    JSValue wlf = JS_Eval(ctx, "new Set()", 9, "<wgsl>", JS_EVAL_TYPE_GLOBAL);
    JS_SetPropertyStr(ctx, gpu, "wgslLanguageFeatures", wlf);
    JS_SetPropertyStr(ctx, global, "_wcGPU", gpu);
    JS_SetPropertyStr(ctx, global, "_wcGPUCanvasContext",
        JS_NewCFunction(ctx, js_get_canvas_context, "_wcGPUCanvasContext", 1));
    JS_FreeValue(ctx, global);

    JSValue r = JS_Eval(ctx, webgpu_js, strlen(webgpu_js), "<webgpu>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        if (s) { wc_log(s, strlen(s)); JS_FreeCString(ctx, s); }
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
}
