/*
 * webgl_shim.c — WebGL2RenderingContext shim for QuickJS
 *
 * Maps browser WebGL2 API → wasmcart GL module imports.
 * GL calls go directly to the host GPU with near-zero overhead.
 *
 * WebGL objects (WebGLBuffer, WebGLTexture, etc.) are just integer IDs.
 * The host's gl_imports.js handles ID→object mapping on the other side.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define WC_USE_GL
#include "wasmcart.h"
#include "quickjs.h"

/* Forward declaration from cart_main.c */
extern wc_info_t info;

/* ── GL constant table ───────────────────────────────────────────── */

typedef struct {
    const char *name;
    uint32_t value;
} gl_const_entry_t;

static const gl_const_entry_t gl_constants[] = {
    /* Clear bits */
    {"DEPTH_BUFFER_BIT", 0x00000100},
    {"STENCIL_BUFFER_BIT", 0x00000400},
    {"COLOR_BUFFER_BIT", 0x00004000},
    /* Boolean */
    {"FALSE", 0}, {"TRUE", 1}, {"NONE", 0},
    /* Primitives */
    {"POINTS", 0x0000}, {"LINES", 0x0001}, {"LINE_LOOP", 0x0002},
    {"LINE_STRIP", 0x0003}, {"TRIANGLES", 0x0004},
    {"TRIANGLE_STRIP", 0x0005}, {"TRIANGLE_FAN", 0x0006},
    /* Data types */
    {"BYTE", 0x1400}, {"UNSIGNED_BYTE", 0x1401},
    {"SHORT", 0x1402}, {"UNSIGNED_SHORT", 0x1403},
    {"INT", 0x1404}, {"UNSIGNED_INT", 0x1405},
    {"FLOAT", 0x1406}, {"HALF_FLOAT", 0x140B},
    /* Enable/Disable */
    {"BLEND", 0x0BE2}, {"CULL_FACE", 0x0B44},
    {"DEPTH_TEST", 0x0B71}, {"DITHER", 0x0BD0},
    {"SCISSOR_TEST", 0x0C11}, {"STENCIL_TEST", 0x0B90},
    {"POLYGON_OFFSET_FILL", 0x8037},
    /* Blend factors */
    {"ZERO", 0}, {"ONE", 1},
    {"SRC_COLOR", 0x0300}, {"ONE_MINUS_SRC_COLOR", 0x0301},
    {"SRC_ALPHA", 0x0302}, {"ONE_MINUS_SRC_ALPHA", 0x0303},
    {"DST_ALPHA", 0x0304}, {"ONE_MINUS_DST_ALPHA", 0x0305},
    {"DST_COLOR", 0x0306}, {"ONE_MINUS_DST_COLOR", 0x0307},
    /* Blend equations */
    {"FUNC_ADD", 0x8006}, {"FUNC_SUBTRACT", 0x800A},
    {"FUNC_REVERSE_SUBTRACT", 0x800B},
    /* Depth */
    {"NEVER", 0x0200}, {"LESS", 0x0201}, {"EQUAL", 0x0202},
    {"LEQUAL", 0x0203}, {"GREATER", 0x0204}, {"NOTEQUAL", 0x0205},
    {"GEQUAL", 0x0206}, {"ALWAYS", 0x0207},
    /* Face */
    {"FRONT", 0x0404}, {"BACK", 0x0405},
    {"FRONT_AND_BACK", 0x0408}, {"CW", 0x0900}, {"CCW", 0x0901},
    /* Buffers */
    {"ARRAY_BUFFER", 0x8892}, {"ELEMENT_ARRAY_BUFFER", 0x8893},
    {"STATIC_DRAW", 0x88E4}, {"DYNAMIC_DRAW", 0x88E8},
    {"STREAM_DRAW", 0x88E0},
    /* Textures */
    {"TEXTURE_2D", 0x0DE1}, {"TEXTURE_CUBE_MAP", 0x8513},
    {"TEXTURE_MIN_FILTER", 0x2801}, {"TEXTURE_MAG_FILTER", 0x2800},
    {"TEXTURE_WRAP_S", 0x2802}, {"TEXTURE_WRAP_T", 0x2803},
    {"NEAREST", 0x2600}, {"LINEAR", 0x2601},
    {"NEAREST_MIPMAP_NEAREST", 0x2700}, {"LINEAR_MIPMAP_NEAREST", 0x2701},
    {"NEAREST_MIPMAP_LINEAR", 0x2702}, {"LINEAR_MIPMAP_LINEAR", 0x2703},
    {"CLAMP_TO_EDGE", 0x812F}, {"REPEAT", 0x2901},
    {"MIRRORED_REPEAT", 0x8370},
    {"TEXTURE0", 0x84C0},
    /* Pixel formats */
    {"ALPHA", 0x1906}, {"RGB", 0x1907}, {"RGBA", 0x1908},
    {"LUMINANCE", 0x1909}, {"LUMINANCE_ALPHA", 0x190A},
    {"RED", 0x1903}, {"RG", 0x8227},
    {"R8", 0x8229}, {"RG8", 0x822B}, {"RGB8", 0x8051}, {"RGBA8", 0x8058},
    /* Shaders */
    {"VERTEX_SHADER", 0x8B31}, {"FRAGMENT_SHADER", 0x8B30},
    {"COMPILE_STATUS", 0x8B81}, {"LINK_STATUS", 0x8B82},
    {"VALIDATE_STATUS", 0x8B83}, {"INFO_LOG_LENGTH", 0x8B84},
    {"ACTIVE_UNIFORMS", 0x8B86}, {"ACTIVE_ATTRIBUTES", 0x8B89},
    {"ACTIVE_UNIFORM_MAX_LENGTH", 0x8B87},
    {"ACTIVE_ATTRIBUTE_MAX_LENGTH", 0x8B8A},
    /* Framebuffer */
    {"FRAMEBUFFER", 0x8D40}, {"RENDERBUFFER", 0x8D41},
    {"READ_FRAMEBUFFER", 0x8CA8}, {"DRAW_FRAMEBUFFER", 0x8CA9},
    {"COLOR_ATTACHMENT0", 0x8CE0},
    {"DEPTH_ATTACHMENT", 0x8D00}, {"STENCIL_ATTACHMENT", 0x8D20},
    {"DEPTH_STENCIL_ATTACHMENT", 0x821A},
    {"FRAMEBUFFER_COMPLETE", 0x8CD5},
    {"DEPTH_COMPONENT16", 0x81A5}, {"DEPTH_COMPONENT24", 0x81A6},
    {"DEPTH24_STENCIL8", 0x88F0},
    /* GetString */
    {"VENDOR", 0x1F00}, {"RENDERER", 0x1F01}, {"VERSION", 0x1F02},
    /* Misc */
    {"NO_ERROR", 0},
    {"UNPACK_FLIP_Y_WEBGL", 0x9240},
    {"UNPACK_PREMULTIPLY_ALPHA_WEBGL", 0x9241},
    {"MAX_TEXTURE_SIZE", 0x0D33},
    {"MAX_VERTEX_ATTRIBS", 0x8869},
    {"MAX_RENDERBUFFER_SIZE", 0x84E8},
    {"VIEWPORT", 0x0BA2},
    {NULL, 0}
};

/* ── Typed array data extraction ─────────────────────────────────── */

