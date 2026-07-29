/*
 * cart_main.c — wasmcart ABI entry point for QuickJS JS runtime
 *
 * This is the reusable cart runtime. It:
 *   1. Exports wc_get_info / wc_init / wc_render (the wasmcart ABI)
 *   2. Boots QuickJS with browser API shims
 *   3. Loads game JS from .wasc assets via wc_load_asset
 *   4. Pumps requestAnimationFrame callbacks each frame
 *
 * Game loop:
 *   wc_render() fires one rAF callback per frame.
 *   Games use standard requestAnimationFrame(cb) — just like a browser.
 *
 * Built-in APIs (shimmed on wasmcart ABI):
 *   - Canvas 2D (software rasterizer → wc_framebuffer)
 *   - WebGL2 (wasmcart GL imports → host GPU)
 *   - Web Audio (PCM mixer → wc_audio ring buffer)
 *   - Gamepad API (reads wc_pads)
 *   - fetch / Image (wc_load_asset from .wasc)
 *   - localStorage (wasmcart save data)
 *   - console.log (wc_log)
 *   - setTimeout / setInterval / requestAnimationFrame
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "wasmcart.h"
#include "quickjs.h"

/* ── Cart configuration ──────────────────────────────────────────── */

#define DEFAULT_WIDTH  800
#define DEFAULT_HEIGHT 600
#define MAX_WIDTH      1920
#define MAX_HEIGHT     1080
#define AUDIO_CAP      8192

/* Actual resolution (set from host preferred, or default) */
uint32_t cur_width  = DEFAULT_WIDTH;
uint32_t cur_height = DEFAULT_HEIGHT;

/* ── Static buffers ──────────────────────────────────────────────── */

/* Non-static: shared with webgl_shim.c, canvas2d.c, audio_shim.c */
uint32_t            framebuffer[MAX_WIDTH * MAX_HEIGHT];

/* ── localStorage backing store (wasmcart SRAM) ──────────────────────
 *
 * The spec's save region: a plain byte blob the HOST owns. It loads any
 * existing bytes here before wc_init() and reads them back to persist after
 * frames, so the cart never touches a filesystem. Before this existed,
 * localStorage was a bare JS object and every save was lost on exit.
 *
 * Format is a length-prefixed blob so a partially-written or truncated region
 * is detectable rather than silently parsed as garbage:
 *
 *   [0..3]  magic 'W','C','L','S'
 *   [4..7]  uint32 payload length (little endian)
 *   [8..]   payload: JSON object of the localStorage key/value map
 *
 * 64KB is the whole budget. It is copied into wasm memory by the host at load
 * and read back wholesale, so this is a real cost, not a reservation --
 * keep it modest. */
#define SAVE_MAGIC0 'W'
#define SAVE_MAGIC1 'C'
#define SAVE_MAGIC2 'L'
#define SAVE_MAGIC3 'S'
#define SAVE_SIZE   (64 * 1024)
#define SAVE_HEADER 8
static uint8_t save_region[SAVE_SIZE];
float               audio_ring[AUDIO_CAP * 2];
uint32_t            audio_write_cursor;
wc_pad_t            pads[4];
wc_time_t           time_info;
wc_info_t           info;
wc_host_info_t      host_info;
wc_pointer_t        pointers[10];
uint8_t             keys[32];

/* Flag: game uses WebGL (not Canvas 2D) — skip Ganesh blit */
int game_uses_webgl = 0;

/* ── QuickJS state ───────────────────────────────────────────────── */

static JSRuntime *rt = NULL;
JSContext *ctx = NULL;  /* non-static: shared with websocket_shim.c */
static int initialized = 0;

/* rAF callback — stored per frame */
static JSValue raf_callback = JS_UNDEFINED;

/* Timer state */
#define MAX_TIMERS 256

typedef struct {
    int id;
    JSValue callback;
    double fire_at_ms;   /* absolute time to fire */
    double interval_ms;  /* 0 = setTimeout, >0 = setInterval */
    int active;
} timer_entry_t;

static timer_entry_t timers[MAX_TIMERS];
static int next_timer_id = 1;

/* ── Forward declarations ────────────────────────────────────────── */

static JSValue js_set_resolution(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_set_webgl(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv);
static void register_console_api(JSContext *ctx);
static void register_timer_api(JSContext *ctx);
static void register_raf_api(JSContext *ctx);
static void register_gamepad_api(JSContext *ctx);
static void register_fetch_api(JSContext *ctx);
static void register_image_api(JSContext *ctx);
static void register_canvas_api(JSContext *ctx);
static void register_localstorage_api(JSContext *ctx);
static void register_performance_api(JSContext *ctx);
static void register_event_shims(JSContext *ctx);
static void register_keyboard_mouse_api(JSContext *ctx);

/* External: webgl_shim.c */
extern void register_webgl_api(JSContext *ctx);
/* External: canvas2d_skia.c (or canvas2d.c fallback) */
extern void register_canvas2d_native(JSContext *ctx);
extern void skia_flush_to_framebuffer(void);
/* External: audio_shim.c */
extern void register_audio_api(JSContext *ctx);
extern void pump_audio(void);
/* External: image_decode.c */
extern void register_image_decode(JSContext *ctx);
/* External: worker_shim.c */
extern void register_worker_api(JSContext *ctx);
extern void pump_workers(void);

/* Previous keyboard state for edge detection */
static uint8_t prev_keys[32];
static uint8_t prev_pointer_buttons[10];

char *load_asset_string(const char *path, int *out_len);
static void pump_timers(void);
static void pump_raf(void);
static int frame_watchdog(JSRuntime *rt, void *opaque);
static void dispatch_input_events(void);
static void fire_key_event(const char *type, uint8_t hid);
static void fire_mouse_event(const char *type, int16_t x, int16_t y, uint8_t button);

/* ── wc_get_info ─────────────────────────────────────────────────── */

__attribute__((export_name("wc_get_info")))
wc_info_t *wc_get_info(void) {
    info.version         = WC_ABI_VERSION;
    info.width           = DEFAULT_WIDTH;
    info.height          = DEFAULT_HEIGHT;
    info.fb_ptr          = (uint32_t)(uintptr_t)framebuffer;
    info.audio_ptr       = (uint32_t)(uintptr_t)audio_ring;
    info.audio_cap       = AUDIO_CAP;
    info.audio_write_ptr = (uint32_t)(uintptr_t)&audio_write_cursor;
    info.input_ptr       = (uint32_t)(uintptr_t)pads;
    info.save_ptr        = (uint32_t)(uintptr_t)save_region;
    info.save_size       = SAVE_SIZE;
    info.time_ptr        = (uint32_t)(uintptr_t)&time_info;
    info.host_info_ptr   = (uint32_t)(uintptr_t)&host_info;
    info.flags           = WC_FLAG_AUDIO_F32 | WC_FLAG_POINTER | WC_FLAG_KEYBOARD ;
    info.audio_sample_rate = 0;
    info.pointer_ptr     = (uint32_t)(uintptr_t)pointers;
    info.keys_ptr        = (uint32_t)(uintptr_t)keys;
    info.gpu_api         = 1;  /* Always WebGL2. Canvas 2D content is uploaded as
                                * a GL texture. Everything goes through the GPU. */
    return &info;
}

/* ── Module loader ───────────────────────────────────────────────── */

/*
 * Custom module loader: import "foo.js" → wc_load_asset("foo.js")
 * Resolves relative to assets/ in the .wasc.
 */
static JSModuleDef *js_module_loader(JSContext *ctx, const char *module_name,
                                      void *opaque)
{
    int len;
    char *src = load_asset_string(module_name, &len);
    if (!src) {
        /* Try with assets/ prefix */
        char prefixed[512];
        snprintf(prefixed, sizeof(prefixed), "assets/%s", module_name);
        src = load_asset_string(prefixed, &len);
    }
    if (!src) {
        JS_ThrowReferenceError(ctx, "module not found: %s", module_name);
        return NULL;
    }

    JSValue func = JS_Eval(ctx, src, len, module_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(src);

    if (JS_IsException(func))
        return NULL;

    JSModuleDef *m = JS_VALUE_GET_PTR(func);
    JS_FreeValue(ctx, func);
    return m;
}

/* ── wc_init ─────────────────────────────────────────────────────── */

__attribute__((export_name("wc_init")))
void wc_init(void) {
    /* Read host preferred resolution (host wrote to host_info after wc_get_info) */
    if (host_info.preferred_width > 0 && host_info.preferred_width <= MAX_WIDTH)
        cur_width = host_info.preferred_width;
    if (host_info.preferred_height > 0 && host_info.preferred_height <= MAX_HEIGHT)
        cur_height = host_info.preferred_height;

    /* Update info struct so host sees actual resolution */
    info.width = cur_width;
    info.height = cur_height;

    /* Initialize timer slots */
    memset(timers, 0, sizeof(timers));

    /* Create QuickJS runtime */
    rt = JS_NewRuntime();
    if (!rt) {
        WC_LOG("ERROR: JS_NewRuntime failed");
        return;
    }
    JS_SetMemoryLimit(rt, 256 * 1024 * 1024);  /* 256 MB */
    JS_SetInterruptHandler(rt, frame_watchdog, NULL);
    JS_SetMaxStackSize(rt, 1024 * 1024);         /* 1 MB stack */

    ctx = JS_NewContext(rt);
    if (!ctx) {
        WC_LOG("ERROR: JS_NewContext failed");
        return;
    }

    /* Register module loader for ES module imports */
    JS_SetModuleLoaderFunc(rt, NULL, js_module_loader, NULL);

    /* Register browser API shims */
    register_console_api(ctx);
    register_timer_api(ctx);
    register_raf_api(ctx);
    register_gamepad_api(ctx);
    register_fetch_api(ctx);
    register_image_api(ctx);
    register_image_decode(ctx);      /* Skia image decoder for Image.src */
    register_worker_api(ctx);        /* Real threaded Workers via pthreads */
         /* WebSocket via wasmcart WS ABI */
    register_canvas_api(ctx);        /* Canvas/document/window shims (JS) */
    /* Register resolution setter for canvas.width/height changes */
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "_wcSetResolution",
            JS_NewCFunction(ctx, js_set_resolution, "_wcSetResolution", 2));
        JS_SetPropertyStr(ctx, global, "_wcSetWebGL",
            JS_NewCFunction(ctx, js_set_webgl, "_wcSetWebGL", 1));
        JS_FreeValue(ctx, global);
    }
    register_canvas2d_native(ctx);   /* Phase 3: native C2D rasterizer */
    register_webgl_api(ctx);         /* Phase 1b: WebGL2 shim */
    register_audio_api(ctx);         /* Phase 2: Web Audio */
    register_localstorage_api(ctx);
    register_performance_api(ctx);
    register_event_shims(ctx);
    register_keyboard_mouse_api(ctx); /* Phase 4: keyboard/mouse events */

    memset(prev_keys, 0, sizeof(prev_keys));
    memset(prev_pointer_buttons, 0, sizeof(prev_pointer_buttons));

    /* Set window/canvas dimensions from actual resolution */
    {
        char dim_js[512];
        snprintf(dim_js, sizeof(dim_js),
            "globalThis.window.innerWidth = %u;"
            "globalThis.window.innerHeight = %u;"
            "if (document.body) { document.body.clientWidth = %u; document.body.clientHeight = %u; }"
            "if (document._canvas) { document._canvas.width = %u; document._canvas.height = %u; }",
            cur_width, cur_height, cur_width, cur_height, cur_width, cur_height);
        JS_Eval(ctx, dim_js, strlen(dim_js), "<dims>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Add all WebGL2 method stubs that aren't already defined.
     * QuickJS Proxy can't see C-registered properties, so we directly
     * add no-op stubs for every WebGL2 method three.js might call. */
    {
        const char *gl_stubs_js =
            "if (typeof _wcGL !== 'undefined') {\n"
            "  const _noop = function() {};\n"
            "  const _noopVoid = function() {};\n"
            "  const _methods = [\n"
            "    'bindBufferBase','bindBufferRange','getUniformBlockIndex','uniformBlockBinding',\n"
            "    'uniform1iv','uniform2iv','uniform3iv','uniform4iv',\n"
            "    'uniform1fv','uniform2fv','uniform3fv','uniform4fv',\n"
            "    'vertexAttribIPointer','vertexAttribI4i','vertexAttribI4ui',\n"
            "    'vertexAttribDivisor','drawArraysInstanced','drawElementsInstanced',\n"
            "    'drawRangeElements','clearBufferfv','clearBufferiv','clearBufferuiv','clearBufferfi',\n"
            "    'readBuffer','blitFramebuffer','invalidateFramebuffer',\n"
            "    'texStorage2D','texStorage3D','texImage3D','texSubImage3D',\n"
            "    'compressedTexImage2D','compressedTexImage3D','compressedTexSubImage2D',\n"
            "    'copyTexSubImage3D','copyTexImage2D',\n"
            "    'getBufferSubData','getInternalformatParameter',\n"
            "    'beginTransformFeedback','endTransformFeedback','transformFeedbackVaryings',\n"
            "    'bindTransformFeedback','createTransformFeedback','deleteTransformFeedback',\n"
            "    'createSampler','deleteSampler','bindSampler','samplerParameteri','samplerParameterf',\n"
            "    'fenceSync','deleteSync','clientWaitSync','waitSync','getSyncParameter',\n"
            "    'createQuery','deleteQuery','beginQuery','endQuery','getQueryParameter','getQuery',\n"
            "    'getFragDataLocation','getIndexedParameter',\n"
            "    'getActiveUniforms','getActiveUniformBlockName','getActiveUniformBlockParameter',\n"
            "    'getTransformFeedbackVarying','getUniformIndices',\n"
            "    'isBuffer','isFramebuffer','isProgram','isRenderbuffer','isShader','isTexture',\n"
            "    'isSampler','isSync','isTransformFeedback','isVertexArray','isQuery',\n"
            "    'renderbufferStorageMultisample',\n"
            "    'getContextAttributes','getSupportedExtensions','getExtension',\n"
            "    'getShaderPrecisionFormat','isContextLost',\n"
            "  ];\n"
            "  for (const m of _methods) {\n"
            "    if (_wcGL[m] === undefined) _wcGL[m] = _noop;\n"
            "  }\n"
            "  /* Fix specific methods that need non-null returns */\n"
            "  if (_wcGL.getContextAttributes === undefined || _wcGL.getContextAttributes === _noop) {\n"
            "    _wcGL.getContextAttributes = function() {\n"
            "      return {alpha:false,antialias:false,depth:true,failIfMajorPerformanceCaveat:false,\n"
            "        powerPreference:'default',premultipliedAlpha:true,preserveDrawingBuffer:false,stencil:false};\n"
            "    };\n"
            "  }\n"
            "  if (_wcGL.getSupportedExtensions === undefined || _wcGL.getSupportedExtensions === _noop) {\n"
            "    _wcGL.getSupportedExtensions = function() { return []; };\n"
            "  }\n"
            "  if (_wcGL.getExtension === undefined || _wcGL.getExtension === _noop) {\n"
            "    _wcGL.getExtension = function(name) {\n"
            "      if (name==='EXT_color_buffer_float') return {};\n"
            "      if (name==='OES_texture_float_linear') return {};\n"
            "      if (name==='EXT_texture_filter_anisotropic') return {TEXTURE_MAX_ANISOTROPY_EXT:0x84FE,MAX_TEXTURE_MAX_ANISOTROPY_EXT:0x84FF};\n"
            "      return null;\n"
            "    };\n"
            "  }\n"
            "  if (_wcGL.getShaderPrecisionFormat === undefined || _wcGL.getShaderPrecisionFormat === _noop) {\n"
            "    _wcGL.getShaderPrecisionFormat = function() { return {rangeMin:127,rangeMax:127,precision:23}; };\n"
            "  }\n"
            "  _wcGL.isContextLost = function() { return false; };\n"
            "  /* uniform*fv via scalar calls */\n"
            "  _wcGL.uniform1fv = function(l,d) { if(d&&d.length>=1) _wcGL.uniform1f(l,d[0]); };\n"
            "  _wcGL.uniform2fv = function(l,d) { if(d&&d.length>=2) _wcGL.uniform2f(l,d[0],d[1]); };\n"
            "  _wcGL.uniform3fv = function(l,d) { if(d&&d.length>=3) _wcGL.uniform3f(l,d[0],d[1],d[2]); };\n"
            "  _wcGL.uniform4fv = function(l,d) { if(d&&d.length>=4) _wcGL.uniform4f(l,d[0],d[1],d[2],d[3]); };\n"
            "  _wcGL.drawRangeElements = function(m,s,e,c,t,o) { _wcGL.drawElements(m,c,t,o); };\n"
            "}\n";
        JS_Eval(ctx, gl_stubs_js, strlen(gl_stubs_js), "<gl-stubs>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Load and execute game entry point from .wasc assets */
    /* Try: main.js, index.js, src/main.js, src/index.js, game.js */
    int len;
    const char *entry_files[] = {
        "main.js", "index.js", "src/main.js", "src/index.js", "game.js", NULL
    };
    char *src = NULL;
    const char *entry_name = NULL;
    for (int i = 0; entry_files[i]; i++) {
        src = load_asset_string(entry_files[i], &len);
        if (src) { entry_name = entry_files[i]; break; }
    }
    if (!src) {
        WC_LOG("ERROR: no entry point found (tried main.js, index.js, etc.)");
        return;
    }

    JSValue result = JS_Eval(ctx, src, len, entry_name, JS_EVAL_TYPE_MODULE);
    free(src);

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *str = JS_ToCString(ctx, exc);
        if (str) {
            wc_log(str, strlen(str));
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        return;
    }
    JS_FreeValue(ctx, result);

    /* Pump pending jobs aggressively during init.
     * Games that do `await loadSound()` etc at startup need their
     * promise chains to resolve before the first wc_render. */
    {
        JSContext *pctx;
        for (int pump = 0; pump < 1000; pump++) {
            int r = JS_ExecutePendingJob(rt, &pctx);
            if (r <= 0) break;
        }
    }

    initialized = 1;
    WC_LOG("jsgame cart initialized");
}

/*
 * Microtask budget per frame: TIME-based, with a job count as a backstop.
 *
 * A count alone is the wrong shape. 4096 trivial microtasks cost ~5ms here,
 * which is 30% of a 60fps frame handed to a hostile cart for free -- and on a
 * slower device, or with microtasks that each do real work, the same count is
 * far worse. What actually needs bounding is TIME, not iterations.
 *
 * clock_gettime(CLOCK_MONOTONIC) does advance within a single wasm call
 * (measured: 7ms inside one call), so this is real elapsed time, not the
 * host's per-frame timestamp, which is frozen for the duration of wc_render.
 *
 * The count remains as a backstop for hosts whose clock does not advance --
 * without it, a frozen clock would turn this back into an unbounded drain.
 */
#define MAX_JOB_MS         4      /* ~25% of a 60fps frame -- VERIFIED controlling: setting this to 1 gives 1ms/frame, 4 gives 4ms */
#define MAX_JOBS_PER_FRAME 4096   /* backstop if the clock never moves */

/*
 * Frame watchdog.
 *
 * The sandbox stops a cart reading your files or reaching the network, but
 * NOT this:
 *
 *     function loop(){ while(true){} }  requestAnimationFrame(loop);
 *
 * Three lines, and wc_render never returns -- the host hangs with it. The
 * microtask bound below cannot catch it, because a synchronous loop never
 * returns control to the job queue at all. QuickJS's interrupt handler is
 * called periodically from the interpreter loop, which is the only place that
 * CAN see it.
 *
 * Budget is deliberately loose. A game legitimately doing heavy work in one
 * frame (level generation, decoding) should not be killed, so this is a
 * runaway detector, not a frame-time enforcer: it fires only when a single
 * wc_render has been running long enough that the host is visibly wedged.
 *
 * When it fires, JS_Call returns an exception, wc_render logs it and returns
 * normally, and the host keeps running. The cart is left alive -- the next
 * frame gets a fresh budget -- so a game with one pathological frame recovers
 * rather than being permanently killed.
 */
#define FRAME_WATCHDOG_MS 2000

static double frame_deadline_ms;   /* 0 = disarmed */

/* Non-zero return = interrupt the running JS. Called periodically by the
 * QuickJS interpreter loop, including from inside a `while(true){}`. */
static int frame_watchdog(JSRuntime *rt, void *opaque) {
    (void)rt; (void)opaque;
    if (frame_deadline_ms == 0) return 0;          /* not inside a frame */
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    double now = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
    if (now < frame_deadline_ms) return 0;
    frame_deadline_ms = 0;                          /* fire once per frame */
    WC_LOG("frame watchdog: JS ran over budget, interrupting");
    return 1;
}

static double job_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* ── wc_render ───────────────────────────────────────────────────── */

__attribute__((export_name("wc_render")))
extern void skia_save_host_fbo(void);

void wc_render(void) {
    if (!initialized)
        return;

    /* Arm the watchdog for this frame. Re-armed every frame, so a game that
     * overruns once is not penalised afterwards. */
    frame_deadline_ms = job_now_ms() + FRAME_WATCHDOG_MS;

    /* Save the host's FBO before any Ganesh operations. */
    skia_save_host_fbo();

    /* Enable GL tracing on frame 5 to capture one full game frame */
    {
        static int frame = 0;
        frame++;
        if (frame == 5) {
            extern void gl_trace_start(void);
            gl_trace_start();
        } else if (frame == 7) {
            extern void gl_trace_stop(void);
            gl_trace_stop();
        }
    }

    /* Execute pending jobs (Promise microtasks).
     *
     * BOUNDED. An unbounded drain never returns for a cart that reschedules a
     * microtask from inside a microtask -- four lines of legal JS:
     *
     *     function spin(){ Promise.resolve().then(spin); }  spin();
     *
     * which hangs wc_render, and with it the host, forever. That is reachable
     * by any untrusted cart, which is exactly what this runtime promises to be
     * safe against. Measured before the bound: runFrame never returned in 45s.
     *
     * A game that legitimately exceeds the budget in one frame just continues
     * draining on the next -- microtasks are not dropped, only spread. The
     * init pump above is already capped the same way. */
    JSContext *pctx;
    {
        const double deadline = job_now_ms() + MAX_JOB_MS;
        for (int job = 0; job < MAX_JOBS_PER_FRAME; job++) {
            if (JS_ExecutePendingJob(rt, &pctx) <= 0) break;
            /* Check every 64 jobs: clock_gettime is a host call, and doing it
             * per microtask costs more than the microtasks themselves. */
            if ((job & 63) == 63 && job_now_ms() >= deadline) break;
        }
    }

    /* Fire expired timers */
    pump_timers();

    /* Dispatch keyboard/mouse events */
    dispatch_input_events();

    /* Fire requestAnimationFrame callback */
    pump_raf();

    /* Flush Skia surface → wasmcart framebuffer */
    skia_flush_to_framebuffer();

    /* Pump audio output to ring buffer */
    pump_audio();

    /* Deliver worker messages to main thread */
    pump_workers();
}

/* ══════════════════════════════════════════════════════════════════
 *  Keyboard/Mouse event dispatch (Phase 4)
 * ══════════════════════════════════════════════════════════════════ */

/* HID keycode → JS key string mapping */
static const char *hid_to_key_str(uint8_t hid) {
    if (hid >= 0x04 && hid <= 0x1D) {
        static char buf[2];
        buf[0] = 'a' + (hid - 0x04);
        buf[1] = '\0';
        return buf;
    }
    if (hid >= 0x1E && hid <= 0x26) {
        static char buf[2];
        buf[0] = '1' + (hid - 0x1E);
        buf[1] = '\0';
        return buf;
    }
    if (hid == 0x27) return "0";
    if (hid == 0x28) return "Enter";
    if (hid == 0x29) return "Escape";
    if (hid == 0x2A) return "Backspace";
    if (hid == 0x2B) return "Tab";
    if (hid == 0x2C) return " ";
    if (hid == 0x2D) return "-";
    if (hid == 0x2E) return "=";
    if (hid == 0x2F) return "[";
    if (hid == 0x30) return "]";
    if (hid == 0x31) return "\\";
    if (hid == 0x33) return ";";
    if (hid == 0x34) return "'";
    if (hid == 0x35) return "`";
    if (hid == 0x36) return ",";
    if (hid == 0x37) return ".";
    if (hid == 0x38) return "/";
    if (hid >= 0x3A && hid <= 0x45) {
        static char buf[4];
        snprintf(buf, sizeof(buf), "F%d", hid - 0x3A + 1);
        return buf;
    }
    if (hid == 0x4F) return "ArrowRight";
    if (hid == 0x50) return "ArrowLeft";
    if (hid == 0x51) return "ArrowDown";
    if (hid == 0x52) return "ArrowUp";
    if (hid == 0xE0) return "Control";
    if (hid == 0xE1) return "Shift";
    if (hid == 0xE2) return "Alt";
    if (hid == 0xE3) return "Meta";
    if (hid == 0xE4) return "Control";
    if (hid == 0xE5) return "Shift";
    if (hid == 0xE6) return "Alt";
    if (hid == 0xE7) return "Meta";
    return "";
}

static void fire_key_event(const char *type, uint8_t hid) {
    const char *key = hid_to_key_str(hid);
    if (!key[0]) return;

    char code[64];
    if (hid >= 0x04 && hid <= 0x1D) {
        snprintf(code, sizeof(code), "Key%c", 'A' + (hid - 0x04));
    } else if (hid >= 0x1E && hid <= 0x27) {
        snprintf(code, sizeof(code), "Digit%s", key);
    } else if (hid == 0x2C) {
        snprintf(code, sizeof(code), "Space");
    } else {
        snprintf(code, sizeof(code), "%s", key);
    }

    char js[512];
    snprintf(js, sizeof(js),
        "if (globalThis._wcKeyListeners) {"
        "  const e = {type:'%s',key:'%s',code:'%s',"
        "    preventDefault(){},stopPropagation(){}};"
        "  globalThis._wcKeyListeners.forEach(fn => fn(e));"
        "}",
        type, key, code);
    JS_Eval(ctx, js, strlen(js), "<keyevent>", JS_EVAL_TYPE_GLOBAL);
}

static int16_t prev_mouse_x = 0, prev_mouse_y = 0;

static void fire_mouse_event(const char *type, int16_t x, int16_t y, uint8_t button) {
    int16_t dx = x - prev_mouse_x, dy = y - prev_mouse_y;
    char js[768];
    snprintf(js, sizeof(js),
        "if (globalThis._wcMouseListeners) {"
        "  const e = {type:'%s',clientX:%d,clientY:%d,pageX:%d,pageY:%d,"
        "    button:%d,offsetX:%d,offsetY:%d,movementX:%d,movementY:%d,"
        "    pointerId:0,pointerType:'mouse',isPrimary:true,"
        "    preventDefault(){},stopPropagation(){}};"
        "  globalThis._wcMouseListeners.forEach(fn => fn(e));"
        "}",
        type, (int)x, (int)y, (int)x, (int)y,
        (int)button, (int)x, (int)y, (int)dx, (int)dy);
    JS_Eval(ctx, js, strlen(js), "<mouseevent>", JS_EVAL_TYPE_GLOBAL);

    /* Also fire corresponding pointer event */
    const char *ptype = NULL;
    if (strcmp(type, "mousedown") == 0) ptype = "pointerdown";
    else if (strcmp(type, "mouseup") == 0) ptype = "pointerup";
    else if (strcmp(type, "mousemove") == 0) ptype = "pointermove";
    if (ptype) {
        snprintf(js, sizeof(js),
            "if (globalThis._wcMouseListeners) {"
            "  const e = {type:'%s',clientX:%d,clientY:%d,pageX:%d,pageY:%d,"
            "    button:%d,offsetX:%d,offsetY:%d,movementX:%d,movementY:%d,"
            "    pointerId:0,pointerType:'mouse',isPrimary:true,pressure:%s,"
            "    width:1,height:1,"
            "    preventDefault(){},stopPropagation(){}};"
            "  globalThis._wcMouseListeners.forEach(fn => fn(e));"
            "}",
            ptype, (int)x, (int)y, (int)x, (int)y,
            (int)button, (int)x, (int)y, (int)dx, (int)dy,
            (strcmp(ptype, "pointerdown") == 0 || strcmp(ptype, "pointermove") == 0) ? "0.5" : "0");
        JS_Eval(ctx, js, strlen(js), "<pointerevent>", JS_EVAL_TYPE_GLOBAL);
    }

    prev_mouse_x = x;
    prev_mouse_y = y;
}

static void dispatch_input_events(void) {
    /* Keyboard: detect edges in wc_keys bitmask */
    for (int byte = 0; byte < 32; byte++) {
        uint8_t cur = keys[byte];
        uint8_t prev = prev_keys[byte];
        uint8_t changed = cur ^ prev;
        if (!changed) continue;
        for (int bit = 0; bit < 8; bit++) {
            if (changed & (1 << bit)) {
                uint8_t hid = byte * 8 + bit;
                if (cur & (1 << bit))
                    fire_key_event("keydown", hid);
                else
                    fire_key_event("keyup", hid);
            }
        }
    }
    memcpy(prev_keys, keys, sizeof(prev_keys));

    /* Mouse/pointer: detect button changes and movement */
    for (int i = 0; i < 10; i++) {
        if (!pointers[i].active) {
            if (prev_pointer_buttons[i]) {
                /* Pointer went inactive — fire mouseup */
                prev_pointer_buttons[i] = 0;
            }
            continue;
        }
        uint8_t cur_btn = pointers[i].buttons;
        uint8_t prev_btn = prev_pointer_buttons[i];

        if (cur_btn != prev_btn) {
            /* Button state changed */
            for (int b = 0; b < 3; b++) {
                int cur_pressed = (cur_btn >> b) & 1;
                int prev_pressed = (prev_btn >> b) & 1;
                if (cur_pressed && !prev_pressed)
                    fire_mouse_event("mousedown", pointers[i].x, pointers[i].y, b);
                else if (!cur_pressed && prev_pressed)
                    fire_mouse_event("mouseup", pointers[i].x, pointers[i].y, b);
            }
        }

        /* Always fire mousemove if pointer is active */
        if (pointers[i].active && i == 0) {
            fire_mouse_event("mousemove", pointers[i].x, pointers[i].y, 0);
        }

        prev_pointer_buttons[i] = cur_btn;
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Utility helpers
 * ══════════════════════════════════════════════════════════════════ */

char *load_asset_string(const char *path, int *out_len) {
    int size = wc_asset_size(path, strlen(path));
    if (size < 0)
        return NULL;
    char *buf = malloc(size + 1);
    if (!buf) return NULL;
    int loaded = wc_load_asset(path, strlen(path), buf, size);
    if (loaded < 0) {
        free(buf);
        return NULL;
    }
    buf[size] = '\0';
    if (out_len) *out_len = size;
    return buf;
}

/* Load a binary asset, returns malloc'd buffer */
static uint8_t *load_asset_binary(const char *path, int *out_len) {
    int size = wc_asset_size(path, strlen(path));
    if (size < 0) return NULL;
    uint8_t *buf = malloc(size);
    if (!buf) return NULL;
    int loaded = wc_load_asset(path, strlen(path), (char *)buf, size);
    if (loaded < 0) {
        free(buf);
        return NULL;
    }
    if (out_len) *out_len = size;
    return buf;
}

/* ══════════════════════════════════════════════════════════════════
 *  Resolution setter — called when game sets canvas.width/height
 * ══════════════════════════════════════════════════════════════════ */

extern void skia_resize_surface(uint32_t w, uint32_t h);

static JSValue js_set_webgl(JSContext *c, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    int32_t v;
    JS_ToInt32(c, &v, argv[0]);
    game_uses_webgl = v;
    return JS_UNDEFINED;
}

static JSValue js_set_resolution(JSContext *c, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    int32_t w, h;
    JS_ToInt32(c, &w, argv[0]);
    JS_ToInt32(c, &h, argv[1]);
    if (w > 0 && w <= MAX_WIDTH && h > 0 && h <= MAX_HEIGHT) {
        cur_width = w;
        cur_height = h;
        info.width = w;
        info.height = h;
        skia_resize_surface(w, h);
    }
    return JS_UNDEFINED;
}

/* ══════════════════════════════════════════════════════════════════
 *  console API
 * ══════════════════════════════════════════════════════════════════ */

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    /* Concatenate all args with spaces, like browser console.log */
    char buf[2048];
    int pos = 0;
    for (int i = 0; i < argc; i++) {
        if (i > 0 && pos < (int)sizeof(buf) - 1) buf[pos++] = ' ';
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            int len = strlen(str);
            int avail = sizeof(buf) - 1 - pos;
            if (len > avail) len = avail;
            memcpy(buf + pos, str, len);
            pos += len;
            JS_FreeCString(ctx, str);
        }
    }
    buf[pos] = '\0';
    if (pos > 0) wc_log(buf, pos);
    return JS_UNDEFINED;
}

static void register_console_api(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "warn",
        JS_NewCFunction(ctx, js_console_log, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunction(ctx, js_console_log, "error", 1));
    JS_SetPropertyStr(ctx, console, "info",
        JS_NewCFunction(ctx, js_console_log, "info", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

/* ══════════════════════════════════════════════════════════════════
 *  Timer API (setTimeout / setInterval / clearTimeout)
 * ══════════════════════════════════════════════════════════════════ */

static JSValue js_set_timeout(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_NewInt32(ctx, 0);

    double delay = 0;
    if (argc >= 2) JS_ToFloat64(ctx, &delay, argv[1]);

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active) {
            timers[i].id = next_timer_id++;
            timers[i].callback = JS_DupValue(ctx, argv[0]);
            timers[i].fire_at_ms = time_info.time_ms + delay;
            timers[i].interval_ms = 0;
            timers[i].active = 1;
            return JS_NewInt32(ctx, timers[i].id);
        }
    }
    return JS_NewInt32(ctx, 0);
}

static JSValue js_set_interval(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_NewInt32(ctx, 0);

    double interval = 0;
    if (argc >= 2) JS_ToFloat64(ctx, &interval, argv[1]);
    if (interval < 1) interval = 1;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active) {
            timers[i].id = next_timer_id++;
            timers[i].callback = JS_DupValue(ctx, argv[0]);
            timers[i].fire_at_ms = time_info.time_ms + interval;
            timers[i].interval_ms = interval;
            timers[i].active = 1;
            return JS_NewInt32(ctx, timers[i].id);
        }
    }
    return JS_NewInt32(ctx, 0);
}

static JSValue js_clear_timeout(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    int id;
    if (argc < 1 || JS_ToInt32(ctx, &id, argv[0]))
        return JS_UNDEFINED;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active && timers[i].id == id) {
            JS_FreeValue(ctx, timers[i].callback);
            timers[i].active = 0;
            break;
        }
    }
    return JS_UNDEFINED;
}

static void pump_timers(void) {
    double now = time_info.time_ms;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active) continue;
        if (now < timers[i].fire_at_ms) continue;

        JSValue ret = JS_Call(ctx, timers[i].callback, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(ctx);
            const char *str = JS_ToCString(ctx, exc);
            if (str) { wc_log(str, strlen(str)); JS_FreeCString(ctx, str); }
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, ret);

        if (timers[i].interval_ms > 0) {
            /* setInterval: reschedule */
            timers[i].fire_at_ms = now + timers[i].interval_ms;
        } else {
            /* setTimeout: one-shot, remove */
            JS_FreeValue(ctx, timers[i].callback);
            timers[i].active = 0;
        }
    }
}