/* Get pointer + length from a JS typed array or ArrayBuffer */
static int get_typed_array_data(JSContext *ctx, JSValue val,
                                 uint8_t **out_ptr, size_t *out_len)
{
    size_t len;
    size_t boff;
    size_t blen;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &len, val);
    if (buf) {
        *out_ptr = buf;
        *out_len = len;
        return 1;
    }

    /* Try typed array */
    JSValue ab = JS_GetTypedArrayBuffer(ctx, val, &boff, &blen, NULL);
    if (!JS_IsException(ab)) {
        buf = JS_GetArrayBuffer(ctx, &len, ab);
        JS_FreeValue(ctx, ab);
        if (buf) {
            *out_ptr = buf + boff;
            *out_len = blen;
            return 1;
        }
    }
    return 0;
}

/* Bytes a GL pixel rectangle needs, or 0 if the format/type is unknown or the
 * arithmetic would overflow.
 *
 * texImage2D/texSubImage2D/readPixels each took width/height from JS and passed
 * them to GL alongside a caller buffer whose length was fetched and then
 * DISCARDED. readPixels was the dangerous one: GL WRITES w*h*bpp bytes into
 * that buffer, so `gl.readPixels(0,0,64,64,RGBA,UNSIGNED_BYTE,
 * new Uint8Array(16))` wrote 16KB into 16 bytes and took the whole runtime
 * down with "memory access out of bounds". The upload paths are the read-side
 * equivalent, leaking adjacent heap into a texture.
 *
 * Unknown enums return 0 and callers skip the check rather than reject: this
 * table covers what the shim exposes, and a wrong REJECT would break drawing.
 * GL itself still validates the call. */
static size_t gl_pixel_bytes(uint32_t format, uint32_t type,
                             int32_t width, int32_t height)
{
    if (width <= 0 || height <= 0) return 0;

    int channels;
    switch (format) {
        case 0x1906: channels = 1; break;  /* ALPHA           */
        case 0x1909: channels = 1; break;  /* LUMINANCE       */
        case 0x190A: channels = 2; break;  /* LUMINANCE_ALPHA */
        case 0x1903: channels = 1; break;  /* RED             */
        case 0x8227: channels = 2; break;  /* RG              */
        case 0x1907: channels = 3; break;  /* RGB             */
        case 0x1908: channels = 4; break;  /* RGBA            */
        default: return 0;
    }

    size_t per_pixel;
    switch (type) {
        case 0x1401: per_pixel = (size_t)channels * 1; break;  /* UNSIGNED_BYTE  */
        case 0x1400: per_pixel = (size_t)channels * 1; break;  /* BYTE           */
        case 0x1403: per_pixel = (size_t)channels * 2; break;  /* UNSIGNED_SHORT */
        case 0x1402: per_pixel = (size_t)channels * 2; break;  /* SHORT          */
        case 0x1405: per_pixel = (size_t)channels * 4; break;  /* UNSIGNED_INT   */
        case 0x1404: per_pixel = (size_t)channels * 4; break;  /* INT            */
        case 0x1406: per_pixel = (size_t)channels * 4; break;  /* FLOAT          */
        case 0x140B: per_pixel = (size_t)channels * 2; break;  /* HALF_FLOAT     */
        /* Packed formats: one unit per pixel regardless of channel count. */
        case 0x8033: per_pixel = 2; break;  /* UNSIGNED_SHORT_4_4_4_4 */
        case 0x8034: per_pixel = 2; break;  /* UNSIGNED_SHORT_5_5_5_1 */
        case 0x8363: per_pixel = 2; break;  /* UNSIGNED_SHORT_5_6_5   */
        default: return 0;
    }

    /* Overflow guard before the product is formed. */
    if ((int64_t)width * (int64_t)height > (int64_t)(SIZE_MAX / per_pixel))
        return 0;
    return (size_t)width * (size_t)height * per_pixel;
}

/* ── GL function bindings ────────────────────────────────────────── */

/* Simple 0-arg void functions */
#define GL_VOID_0(name) \
    static JSValue js_##name(JSContext *ctx, JSValueConst this_val, \
                              int argc, JSValueConst *argv) { \
        name(); return JS_UNDEFINED; }

/* 1-arg GLuint/GLenum functions */
#define GL_VOID_1I(name) \
    static JSValue js_##name(JSContext *ctx, JSValueConst this_val, \
                              int argc, JSValueConst *argv) { \
        uint32_t a; JS_ToUint32(ctx, &a, argv[0]); \
        name(a); return JS_UNDEFINED; }

/* 2-arg GLuint functions */
#define GL_VOID_2I(name) \
    static JSValue js_##name(JSContext *ctx, JSValueConst this_val, \
                              int argc, JSValueConst *argv) { \
        uint32_t a, b; JS_ToUint32(ctx, &a, argv[0]); JS_ToUint32(ctx, &b, argv[1]); \
        name(a, b); return JS_UNDEFINED; }

GL_VOID_0(glFinish)
GL_VOID_0(glFlush)
GL_VOID_1I(glEnable)
GL_VOID_1I(glDisable)
GL_VOID_1I(glCullFace)
GL_VOID_1I(glFrontFace)
GL_VOID_1I(glActiveTexture)
GL_VOID_1I(glUseProgram)
GL_VOID_1I(glGenerateMipmap)
GL_VOID_1I(glBlendEquation)
GL_VOID_1I(glDepthFunc)
GL_VOID_1I(glStencilMask)
GL_VOID_2I(glBindBuffer)
GL_VOID_2I(glBindTexture)
GL_VOID_2I(glBindFramebuffer)
GL_VOID_2I(glBindRenderbuffer)
GL_VOID_2I(glBlendFunc)
GL_VOID_2I(glBlendEquationSeparate)
GL_VOID_2I(glHint)
GL_VOID_2I(glAttachShader)
GL_VOID_2I(glDetachShader)
GL_VOID_2I(glPixelStorei)

static JSValue js_glGetError(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    return JS_NewUint32(ctx, glGetError());
}

static JSValue js_glViewport(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    int32_t x, y, w, h;
    JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]); JS_ToInt32(ctx, &h, argv[3]);
    glViewport(x, y, w, h);
    return JS_UNDEFINED;
}

static JSValue js_glScissor(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    int32_t x, y, w, h;
    JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]); JS_ToInt32(ctx, &h, argv[3]);
    glScissor(x, y, w, h);
    return JS_UNDEFINED;
}

static JSValue js_glClear(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    uint32_t mask; JS_ToUint32(ctx, &mask, argv[0]);
    glClear(mask);
    return JS_UNDEFINED;
}

static JSValue js_glClearColor(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    double r, g, b, a;
    JS_ToFloat64(ctx, &r, argv[0]); JS_ToFloat64(ctx, &g, argv[1]);
    JS_ToFloat64(ctx, &b, argv[2]); JS_ToFloat64(ctx, &a, argv[3]);
    glClearColor(r, g, b, a);
    return JS_UNDEFINED;
}

static JSValue js_glClearDepthf(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    double d; JS_ToFloat64(ctx, &d, argv[0]);
    glClearDepthf(d);
    return JS_UNDEFINED;
}

static JSValue js_glClearStencil(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    int32_t s; JS_ToInt32(ctx, &s, argv[0]);
    glClearStencil(s);
    return JS_UNDEFINED;
}