static void register_timer_api(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "setTimeout",
        JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "setInterval",
        JS_NewCFunction(ctx, js_set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
        JS_NewCFunction(ctx, js_clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "clearInterval",
        JS_NewCFunction(ctx, js_clear_timeout, "clearInterval", 1));
    JS_FreeValue(ctx, global);
}

/* ══════════════════════════════════════════════════════════════════
 *  requestAnimationFrame
 * ══════════════════════════════════════════════════════════════════ */

static JSValue js_request_animation_frame(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_NewInt32(ctx, 0);

    /* Replace any existing rAF callback (only one fires per frame) */
    JS_FreeValue(ctx, raf_callback);
    raf_callback = JS_DupValue(ctx, argv[0]);
    return JS_NewInt32(ctx, 1);
}

static JSValue js_cancel_animation_frame(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    JS_FreeValue(ctx, raf_callback);
    raf_callback = JS_UNDEFINED;
    return JS_UNDEFINED;
}

static void pump_raf(void) {
    if (JS_IsUndefined(raf_callback))
        return;

    /* Consume the callback (rAF is one-shot per registration) */
    JSValue cb = raf_callback;
    raf_callback = JS_UNDEFINED;

    JSValue timestamp = JS_NewFloat64(ctx, time_info.time_ms);
    JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 1, &timestamp);
    JS_FreeValue(ctx, timestamp);
    JS_FreeValue(ctx, cb);

    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char *str = JS_ToCString(ctx, exc);
        if (str) { wc_log(str, strlen(str)); JS_FreeCString(ctx, str); }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, ret);
}

static void register_raf_api(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "requestAnimationFrame",
        JS_NewCFunction(ctx, js_request_animation_frame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, global, "cancelAnimationFrame",
        JS_NewCFunction(ctx, js_cancel_animation_frame, "cancelAnimationFrame", 1));
    JS_FreeValue(ctx, global);
}

/* ══════════════════════════════════════════════════════════════════
 *  Gamepad API — navigator.getGamepads()
 * ══════════════════════════════════════════════════════════════════ */

static JSValue js_get_gamepads(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue arr = JS_NewArray(ctx);

    for (int i = 0; i < 4; i++) {
        if (!pads[i].connected) {
            JS_SetPropertyUint32(ctx, arr, i, JS_NULL);
            continue;
        }

        JSValue pad = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, pad, "index", JS_NewInt32(ctx, i));
        JS_SetPropertyStr(ctx, pad, "connected", JS_TRUE);
        JS_SetPropertyStr(ctx, pad, "id",
            JS_NewString(ctx, "wasmcart Gamepad"));
        JS_SetPropertyStr(ctx, pad, "mapping",
            JS_NewString(ctx, "standard"));

        /* Buttons array — 16 standard gamepad buttons */
        JSValue buttons = JS_NewArray(ctx);
        uint16_t btn = pads[i].buttons;
        const uint16_t btn_masks[] = {
            WC_BTN_A, WC_BTN_B, WC_BTN_X, WC_BTN_Y,
            WC_BTN_L, WC_BTN_R, 0, 0,  /* L2/R2 handled by triggers */
            WC_BTN_SELECT, WC_BTN_START,
            WC_BTN_L3, WC_BTN_R3,
            WC_BTN_UP, WC_BTN_DOWN, WC_BTN_LEFT, WC_BTN_RIGHT
        };
        for (int b = 0; b < 16; b++) {
            JSValue button = JS_NewObject(ctx);
            int pressed = (btn_masks[b] && (btn & btn_masks[b])) ? 1 : 0;
            /* Triggers for L2/R2 (indices 6,7) */
            double value = pressed ? 1.0 : 0.0;
            if (b == 6) value = pads[i].left_trigger / 255.0;
            if (b == 7) value = pads[i].right_trigger / 255.0;
            if (b == 6) pressed = pads[i].left_trigger > 128;
            if (b == 7) pressed = pads[i].right_trigger > 128;

            JS_SetPropertyStr(ctx, button, "pressed",
                pressed ? JS_TRUE : JS_FALSE);
            JS_SetPropertyStr(ctx, button, "value",
                JS_NewFloat64(ctx, value));
            JS_SetPropertyUint32(ctx, buttons, b, button);
        }
        JS_SetPropertyStr(ctx, pad, "buttons", buttons);

        /* Axes array — [leftX, leftY, rightX, rightY] normalized to -1..1.
         * The wire type is int16_t, so full negative deflection is -32768 and
         * -32768/32767.0 is -1.0000305 — outside the range the Gamepad API
         * promises. Games that trust the contract (Math.acos(axis), or an
         * unclamped position += axis) misbehave on exactly one stick position.
         * Clamp rather than divide by 32768: 32767 must still map to 1.0. */
        JSValue axes = JS_NewArray(ctx);
        const int16_t raw_axes[4] = {
            pads[i].left_x, pads[i].left_y, pads[i].right_x, pads[i].right_y
        };
        for (int a = 0; a < 4; a++) {
            double v = raw_axes[a] / 32767.0;
            if (v < -1.0) v = -1.0;
            JS_SetPropertyUint32(ctx, axes, a, JS_NewFloat64(ctx, v));
        }
        JS_SetPropertyStr(ctx, pad, "axes", axes);

        JS_SetPropertyUint32(ctx, arr, i, pad);
    }
    return arr;
}

static void register_gamepad_api(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue navigator = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, navigator, "getGamepads",
        JS_NewCFunction(ctx, js_get_gamepads, "getGamepads", 0));
    JS_SetPropertyStr(ctx, global, "navigator", navigator);
    JS_FreeValue(ctx, global);
}

/* ══════════════════════════════════════════════════════════════════
 *  fetch API — relative paths → wc_load_asset
 * ══════════════════════════════════════════════════════════════════ */

/*
 * Give a fetch Response its body methods and wrap it in a Promise.
 *
 * Shared by the 200 and 404 paths deliberately: they used to be written out
 * separately and the 404 one had neither, so `fetch(missing).then(...)` threw
 * "not a function". Any future field belongs here, once.
 */
static JSValue finish_response(JSContext *ctx, JSValue response) {
    const char *methods_src =
        "(function(resp) {\n"
        "  resp.arrayBuffer = function() { return Promise.resolve(this._data); };\n"
        "  resp.text = function() { return Promise.resolve(this._text); };\n"
        "  resp.json = function() { return Promise.resolve(JSON.parse(this._text)); };\n"
        "  resp.blob = function() { return Promise.resolve(new Blob([this._data])); };\n"
        "})";
    JSValue setup_fn = JS_Eval(ctx, methods_src, strlen(methods_src),
                                "<fetch>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(setup_fn)) {
        JS_Call(ctx, setup_fn, JS_UNDEFINED, 1, &response);
    }
    JS_FreeValue(ctx, setup_fn);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
    JSValue promise = JS_Call(ctx, resolve_fn, promise_ctor, 1, &response);
    JS_FreeValue(ctx, resolve_fn);
    JS_FreeValue(ctx, promise_ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, response);
    return promise;
}

static JSValue js_fetch(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "fetch requires a URL");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    /* TODO: absolute URLs should go through host fetch (wasmcart ABI).
     * For now, treat as not found — don't throw, let game handle gracefully. */

    /* Strip leading ./ */
    const char *path = url;
    if (path[0] == '.' && path[1] == '/') path += 2;

    int size;
    uint8_t *data = load_asset_binary(path, &size);
    JS_FreeCString(ctx, url);

    if (!data) {
        /*
         * A 404 response must have the SAME SHAPE as a 200 one. It used to
         * return a bare {ok:false, status:404} with no _data, no _text and no
         * body methods, so `fetch(missing).then(...)` threw "not a function"
         * and `.text()` was absent entirely -- a missing asset crashed the game
         * instead of letting it handle the error. `await fetch(...)` happened
         * to work, which is why hello_fetch never caught it.
         *
         * The dead JS_Call/JS_GetPropertyStr pair that used to sit here also
         * leaked two JSValues on every miss.
         */
        JSValue response = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, response, "ok", JS_FALSE);
        JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, 404));
        JS_SetPropertyStr(ctx, response, "_data", JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0));
        JS_SetPropertyStr(ctx, response, "_text", JS_NewStringLen(ctx, "", 0));
        return finish_response(ctx, response);
    }

    /* Build Response-like object */
    JSValue response = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, response, "ok", JS_TRUE);
    JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, 200));

    /* Store raw data for .arrayBuffer(), .text(), .json() */
    JSValue ab = JS_NewArrayBufferCopy(ctx, data, size);

    /* .arrayBuffer() — returns the ArrayBuffer (sync for simplicity) */
    /* We create closures that capture the data */
    JS_SetPropertyStr(ctx, response, "_data", ab);

    /* .text() */
    JSValue text_val = JS_NewStringLen(ctx, (const char *)data, size);
    JS_SetPropertyStr(ctx, response, "_text", text_val);

    free(data);

    /* Add .text(), .json(), .arrayBuffer() methods via eval */
    const char *methods_src =
        "(function(resp) {\n"
        "  resp.arrayBuffer = function() { return Promise.resolve(this._data); };\n"
        "  resp.text = function() { return Promise.resolve(this._text); };\n"
        "  resp.json = function() { return Promise.resolve(JSON.parse(this._text)); };\n"
        "  resp.blob = function() { return Promise.resolve(new Blob([this._data])); };\n"
        "})";
    JSValue setup_fn = JS_Eval(ctx, methods_src, strlen(methods_src),
                                "<fetch>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(setup_fn)) {
        JS_Call(ctx, setup_fn, JS_UNDEFINED, 1, &response);
        JS_FreeValue(ctx, setup_fn);
    }

    /* Wrap in Promise.resolve() for async compat */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
    JSValue resolved = JS_Call(ctx, resolve_fn, promise_ctor, 1, &response);
    JS_FreeValue(ctx, resolve_fn);
    JS_FreeValue(ctx, promise_ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, response);

    return resolved;
}

static void register_fetch_api(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fetch",
        JS_NewCFunction(ctx, js_fetch, "fetch", 1));
    JS_FreeValue(ctx, global);
}

/* ══════════════════════════════════════════════════════════════════
 *  Image shim — new Image(), img.src = "foo.png"
 * ══════════════════════════════════════════════════════════════════ */

/*
 * Minimal Image class. Setting .src loads the asset and decodes via
 * stb_image. The decoded RGBA pixels are accessible for Canvas 2D
 * drawImage() and WebGL texImage2D().
 *
 * Full implementation in image_shim.c (uses stb_image).
 * This is a placeholder that registers the class.
 */
static void register_image_api(JSContext *ctx) {
    /* Image class will be implemented in image_shim.c */
    /* For now, register a minimal constructor */
    const char *src =
        "globalThis.Image = class Image {\n"
        "  constructor(w, h) {\n"
        "    this.width = w || 0;\n"
        "    this.height = h || 0;\n"
        "    this.complete = false;\n"
        "    this._src = '';\n"
        "    this._onload = null;\n"
        "    this._onerror = null;\n"
        "    this._rgba = null;\n"
        "  }\n"
        "  set src(v) {\n"
        "    this._src = v;\n"
        "    fetch(v).then(r => r.arrayBuffer()).then(ab => {\n"
        "      /* Decode image using Skia (same as @napi-rs/canvas) */\n"
        "      const decoded = _wcDecodeImage(ab);\n"
        "      if (decoded) {\n"
        "        this.width = decoded.width;\n"
        "        this.height = decoded.height;\n"
        "        this._rgba = decoded._rgba || new Uint8Array(decoded.data);\n"
        "        this.naturalWidth = decoded.width;\n"
        "        this.naturalHeight = decoded.height;\n"
        "      }\n"
        "      this.complete = true;\n"
        "      this._data = ab;\n"
        "      if (this._onload) this._onload();\n"
        "    }).catch(e => {\n"
        "      if (this._onerror) this._onerror(e);\n"
        "    });\n"
        "  }\n"
        "  get src() { return this._src; }\n"
        "  set onload(fn) { this._onload = fn; }\n"
        "  get onload() { return this._onload; }\n"
        "  set onerror(fn) { this._onerror = fn; }\n"
        "  get onerror() { return this._onerror; }\n"
        "};\n";
    JS_Eval(ctx, src, strlen(src), "<image>", JS_EVAL_TYPE_GLOBAL);
}

/* ══════════════════════════════════════════════════════════════════
 *  Canvas API — minimal 2D context backed by wc_framebuffer
 *
 *  Phase 1: software rasterizer for basic 2D operations.
 *  Full implementation in canvas_shim.c.
 * ══════════════════════════════════════════════════════════════════ */