static JSValue js_glBlendFuncSeparate(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    uint32_t a, b, c, d;
    JS_ToUint32(ctx, &a, argv[0]); JS_ToUint32(ctx, &b, argv[1]);
    JS_ToUint32(ctx, &c, argv[2]); JS_ToUint32(ctx, &d, argv[3]);
    glBlendFuncSeparate(a, b, c, d);
    return JS_UNDEFINED;
}

static JSValue js_glBlendColor(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    double r, g, b, a;
    JS_ToFloat64(ctx, &r, argv[0]); JS_ToFloat64(ctx, &g, argv[1]);
    JS_ToFloat64(ctx, &b, argv[2]); JS_ToFloat64(ctx, &a, argv[3]);
    glBlendColor(r, g, b, a);
    return JS_UNDEFINED;
}

static JSValue js_glColorMask(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    glColorMask(JS_ToBool(ctx, argv[0]), JS_ToBool(ctx, argv[1]),
                JS_ToBool(ctx, argv[2]), JS_ToBool(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue js_glDepthMask(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    glDepthMask(JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_glDepthRangef(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    double n, f;
    JS_ToFloat64(ctx, &n, argv[0]); JS_ToFloat64(ctx, &f, argv[1]);
    glDepthRangef(n, f);
    return JS_UNDEFINED;
}

static JSValue js_glStencilFunc(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    uint32_t func, ref, mask;
    JS_ToUint32(ctx, &func, argv[0]); JS_ToUint32(ctx, &ref, argv[1]);
    JS_ToUint32(ctx, &mask, argv[2]);
    glStencilFunc(func, ref, mask);
    return JS_UNDEFINED;
}

static JSValue js_glStencilOp(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    uint32_t a, b, c;
    JS_ToUint32(ctx, &a, argv[0]); JS_ToUint32(ctx, &b, argv[1]);
    JS_ToUint32(ctx, &c, argv[2]);
    glStencilOp(a, b, c);
    return JS_UNDEFINED;
}

static JSValue js_glPolygonOffset(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    double f, u;
    JS_ToFloat64(ctx, &f, argv[0]); JS_ToFloat64(ctx, &u, argv[1]);
    glPolygonOffset(f, u);
    return JS_UNDEFINED;
}

static JSValue js_glLineWidth(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    double w; JS_ToFloat64(ctx, &w, argv[0]);
    glLineWidth(w);
    return JS_UNDEFINED;
}

/* ── Buffer operations ───────────────────────────────────────────── */

static JSValue js_glCreateBuffer(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    GLuint buf;
    glGenBuffers(1, &buf);
    return JS_NewUint32(ctx, buf);
}

static JSValue js_glDeleteBuffer(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    uint32_t buf; JS_ToUint32(ctx, &buf, argv[0]);
    glDeleteBuffers(1, &buf);
    return JS_UNDEFINED;
}

static JSValue js_glBufferData(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    uint32_t target, usage;
    JS_ToUint32(ctx, &target, argv[0]);
    JS_ToUint32(ctx, &usage, argv[2]);

    uint8_t *data; size_t len;
    if (get_typed_array_data(ctx, argv[1], &data, &len)) {
        glBufferData(target, len, data, usage);
    } else {
        /* size-only variant */
        int32_t size; JS_ToInt32(ctx, &size, argv[1]);
        glBufferData(target, size, NULL, usage);
    }
    return JS_UNDEFINED;
}

static JSValue js_glBufferSubData(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    uint32_t target; int32_t offset;
    JS_ToUint32(ctx, &target, argv[0]);
    JS_ToInt32(ctx, &offset, argv[1]);
    uint8_t *data; size_t len;
    if (get_typed_array_data(ctx, argv[2], &data, &len)) {
        glBufferSubData(target, offset, len, data);
    }
    return JS_UNDEFINED;
}

/* ── Texture operations ──────────────────────────────────────────── */

static JSValue js_glCreateTexture(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    GLuint tex;
    glGenTextures(1, &tex);
    return JS_NewUint32(ctx, tex);
}

static JSValue js_glDeleteTexture(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    uint32_t tex; JS_ToUint32(ctx, &tex, argv[0]);
    glDeleteTextures(1, &tex);
    return JS_UNDEFINED;
}

static JSValue js_glTexParameteri(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    uint32_t target, pname; int32_t param;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToUint32(ctx, &pname, argv[1]);
    JS_ToInt32(ctx, &param, argv[2]);
    glTexParameteri(target, pname, param);
    return JS_UNDEFINED;
}

static JSValue js_glTexImage2D(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    uint32_t target, level, internalformat, width, height, border, format, type;
    JS_ToUint32(ctx, &target, argv[0]);
    JS_ToUint32(ctx, &level, argv[1]);
    JS_ToUint32(ctx, &internalformat, argv[2]);
    JS_ToUint32(ctx, &width, argv[3]);
    JS_ToUint32(ctx, &height, argv[4]);
    JS_ToUint32(ctx, &border, argv[5]);
    JS_ToUint32(ctx, &format, argv[6]);
    JS_ToUint32(ctx, &type, argv[7]);

    const void *pixels = NULL;
    uint8_t *data; size_t len;
    if (argc > 8 && !JS_IsNull(argv[8]) && !JS_IsUndefined(argv[8])) {
        if (get_typed_array_data(ctx, argv[8], &data, &len)) {
            size_t need = gl_pixel_bytes(format, type, (int32_t)width, (int32_t)height);
            if (need && need > len)
                return JS_ThrowRangeError(ctx,
                    "texImage2D: data is %u bytes but %ux%u needs %u",
                    (unsigned)len, width, height, (unsigned)need);
            pixels = data;
        }
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    return JS_UNDEFINED;
}

static JSValue js_glTexSubImage2D(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    uint32_t target, level, xoff, yoff, width, height, format, type;
    JS_ToUint32(ctx, &target, argv[0]); JS_ToUint32(ctx, &level, argv[1]);
    JS_ToUint32(ctx, &xoff, argv[2]); JS_ToUint32(ctx, &yoff, argv[3]);
    JS_ToUint32(ctx, &width, argv[4]); JS_ToUint32(ctx, &height, argv[5]);
    JS_ToUint32(ctx, &format, argv[6]); JS_ToUint32(ctx, &type, argv[7]);

    uint8_t *data; size_t len;
    const void *pixels = NULL;
    if (argc > 8 && get_typed_array_data(ctx, argv[8], &data, &len)) {
        size_t need = gl_pixel_bytes(format, type, (int32_t)width, (int32_t)height);
        if (need && need > len)
            return JS_ThrowRangeError(ctx,
                "texSubImage2D: data is %u bytes but %ux%u needs %u",
                (unsigned)len, width, height, (unsigned)need);
        pixels = data;
    }
    glTexSubImage2D(target, level, xoff, yoff, width, height, format, type, pixels);
    return JS_UNDEFINED;
}

/* ── Shader operations ───────────────────────────────────────────── */

static JSValue js_glCreateShader(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    uint32_t type; JS_ToUint32(ctx, &type, argv[0]);
    return JS_NewUint32(ctx, glCreateShader(type));
}

static JSValue js_glDeleteShader(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    uint32_t s; JS_ToUint32(ctx, &s, argv[0]);
    glDeleteShader(s);
    return JS_UNDEFINED;
}

static JSValue js_glShaderSource(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    uint32_t shader; JS_ToUint32(ctx, &shader, argv[0]);
    const char *src = JS_ToCString(ctx, argv[1]);
    if (src) {
        GLint len = strlen(src);
        glShaderSource(shader, 1, &src, &len);
        JS_FreeCString(ctx, src);
    }
    return JS_UNDEFINED;
}

static JSValue js_glCompileShader(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    uint32_t s; JS_ToUint32(ctx, &s, argv[0]);
    glCompileShader(s);
    return JS_UNDEFINED;
}

static JSValue js_glGetShaderParameter(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    uint32_t shader, pname;
    JS_ToUint32(ctx, &shader, argv[0]); JS_ToUint32(ctx, &pname, argv[1]);
    GLint val;
    glGetShaderiv(shader, pname, &val);
    if (pname == GL_COMPILE_STATUS)
        return val ? JS_TRUE : JS_FALSE;
    return JS_NewInt32(ctx, val);
}

static JSValue js_glGetShaderInfoLog(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    uint32_t shader; JS_ToUint32(ctx, &shader, argv[0]);
    GLint len;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
    if (len <= 0) return JS_NewString(ctx, "");
    char *buf = malloc(len + 1);
    glGetShaderInfoLog(shader, len, NULL, buf);
    buf[len] = '\0';
    JSValue ret = JS_NewString(ctx, buf);
    free(buf);
    return ret;
}

/* ── Program operations ──────────────────────────────────────────── */

static JSValue js_glCreateProgram(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    return JS_NewUint32(ctx, glCreateProgram());
}

static JSValue js_glDeleteProgram(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    uint32_t p; JS_ToUint32(ctx, &p, argv[0]);
    glDeleteProgram(p);
    return JS_UNDEFINED;
}

static JSValue js_glLinkProgram(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    uint32_t p; JS_ToUint32(ctx, &p, argv[0]);
    glLinkProgram(p);
    return JS_UNDEFINED;
}

static JSValue js_glGetProgramParameter(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    uint32_t prog, pname;
    JS_ToUint32(ctx, &prog, argv[0]); JS_ToUint32(ctx, &pname, argv[1]);
    GLint val;
    glGetProgramiv(prog, pname, &val);
    if (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS)
        return val ? JS_TRUE : JS_FALSE;
    return JS_NewInt32(ctx, val);
}

static JSValue js_glGetProgramInfoLog(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    uint32_t prog; JS_ToUint32(ctx, &prog, argv[0]);
    GLint len;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
    if (len <= 0) return JS_NewString(ctx, "");
    char *buf = malloc(len + 1);
    glGetProgramInfoLog(prog, len, NULL, buf);
    buf[len] = '\0';
    JSValue ret = JS_NewString(ctx, buf);
    free(buf);
    return ret;
}

static JSValue js_glGetAttribLocation(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    uint32_t prog; JS_ToUint32(ctx, &prog, argv[0]);
    const char *name = JS_ToCString(ctx, argv[1]);
    GLint loc = glGetAttribLocation(prog, name);
    JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, loc);
}

static JSValue js_glGetUniformLocation(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    uint32_t prog; JS_ToUint32(ctx, &prog, argv[0]);
    const char *name = JS_ToCString(ctx, argv[1]);
    GLint loc = glGetUniformLocation(prog, name);
    JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, loc);
}

/* ── UBO / WebGL2 buffer binding ──────────────────────────────────── */

static JSValue js_glBindBufferBase(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint32_t target, index, buffer;
    JS_ToUint32(ctx, &target, a[0]); JS_ToUint32(ctx, &index, a[1]); JS_ToUint32(ctx, &buffer, a[2]);
    glBindBufferBase(target, index, buffer);
    return JS_UNDEFINED;
}
static JSValue js_glBindBufferRange(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint32_t target, index, buffer; int32_t offset, size;
    JS_ToUint32(ctx, &target, a[0]); JS_ToUint32(ctx, &index, a[1]); JS_ToUint32(ctx, &buffer, a[2]);
    JS_ToInt32(ctx, &offset, a[3]); JS_ToInt32(ctx, &size, a[4]);
    glBindBufferRange(target, index, buffer, offset, size);
    return JS_UNDEFINED;
}
static JSValue js_glGetUniformBlockIndex(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint32_t prog; JS_ToUint32(ctx, &prog, a[0]);
    const char *name = JS_ToCString(ctx, a[1]);
    GLuint idx = glGetUniformBlockIndex(prog, name);
    JS_FreeCString(ctx, name);
    return JS_NewUint32(ctx, idx);
}
static JSValue js_glUniformBlockBinding(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint32_t prog, blockIndex, blockBinding;
    JS_ToUint32(ctx, &prog, a[0]); JS_ToUint32(ctx, &blockIndex, a[1]); JS_ToUint32(ctx, &blockBinding, a[2]);
    glUniformBlockBinding(prog, blockIndex, blockBinding);
    return JS_UNDEFINED;
}
static JSValue js_glTexStorage2D(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint32_t target, levels, internalformat, width, height;
    JS_ToUint32(ctx, &target, a[0]); JS_ToUint32(ctx, &levels, a[1]); JS_ToUint32(ctx, &internalformat, a[2]);
    JS_ToUint32(ctx, &width, a[3]); JS_ToUint32(ctx, &height, a[4]);
    glTexStorage2D(target, levels, internalformat, width, height);
    return JS_UNDEFINED;
}

/* ── Active attrib/uniform introspection ──────────────────────────── */

static JSValue js_glGetActiveAttrib(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    uint32_t prog, index;
    JS_ToUint32(ctx, &prog, argv[0]); JS_ToUint32(ctx, &index, argv[1]);
    char name[256]; GLsizei length; GLint size; GLenum type;
    glGetActiveAttrib(prog, index, sizeof(name), &length, &size, &type, name);
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "name", JS_NewStringLen(ctx, name, length));
    JS_SetPropertyStr(ctx, result, "size", JS_NewInt32(ctx, size));
    JS_SetPropertyStr(ctx, result, "type", JS_NewUint32(ctx, type));
    return result;
}

static JSValue js_glGetActiveUniform(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    uint32_t prog, index;
    JS_ToUint32(ctx, &prog, argv[0]); JS_ToUint32(ctx, &index, argv[1]);
    char name[256]; GLsizei length; GLint size; GLenum type;
    glGetActiveUniform(prog, index, sizeof(name), &length, &size, &type, name);
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "name", JS_NewStringLen(ctx, name, length));
    JS_SetPropertyStr(ctx, result, "size", JS_NewInt32(ctx, size));
    JS_SetPropertyStr(ctx, result, "type", JS_NewUint32(ctx, type));
    return result;
}

/* ── Uniform setters ─────────────────────────────────────────────── */

static JSValue js_glUniform1i(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
    int32_t loc, v; JS_ToInt32(c, &loc, a[0]); JS_ToInt32(c, &v, a[1]);
    glUniform1i(loc, v); return JS_UNDEFINED;
}
static JSValue js_glUniform1f(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
    int32_t loc; double v; JS_ToInt32(c, &loc, a[0]); JS_ToFloat64(c, &v, a[1]);
    glUniform1f(loc, v); return JS_UNDEFINED;
}
static JSValue js_glUniform2f(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
    int32_t loc; double v0, v1; JS_ToInt32(c, &loc, a[0]);
    JS_ToFloat64(c, &v0, a[1]); JS_ToFloat64(c, &v1, a[2]);
    glUniform2f(loc, v0, v1); return JS_UNDEFINED;
}
static JSValue js_glUniform3f(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
    int32_t loc; double v0, v1, v2; JS_ToInt32(c, &loc, a[0]);
    JS_ToFloat64(c, &v0, a[1]); JS_ToFloat64(c, &v1, a[2]); JS_ToFloat64(c, &v2, a[3]);
    glUniform3f(loc, v0, v1, v2); return JS_UNDEFINED;
}
static JSValue js_glUniform4f(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
    int32_t loc; double v0, v1, v2, v3; JS_ToInt32(c, &loc, a[0]);
    JS_ToFloat64(c, &v0, a[1]); JS_ToFloat64(c, &v1, a[2]);
    JS_ToFloat64(c, &v2, a[3]); JS_ToFloat64(c, &v3, a[4]);
    glUniform4f(loc, v0, v1, v2, v3); return JS_UNDEFINED;
}

static JSValue js_glUniformMatrix4fv(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    int32_t loc; JS_ToInt32(ctx, &loc, argv[0]);
    int transpose = JS_ToBool(ctx, argv[1]);
    uint8_t *data; size_t len;
    if (get_typed_array_data(ctx, argv[2], &data, &len)) {
        /* `if (count < 1) count = 1` forced GL to read one full matrix even
         * when the buffer held less, over-reading up to 60 bytes. Skip the
         * call instead: a buffer too short for a single matrix is a caller
         * bug, and uploading a matrix built from adjacent heap is worse than
         * uploading nothing. */
        int count = len / (16 * sizeof(float));
        if (count >= 1)
            glUniformMatrix4fv(loc, count, transpose, (const float *)data);
    }
    return JS_UNDEFINED;
}

static JSValue js_glUniformMatrix3fv(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    int32_t loc; JS_ToInt32(ctx, &loc, argv[0]);
    int transpose = JS_ToBool(ctx, argv[1]);
    uint8_t *data; size_t len;
    if (get_typed_array_data(ctx, argv[2], &data, &len)) {
        /* `if (count < 1) count = 1` forced GL to read one full matrix even
         * when the buffer held less, over-reading up to 32 bytes. Skip the
         * call instead: a buffer too short for a single matrix is a caller
         * bug, and uploading a matrix built from adjacent heap is worse than
         * uploading nothing. */
        int count = len / (9 * sizeof(float));
        if (count >= 1)
            glUniformMatrix3fv(loc, count, transpose, (const float *)data);
    }
    return JS_UNDEFINED;
}

static JSValue js_glUniformMatrix2fv(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    int32_t loc; JS_ToInt32(ctx, &loc, argv[0]);
    int transpose = JS_ToBool(ctx, argv[1]);
    uint8_t *data; size_t len;
    if (get_typed_array_data(ctx, argv[2], &data, &len)) {
        /* `if (count < 1) count = 1` forced GL to read one full matrix even
         * when the buffer held less, over-reading up to 12 bytes. Skip the
         * call instead: a buffer too short for a single matrix is a caller
         * bug, and uploading a matrix built from adjacent heap is worse than
         * uploading nothing. */
        int count = len / (4 * sizeof(float));
        if (count >= 1)
            glUniformMatrix2fv(loc, count, transpose, (const float *)data);
    }
    return JS_UNDEFINED;
}

/* ── Vertex attribs ──────────────────────────────────────────────── */

static JSValue js_glEnableVertexAttribArray(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
    uint32_t idx; JS_ToUint32(c, &idx, a[0]);
    glEnableVertexAttribArray(idx); return JS_UNDEFINED;
}

static JSValue js_glDisableVertexAttribArray(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
    uint32_t idx; JS_ToUint32(c, &idx, a[0]);
    glDisableVertexAttribArray(idx); return JS_UNDEFINED;
}

static JSValue js_glVertexAttribPointer(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    uint32_t index, size, type, stride;
    int32_t offset;
    JS_ToUint32(ctx, &index, argv[0]); JS_ToUint32(ctx, &size, argv[1]);
    JS_ToUint32(ctx, &type, argv[2]);
    int normalized = JS_ToBool(ctx, argv[3]);
    JS_ToUint32(ctx, &stride, argv[4]); JS_ToInt32(ctx, &offset, argv[5]);
    glVertexAttribPointer(index, size, type, normalized, stride, (const void *)(uintptr_t)offset);
    return JS_UNDEFINED;
}

/* ── Drawing ─────────────────────────────────────────────────────── */

static JSValue js_glDrawArrays(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    uint32_t mode; int32_t first, count;
    JS_ToUint32(ctx, &mode, argv[0]); JS_ToInt32(ctx, &first, argv[1]);
    JS_ToInt32(ctx, &count, argv[2]);
    glDrawArrays(mode, first, count);
    return JS_UNDEFINED;
}

static JSValue js_glDrawElements(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    uint32_t mode, type; int32_t count, offset;
    JS_ToUint32(ctx, &mode, argv[0]); JS_ToInt32(ctx, &count, argv[1]);
    JS_ToUint32(ctx, &type, argv[2]); JS_ToInt32(ctx, &offset, argv[3]);
    glDrawElements(mode, count, type, (const void *)(uintptr_t)offset);
    return JS_UNDEFINED;
}

/* ── VAO ─────────────────────────────────────────────────────────── */

static JSValue js_glCreateVertexArray(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    GLuint vao; glGenVertexArrays(1, &vao);
    return JS_NewUint32(ctx, vao);
}

static JSValue js_glDeleteVertexArray(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    uint32_t vao; JS_ToUint32(ctx, &vao, argv[0]);
    glDeleteVertexArrays(1, &vao);
    return JS_UNDEFINED;
}

static JSValue js_glBindVertexArray(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    uint32_t vao; JS_ToUint32(ctx, &vao, argv[0]);
    glBindVertexArray(vao);
    return JS_UNDEFINED;
}

/* ── FBO/RBO ─────────────────────────────────────────────────────── */

static JSValue js_glCreateFramebuffer(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    GLuint fbo; glGenFramebuffers(1, &fbo); return JS_NewUint32(ctx, fbo);
}
static JSValue js_glDeleteFramebuffer(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint32_t fbo; JS_ToUint32(ctx, &fbo, a[0]); glDeleteFramebuffers(1, &fbo); return JS_UNDEFINED;
}
static JSValue js_glCheckFramebufferStatus(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint32_t target; JS_ToUint32(ctx, &target, a[0]);
    return JS_NewUint32(ctx, glCheckFramebufferStatus(target));
}
static JSValue js_glFramebufferTexture2D(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint32_t target, attachment, textarget, tex, level;
    JS_ToUint32(ctx, &target, a[0]); JS_ToUint32(ctx, &attachment, a[1]);
    JS_ToUint32(ctx, &textarget, a[2]); JS_ToUint32(ctx, &tex, a[3]);
    JS_ToUint32(ctx, &level, a[4]);
    glFramebufferTexture2D(target, attachment, textarget, tex, level);
    return JS_UNDEFINED;
}
static JSValue js_glFramebufferRenderbuffer(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint32_t target, attachment, rbtarget, rb;
    JS_ToUint32(ctx, &target, a[0]); JS_ToUint32(ctx, &attachment, a[1]);
    JS_ToUint32(ctx, &rbtarget, a[2]); JS_ToUint32(ctx, &rb, a[3]);
    glFramebufferRenderbuffer(target, attachment, rbtarget, rb);
    return JS_UNDEFINED;
}
static JSValue js_glCreateRenderbuffer(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    GLuint rbo; glGenRenderbuffers(1, &rbo); return JS_NewUint32(ctx, rbo);
}
static JSValue js_glDeleteRenderbuffer(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint32_t rbo; JS_ToUint32(ctx, &rbo, a[0]); glDeleteRenderbuffers(1, &rbo); return JS_UNDEFINED;
}
static JSValue js_glRenderbufferStorage(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint32_t target, fmt, w, h;
    JS_ToUint32(ctx, &target, a[0]); JS_ToUint32(ctx, &fmt, a[1]);
    JS_ToUint32(ctx, &w, a[2]); JS_ToUint32(ctx, &h, a[3]);
    glRenderbufferStorage(target, fmt, w, h);
    return JS_UNDEFINED;
}

static JSValue js_glReadPixels(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    int32_t x, y, w, h; uint32_t fmt, type;
    JS_ToInt32(ctx, &x, a[0]); JS_ToInt32(ctx, &y, a[1]);
    JS_ToInt32(ctx, &w, a[2]); JS_ToInt32(ctx, &h, a[3]);
    JS_ToUint32(ctx, &fmt, a[4]); JS_ToUint32(ctx, &type, a[5]);
    uint8_t *data; size_t len;
    if (n > 6 && get_typed_array_data(ctx, a[6], &data, &len)) {
        /* GL WRITES here. A short buffer is a heap overflow, not a leak:
         * 64x64 RGBA into a 16-byte Uint8Array took the runtime down. */
        size_t need = gl_pixel_bytes(fmt, type, w, h);
        if (need && need > len)
            return JS_ThrowRangeError(ctx,
                "readPixels: data is %u bytes but %dx%d needs %u",
                (unsigned)len, w, h, (unsigned)need);
        glReadPixels(x, y, w, h, fmt, type, data);
    }
    return JS_UNDEFINED;
}

static JSValue js_glGetParameter(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint32_t pname; JS_ToUint32(ctx, &pname, a[0]);

    /* String parameters */
    if (pname == 0x1F00 || pname == 0x1F01 || pname == 0x1F02 || pname == 0x8B8C) {
        /* VENDOR, RENDERER, VERSION, SHADING_LANGUAGE_VERSION */
        const unsigned char *str = glGetString(pname);
        if (str) return JS_NewString(ctx, (const char *)str);
        /* Fallback strings if glGetString returns NULL */
        if (pname == 0x1F02) return JS_NewString(ctx, "OpenGL ES 3.0");
        if (pname == 0x8B8C) return JS_NewString(ctx, "OpenGL ES GLSL ES 3.00");
        if (pname == 0x1F01) return JS_NewString(ctx, "wasmcart");
        return JS_NewString(ctx, "wasmcart");
    }

    /* Array parameters — return Int32Array */
    if (pname == 0x0BA2 || pname == 0x0C10) {
        /* GL_VIEWPORT (0x0BA2) or GL_SCISSOR_BOX (0x0C10) → [x,y,w,h] */
        GLint vals[4] = {0};
        glGetIntegerv(pname, vals);
        JSValue arr = JS_NewArray(ctx);
        for (int i = 0; i < 4; i++)
            JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, vals[i]));
        return arr;
    }

    GLint val;
    glGetIntegerv(pname, &val);
    return JS_NewInt32(ctx, val);
}