static void register_canvas_api(JSContext *ctx) {
    /* Register Canvas/Context2D classes wired to native C2D + WebGL */
    const char *src =
        "class _WCCanvas {\n"
        "  constructor(w, h, isMain) {\n"
        "    this._width = w || 800;\n"
        "    this._height = h || 600;\n"
        "    this._isMain = !!isMain;\n"
        "    this._ctx2d = null;\n"
        "    this._glctx = null;\n"
        "  }\n"
        "  get width() { return this._width; }\n"
        "  set width(v) {\n"
        "    this._width = v;\n"
        "    if (this._isMain && typeof _wcSetResolution === 'function') _wcSetResolution(v, this._height);\n"
        "    if (this._glctx) { this._glctx.drawingBufferWidth = v; }\n"
        "  }\n"
        "  get height() { return this._height; }\n"
        "  set height(v) {\n"
        "    this._height = v;\n"
        "    if (this._isMain && typeof _wcSetResolution === 'function') _wcSetResolution(this._width, v);\n"
        "    if (this._glctx) { this._glctx.drawingBufferHeight = v; }\n"
        "  }\n"
        "  getContext(type) {\n"
        "    if (type === '2d') {\n"
        "      if (!this._ctx2d) this._ctx2d = new _WCContext2D(this);\n"
        "      return this._ctx2d;\n"
        "    }\n"
        "    if (type === 'webgl' || type === 'webgl2' ||\n"
        "        type === 'experimental-webgl') {\n"
        "      if (!this._glctx && typeof _wcGL !== 'undefined' && this._isMain) {\n"
        "        if (typeof _wcSetWebGL === 'function') _wcSetWebGL(1);\n"
        "        _wcGL.canvas = this;\n"
        "        _wcGL.drawingBufferWidth = this.width;\n"
        "        _wcGL.drawingBufferHeight = this.height;\n"
        "        this._glctx = _wcGL;\n"
        "      }\n"
        "      return this._glctx;\n"
        "    }\n"
        "    return null;\n"
        "  }\n"
        "  get clientWidth() { return this.width; }\n"
        "  get clientHeight() { return this.height; }\n"
        "  getBoundingClientRect() {\n"
        "    return { x: 0, y: 0, width: this.width, height: this.height,\n"
        "             top: 0, left: 0, bottom: this.height, right: this.width };\n"
        "  }\n"
        "  addEventListener(type, fn) {\n"
        "    if (type.startsWith('key')) {\n"
        "      if (!globalThis._wcKeyListeners) globalThis._wcKeyListeners = [];\n"
        "      globalThis._wcKeyListeners.push(fn);\n"
        "    } else if (type.startsWith('mouse') || type.startsWith('pointer') || type.startsWith('touch') || type === 'click' || type === 'wheel') {\n"
        "      if (!globalThis._wcMouseListeners) globalThis._wcMouseListeners = [];\n"
        "      globalThis._wcMouseListeners.push(fn);\n"
        "    }\n"
        "  }\n"
        "  removeEventListener(type, fn) { /* TODO */ }\n"
        "  setAttribute(k, v) { /* stub */ }\n"
        "  requestFullscreen() { return Promise.resolve(); }\n"
        "  get style() { return { width: '', height: '' }; }\n"
        "  set tabIndex(v) {}\n"
        "  focus() {}\n"
        "}\n"
        "\n"
        "/* Context2D backed by native C rasterizer (_wcC2D) */\n"
        "class _WCContext2D {\n"
        "  constructor(canvas) {\n"
        "    this.canvas = canvas;\n"
        "    this._fillStyle = '#000000';\n"
        "    this._strokeStyle = '#000000';\n"
        "    this._lineWidth = 1;\n"
        "    this._font = '10px sans-serif';\n"
        "    this._fontSize = 10;\n"
        "    this.textAlign = 'start';\n"
        "    this.textBaseline = 'alphabetic';\n"
        "    this._globalAlpha = 1.0;\n"
        "    this._compositeOp = 'source-over';\n"
        "    this._imageSmoothing = true;\n"
        "  }\n"
        "  /* Property setters that sync to native state */\n"
        "  /* fillStyle/strokeStyle defined below (handles gradients + patterns) */\n"
        "  set globalAlpha(v) { this._globalAlpha = v; _wcC2D._setGlobalAlpha(v); }\n"
        "  get globalAlpha() { return this._globalAlpha; }\n"
        "  set lineWidth(v) { this._lineWidth = v; _wcC2D._setLineWidth(v); }\n"
        "  get lineWidth() { return this._lineWidth; }\n"
        "  set font(v) {\n"
        "    this._font = v;\n"
        "    const m = v.match(/(\\d+(?:\\.\\d+)?)px/);\n"
        "    if (m) this._fontSize = parseFloat(m[1]);\n"
        "    _wcC2D._setFont(v);\n"
        "  }\n"
        "  get font() { return this._font; }\n"
        "  /* Drawing operations → native C */\n"
        "  clearRect(x, y, w, h) { _wcC2D.clearRect(x, y, w, h); }\n"
        "  fillRect(x, y, w, h) { _wcC2D.fillRect(x, y, w, h); }\n"
        "  strokeRect(x, y, w, h) { _wcC2D.strokeRect(x, y, w, h); }\n"
        "  fillText(text, x, y) {\n"
        "    let ax = x, ay = y;\n"
        "    if (this.textAlign === 'center' || this.textAlign === 'right' || this.textAlign === 'end') {\n"
        "      const w = _wcC2D.measureText(text).width;\n"
        "      if (this.textAlign === 'center') ax -= w / 2;\n"
        "      else ax -= w;\n"
        "    }\n"
        "    /* pass baseline mode: 0=alphabetic(default), 1=top, 2=middle, 3=bottom */\n"
        "    var _bl = 0;\n"
        "    if (this.textBaseline === 'top' || this.textBaseline === 'hanging') _bl = 1;\n"
        "    else if (this.textBaseline === 'middle') _bl = 2;\n"
        "    else if (this.textBaseline === 'bottom' || this.textBaseline === 'ideographic') _bl = 3;\n"
        "    _wcC2D.fillText(text, ax, ay, _bl);\n"
        "  }\n"
        "  strokeText(text, x, y) {\n"
        "    let ax = x, ay = y;\n"
        "    if (this.textAlign === 'center' || this.textAlign === 'right' || this.textAlign === 'end') {\n"
        "      const w = _wcC2D.measureText(text).width;\n"
        "      if (this.textAlign === 'center') ax -= w / 2;\n"
        "      else ax -= w;\n"
        "    }\n"
        "    /* pass baseline mode: 0=alphabetic(default), 1=top, 2=middle, 3=bottom */\n"
        "    var _bl = 0;\n"
        "    if (this.textBaseline === 'top' || this.textBaseline === 'hanging') _bl = 1;\n"
        "    else if (this.textBaseline === 'middle') _bl = 2;\n"
        "    else if (this.textBaseline === 'bottom' || this.textBaseline === 'ideographic') _bl = 3;\n"
        "    _wcC2D.strokeText(text, ax, ay, _bl);\n"
        "  }\n"
        "  measureText(text) { return _wcC2D.measureText(text); }\n"
        "  beginPath() { _wcC2D.beginPath(); }\n"
        "  closePath() { _wcC2D.closePath(); }\n"
        "  moveTo(x, y) { _wcC2D.moveTo(x, y); }\n"
        "  lineTo(x, y) { _wcC2D.lineTo(x, y); }\n"
        "  arc(x, y, r, start, end, ccw) { _wcC2D.arc(x, y, r, start, end); }\n"
        "  fill() { _wcC2D.fill(); }\n"
        "  stroke() { _wcC2D.stroke(); }\n"
        "  save() { _wcC2D.save(); }\n"
        "  restore() { _wcC2D.restore(); }\n"
        "  translate(x, y) { _wcC2D.translate(x, y); }\n"
        "  rotate(angle) { _wcC2D.rotate(angle); }\n"
        "  scale(x, y) { _wcC2D.scale(x, y); }\n"
        "  drawImage(img, ...args) {\n"
        "    if (!img || !img._rgba) return;\n"
        "    /* Store for getImageData */\n"
        "    this._imgPixels = img._rgba;\n"
        "    this._imgW = img.width;\n"
        "    this._imgH = img.height;\n"
        "    if (args.length >= 8) {\n"
        "      /* 9-arg: drawImage(img, sx, sy, sw, sh, dx, dy, dw, dh) */\n"
        "      _wcC2D.drawImage(img._rgba, img.width, img.height,\n"
        "        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);\n"
        "    } else if (args.length >= 4) {\n"
        "      /* 5-arg: drawImage(img, dx, dy, dw, dh) */\n"
        "      _wcC2D.drawImage(img._rgba, img.width, img.height,\n"
        "        0, 0, img.width, img.height, args[0], args[1], args[2], args[3]);\n"
        "    } else {\n"
        "      /* 3-arg: drawImage(img, dx, dy) */\n"
        "      _wcC2D.drawImage(img._rgba, img.width, img.height,\n"
        "        0, 0, img.width, img.height, args[0]||0, args[1]||0, img.width, img.height);\n"
        "    }\n"
        "  }\n"
        "  createImageData(w, h) {\n"
        "    return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) };\n"
        "  }\n"
        "  getImageData(x, y, w, h) {\n"
        "    /* If we have stored image pixels from drawImage, return those */\n"
        "    if (this._imgPixels && x === 0 && y === 0 && w === this._imgW && h === this._imgH) {\n"
        "      return { width: w, height: h, data: new Uint8ClampedArray(this._imgPixels.buffer.slice(0)) };\n"
        "    }\n"
        "    /* No try/catch swallow here: it hid the fact that native\n"
        "     * getImageData was unregistered, so every read returned black. */\n"
        "    const r = _wcC2D.getImageData(x, y, w, h);\n"
        "    if (r) return r;\n"
        "    return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) };\n"
        "  }\n"
        "  putImageData(imageData, x, y) { _wcC2D.putImageData(imageData, x, y); }\n"
        "  /* Gradients — store color stops, apply first stop as flat color */\n"
        "  createLinearGradient(x0,y0,x1,y1) {\n"
        "    const g = { _stops: [], addColorStop(p,c) { this._stops.push({p,c}); } };\n"
        "    return g;\n"
        "  }\n"
        "  createRadialGradient(x0,y0,r0,x1,y1,r1) {\n"
        "    const g = { _stops: [], addColorStop(p,c) { this._stops.push({p,c}); } };\n"
        "    return g;\n"
        "  }\n"
        "  createConicGradient(startAngle,x,y) {\n"
        "    const g = { _stops: [], addColorStop(p,c) { this._stops.push({p,c}); } };\n"
        "    return g;\n"
        "  }\n"
        "  createPattern(image, repetition) {\n"
        "    return { _image: image, _rep: repetition, setTransform(m) {} };\n"
        "  }\n"
        "  /* When fillStyle/strokeStyle is set to a gradient/pattern, use first color stop */\n"
        "  set fillStyle(v) {\n"
        "    if (v && v._stops && v._stops.length > 0) {\n"
        "      this._fillStyle = v._stops[0].c;\n"
        "    } else if (typeof v === 'string') {\n"
        "      this._fillStyle = v;\n"
        "    } else {\n"
        "      this._fillStyle = '#000';\n"
        "    }\n"
        "    _wcC2D._setFillStyle(this._fillStyle);\n"
        "  }\n"
        "  get fillStyle() { return this._fillStyle; }\n"
        "  set strokeStyle(v) {\n"
        "    if (v && v._stops && v._stops.length > 0) {\n"
        "      this._strokeStyle = v._stops[0].c;\n"
        "    } else if (typeof v === 'string') {\n"
        "      this._strokeStyle = v;\n"
        "    } else {\n"
        "      this._strokeStyle = '#000';\n"
        "    }\n"
        "    _wcC2D._setStrokeStyle(this._strokeStyle);\n"
        "  }\n"
        "  get strokeStyle() { return this._strokeStyle; }\n"
        "  /* globalCompositeOperation */\n"
        "  set globalCompositeOperation(v) { this._compositeOp = v; }\n"
        "  get globalCompositeOperation() { return this._compositeOp || 'source-over'; }\n"
        "  /* Line dash */\n"
        "  setLineDash(segments) { this._lineDash = segments; }\n"
        "  getLineDash() { return this._lineDash || []; }\n"
        "  set lineDashOffset(v) { this._lineDashOffset = v; }\n"
        "  get lineDashOffset() { return this._lineDashOffset || 0; }\n"
        "  /* Shadow — store properties, Skia has mask filter support */\n"
        "  set shadowColor(v) { this._shadowColor = v; }\n"
        "  get shadowColor() { return this._shadowColor || 'rgba(0,0,0,0)'; }\n"
        "  set shadowBlur(v) { this._shadowBlur = v; }\n"
        "  get shadowBlur() { return this._shadowBlur || 0; }\n"
        "  set shadowOffsetX(v) { this._shadowOffsetX = v; }\n"
        "  get shadowOffsetX() { return this._shadowOffsetX || 0; }\n"
        "  set shadowOffsetY(v) { this._shadowOffsetY = v; }\n"
        "  get shadowOffsetY() { return this._shadowOffsetY || 0; }\n"
        "  /* Line cap/join */\n"
        "  set lineCap(v) { this._lineCap = v; }\n"
        "  get lineCap() { return this._lineCap || 'butt'; }\n"
        "  set lineJoin(v) { this._lineJoin = v; }\n"
        "  get lineJoin() { return this._lineJoin || 'miter'; }\n"
        "  set miterLimit(v) { this._miterLimit = v; }\n"
        "  get miterLimit() { return this._miterLimit || 10; }\n"
        "  /* Transforms — delegate to native */\n"
        "  setTransform(a,b,c,d,e,f) {\n"
        "    if (typeof a === 'object') { _wcC2D.setTransform(a.a,a.b,a.c,a.d,a.e,a.f); }\n"
        "    else { _wcC2D.setTransform(a,b,c,d,e,f); }\n"
        "  }\n"
        "  resetTransform() { _wcC2D.resetTransform(); }\n"
        "  getTransform() { return {a:1,b:0,c:0,d:1,e:0,f:0}; }\n"
        "  /* Path ops — delegate to native Skia */\n"
        "  clip() { _wcC2D.clip(); }\n"
        "  rect(x,y,w,h) { _wcC2D.rect(x,y,w,h); }\n"
        "  /* arcTo was registered natively (canvas2d_skia.c) but never exposed\n"
        "   * here, so ctx.arcTo was undefined for every cart. */\n"
        "  arcTo(x1,y1,x2,y2,r) { _wcC2D.arcTo(x1,y1,x2,y2,r); }\n"
        "  quadraticCurveTo(cpx,cpy,x,y) { _wcC2D.quadraticCurveTo(cpx,cpy,x,y); }\n"
        "  bezierCurveTo(cp1x,cp1y,cp2x,cp2y,x,y) { _wcC2D.bezierCurveTo(cp1x,cp1y,cp2x,cp2y,x,y); }\n"
        "  ellipse(x,y,rx,ry,rot,start,end,ccw) { _wcC2D.arc(x,y,Math.max(rx,ry),start,end); }\n"
        "  /* roundRect: real path, not an approximation -- arcTo is already native.\n"
        "   * Accepts the spec's number | [all] | [tl,tr,br,bl] radii forms. */\n"
        "  roundRect(x, y, w, h, radii) {\n"
        "    let r = radii === undefined ? 0 : radii;\n"
        "    if (typeof r === 'number') r = [r, r, r, r];\n"
        "    else if (Array.isArray(r)) {\n"
        "      if (r.length === 1) r = [r[0], r[0], r[0], r[0]];\n"
        "      else if (r.length === 2) r = [r[0], r[1], r[0], r[1]];\n"
        "      else if (r.length === 3) r = [r[0], r[1], r[2], r[1]];\n"
        "    } else r = [0, 0, 0, 0];\n"
        "    const lim = Math.min(Math.abs(w), Math.abs(h)) / 2;\n"
        "    r = r.map(v => Math.min(Math.max(Number(v) || 0, 0), lim));\n"
        "    const [tl, tr, br, bl] = r;\n"
        "    this.moveTo(x + tl, y);\n"
        "    this.lineTo(x + w - tr, y);\n"
        "    this.arcTo(x + w, y, x + w, y + tr, tr);\n"
        "    this.lineTo(x + w, y + h - br);\n"
        "    this.arcTo(x + w, y + h, x + w - br, y + h, br);\n"
        "    this.lineTo(x + bl, y + h);\n"
        "    this.arcTo(x, y + h, x, y + h - bl, bl);\n"
        "    this.lineTo(x, y + tl);\n"
        "    this.arcTo(x, y, x + tl, y, tl);\n"
        "    this.closePath();\n"
        "  }\n"
        "  isPointInPath() { return false; }\n"
        "  isPointInStroke() { return false; }\n"
        "  /* Image smoothing */\n"
        "  set imageSmoothingEnabled(v) { this._imageSmoothing = v; }\n"
        "  get imageSmoothingEnabled() { return this._imageSmoothing !== false; }\n"
        "  set imageSmoothingQuality(v) { this._imageSmoothingQuality = v; }\n"
        "  get imageSmoothingQuality() { return this._imageSmoothingQuality || 'low'; }\n"
        "  /* Text direction */\n"
        "  set direction(v) { this._direction = v; }\n"
        "  get direction() { return this._direction || 'ltr'; }\n"
        "  set letterSpacing(v) { this._letterSpacing = v; }\n"
        "  get letterSpacing() { return this._letterSpacing || '0px'; }\n"
        "  set wordSpacing(v) { this._wordSpacing = v; }\n"
        "  get wordSpacing() { return this._wordSpacing || '0px'; }\n"
        "}\n"
        "\n"
        "/* document shim */\n"
        "globalThis.document = {\n"
        "  _canvas: null,\n"
        "  createElement(tag) {\n"
        "    if (tag === 'canvas') return new _WCCanvas(0, 0, false);\n"
        "    return { style: {}, appendChild() {}, setAttribute() {},\n"
        "             addEventListener() {}, innerHTML: '' };\n"
        "  },\n"
        "  getElementById(id) {\n"
        "    if (!this._canvas) this._canvas = new _WCCanvas(800, 600, true);\n"
        "    return this._canvas;\n"
        "  },\n"
        "  querySelector(sel) { return this.getElementById(sel); },\n"
        "  querySelectorAll(sel) { return []; },\n"
        "  body: {\n"
        "    appendChild(el) {},\n"
        "    removeChild(el) {},\n"
        "    style: {},\n"
        "    clientWidth: 800,\n"
        "    clientHeight: 600\n"
        "  },\n"
        "  head: { appendChild(el) {} },\n"
        "  documentElement: { style: {} },\n"
        "  addEventListener(type, fn) {\n"
        "    if (type.startsWith('key')) {\n"
        "      if (!globalThis._wcKeyListeners) globalThis._wcKeyListeners = [];\n"
        "      globalThis._wcKeyListeners.push(fn);\n"
        "    } else if (type.startsWith('mouse') || type.startsWith('pointer') || type.startsWith('touch') || type === 'click' || type === 'wheel') {\n"
        "      if (!globalThis._wcMouseListeners) globalThis._wcMouseListeners = [];\n"
        "      globalThis._wcMouseListeners.push(fn);\n"
        "    }\n"
        "  },\n"
        "  removeEventListener(type, fn) { /* stub */ },\n"
        "  createTextNode(text) { return {}; },\n"
        "};\n"
        "\n"
        "/* WebGL2RenderingContext — three.js checks instanceof */\n"
        "globalThis.WebGL2RenderingContext = class WebGL2RenderingContext {\n"
        "  static [Symbol.hasInstance](obj) { return obj && typeof obj.bindBuffer === 'function'; }\n"
        "};\n"
        "\n"
        "/* window shim */\n"
        "globalThis.window = globalThis;\n"
        "globalThis.window.innerWidth = 800;\n"
        "globalThis.window.innerHeight = 600;\n"
        "globalThis.window.devicePixelRatio = 1;\n"
        "globalThis.self = globalThis;\n";

    JS_Eval(ctx, src, strlen(src), "<canvas>", JS_EVAL_TYPE_GLOBAL);
}

/* ══════════════════════════════════════════════════════════════════
 *  localStorage shim — IN-MEMORY ONLY, not persisted.
 *
 * The comment here used to claim "backed by wasmcart save data". It is not:
 * _data is a plain object, so everything written is lost when the cart exits.
 * Games that expect saves to survive a restart silently lose them. Wiring this
 * to the wasmcart save ABI is real work and is not done.
 * ══════════════════════════════════════════════════════════════════ */

/* Read the save region back out as a JSON string, or "" if empty/invalid. */
static JSValue js_save_read(JSContext *c, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (save_region[0] != SAVE_MAGIC0 || save_region[1] != SAVE_MAGIC1 ||
        save_region[2] != SAVE_MAGIC2 || save_region[3] != SAVE_MAGIC3)
        return JS_NewString(c, "");          /* first run, or foreign bytes */

    uint32_t len = (uint32_t)save_region[4]        |
                   ((uint32_t)save_region[5] << 8) |
                   ((uint32_t)save_region[6] << 16)|
                   ((uint32_t)save_region[7] << 24);
    /* A truncated or hostile length must not read past the region. */
    if (len == 0 || len > SAVE_SIZE - SAVE_HEADER)
        return JS_NewString(c, "");
    return JS_NewStringLen(c, (const char *)(save_region + SAVE_HEADER), len);
}

/* Serialize a JSON string into the save region. Returns 0 if it does not fit,
 * so the JS side can surface a real QuotaExceededError instead of truncating
 * a save into corruption. */
static JSValue js_save_write(JSContext *c, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewInt32(c, 0);
    size_t len = 0;
    const char *json = JS_ToCStringLen(c, &len, argv[0]);
    if (!json) return JS_NewInt32(c, 0);

    if (len > SAVE_SIZE - SAVE_HEADER) {      /* refuse rather than truncate */
        JS_FreeCString(c, json);
        return JS_NewInt32(c, 0);
    }
    save_region[0] = SAVE_MAGIC0; save_region[1] = SAVE_MAGIC1;
    save_region[2] = SAVE_MAGIC2; save_region[3] = SAVE_MAGIC3;
    save_region[4] = (uint8_t)(len & 0xff);
    save_region[5] = (uint8_t)((len >> 8) & 0xff);
    save_region[6] = (uint8_t)((len >> 16) & 0xff);
    save_region[7] = (uint8_t)((len >> 24) & 0xff);
    memcpy(save_region + SAVE_HEADER, json, len);
    /* Zero the tail: the host persists the WHOLE region, and leaving a longer
     * previous save behind it would ship stale bytes in every .sav file. */
    if (SAVE_HEADER + len < SAVE_SIZE)
        memset(save_region + SAVE_HEADER + len, 0, SAVE_SIZE - SAVE_HEADER - len);
    JS_FreeCString(c, json);
    return JS_NewInt32(c, 1);
}

static void register_localstorage_api(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "_wcSaveRead",
        JS_NewCFunction(ctx, js_save_read, "_wcSaveRead", 0));
    JS_SetPropertyStr(ctx, global, "_wcSaveWrite",
        JS_NewCFunction(ctx, js_save_write, "_wcSaveWrite", 1));
    JS_FreeValue(ctx, global);

    /* Backed by the wasmcart save region, so writes survive a restart.
     * Every mutation flushes immediately: there is no beforeunload in a cart,
     * and the host may read the region at any frame boundary, so deferring
     * would lose the last writes on a hard quit. Saves are small and games
     * write them rarely, so the JSON round-trip per setItem is not worth
     * optimising until something measures it. */
    const char *src =
        "globalThis.localStorage = (function () {\n"
        "  let data = {};\n"
        "  try {\n"
        "    const raw = _wcSaveRead();\n"
        "    if (raw) {\n"
        "      const parsed = JSON.parse(raw);\n"
        "      if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {\n"
        "        for (const k of Object.keys(parsed)) data[k] = String(parsed[k]);\n"
        "      }\n"
        "    }\n"
        "  } catch (e) { data = {}; }   /* corrupt save must not brick the game */\n"
        "  function flush(next) {\n"
        "    const json = JSON.stringify(next);\n"
        "    if (!_wcSaveWrite(json)) {\n"
        "      const err = new Error('localStorage quota exceeded');\n"
        "      err.name = 'QuotaExceededError';\n"
        "      throw err;\n"
        "    }\n"
        "    data = next;\n"
        "  }\n"
        "  return {\n"
        "    getItem(key) { const k = String(key);\n"
        "      return Object.prototype.hasOwnProperty.call(data, k) ? data[k] : null; },\n"
        "    setItem(key, value) {\n"
        "      /* Build the next state first: if it does not fit, flush throws\n"
        "       * and `data` is left exactly as it was, rather than half-applied. */\n"
        "      const next = Object.assign({}, data);\n"
        "      next[String(key)] = String(value);\n"
        "      flush(next);\n"
        "    },\n"
        "    removeItem(key) {\n"
        "      const next = Object.assign({}, data);\n"
        "      delete next[String(key)];\n"
        "      flush(next);\n"
        "    },\n"
        "    clear() { flush({}); },\n"
        "    get length() { return Object.keys(data).length; },\n"
        "    key(i) { const ks = Object.keys(data);\n"
        "      return (i >= 0 && i < ks.length) ? ks[i] : null; }\n"
        "  };\n"
        "})();\n";
    JS_Eval(ctx, src, strlen(src), "<localStorage>", JS_EVAL_TYPE_GLOBAL);
}

/* ══════════════════════════════════════════════════════════════════
 *  performance.now()
 * ══════════════════════════════════════════════════════════════════ */

static JSValue js_performance_now(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    return JS_NewFloat64(ctx, time_info.time_ms);
}

static void register_performance_api(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue perf = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, perf, "now",
        JS_NewCFunction(ctx, js_performance_now, "now", 0));
    JS_SetPropertyStr(ctx, global, "performance", perf);
    JS_FreeValue(ctx, global);
}

/* ══════════════════════════════════════════════════════════════════
 *  Event shims (keyboard, mouse, etc.)
 * ══════════════════════════════════════════════════════════════════ */