static JSValue js_glDrawBuffers(JSContext *ctx, JSValueConst t, int n, JSValueConst *a) {
    uint8_t *data; size_t len;
    if (get_typed_array_data(ctx, a[0], &data, &len)) {
        glDrawBuffers(len / sizeof(uint32_t), (const GLenum *)data);
    } else {
        /* Single value */
        uint32_t buf; JS_ToUint32(ctx, &buf, a[0]);
        glDrawBuffers(1, &buf);
    }
    return JS_UNDEFINED;
}

/* ── Registration ────────────────────────────────────────────────── */

void register_webgl_api(JSContext *ctx) {
    /* Build the WebGL2RenderingContext-like object via JS, then attach
     * native methods. This keeps the JS-side API shape clean. */

    /* First, register all native GL functions as a _gl object */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue gl_obj = JS_NewObject(ctx);

    /* Add all GL constants */
    for (int i = 0; gl_constants[i].name; i++) {
        JS_SetPropertyStr(ctx, gl_obj, gl_constants[i].name,
            JS_NewUint32(ctx, gl_constants[i].value));
    }

    /* Add GL methods */
    #define REG(jsname, cfunc, nargs) \
        JS_SetPropertyStr(ctx, gl_obj, jsname, \
            JS_NewCFunction(ctx, cfunc, jsname, nargs))

    /* State */
    REG("enable", js_glEnable, 1);
    REG("disable", js_glDisable, 1);
    REG("getError", js_glGetError, 0);
    REG("finish", js_glFinish, 0);
    REG("flush", js_glFlush, 0);
    REG("hint", js_glHint, 2);
    REG("pixelStorei", js_glPixelStorei, 2);
    REG("getParameter", js_glGetParameter, 1);

    /* Viewport / Clear */
    REG("viewport", js_glViewport, 4);
    REG("scissor", js_glScissor, 4);
    REG("clear", js_glClear, 1);
    REG("clearColor", js_glClearColor, 4);
    REG("clearDepth", js_glClearDepthf, 1);
    REG("clearStencil", js_glClearStencil, 1);

    /* Blending */
    REG("blendFunc", js_glBlendFunc, 2);
    REG("blendFuncSeparate", js_glBlendFuncSeparate, 4);
    REG("blendEquation", js_glBlendEquation, 1);
    REG("blendEquationSeparate", js_glBlendEquationSeparate, 2);
    REG("blendColor", js_glBlendColor, 4);
    REG("colorMask", js_glColorMask, 4);

    /* Depth / Stencil */
    REG("depthFunc", js_glDepthFunc, 1);
    REG("depthMask", js_glDepthMask, 1);
    REG("depthRange", js_glDepthRangef, 2);
    REG("stencilFunc", js_glStencilFunc, 3);
    REG("stencilOp", js_glStencilOp, 3);
    REG("stencilMask", js_glStencilMask, 1);

    /* Face culling */
    REG("cullFace", js_glCullFace, 1);
    REG("frontFace", js_glFrontFace, 1);
    REG("polygonOffset", js_glPolygonOffset, 2);
    REG("lineWidth", js_glLineWidth, 1);

    /* Buffers */
    REG("createBuffer", js_glCreateBuffer, 0);
    REG("deleteBuffer", js_glDeleteBuffer, 1);
    REG("bindBuffer", js_glBindBuffer, 2);
    REG("bufferData", js_glBufferData, 3);
    REG("bufferSubData", js_glBufferSubData, 3);

    /* Textures */
    REG("createTexture", js_glCreateTexture, 0);
    REG("deleteTexture", js_glDeleteTexture, 1);
    REG("bindTexture", js_glBindTexture, 2);
    REG("activeTexture", js_glActiveTexture, 1);
    REG("texImage2D", js_glTexImage2D, 9);
    REG("texSubImage2D", js_glTexSubImage2D, 9);
    REG("texParameteri", js_glTexParameteri, 3);
    REG("generateMipmap", js_glGenerateMipmap, 1);

    /* Shaders */
    REG("createShader", js_glCreateShader, 1);
    REG("deleteShader", js_glDeleteShader, 1);
    REG("shaderSource", js_glShaderSource, 2);
    REG("compileShader", js_glCompileShader, 1);
    REG("getShaderParameter", js_glGetShaderParameter, 2);
    REG("getShaderInfoLog", js_glGetShaderInfoLog, 1);

    /* Programs */
    REG("createProgram", js_glCreateProgram, 0);
    REG("deleteProgram", js_glDeleteProgram, 1);
    REG("attachShader", js_glAttachShader, 2);
    REG("detachShader", js_glDetachShader, 2);
    REG("linkProgram", js_glLinkProgram, 1);
    REG("useProgram", js_glUseProgram, 1);
    REG("getProgramParameter", js_glGetProgramParameter, 2);
    REG("getProgramInfoLog", js_glGetProgramInfoLog, 1);
    REG("getAttribLocation", js_glGetAttribLocation, 2);
    REG("getUniformLocation", js_glGetUniformLocation, 2);
    REG("getActiveAttrib", js_glGetActiveAttrib, 2);
    REG("getActiveUniform", js_glGetActiveUniform, 2);

    /* Uniforms */
    REG("uniform1i", js_glUniform1i, 2);
    REG("uniform1f", js_glUniform1f, 2);
    REG("uniform2f", js_glUniform2f, 3);
    REG("uniform3f", js_glUniform3f, 4);
    REG("uniform4f", js_glUniform4f, 5);
    REG("uniformMatrix2fv", js_glUniformMatrix2fv, 3);
    REG("uniformMatrix3fv", js_glUniformMatrix3fv, 3);
    REG("uniformMatrix4fv", js_glUniformMatrix4fv, 3);

    /* Vertex attribs */
    REG("enableVertexAttribArray", js_glEnableVertexAttribArray, 1);
    REG("disableVertexAttribArray", js_glDisableVertexAttribArray, 1);
    REG("vertexAttribPointer", js_glVertexAttribPointer, 6);

    /* Drawing */
    REG("drawArrays", js_glDrawArrays, 3);
    REG("drawElements", js_glDrawElements, 4);

    /* VAO */
    REG("createVertexArray", js_glCreateVertexArray, 0);
    REG("deleteVertexArray", js_glDeleteVertexArray, 1);
    REG("bindVertexArray", js_glBindVertexArray, 1);

    /* FBO / RBO */
    REG("createFramebuffer", js_glCreateFramebuffer, 0);
    REG("deleteFramebuffer", js_glDeleteFramebuffer, 1);
    REG("bindFramebuffer", js_glBindFramebuffer, 2);
    REG("checkFramebufferStatus", js_glCheckFramebufferStatus, 1);
    REG("framebufferTexture2D", js_glFramebufferTexture2D, 5);
    REG("framebufferRenderbuffer", js_glFramebufferRenderbuffer, 4);
    REG("createRenderbuffer", js_glCreateRenderbuffer, 0);
    REG("deleteRenderbuffer", js_glDeleteRenderbuffer, 1);
    REG("bindRenderbuffer", js_glBindRenderbuffer, 2);
    REG("renderbufferStorage", js_glRenderbufferStorage, 4);
    REG("readPixels", js_glReadPixels, 7);
    REG("drawBuffers", js_glDrawBuffers, 1);

    /* UBO / WebGL2 buffer binding */
    REG("bindBufferBase", js_glBindBufferBase, 3);
    REG("bindBufferRange", js_glBindBufferRange, 5);
    REG("getUniformBlockIndex", js_glGetUniformBlockIndex, 2);
    REG("uniformBlockBinding", js_glUniformBlockBinding, 3);
    REG("texStorage2D", js_glTexStorage2D, 5);

    #undef REG

    /* Store as _gl for getContext('webgl2') to return */
    /* Wrap _wcGL in a Proxy that auto-stubs missing methods/constants.
     * Three.js accesses many WebGL2 properties we don't explicitly define.
     * Missing constants → return 0, missing methods → return no-op function. */
    /* Store as _wcGL — the GL proxy shim wraps it later in wc_init
     * after all JS_Eval setup is complete (QuickJS Proxy can't see
     * C-registered properties via Reflect.get during register phase) */
    JS_SetPropertyStr(ctx, global, "_wcGL", gl_obj);

    /* Add WebGL capability query methods that three.js needs */
    const char *gl_stubs =
        "Object.assign(_wcGL, {\n"
        "  getSupportedExtensions() { return []; },\n"
        "  getExtension(name) {\n"
        "    /* three.js checks these - return minimal stubs */\n"
        "    if (name === 'EXT_color_buffer_float') return {};\n"
        "    if (name === 'OES_texture_float_linear') return {};\n"
        "    if (name === 'EXT_color_buffer_half_float') return {};\n"
        "    if (name === 'WEBGL_compressed_texture_s3tc') return null;\n"
        "    if (name === 'WEBGL_compressed_texture_pvrtc') return null;\n"
        "    if (name === 'WEBGL_compressed_texture_etc1') return null;\n"
        "    if (name === 'WEBGL_compressed_texture_astc') return null;\n"
        "    if (name === 'EXT_texture_filter_anisotropic') return { TEXTURE_MAX_ANISOTROPY_EXT: 0x84FE, MAX_TEXTURE_MAX_ANISOTROPY_EXT: 0x84FF };\n"
        "    if (name === 'WEBGL_lose_context') return { loseContext(){}, restoreContext(){} };\n"
        "    return null;\n"
        "  },\n"
        "  getShaderPrecisionFormat(shaderType, precisionType) {\n"
        "    return { rangeMin: 127, rangeMax: 127, precision: 23 };\n"
        "  },\n"
        "  isContextLost() { return false; },\n"
        "  getContextAttributes() {\n"
        "    return { alpha: false, antialias: false, depth: true, failIfMajorPerformanceCaveat: false,\n"
        "             powerPreference: 'default', premultipliedAlpha: true, preserveDrawingBuffer: false, stencil: false, xrCompatible: false };\n"
        "  },\n"
        "  /* getActiveAttrib/getActiveUniform are native — see _wcGL object */\n"
        "  getBufferParameter(target, pname) { return 0; },\n"
        "  getTexParameter(target, pname) { return 0; },\n"
        "  bindAttribLocation(program, index, name) {},\n"
        "  validateProgram(program) {},\n"
        "  isEnabled(cap) { return false; },\n"
        "  getString(name) { return ''; },\n"
        "  createQuery() { return {}; },\n"
        "  deleteQuery(q) {},\n"
        "  beginQuery(target, query) {},\n"
        "  endQuery(target) {},\n"
        "  getQueryParameter(query, pname) { return 0; },\n"
        "  createSampler() { return {}; },\n"
        "  deleteSampler(s) {},\n"
        "  bindSampler(unit, sampler) {},\n"
        "  samplerParameteri(sampler, pname, param) {},\n"
        "  samplerParameterf(sampler, pname, param) {},\n"
        "  fenceSync(condition, flags) { return {}; },\n"
        "  deleteSync(sync) {},\n"
        "  clientWaitSync(sync, flags, timeout) { return 0x911D; },\n"
        "  /* uniform*v functions — delegate to scalar uniforms via typed array reads */\n"
        "  uniform1iv(loc, data) { if(data&&data.length>=1) this.uniform1i(loc, data[0]); },\n"
        "  uniform2iv(loc, data) { if(data&&data.length>=2) this.uniform2i(loc, data[0],data[1]); },\n"
        "  uniform3iv(loc, data) { if(data&&data.length>=3) this.uniform3i(loc, data[0],data[1],data[2]); },\n"
        "  uniform4iv(loc, data) { if(data&&data.length>=4) this.uniform4i(loc, data[0],data[1],data[2],data[3]); },\n"
        "  uniform1fv(loc, data) { if(data&&data.length>=1) this.uniform1f(loc, data[0]); },\n"
        "  uniform2fv(loc, data) { if(data&&data.length>=2) this.uniform2f(loc, data[0],data[1]); },\n"
        "  uniform3fv(loc, data) { if(data&&data.length>=3) this.uniform3f(loc, data[0],data[1],data[2]); },\n"
        "  uniform4fv(loc, data) { if(data&&data.length>=4) this.uniform4f(loc, data[0],data[1],data[2],data[3]); },\n"
        "  vertexAttribDivisor(index, divisor) {},\n"
        "  drawArraysInstanced(mode, first, count, instanceCount) {},\n"
        "  drawElementsInstanced(mode, count, type, offset, instanceCount) {},\n"
        "  drawRangeElements(mode, start, end, count, type, offset) {\n"
        "    this.drawElements(mode, count, type, offset);\n"
        "  },\n"
        "  /* texStorage2D is now a native C function */\n"
        "  texStorage3D(target, levels, internalformat, width, height, depth) {},\n"
        "  texImage3D() {},\n"
        "  compressedTexImage2D() {},\n"
        "  copyTexImage2D(target, level, internalformat, x, y, width, height, border) {},\n"
        "  /* WebGL2 constants three.js checks */\n"
        "  MAX_SAMPLES: 4,\n"
        "  READ_BUFFER: 0x0C02,\n"
        "  UNPACK_ROW_LENGTH: 0x0CF2,\n"
        "  UNPACK_SKIP_ROWS: 0x0CF3,\n"
        "  UNPACK_SKIP_PIXELS: 0x0CF4,\n"
        "  PACK_ROW_LENGTH: 0x0D02,\n"
        "  COLOR: 0x1800,\n"
        "  DEPTH: 0x1801,\n"
        "  STENCIL: 0x1802,\n"
        "  HALF_FLOAT: 0x140B,\n"
        "  RG: 0x8227,\n"
        "  R8: 0x8229,\n"
        "  RG8: 0x822B,\n"
        "  R16F: 0x822D,\n"
        "  R32F: 0x822E,\n"
        "  RG16F: 0x822F,\n"
        "  RG32F: 0x8230,\n"
        "  RGBA32F: 0x8814,\n"
        "  RGB32F: 0x8815,\n"
        "  RGBA16F: 0x881A,\n"
        "  RGB16F: 0x881B,\n"
        "  TEXTURE_3D: 0x806F,\n"
        "  TEXTURE_WRAP_R: 0x8072,\n"
        "  TEXTURE_MIN_LOD: 0x813A,\n"
        "  TEXTURE_MAX_LOD: 0x813B,\n"
        "  TEXTURE_COMPARE_MODE: 0x884C,\n"
        "  TEXTURE_COMPARE_FUNC: 0x884E,\n"
        "  COMPARE_REF_TO_TEXTURE: 0x884E,\n"
        "  TEXTURE_2D_ARRAY: 0x8C1A,\n"
        "  UNSIGNED_INT_24_8: 0x84FA,\n"
        "  DEPTH24_STENCIL8: 0x88F0,\n"
        "  DEPTH_COMPONENT24: 0x81A6,\n"
        "  DEPTH_COMPONENT32F: 0x8CAC,\n"
        "  DEPTH32F_STENCIL8: 0x8CAD,\n"
        "  DEPTH_STENCIL: 0x84F9,\n"
        "  RGBA_INTEGER: 0x8D99,\n"
        "  RGB_INTEGER: 0x8D98,\n"
        "  RG_INTEGER: 0x8228,\n"
        "  RED_INTEGER: 0x8D94,\n"
        "  UNSIGNED_INT_2_10_10_10_REV: 0x8368,\n"
        "  TRANSFORM_FEEDBACK_BUFFER: 0x8C8E,\n"
        "  UNIFORM_BUFFER: 0x8A11,\n"
        "  ALREADY_SIGNALED: 0x911A,\n"
        "  CONDITION_SATISFIED: 0x911C,\n"
        "  WAIT_FAILED: 0x911D,\n"
        "  /* Shader precision */\n"
        "  HIGH_FLOAT: 0x8DF2, MEDIUM_FLOAT: 0x8DF1, LOW_FLOAT: 0x8DF0,\n"
        "  HIGH_INT: 0x8DF5, MEDIUM_INT: 0x8DF4, LOW_INT: 0x8DF3,\n"
        "  /* Texture/uniform limits */\n"
        "  MAX_TEXTURE_IMAGE_UNITS: 0x8872,\n"
        "  MAX_VERTEX_TEXTURE_IMAGE_UNITS: 0x8B4C,\n"
        "  MAX_COMBINED_TEXTURE_IMAGE_UNITS: 0x8B4D,\n"
        "  MAX_CUBE_MAP_TEXTURE_SIZE: 0x851C,\n"
        "  MAX_VERTEX_UNIFORM_VECTORS: 0x8DFB,\n"
        "  MAX_VARYING_VECTORS: 0x8DFC,\n"
        "  MAX_FRAGMENT_UNIFORM_VECTORS: 0x8DFD,\n"
        "  MAX_UNIFORM_BUFFER_BINDINGS: 0x8A2F,\n"
        "  SAMPLES: 0x80A9,\n"
        "  /* Scissor */\n"
        "  SCISSOR_BOX: 0x0C10,\n"
        "  /* Cubemap faces */\n"
        "  TEXTURE_CUBE_MAP_POSITIVE_X: 0x8515,\n"
        "  TEXTURE_CUBE_MAP_NEGATIVE_X: 0x8516,\n"
        "  TEXTURE_CUBE_MAP_POSITIVE_Y: 0x8517,\n"
        "  TEXTURE_CUBE_MAP_NEGATIVE_Y: 0x8518,\n"
        "  TEXTURE_CUBE_MAP_POSITIVE_Z: 0x8519,\n"
        "  TEXTURE_CUBE_MAP_NEGATIVE_Z: 0x851A,\n"
        "  /* Blend extras */\n"
        "  MIN: 0x8007, MAX: 0x8008,\n"
        "  SRC_ALPHA_SATURATE: 0x0308,\n"
        "  CONSTANT_COLOR: 0x8001, ONE_MINUS_CONSTANT_COLOR: 0x8002,\n"
        "  CONSTANT_ALPHA: 0x8003, ONE_MINUS_CONSTANT_ALPHA: 0x8004,\n"
        "  /* Misc */\n"
        "  INCR_WRAP: 0x8507, DECR_WRAP: 0x8508,\n"
        "  UNPACK_ALIGNMENT: 0x0CF5, PACK_ALIGNMENT: 0x0D05,\n"
        "  CURRENT_PROGRAM: 0x8B8D,\n"
        "  ACTIVE_TEXTURE: 0x84E0,\n"
        "  IMPLEMENTATION_COLOR_READ_TYPE: 0x8B9A,\n"
        "  IMPLEMENTATION_COLOR_READ_FORMAT: 0x8B9B,\n"
        "});\n";

    JS_Eval(ctx, gl_stubs, strlen(gl_stubs), "<gl-stubs>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, global);
}