static void register_event_shims(JSContext *ctx) {
    const char *src =
        "/* Event listener arrays */\n"
        "if (!globalThis._wcKeyListeners) globalThis._wcKeyListeners = [];\n"
        "if (!globalThis._wcMouseListeners) globalThis._wcMouseListeners = [];\n"
        "\n"
        "globalThis.addEventListener = function(type, fn) {\n"
        "  if (type.startsWith('key')) globalThis._wcKeyListeners.push(fn);\n"
        "  else if (type.startsWith('mouse') || type.startsWith('pointer') || type.startsWith('touch') || type === 'click' || type === 'contextmenu' || type === 'wheel')\n"
        "    globalThis._wcMouseListeners.push(fn);\n"
        "};\n"
        "globalThis.removeEventListener = function(type, fn) {\n"
        "  if (type.startsWith('key')) {\n"
        "    const i = globalThis._wcKeyListeners.indexOf(fn);\n"
        "    if (i >= 0) globalThis._wcKeyListeners.splice(i, 1);\n"
        "  }\n"
        "};\n"
        "\n"
        "/* Date.now() — must use performance.now() from wasmcart time.\n"
        "   QuickJS built-in Date.now returns system time which is 0 in WASM standalone. */\n"
        "Date.now = function() { return performance.now(); };\n"
        "\n"
        "/* FontFace class */\n"
        "globalThis.FontFace = class FontFace {\n"
        "  constructor(family, source) {\n"
        "    this.family = family;\n"
        "    this._source = source;\n"
        "    this.status = 'unloaded';\n"
        "  }\n"
        "  async load() {\n"
        "    /* Extract URL from 'url(...)' format */\n"
        "    let url = this._source;\n"
        "    if (typeof url === 'string') {\n"
        "      const m = url.match(/url\\(['\"]?([^)'\"]+)['\"]?\\)/);\n"
        "      if (m) url = m[1];\n"
        "    }\n"
        "    try {\n"
        "      const resp = await fetch(url);\n"
        "      if (resp.ok) {\n"
        "        const data = await resp.arrayBuffer();\n"
        "        _wcC2D._loadFont(this.family, data);\n"
        "        this.status = 'loaded';\n"
        "      }\n"
        "    } catch(e) { console.log('FontFace load error:', e); }\n"
        "    return this;\n"
        "  }\n"
        "};\n"
        "\n"
        "/* document.fonts stub */\n"
        "if (!document.fonts) document.fonts = { add(f) {} };\n"
        "\n"
        "/* Blob with data access */\n"
        "if (typeof Blob === 'undefined') {\n"
        "  globalThis.Blob = class Blob {\n"
        "    constructor(parts, opts) {\n"
        "      this._parts = parts || [];\n"
        "      this.type = (opts && opts.type) || '';\n"
        "      /* Flatten parts into single buffer */\n"
        "      let totalLen = 0;\n"
        "      for (const p of this._parts) {\n"
        "        if (p instanceof ArrayBuffer) totalLen += p.byteLength;\n"
        "        else if (p && p.buffer) totalLen += p.byteLength;\n"
        "        else if (typeof p === 'string') totalLen += p.length;\n"
        "      }\n"
        "      this.size = totalLen;\n"
        "    }\n"
        "    async arrayBuffer() {\n"
        "      const buf = new Uint8Array(this.size);\n"
        "      let off = 0;\n"
        "      for (const p of this._parts) {\n"
        "        if (p instanceof ArrayBuffer) { buf.set(new Uint8Array(p), off); off += p.byteLength; }\n"
        "        else if (p && p.buffer) { buf.set(new Uint8Array(p.buffer, p.byteOffset, p.byteLength), off); off += p.byteLength; }\n"
        "        else if (typeof p === 'string') { for (let i=0;i<p.length;i++) buf[off++]=p.charCodeAt(i); }\n"
        "      }\n"
        "      return buf.buffer;\n"
        "    }\n"
        "    async text() { const ab = await this.arrayBuffer(); return new TextDecoder().decode(ab); }\n"
        "  };\n"
        "}\n"
        "\n"
        "/* URL with blob registry */\n"
        "if (typeof URL === 'undefined' || !URL.createObjectURL) {\n"
        "  const _blobRegistry = new Map();\n"
        "  globalThis.URL = class URL {\n"
        "    constructor(url, base) {\n"
        "      if (base) {\n"
        "        /* Resolve relative URL against base */\n"
        "        const b = base.replace(/\\/[^\\/]*$/, '/');\n"
        "        this.href = url.startsWith('.') ? b + url.replace(/^\\.\\//,'') : url;\n"
        "      } else {\n"
        "        this.href = url;\n"
        "      }\n"
        "      this.pathname = this.href;\n"
        "    }\n"
        "    static createObjectURL(blob) {\n"
        "      const id = 'blob:wc:' + Math.random().toString(36).slice(2);\n"
        "      _blobRegistry.set(id, blob);\n"
        "      return id;\n"
        "    }\n"
        "    static revokeObjectURL(url) { _blobRegistry.delete(url); }\n"
        "    static _getBlob(url) { return _blobRegistry.get(url); }\n"
        "  };\n"
        "}\n"
        "\n"
        "/* MutationObserver / ResizeObserver / IntersectionObserver stubs */\n"
        "if (typeof MutationObserver === 'undefined') {\n"
        "  globalThis.MutationObserver = class MutationObserver {\n"
        "    constructor(cb) { this._cb = cb; }\n"
        "    observe() {} disconnect() {} takeRecords() { return []; }\n"
        "  };\n"
        "}\n"
        "if (typeof ResizeObserver === 'undefined') {\n"
        "  globalThis.ResizeObserver = class ResizeObserver {\n"
        "    constructor(cb) { this._cb = cb; }\n"
        "    observe() {} unobserve() {} disconnect() {}\n"
        "  };\n"
        "}\n"
        "if (typeof IntersectionObserver === 'undefined') {\n"
        "  globalThis.IntersectionObserver = class IntersectionObserver {\n"
        "    constructor(cb) { this._cb = cb; }\n"
        "    observe() {} unobserve() {} disconnect() {}\n"
        "  };\n"
        "}\n"
        "\n"
        "/* XMLHttpRequest */\n"
        "globalThis.XMLHttpRequest = class XMLHttpRequest {\n"
        "  constructor() {\n"
        "    this.readyState = 0; this.status = 0; this.responseText = '';\n"
        "    this.response = null; this.responseType = '';\n"
        "    this._headers = {};\n"
        "  }\n"
        "  open(method, url) { this._method = method; this._url = url; this.readyState = 1; }\n"
        "  setRequestHeader(k,v) { this._headers[k] = v; }\n"
        "  send(data) {\n"
        "    if (this._method === 'GET') {\n"
        "      fetch(this._url).then(r => {\n"
        "        this.status = r.status;\n"
        "        if (this.responseType === 'arraybuffer') return r.arrayBuffer();\n"
        "        return r.text();\n"
        "      }).then(body => {\n"
        "        this.readyState = 4;\n"
        "        if (typeof body === 'string') { this.responseText = body; this.response = body; }\n"
        "        else { this.response = body; }\n"
        "        if (this.onreadystatechange) this.onreadystatechange();\n"
        "        if (this.onload) this.onload();\n"
        "      }).catch(e => { if (this.onerror) this.onerror(e); });\n"
        "    }\n"
        "  }\n"
        "  abort() {}\n"
        "  getResponseHeader(h) { return null; }\n"
        "  getAllResponseHeaders() { return ''; }\n"
        "};\n"
        "\n"
        "/* require() for CommonJS modules — loads from .wasc assets */\n"
        "globalThis.require = function(name) {\n"
        "  /* Return empty module for Node.js builtins */\n"
        "  const builtins = ['fs','path','os','child_process','http','https','net','crypto','stream','util','events','buffer'];\n"
        "  if (builtins.includes(name)) return {};\n"
        "  /* Try to load from assets */\n"
        "  const paths = [name, name + '.js', name + '/index.js'];\n"
        "  for (const p of paths) {\n"
        "    try {\n"
        "      const resp = fetch(p);\n"
        "      /* require is sync but fetch is async — this won't work for real CJS */\n"
        "      /* Return empty module as fallback */\n"
        "    } catch(e) {}\n"
        "  }\n"
        "  return {};\n"
        "};\n"
        "globalThis.module = { exports: {} };\n"
        "globalThis.exports = globalThis.module.exports;\n"
        "\n"
        "/* sessionStorage (same as localStorage) */\n"
        "globalThis.sessionStorage = globalThis.localStorage;\n"
        "\n"
        "/* TextEncoder/TextDecoder */\n"
        "if (typeof TextEncoder === 'undefined') {\n"
        "  globalThis.TextEncoder = class TextEncoder {\n"
        "    encode(str) { const a = new Uint8Array(str.length); for(let i=0;i<str.length;i++) a[i]=str.charCodeAt(i); return a; }\n"
        "  };\n"
        "}\n"
        "if (typeof TextDecoder === 'undefined') {\n"
        "  globalThis.TextDecoder = class TextDecoder {\n"
        "    decode(buf) { const a = new Uint8Array(buf); let s=''; for(let i=0;i<a.length;i++) s+=String.fromCharCode(a[i]); return s; }\n"
        "  };\n"
        "}\n"
        "\n"
        "/* DOMParser stub */\n"
        "globalThis.DOMParser = class DOMParser {\n"
        "  parseFromString(str, type) { return { querySelector() { return null; }, querySelectorAll() { return []; } }; }\n"
        "};\n"
        "\n"
        "/* WebSocket — backed by wasmcart WS ABI (wc_ws_*) */\n"
        "globalThis.WebSocket = class WebSocket {\n"
        "  static CONNECTING = 0;\n"
        "  static OPEN = 1;\n"
        "  static CLOSING = 2;\n"
        "  static CLOSED = 3;\n"
        "  constructor(url, protocols) {\n"
        "    this.url = url;\n"
        "    this.readyState = WebSocket.CONNECTING;\n"
        "    this.bufferedAmount = 0;\n"
        "    this.extensions = '';\n"
        "    this.protocol = typeof protocols === 'string' ? protocols : (protocols && protocols[0]) || '';\n"
        "    this.binaryType = 'arraybuffer';\n"
        "    this.onopen = null;\n"
        "    this.onclose = null;\n"
        "    this.onmessage = null;\n"
        "    this.onerror = null;\n"
        "    /* Open via wasmcart WS ABI — host connects to URL if manifest allows */\n"
        "    this._connId = typeof _wcWsOpen === 'function' ? _wcWsOpen(url, this) : -1;\n"
        "    if (this._connId < 0) {\n"
        "      /* Host doesn't support WS or URL not in manifest allowlist */\n"
        "      setTimeout(() => {\n"
        "        this.readyState = WebSocket.CLOSED;\n"
        "        if (this.onerror) this.onerror(new Event('error'));\n"
        "        if (this.onclose) this.onclose({ code: 1006, reason: 'not available', wasClean: false });\n"
        "      }, 0);\n"
        "    }\n"
        "  }\n"
        "  send(data) {\n"
        "    if (this.readyState !== WebSocket.OPEN) throw new Error('WebSocket is not open');\n"
        "    if (this._connId >= 0) _wcWsSend(this._connId, data);\n"
        "  }\n"
        "  close(code, reason) {\n"
        "    this.readyState = WebSocket.CLOSING;\n"
        "    if (this._connId >= 0) _wcWsClose(this._connId, code || 1000);\n"
        "    else setTimeout(() => {\n"
        "      this.readyState = WebSocket.CLOSED;\n"
        "      if (this.onclose) this.onclose({ code: code || 1000, reason: reason || '', wasClean: true });\n"
        "    }, 0);\n"
        "  }\n"
        "  addEventListener(type, fn) {\n"
        "    if (type === 'open') this.onopen = fn;\n"
        "    else if (type === 'close') this.onclose = fn;\n"
        "    else if (type === 'message') this.onmessage = fn;\n"
        "    else if (type === 'error') this.onerror = fn;\n"
        "  }\n"
        "  removeEventListener(type, fn) {\n"
        "    if (type === 'open' && this.onopen === fn) this.onopen = null;\n"
        "    else if (type === 'close' && this.onclose === fn) this.onclose = null;\n"
        "    else if (type === 'message' && this.onmessage === fn) this.onmessage = null;\n"
        "    else if (type === 'error' && this.onerror === fn) this.onerror = null;\n"
        "  }\n"
        "};\n"
        "\n"
        "/* Worker — real threads via pthreads + QuickJS */\n"
        "globalThis.Worker = class Worker {\n"
        "  constructor(url) {\n"
        "    this.url = url;\n"
        "    this.onmessage = null;\n"
        "    this.onerror = null;\n"
        "    this._onmsgHandler = (evt) => {\n"
        "      if (this.onmessage) this.onmessage(evt);\n"
        "    };\n"
        "    this._id = _wcWorkerCreate(url, this._onmsgHandler);\n"
        "    if (this._id < 0 && this.onerror) {\n"
        "      setTimeout(() => this.onerror(new Error('Worker creation failed')), 0);\n"
        "    }\n"
        "  }\n"
        "  postMessage(data) {\n"
        "    if (this._id >= 0) _wcWorkerPost(this._id, JSON.stringify(data));\n"
        "  }\n"
        "  terminate() {\n"
        "    if (this._id >= 0) { _wcWorkerTerminate(this._id); this._id = -1; }\n"
        "  }\n"
        "  addEventListener(type, fn) {\n"
        "    if (type === 'message') this.onmessage = fn;\n"
        "    else if (type === 'error') this.onerror = fn;\n"
        "  }\n"
        "  removeEventListener() {}\n"
        "};\n"
        "\n"
        "/* Event class */\n"
        "if (typeof Event === 'undefined') {\n"
        "  globalThis.Event = class Event {\n"
        "    constructor(type, opts) {\n"
        "      this.type = type;\n"
        "      this.bubbles = opts?.bubbles || false;\n"
        "      this.cancelable = opts?.cancelable || false;\n"
        "      this.defaultPrevented = false;\n"
        "    }\n"
        "    preventDefault() { this.defaultPrevented = true; }\n"
        "    stopPropagation() {}\n"
        "  };\n"
        "}\n"
        "if (typeof CustomEvent === 'undefined') {\n"
        "  globalThis.CustomEvent = class CustomEvent extends Event {\n"
        "    constructor(type, opts) { super(type, opts); this.detail = opts?.detail || null; }\n"
        "  };\n"
        "}\n";
    JS_Eval(ctx, src, strlen(src), "<events>", JS_EVAL_TYPE_GLOBAL);

    /* ── Additional browser globals ─────────────────────────────── */
    const char *extra =
        "/* crypto.getRandomValues — many games use this */\n"
        "if (typeof crypto === 'undefined') {\n"
        "  globalThis.crypto = {\n"
        "    getRandomValues(arr) {\n"
        "      for (let i = 0; i < arr.length; i++) {\n"
        "        if (arr.BYTES_PER_ELEMENT === 4) arr[i] = (Math.random() * 0xFFFFFFFF) >>> 0;\n"
        "        else if (arr.BYTES_PER_ELEMENT === 2) arr[i] = (Math.random() * 0xFFFF) >>> 0;\n"
        "        else arr[i] = (Math.random() * 256) >>> 0;\n"
        "      }\n"
        "      return arr;\n"
        "    },\n"
        "    randomUUID() {\n"
        "      return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => {\n"
        "        const r = (Math.random() * 16) | 0;\n"
        "        return (c === 'x' ? r : (r & 0x3 | 0x8)).toString(16);\n"
        "      });\n"
        "    }\n"
        "  };\n"
        "}\n"
        "\n"
        "/* atob / btoa — base64 encode/decode */\n"
        "if (typeof atob === 'undefined') {\n"
        "  const _b64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=';\n"
        "  globalThis.atob = function(s) {\n"
        "    let o = '', i = 0;\n"
        "    s = s.replace(/[^A-Za-z0-9+/=]/g, '');\n"
        "    while (i < s.length) {\n"
        "      const a = _b64.indexOf(s[i++]), b = _b64.indexOf(s[i++]);\n"
        "      const c = _b64.indexOf(s[i++]), d = _b64.indexOf(s[i++]);\n"
        "      const n = (a << 18) | (b << 12) | (c << 6) | d;\n"
        "      o += String.fromCharCode((n >> 16) & 0xFF);\n"
        "      if (c !== 64) o += String.fromCharCode((n >> 8) & 0xFF);\n"
        "      if (d !== 64) o += String.fromCharCode(n & 0xFF);\n"
        "    }\n"
        "    return o;\n"
        "  };\n"
        "  globalThis.btoa = function(s) {\n"
        "    let o = '', i = 0;\n"
        "    while (i < s.length) {\n"
        "      const a = s.charCodeAt(i++), b = i < s.length ? s.charCodeAt(i++) : NaN;\n"
        "      const c = i < s.length ? s.charCodeAt(i++) : NaN;\n"
        "      o += _b64[(a >> 2) & 0x3F];\n"
        "      o += _b64[((a & 0x3) << 4) | ((b >> 4) & 0xF)];\n"
        "      o += isNaN(b) ? '=' : _b64[((b & 0xF) << 2) | ((c >> 6) & 0x3)];\n"
        "      o += isNaN(c) ? '=' : _b64[c & 0x3F];\n"
        "    }\n"
        "    return o;\n"
        "  };\n"
        "}\n"
        "\n"
        "/* queueMicrotask */\n"
        "if (typeof queueMicrotask === 'undefined') {\n"
        "  globalThis.queueMicrotask = function(fn) { Promise.resolve().then(fn); };\n"
        "}\n"
        "\n"
        "/* structuredClone */\n"
        "if (typeof structuredClone === 'undefined') {\n"
        "  globalThis.structuredClone = function(obj) { return JSON.parse(JSON.stringify(obj)); };\n"
        "}\n"
        "\n"
        "/* matchMedia — many responsive games check this */\n"
        "if (typeof matchMedia === 'undefined') {\n"
        "  globalThis.matchMedia = function(query) {\n"
        "    return {\n"
        "      matches: false, media: query,\n"
        "      addEventListener() {}, removeEventListener() {},\n"
        "      addListener() {}, removeListener() {},\n"
        "      onchange: null\n"
        "    };\n"
        "  };\n"
        "}\n"
        "\n"
        "/* getComputedStyle */\n"
        "globalThis.getComputedStyle = function(el) {\n"
        "  return new Proxy({}, { get(t,p) { return ''; } });\n"
        "};\n"
        "\n"
        "/* requestIdleCallback */\n"
        "if (typeof requestIdleCallback === 'undefined') {\n"
        "  globalThis.requestIdleCallback = function(fn) { return setTimeout(() => fn({ didTimeout: false, timeRemaining: () => 16 }), 0); };\n"
        "  globalThis.cancelIdleCallback = function(id) { clearTimeout(id); };\n"
        "}\n"
        "\n"
        "/* AbortController / AbortSignal */\n"
        "if (typeof AbortController === 'undefined') {\n"
        "  globalThis.AbortSignal = class AbortSignal {\n"
        "    constructor() { this.aborted = false; this.reason = undefined; this.onabort = null; }\n"
        "    addEventListener() {} removeEventListener() {}\n"
        "    throwIfAborted() { if (this.aborted) throw this.reason; }\n"
        "  };\n"
        "  globalThis.AbortController = class AbortController {\n"
        "    constructor() { this.signal = new AbortSignal(); }\n"
        "    abort(reason) { this.signal.aborted = true; this.signal.reason = reason || new Error('AbortError'); }\n"
        "  };\n"
        "}\n"
        "\n"
        "/* MessageChannel */\n"
        "if (typeof MessageChannel === 'undefined') {\n"
        "  globalThis.MessageChannel = class MessageChannel {\n"
        "    constructor() {\n"
        "      this.port1 = { postMessage(d) { if (this._other.onmessage) this._other.onmessage({data:d}); }, onmessage: null, close() {} };\n"
        "      this.port2 = { postMessage(d) { if (this._other.onmessage) this._other.onmessage({data:d}); }, onmessage: null, close() {} };\n"
        "      this.port1._other = this.port2;\n"
        "      this.port2._other = this.port1;\n"
        "    }\n"
        "  };\n"
        "}\n"
        "\n"
        "/* location object */\n"
        "if (typeof location === 'undefined') {\n"
        "  globalThis.location = {\n"
        "    href: 'https://wasmcart.local/', protocol: 'https:', host: 'wasmcart.local',\n"
        "    hostname: 'wasmcart.local', port: '', pathname: '/', search: '', hash: '',\n"
        "    origin: 'https://wasmcart.local', reload() {}, replace() {}, assign() {}\n"
        "  };\n"
        "}\n"
        "\n"
        "/* history stub */\n"
        "if (typeof history === 'undefined') {\n"
        "  globalThis.history = { length: 1, state: null, pushState() {}, replaceState() {}, back() {}, forward() {}, go() {} };\n"
        "}\n"
        "\n"
        "/* screen object */\n"
        "if (typeof screen === 'undefined') {\n"
        "  globalThis.screen = {\n"
        "    get width() { return globalThis.window?.innerWidth || 800; },\n"
        "    get height() { return globalThis.window?.innerHeight || 600; },\n"
        "    get availWidth() { return this.width; },\n"
        "    get availHeight() { return this.height; },\n"
        "    colorDepth: 24, pixelDepth: 24\n"
        "  };\n"
        "}\n"
        "\n"
        "/* PointerEvent class */\n"
        "if (typeof PointerEvent === 'undefined') {\n"
        "  globalThis.PointerEvent = class PointerEvent extends Event {\n"
        "    constructor(type, init) {\n"
        "      super(type, init);\n"
        "      this.pointerId = init?.pointerId || 0;\n"
        "      this.pointerType = init?.pointerType || 'mouse';\n"
        "      this.clientX = init?.clientX || 0;\n"
        "      this.clientY = init?.clientY || 0;\n"
        "      this.pressure = init?.pressure || 0;\n"
        "      this.width = init?.width || 1;\n"
        "      this.height = init?.height || 1;\n"
        "      this.isPrimary = init?.isPrimary !== undefined ? init.isPrimary : true;\n"
        "    }\n"
        "  };\n"
        "}\n"
        "\n"
        "/* TouchEvent / Touch class */\n"
        "if (typeof Touch === 'undefined') {\n"
        "  globalThis.Touch = class Touch {\n"
        "    constructor(init) {\n"
        "      this.identifier = init?.identifier || 0;\n"
        "      this.target = init?.target || null;\n"
        "      this.clientX = init?.clientX || 0;\n"
        "      this.clientY = init?.clientY || 0;\n"
        "      this.pageX = init?.pageX || this.clientX;\n"
        "      this.pageY = init?.pageY || this.clientY;\n"
        "    }\n"
        "  };\n"
        "  globalThis.TouchEvent = class TouchEvent extends Event {\n"
        "    constructor(type, init) {\n"
        "      super(type, init);\n"
        "      this.touches = init?.touches || [];\n"
        "      this.targetTouches = init?.targetTouches || [];\n"
        "      this.changedTouches = init?.changedTouches || [];\n"
        "    }\n"
        "  };\n"
        "}\n"
        "\n"
        "/* WheelEvent class */\n"
        "if (typeof WheelEvent === 'undefined') {\n"
        "  globalThis.WheelEvent = class WheelEvent extends Event {\n"
        "    constructor(type, init) {\n"
        "      super(type, init);\n"
        "      this.deltaX = init?.deltaX || 0;\n"
        "      this.deltaY = init?.deltaY || 0;\n"
        "      this.deltaZ = init?.deltaZ || 0;\n"
        "      this.deltaMode = init?.deltaMode || 0;\n"
        "    }\n"
        "  };\n"
        "}\n"
        "\n"
        "/* EventTarget base class */\n"
        "if (typeof EventTarget === 'undefined') {\n"
        "  globalThis.EventTarget = class EventTarget {\n"
        "    constructor() { this._listeners = {}; }\n"
        "    addEventListener(type, fn) {\n"
        "      if (!this._listeners[type]) this._listeners[type] = [];\n"
        "      this._listeners[type].push(fn);\n"
        "    }\n"
        "    removeEventListener(type, fn) {\n"
        "      if (!this._listeners[type]) return;\n"
        "      this._listeners[type] = this._listeners[type].filter(f => f !== fn);\n"
        "    }\n"
        "    dispatchEvent(evt) {\n"
        "      const fns = this._listeners[evt.type] || [];\n"
        "      for (const fn of fns) fn.call(this, evt);\n"
        "      return !evt.defaultPrevented;\n"
        "    }\n"
        "  };\n"
        "}\n";

    JS_Eval(ctx, extra, strlen(extra), "<extra>", JS_EVAL_TYPE_GLOBAL);
}

/* ══════════════════════════════════════════════════════════════════
 *  Keyboard/Mouse API registration (Phase 4)
 *  Event listeners are set up in register_event_shims.
 *  Actual dispatch happens in dispatch_input_events() called from wc_render.
 * ══════════════════════════════════════════════════════════════════ */

static void register_keyboard_mouse_api(JSContext *ctx) {
    /* Nothing extra to register — listeners are set up via
     * addEventListener in register_event_shims, and events are
     * dispatched from C in dispatch_input_events(). */
}
