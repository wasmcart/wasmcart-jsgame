/*
 * canvas2d_skia.c — Canvas 2D backed by Skia (via @napi-rs/canvas's skia_c API)
 *
 * Bridges QuickJS CanvasRenderingContext2D → skia_c.cpp → Skia C++.
 * Renders to an ARGB pixel buffer, copied to wc_framebuffer each frame.
 *
 * This replaces the toy bitmap rasterizer (canvas2d.c) with production-quality
 * antialiased rendering, proper text shaping, gradients, patterns, compositing.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "quickjs.h"
#include "wasmcart.h"

/* From cart_main.c */
extern uint32_t framebuffer[];
extern uint32_t cur_width;
extern uint32_t cur_height;

/* ── skia_c extern declarations ──────────────────────────────────── */

/* Opaque types from skia_c.hpp */
typedef struct skiac_surface skiac_surface;
typedef struct skiac_canvas skiac_canvas;
typedef struct skiac_paint skiac_paint;
typedef struct skiac_path skiac_path;
typedef struct skiac_shader skiac_shader;
typedef struct skiac_path_effect skiac_path_effect;
typedef struct skiac_mask_filter skiac_mask_filter;
typedef struct skiac_image_filter skiac_image_filter;
typedef struct skiac_font_collection skiac_font_collection;

typedef struct { float x; float y; } skiac_point;
typedef struct { float a; float b; float c; float d; float e; float f; } skiac_transform;

#ifdef __cplusplus
extern "C" {
#endif

/* Surface */
skiac_surface* skiac_surface_create_rgba_premultiplied(int width, int height, uint8_t cs);
skiac_surface* skiac_surface_create_rgba(int width, int height, uint8_t cs);
void skiac_surface_destroy(skiac_surface* c_surface);
int skiac_surface_get_width(skiac_surface* c_surface);
int skiac_surface_get_height(skiac_surface* c_surface);
skiac_canvas* skiac_surface_get_canvas(skiac_surface* c_surface);
typedef struct { uint8_t *ptr; unsigned int size; } skiac_surface_data;
void skiac_surface_read_pixels(skiac_surface* c_surface, skiac_surface_data* data);
int skiac_surface_read_pixels_rect(skiac_surface* c_surface, uint8_t* data,
                                    int x, int y, int w, int h, uint8_t cs);

/* Canvas */
void skiac_canvas_clear(skiac_canvas* c_canvas, uint32_t color);
void skiac_canvas_save(skiac_canvas* c_canvas);
void skiac_canvas_restore(skiac_canvas* c_canvas);
void skiac_canvas_reset(skiac_canvas* c_canvas);
void skiac_canvas_translate(skiac_canvas* c_canvas, float dx, float dy);
void skiac_canvas_scale(skiac_canvas* c_canvas, float sx, float sy);
void skiac_canvas_rotate(skiac_canvas* c_canvas, float degrees);
void skiac_canvas_set_transform(skiac_canvas* c_canvas, skiac_transform ts);
void skiac_canvas_reset_transform(skiac_canvas* c_canvas);
void skiac_canvas_draw_path(skiac_canvas* c_canvas, skiac_path* c_path, skiac_paint* c_paint);
void skiac_canvas_draw_rect(skiac_canvas* c_canvas, float x, float y, float w, float h, skiac_paint* c_paint);
void skiac_canvas_draw_image(skiac_canvas* c_canvas, void* bitmap_or_surface,
                              int is_canvas,
                              float sx, float sy, float sw, float sh,
                              float dx, float dy, float dw, float dh,
                              int enable_smoothing, int filter_quality,
                              skiac_paint* c_paint);
void skiac_surface_destroy(skiac_surface* c_surface);
void skiac_canvas_clip_rect(skiac_canvas* c_canvas, float x, float y, float w, float h);
void skiac_canvas_clip_path(skiac_canvas* c_canvas, skiac_path* c_path);
void skiac_canvas_write_pixels(skiac_canvas* c_canvas, int width, int height,
                                const uint8_t* pixels, int row_size, int x, int y);
void skiac_canvas_put_image_data(skiac_canvas* c_canvas, int width, int height,
                                  uint8_t* pixels, unsigned int row_bytes, unsigned int length,
                                  float x, float y,
                                  float dirty_x, float dirty_y,
                                  float dirty_width, float dirty_height,
                                  uint8_t cs, int snapshot);

/* Paint */
skiac_paint* skiac_paint_create(void);
skiac_paint* skiac_paint_clone(skiac_paint* c_paint);
void skiac_paint_destroy(skiac_paint* c_paint);
void skiac_paint_set_color(skiac_paint* c_paint, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void skiac_paint_set_alpha(skiac_paint* c_paint, uint8_t a);
void skiac_paint_set_anti_alias(skiac_paint* c_paint, int aa);
void skiac_paint_set_blend_mode(skiac_paint* c_paint, int blend_mode);
void skiac_paint_set_shader(skiac_paint* c_paint, skiac_shader* c_shader);
void skiac_paint_set_style(skiac_paint* c_paint, int style);
void skiac_paint_set_stroke_width(skiac_paint* c_paint, float width);
void skiac_paint_set_stroke_cap(skiac_paint* c_paint, int cap);
void skiac_paint_set_stroke_join(skiac_paint* c_paint, uint8_t join);
void skiac_paint_set_stroke_miter(skiac_paint* c_paint, float miter);
void skiac_paint_set_mask_filter(skiac_paint* c_paint, skiac_mask_filter* filter);
void skiac_paint_set_image_filter(skiac_paint* c_paint, skiac_image_filter* filter);
void skiac_paint_set_path_effect(skiac_paint* c_paint, skiac_path_effect* effect);

/* Path */
skiac_path* skiac_path_create(void);
void skiac_path_destroy(skiac_path* c_path);
void skiac_path_move_to(skiac_path* c_path, float x, float y);
void skiac_path_line_to(skiac_path* c_path, float x, float y);
void skiac_path_cubic_to(skiac_path* c_path, float x1, float y1, float x2, float y2, float x3, float y3);
void skiac_path_quad_to(skiac_path* c_path, float cpx, float cpy, float x, float y);
void skiac_path_close(skiac_path* c_path);
void skiac_path_add_rect(skiac_path* c_path, float x, float y, float width, float height);
void skiac_path_add_circle(skiac_path* c_path, float x, float y, float r);
void skiac_path_arc_to(skiac_path* c_path, float left, float top, float right, float bottom,
                        float startAngle, float sweepAngle, int forceMoveTo);
void skiac_path_arc_to_tangent(skiac_path* c_path, float x1, float y1, float x2, float y2, float radius);
void skiac_path_set_fill_type(skiac_path* c_path, int type);
void skiac_path_round_rect(skiac_path* c_path, float x, float y, float w, float h,
                            float tl, float tr, float br, float bl);
skiac_path* skiac_path_from_svg(char* svg_path);

/* Shader (gradients, patterns) */
skiac_shader* skiac_shader_make_linear_gradient(const skiac_point* points,
    const uint32_t* colors, const float* positions, int count,
    int tile_mode, uint32_t flags, float* ts);
skiac_shader* skiac_shader_make_radial_gradient(skiac_point start_point,
    float start_radius, const uint32_t* colors, const float* positions,
    int count, int tile_mode, uint32_t flags, float* ts);
skiac_shader* skiac_shader_make_from_surface_image(skiac_surface* c_surface,
    float* ts, int filter_quality, int repeat_x, int repeat_y);
void skiac_shader_destroy(skiac_shader* c_shader);

/* Mask filter (shadows) */
skiac_mask_filter* skiac_mask_filter_make_blur(float radius);
void skiac_mask_filter_destroy(skiac_mask_filter* filter);

/* Path effect (dashes) */
skiac_path_effect* skiac_path_effect_make_dash_path(const float* intervals,
    int count, float phase);
void skiac_path_effect_destroy(skiac_path_effect* effect);

/* Font collection */
skiac_font_collection* skiac_font_collection_create(void);
void skiac_font_collection_destroy(skiac_font_collection* c_font_collection);
uint32_t skiac_font_collection_register(skiac_font_collection* c_font_collection,
    const uint8_t* font_data, unsigned int font_data_size, const char* name_alias);

/* WASM-specific direct text rendering (bypasses paragraph/ICU) */
int skiac_wasm_register_font(const uint8_t* data, unsigned int size);
int skiac_wasm_register_font_named(const uint8_t* data, unsigned int size, const char* family);

/* GL-backed Skia surface (from skia_gl_surface.cpp) */
void *skia_create_gl_surface(int width, int height);
void skia_gl_flush(void);
int skia_has_gl_surface(void);
void skia_gl_reset_context(void);
void skia_gl_flush_nosync(void);
float skiac_wasm_draw_text(void* canvas_ptr, void* paint_ptr,
                            const char* text, unsigned int text_len,
                            float x, float y, float font_size, int baseline);
float skiac_wasm_measure_text(const char* text, unsigned int text_len,
                               float font_size);

/* Line metrics struct */
typedef struct {
    float width;
    float left;
    /* ... more fields we don't need right now */
    float padding[8];
} skiac_line_metrics;

typedef struct { uint32_t tag; float value; } skiac_font_variation;

/* Text - matches the actual C++ signature in skia_c.cpp */
void skiac_canvas_get_line_metrics_or_draw_text(
    const char* text, unsigned int text_len,
    float max_width,
    float x, float y,
    float canvas_width,
    skiac_font_collection* c_collection,
    float font_size,
    int weight,        /* font weight (400=normal, 700=bold) */
    int stretch,       /* font stretch (5=normal) */
    float stretch_width,
    int slant,         /* 0=upright, 1=italic, 2=oblique */
    const char* font_family,
    int baseline,
    int align,
    int direction,
    float letter_spacing,
    float word_spacing,
    skiac_paint* c_paint,
    skiac_canvas* c_canvas,
    skiac_line_metrics* c_line_metrics,
    const skiac_font_variation* variations,
    int variations_count,
    int kerning,
    int variant_caps,
    const char* lang,
    int text_rendering);

#ifdef __cplusplus
}
#endif

/* ── Canvas state ────────────────────────────────────────────────── */

static skiac_surface *skia_surface = NULL;
static skiac_canvas  *skia_canvas  = NULL;
static skiac_paint   *fill_paint   = NULL;
static skiac_paint   *stroke_paint = NULL;
static skiac_path    *current_path = NULL;
static skiac_font_collection *font_collection = NULL;
static int skia_initialized = 0;

static float cur_font_size = 10.0f;
static char  cur_font_family[128] = "sans-serif";
static int   cur_text_align = 0;     /* 0=left */
static int   cur_text_baseline = 3;  /* 3=alphabetic */
static float cur_global_alpha = 1.0f;
static float cur_line_width = 1.0f;

/* Save/restore stack for Canvas 2D state (paint properties) */
#define MAX_STATE_STACK 32
typedef struct {
    float global_alpha;
    float line_width;
    float font_size;
    /* fill/stroke colors stored as RGBA */
    uint8_t fill_r, fill_g, fill_b, fill_a;
    uint8_t stroke_r, stroke_g, stroke_b, stroke_a;
} canvas_state_t;
static canvas_state_t state_stack[MAX_STATE_STACK];
static int state_stack_idx = 0;

/* Track current fill/stroke colors for save/restore */
static uint8_t cur_fill_r = 0, cur_fill_g = 0, cur_fill_b = 0, cur_fill_a = 255;
static uint8_t cur_stroke_r = 0, cur_stroke_g = 0, cur_stroke_b = 0, cur_stroke_a = 255;

/* ── Initialization ──────────────────────────────────────────────── */

static int using_gl_surface = 0;
static int _host_fbo = 0; /* FBO the host had bound before Ganesh init */

#ifdef __wasm__
__attribute__((import_module("gl"), import_name("glGetIntegerv")))
extern void _gl_GetIntegerv(unsigned int pname, int *data);
#endif
extern int game_uses_webgl;

void skia_save_host_fbo(void) {
    int fbo = 0;
    _gl_GetIntegerv(0x8CA6, &fbo); /* GL_DRAW_FRAMEBUFFER_BINDING */
    if (fbo != _host_fbo) {
        _host_fbo = fbo;
        static int logged = 0;
        if (!logged) {
            logged = 1;
            char msg[64];
            snprintf(msg, sizeof(msg), "Host FBO per-frame: %d", _host_fbo);
            wc_log(msg, strlen(msg));
        }
    }
}

/* Per-op Ganesh flush with no CPU sync — fast submit to GPU.
 * Needed because Ganesh drops batched commands without intermediate flushes. */
extern int _skia_is_desktop_context(void);

#ifdef __wasm__
__attribute__((import_module("gl"), import_name("glGetError")))
extern unsigned int _gl_GetError2(void);
#endif

static inline void ganesh_flush_if_needed(void) {
    if (!using_gl_surface) return;
    static int logged = 0;
    int desktop = _skia_is_desktop_context();
    if (!logged) {
        logged = 1;
        char msg[64];
        snprintf(msg, sizeof(msg), "Ganesh per-op flush: desktop=%d (using %s)", desktop, desktop ? "kYes" : "kNo");
        wc_log(msg, strlen(msg));
    }
    if (desktop) {
        skia_gl_flush_nosync();
    } else {
        skia_gl_flush_nosync();
    }

    /* Log GL errors on desktop to diagnose rendering issues */
    if (desktop) {
        static int err_count = 0;
        unsigned int err = _gl_GetError2();
        if (err && err_count < 10) {
            err_count++;
            char msg[64];
            snprintf(msg, sizeof(msg), "Ganesh GL error after flush: 0x%04x", err);
            wc_log(msg, strlen(msg));
        }
    }
}

/* Resize Skia surface when game changes canvas dimensions */
/* Game's logical canvas size (may differ from Skia surface size) */
static uint32_t game_width = 0, game_height = 0;
static float game_scale_x = 1.0f, game_scale_y = 1.0f;
static float game_offset_x = 0.0f, game_offset_y = 0.0f;

/* Reapply game→surface scale + centering offset + clip to game bounds */
static inline void reapply_game_scale(void) {
    if (game_offset_x != 0.0f || game_offset_y != 0.0f) {
        skiac_canvas_translate(skia_canvas, game_offset_x, game_offset_y);
    }
    if (game_scale_x != 1.0f || game_scale_y != 1.0f) {
        skiac_canvas_scale(skia_canvas, game_scale_x, game_scale_y);
    }
    /* Clip to game's logical bounds — browser canvases clip automatically */
    if (game_width > 0 && game_height > 0) {
        skiac_canvas_clip_rect(skia_canvas, 0, 0, game_width, game_height);
    }
}

void skia_resize_surface(uint32_t w, uint32_t h) {
    /* Don't resize the Skia surface — keep it at preferred (host) resolution.
     * Instead, track the game's logical size and apply a scale transform.
     * This is equivalent to CSS scaling a <canvas> in a browser. */
    game_width = w;
    game_height = h;

    /* Apply scale on next frame's first draw via ensure_skia_init check */
    /* Uniform scale with letterboxing to preserve aspect ratio */
    float sx = (float)cur_width / (float)w;
    float sy = (float)cur_height / (float)h;
    float scale = sx < sy ? sx : sy; /* min — fit within surface */
    game_scale_x = scale;
    game_scale_y = scale;
    /* Center the scaled game within the surface */
    game_offset_x = ((float)cur_width - (float)w * scale) * 0.5f;
    game_offset_y = ((float)cur_height - (float)h * scale) * 0.5f;

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "resize: game=%dx%d surface=%dx%d scale=%.2fx%.2f init=%d",
                 w, h, cur_width, cur_height, game_scale_x, game_scale_y, skia_initialized);
        wc_log(msg, strlen(msg));
    }

    if (skia_initialized && skia_canvas) {
        skiac_canvas_reset_transform(skia_canvas);
        reapply_game_scale();
    }
}

static void ensure_skia_init(void) {
    if (skia_initialized) return;

    /* Try Ganesh GL surface (GPU-accelerated Canvas 2D, zero CPU pixel copy).
     * Skip Ganesh if game uses WebGL — Ganesh would corrupt WebGL state. */
    if (!game_uses_webgl) {
        void *gl_surf = skia_create_gl_surface(cur_width, cur_height);
        if (gl_surf && skia_has_gl_surface()) {
            skia_surface = gl_surf;
            using_gl_surface = 1;
        }
    }
    if (!skia_surface) {
        /* CPU raster + GL texture blit */
        skia_surface = skiac_surface_create_rgba_premultiplied(cur_width, cur_height, 0);
        using_gl_surface = 0;
    }
    if (!skia_surface) {
        WC_LOG("ERROR: skiac_surface_create failed");
        return;
    }
    skia_canvas = skiac_surface_get_canvas(skia_surface);

    if (using_gl_surface) {
        WC_LOG("Canvas 2D: GPU (Skia Ganesh GL)");
    } else {
        WC_LOG("Canvas 2D: CPU (Skia raster + GL blit)");
    }

    /* Host FBO saved per-frame in skia_save_host_fbo() */

    fill_paint = skiac_paint_create();
    skiac_paint_set_anti_alias(fill_paint, using_gl_surface ? 0 : 1);
    skiac_paint_set_style(fill_paint, 0);  /* 0 = fill */
    skiac_paint_set_color(fill_paint, 0, 0, 0, 255);

    stroke_paint = skiac_paint_create();
    skiac_paint_set_anti_alias(stroke_paint, using_gl_surface ? 0 : 1);
    skiac_paint_set_style(stroke_paint, 1);  /* 1 = stroke */
    skiac_paint_set_color(stroke_paint, 0, 0, 0, 255);
    skiac_paint_set_stroke_width(stroke_paint, 1.0f);

    current_path = skiac_path_create();
    font_collection = skiac_font_collection_create();

    /* Try to load a default font from .wasc assets */
    {
        const char *font_paths[] = {
            "fonts/DejaVuSans.ttf",
            "DejaVuSans.ttf",
            "FreeSansBold.ttf",
            "fonts/FreeSansBold.ttf",
            NULL
        };
        for (int i = 0; font_paths[i]; i++) {
            int sz = wc_asset_size(font_paths[i], strlen(font_paths[i]));
            if (sz > 0) {
                uint8_t *fdata = malloc(sz);
                wc_load_asset(font_paths[i], strlen(font_paths[i]), (char*)fdata, sz);
                /* Register with direct WASM font API (bypasses ICU/paragraph) */
                int glyph_id = skiac_wasm_register_font(fdata, sz);
                WC_LOG("font registered");
                /* Log glyph ID for 'A' — 0 means .notdef */
                int glyph_count_text = glyph_id & 0xFF;
                int glyph_a = (glyph_id >> 8) & 0xFF;
                int num_glyphs = (glyph_id >> 16) & 0xFFFF;
                char msg[128];
                snprintf(msg, sizeof(msg), "glyphA=%d numGlyphs=%d countText=%d", glyph_a, num_glyphs, glyph_count_text);
                wc_log(msg, strlen(msg));
                free(fdata);
                break;
            }
        }
    }

    skia_initialized = 1;

    /* Apply pending game scale if canvas size was set before Skia init */
    if (game_width > 0 && game_height > 0) {
        reapply_game_scale();
    }
}

/* ── Copy Skia surface → wasmcart framebuffer ────────────────────── */
/* Called each frame after all drawing is done */

/* GL imports for texture upload + fullscreen quad + FBO blit */
#ifdef __wasm__
/* _gl_GetIntegerv declared earlier (before ensure_skia_init) */
__attribute__((import_module("gl"), import_name("glReadPixels")))
extern void _gl_ReadPixels(int x, int y, int w, int h, unsigned int format, unsigned int type, void *pixels);
__attribute__((import_module("gl"), import_name("glBlitFramebuffer")))
extern void _gl_BlitFramebuffer(int sx0, int sy0, int sx1, int sy1,
                                 int dx0, int dy0, int dx1, int dy1,
                                 unsigned int mask, unsigned int filter);
__attribute__((import_module("gl"), import_name("glGenTextures")))
extern void _gl_GenTextures(int n, unsigned int *textures);
__attribute__((import_module("gl"), import_name("glBindTexture")))
extern void _gl_BindTexture(unsigned int target, unsigned int texture);
__attribute__((import_module("gl"), import_name("glTexImage2D")))
extern void _gl_TexImage2D(unsigned int target, int level, int internalformat,
    int width, int height, int border, unsigned int format, unsigned int type, const void *pixels);
__attribute__((import_module("gl"), import_name("glTexParameteri")))
extern void _gl_TexParameteri(unsigned int target, unsigned int pname, int param);
__attribute__((import_module("gl"), import_name("glEnable")))
extern void _gl_Enable(unsigned int cap);
__attribute__((import_module("gl"), import_name("glDisable")))
extern void _gl_Disable(unsigned int cap);
__attribute__((import_module("gl"), import_name("glViewport")))
extern void _gl_Viewport(int x, int y, int width, int height);
__attribute__((import_module("gl"), import_name("glClear")))
extern void _gl_Clear(unsigned int mask);
__attribute__((import_module("gl"), import_name("glClearColor")))
extern void _gl_ClearColor(float r, float g, float b, float a);
__attribute__((import_module("gl"), import_name("glBindFramebuffer")))
extern void _gl_BindFramebuffer(unsigned int target, unsigned int fb);
__attribute__((import_module("gl"), import_name("glCreateShader")))
extern unsigned int _gl_CreateShader(unsigned int type);
__attribute__((import_module("gl"), import_name("glShaderSource")))
extern void _gl_ShaderSource(unsigned int shader, int count, const char *const *string, const int *length);
__attribute__((import_module("gl"), import_name("glCompileShader")))
extern void _gl_CompileShader(unsigned int shader);
__attribute__((import_module("gl"), import_name("glCreateProgram")))
extern unsigned int _gl_CreateProgram(void);
__attribute__((import_module("gl"), import_name("glAttachShader")))
extern void _gl_AttachShader(unsigned int program, unsigned int shader);
__attribute__((import_module("gl"), import_name("glLinkProgram")))
extern void _gl_LinkProgram(unsigned int program);
__attribute__((import_module("gl"), import_name("glUseProgram")))
extern void _gl_UseProgram(unsigned int program);
__attribute__((import_module("gl"), import_name("glGetAttribLocation")))
extern int _gl_GetAttribLocation(unsigned int program, const char *name);
__attribute__((import_module("gl"), import_name("glGetUniformLocation")))
extern int _gl_GetUniformLocation(unsigned int program, const char *name);
__attribute__((import_module("gl"), import_name("glGenBuffers")))
extern void _gl_GenBuffers(int n, unsigned int *buffers);
__attribute__((import_module("gl"), import_name("glBindBuffer")))
extern void _gl_BindBuffer(unsigned int target, unsigned int buffer);
__attribute__((import_module("gl"), import_name("glBufferData")))
extern void _gl_BufferData(unsigned int target, long size, const void *data, unsigned int usage);
__attribute__((import_module("gl"), import_name("glEnableVertexAttribArray")))
extern void _gl_EnableVertexAttribArray(unsigned int index);
__attribute__((import_module("gl"), import_name("glVertexAttribPointer")))
extern void _gl_VertexAttribPointer(unsigned int index, int size, unsigned int type, unsigned char normalized, int stride, const void *pointer);
__attribute__((import_module("gl"), import_name("glDrawArrays")))
extern void _gl_DrawArrays(unsigned int mode, int first, int count);
__attribute__((import_module("gl"), import_name("glGenVertexArrays")))
extern void _gl_GenVertexArrays(int n, unsigned int *arrays);
__attribute__((import_module("gl"), import_name("glBindVertexArray")))
extern void _gl_BindVertexArray(unsigned int array);
__attribute__((import_module("gl"), import_name("glUniform1i")))
extern void _gl_Uniform1i(int location, int v0);
#endif

static unsigned int blit_tex = 0;
static unsigned int blit_program = 0;
static unsigned int blit_vao = 0;
static unsigned int blit_vbo = 0;
static int blit_initialized = 0;

static void init_blit(void) {
    /* Create fullscreen quad shader */
    const char *vs_src =
        "#version 300 es\n"
        "in vec2 aPos;\n"
        "out vec2 vUV;\n"
        "void main() {\n"
        "  vUV = aPos * 0.5 + 0.5;\n"
        "  vUV.y = 1.0 - vUV.y;\n"  /* flip Y for top-down pixel data */
        "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";
    const char *fs_src =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 vUV;\n"
        "uniform sampler2D uTex;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  fragColor = texture(uTex, vUV);\n"
        "}\n";

    unsigned int vs = _gl_CreateShader(0x8B31);
    int vs_len = strlen(vs_src);
    _gl_ShaderSource(vs, 1, &vs_src, &vs_len);
    _gl_CompileShader(vs);

    unsigned int fs = _gl_CreateShader(0x8B30);
    int fs_len = strlen(fs_src);
    _gl_ShaderSource(fs, 1, &fs_src, &fs_len);
    _gl_CompileShader(fs);

    blit_program = _gl_CreateProgram();
    _gl_AttachShader(blit_program, vs);
    _gl_AttachShader(blit_program, fs);
    _gl_LinkProgram(blit_program);

    /* Fullscreen quad: two triangles */
    float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
    _gl_GenVertexArrays(1, &blit_vao);
    _gl_BindVertexArray(blit_vao);
    _gl_GenBuffers(1, &blit_vbo);
    _gl_BindBuffer(0x8892, blit_vbo);
    _gl_BufferData(0x8892, sizeof(quad), quad, 0x88E4);

    int aPos = _gl_GetAttribLocation(blit_program, "aPos");
    _gl_EnableVertexAttribArray(aPos);
    _gl_VertexAttribPointer(aPos, 2, 0x1406, 0, 0, 0);

    /* Texture for framebuffer upload */
    _gl_GenTextures(1, &blit_tex);
    _gl_BindTexture(0x0DE1, blit_tex);
    _gl_TexParameteri(0x0DE1, 0x2801, 0x2600); /* MIN = NEAREST */
    _gl_TexParameteri(0x0DE1, 0x2800, 0x2600); /* MAG = NEAREST */
    _gl_TexParameteri(0x0DE1, 0x2802, 0x812F); /* WRAP_S = CLAMP_TO_EDGE */
    _gl_TexParameteri(0x0DE1, 0x2803, 0x812F); /* WRAP_T = CLAMP_TO_EDGE */

    blit_initialized = 1;
}

extern void gl_trace_frame_start(int frame);
extern void gl_trace_frame_end(void);

void skia_flush_to_framebuffer(void) {
    if (!skia_initialized) return;
    if (game_uses_webgl) return; /* WebGL renders directly — no Ganesh blit */

    if (using_gl_surface) {
        /* Flush Ganesh GPU commands */
        skia_gl_flush();

        /* Get Ganesh's current FBO (where it just rendered) */
        int ganesh_fbo = 0;
        _gl_GetIntegerv(0x8CA6, &ganesh_fbo); /* GL_DRAW_FRAMEBUFFER_BINDING */
        {
            static int logged = 0;
            if (!logged) {
                logged = 1;
                char msg[64];
                snprintf(msg, sizeof(msg), "Ganesh blit: FBO=%d size=%dx%d", ganesh_fbo, cur_width, cur_height);
                wc_log(msg, strlen(msg));
            }
        }

        /* Y-orientation check: read top, center, bottom of Ganesh FBO */
        {
            static int check_count = 0;
            check_count++;
            if (check_count == 60) {
                unsigned char px[4];
                char msg[256];

                _gl_ReadPixels(cur_width/2, 10, 1, 1, 0x1908, 0x1401, px);
                snprintf(msg, sizeof(msg), "Ganesh FBO GL-bottom(y=10): R=%d G=%d B=%d", px[0], px[1], px[2]);
                wc_log(msg, strlen(msg));

                _gl_ReadPixels(cur_width/2, cur_height/2, 1, 1, 0x1908, 0x1401, px);
                snprintf(msg, sizeof(msg), "Ganesh FBO GL-center(y=%d): R=%d G=%d B=%d", cur_height/2, px[0], px[1], px[2]);
                wc_log(msg, strlen(msg));

                _gl_ReadPixels(cur_width/2, cur_height-10, 1, 1, 0x1908, 0x1401, px);
                snprintf(msg, sizeof(msg), "Ganesh FBO GL-top(y=%d): R=%d G=%d B=%d", cur_height-10, px[0], px[1], px[2]);
                wc_log(msg, strlen(msg));

                /* After readback+upload, check redirect FBO */
                _gl_BindFramebuffer(0x8CA8, 0); /* READ from FBO 0 (redirect) */
                _gl_ReadPixels(cur_width/2, 10, 1, 1, 0x1908, 0x1401, px);
                snprintf(msg, sizeof(msg), "Redirect FBO GL-bottom(y=10): R=%d G=%d B=%d", px[0], px[1], px[2]);
                wc_log(msg, strlen(msg));

                _gl_ReadPixels(cur_width/2, cur_height-10, 1, 1, 0x1908, 0x1401, px);
                snprintf(msg, sizeof(msg), "Redirect FBO GL-top(y=%d): R=%d G=%d B=%d", cur_height-10, px[0], px[1], px[2]);
                wc_log(msg, strlen(msg));
            }
        }

        if (_skia_is_desktop_context()) {
            /* Desktop: readback + texture upload to avoid self-blit.
             * Use raw glReadPixels (not Ganesh's readPixels which uses stubbed functions). */
            static uint8_t *rb_buf = NULL;
            if (!rb_buf) rb_buf = (uint8_t*)malloc(cur_width * cur_height * 4);
            if (rb_buf) {
                /* Ensure Ganesh's FBO is bound for readback */
                _gl_BindFramebuffer(0x8D40, ganesh_fbo);
                _gl_ReadPixels(0, 0, cur_width, cur_height, 0x1908, 0x1401, rb_buf);
                skia_gl_reset_context();

                if (!blit_initialized) init_blit();
                if (blit_initialized) {
                    _gl_BindFramebuffer(0x8D40, 0);
                    _gl_Viewport(0, 0, cur_width, cur_height);
                    _gl_Disable(0x0B71);
                    _gl_Disable(0x0BE2);
                    _gl_BindTexture(0x0DE1, blit_tex);
                    _gl_TexImage2D(0x0DE1, 0, 0x1908, cur_width, cur_height, 0,
                                    0x1908, 0x1401, rb_buf);
                    _gl_UseProgram(blit_program);
                    _gl_Uniform1i(_gl_GetUniformLocation(blit_program, "uTex"), 0);
                    _gl_BindVertexArray(blit_vao);
                    _gl_DrawArrays(5, 0, 4);
                }
            }
            skia_gl_reset_context();
        } else {
            /* GLES: direct FBO blit */
            _gl_BindFramebuffer(0x8CA8, ganesh_fbo);
            _gl_BindFramebuffer(0x8CA9, 0);
            _gl_Disable(0x0C11);
            _gl_BlitFramebuffer(
                0, cur_height, cur_width, 0,
                0, 0, cur_width, cur_height,
                0x4000, 0x2600);
            _gl_BindFramebuffer(0x8D40, ganesh_fbo);
            skia_gl_reset_context();
        }
        return;
    }

    /* CPU raster path: peek pixels directly */
    skiac_surface_data data;
    skiac_surface_read_pixels(skia_surface, &data);
    if (!data.ptr || data.size == 0) return;

    /* Initialize blit shader on first use */
    if (!blit_initialized) init_blit();
    if (!blit_initialized) return;

    /* Upload Skia RGBA pixels as GL texture */
    _gl_BindFramebuffer(0x8D40, 0); /* bind display FBO (host redirects 0) */
    _gl_Viewport(0, 0, cur_width, cur_height);
    _gl_Disable(0x0B71); /* depth test off */
    _gl_Disable(0x0BE2); /* blending off */

    _gl_BindTexture(0x0DE1, blit_tex);
    _gl_TexImage2D(0x0DE1, 0, 0x1908, cur_width, cur_height, 0,
                    0x1908, 0x1401, data.ptr); /* RGBA, UNSIGNED_BYTE */

    /* Draw fullscreen quad */
    _gl_UseProgram(blit_program);
    _gl_Uniform1i(_gl_GetUniformLocation(blit_program, "uTex"), 0);
    _gl_BindVertexArray(blit_vao);
    _gl_DrawArrays(5, 0, 4); /* TRIANGLE_STRIP */
}

/* ── Color parsing ───────────────────────────────────────────────── */

static void parse_color_rgba(const char *str, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    *r = 0; *g = 0; *b = 0; *a = 255;
    if (!str) return;

    if (strcmp(str, "black") == 0) { return; }
    if (strcmp(str, "white") == 0) { *r = *g = *b = 255; return; }
    if (strcmp(str, "red") == 0) { *r = 255; return; }
    if (strcmp(str, "green") == 0) { *g = 128; return; }
    if (strcmp(str, "blue") == 0) { *b = 255; return; }
    if (strcmp(str, "yellow") == 0) { *r = 255; *g = 255; return; }
    if (strcmp(str, "cyan") == 0) { *g = 255; *b = 255; return; }
    if (strcmp(str, "magenta") == 0) { *r = 255; *b = 255; return; }
    if (strcmp(str, "orange") == 0) { *r = 255; *g = 165; return; }
    if (strcmp(str, "gray") == 0 || strcmp(str, "grey") == 0) { *r = *g = *b = 128; return; }
    if (strcmp(str, "transparent") == 0) { *a = 0; return; }

    if (str[0] == '#') {
        unsigned int c = 0;
        int len = strlen(str);
        if (len == 4) {
            int ri, gi, bi;
            sscanf(str, "#%1x%1x%1x", &ri, &gi, &bi);
            *r = ri * 17; *g = gi * 17; *b = bi * 17;
        } else if (len == 7) {
            sscanf(str + 1, "%06x", &c);
            *r = (c >> 16) & 0xFF; *g = (c >> 8) & 0xFF; *b = c & 0xFF;
        } else if (len == 9) {
            sscanf(str + 1, "%08x", &c);
            *r = (c >> 24) & 0xFF; *g = (c >> 16) & 0xFF; *b = (c >> 8) & 0xFF; *a = c & 0xFF;
        }
        return;
    }

    if (strncmp(str, "rgba(", 5) == 0) {
        int ri, gi, bi; float af = 1.0f;
        sscanf(str, "rgba(%d,%d,%d,%f)", &ri, &gi, &bi, &af);
        *r = ri; *g = gi; *b = bi; *a = (uint8_t)(af * 255);
        return;
    }
    if (strncmp(str, "rgb(", 4) == 0) {
        int ri, gi, bi;
        sscanf(str, "rgb(%d,%d,%d)", &ri, &gi, &bi);
        *r = ri; *g = gi; *b = bi;
        return;
    }
}

/* ── QuickJS bindings ────────────────────────────────────────────── */

/* clearRect */
static JSValue js_clearRect(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    double x, y, w, h;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &w, argv[2]); JS_ToFloat64(ctx, &h, argv[3]);
    skiac_canvas_save(skia_canvas);
    skiac_canvas_clip_rect(skia_canvas, x, y, w, h);
    skiac_canvas_clear(skia_canvas, 0x00000000);
    ganesh_flush_if_needed();
    skiac_canvas_restore(skia_canvas);
    return JS_UNDEFINED;
}

/* fillRect */
static JSValue js_fillRect(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    double x, y, w, h;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &w, argv[2]); JS_ToFloat64(ctx, &h, argv[3]);

    skiac_canvas_draw_rect(skia_canvas, x, y, w, h, fill_paint);
    ganesh_flush_if_needed();
    return JS_UNDEFINED;
}

/* strokeRect */
static JSValue js_strokeRect(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    double x, y, w, h;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &w, argv[2]); JS_ToFloat64(ctx, &h, argv[3]);
    /* Draw rect outline using path */
    skiac_path *p = skiac_path_create();
    skiac_path_add_rect(p, x, y, w, h);
    skiac_canvas_draw_path(skia_canvas, p, stroke_paint);
    ganesh_flush_if_needed();
    skiac_path_destroy(p);
    return JS_UNDEFINED;
}

/* fillText / strokeText */
static JSValue js_fillText(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_UNDEFINED;
    double x, y;
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]);
    int baseline = 0; /* 0=alphabetic, 1=top, 2=middle, 3=bottom */
    if (argc > 3) JS_ToInt32(ctx, &baseline, argv[3]);
    skiac_wasm_draw_text(skia_canvas, fill_paint,
                          text, strlen(text), x, y, cur_font_size, baseline);
    ganesh_flush_if_needed();
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

static JSValue js_strokeText(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_UNDEFINED;
    double x, y;
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]);
    int baseline = 0;
    if (argc > 3) JS_ToInt32(ctx, &baseline, argv[3]);
    skiac_wasm_draw_text(skia_canvas, stroke_paint,
                          text, strlen(text), x, y, cur_font_size, baseline);
    ganesh_flush_if_needed();
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

static JSValue js_measureText(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    const char *text = JS_ToCString(ctx, argv[0]);
    float width = 0;
    if (text) {
        width = skiac_wasm_measure_text(text, strlen(text), cur_font_size);
        JS_FreeCString(ctx, text);
    }
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "width", JS_NewFloat64(ctx, width));
    return result;
}

/* Path operations */
static JSValue js_beginPath(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    skiac_path_destroy(current_path);
    current_path = skiac_path_create();
    return JS_UNDEFINED;
}

static JSValue js_closePath(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    skiac_path_close(current_path);
    return JS_UNDEFINED;
}

static JSValue js_moveTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    double x, y;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    skiac_path_move_to(current_path, x, y);
    return JS_UNDEFINED;
}

static JSValue js_lineTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    double x, y;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    skiac_path_line_to(current_path, x, y);
    return JS_UNDEFINED;
}

static JSValue js_arc(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    double cx, cy, r, startAngle, endAngle;
    JS_ToFloat64(ctx, &cx, argv[0]); JS_ToFloat64(ctx, &cy, argv[1]);
    JS_ToFloat64(ctx, &r, argv[2]);
    JS_ToFloat64(ctx, &startAngle, argv[3]); JS_ToFloat64(ctx, &endAngle, argv[4]);
    int ccw = (argc > 5) ? JS_ToBool(ctx, argv[5]) : 0;

    /* Full circle optimization (0 to 2π) */
    double sweep = endAngle - startAngle;
    if (!ccw && sweep >= 2.0 * M_PI - 0.001) {
        skiac_path_add_circle(current_path, cx, cy, r);
        return JS_UNDEFINED;
    }
    if (ccw && sweep <= -(2.0 * M_PI - 0.001)) {
        skiac_path_add_circle(current_path, cx, cy, r);
        return JS_UNDEFINED;
    }

    /* Partial arc: convert to Skia's bounding rect + degrees */
    float left = cx - r, top = cy - r, right = cx + r, bottom = cy + r;
    float startDeg = startAngle * 180.0f / M_PI;
    float sweepDeg = (endAngle - startAngle) * 180.0f / M_PI;
    if (ccw && sweepDeg > 0) sweepDeg -= 360.0f;
    if (!ccw && sweepDeg < 0) sweepDeg += 360.0f;

    skiac_path_arc_to(current_path, left, top, right, bottom, startDeg, sweepDeg, 0);
    return JS_UNDEFINED;
}

static JSValue js_arcTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    double x1, y1, x2, y2, radius;
    JS_ToFloat64(ctx, &x1, argv[0]); JS_ToFloat64(ctx, &y1, argv[1]);
    JS_ToFloat64(ctx, &x2, argv[2]); JS_ToFloat64(ctx, &y2, argv[3]);
    JS_ToFloat64(ctx, &radius, argv[4]);
    skiac_path_arc_to_tangent(current_path, x1, y1, x2, y2, radius);
    return JS_UNDEFINED;
}

static JSValue js_quadraticCurveTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    double cpx, cpy, x, y;
    JS_ToFloat64(ctx, &cpx, argv[0]); JS_ToFloat64(ctx, &cpy, argv[1]);
    JS_ToFloat64(ctx, &x, argv[2]); JS_ToFloat64(ctx, &y, argv[3]);
    skiac_path_quad_to(current_path, cpx, cpy, x, y);
    return JS_UNDEFINED;
}

static JSValue js_bezierCurveTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    double cp1x, cp1y, cp2x, cp2y, x, y;
    JS_ToFloat64(ctx, &cp1x, argv[0]); JS_ToFloat64(ctx, &cp1y, argv[1]);
    JS_ToFloat64(ctx, &cp2x, argv[2]); JS_ToFloat64(ctx, &cp2y, argv[3]);
    JS_ToFloat64(ctx, &x, argv[4]); JS_ToFloat64(ctx, &y, argv[5]);
    skiac_path_cubic_to(current_path, cp1x, cp1y, cp2x, cp2y, x, y);
    return JS_UNDEFINED;
}

static JSValue js_rect(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    double x, y, w, h;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &w, argv[2]); JS_ToFloat64(ctx, &h, argv[3]);
    skiac_path_add_rect(current_path, x, y, w, h);
    return JS_UNDEFINED;
}

static JSValue js_fill(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    skiac_canvas_draw_path(skia_canvas, current_path, fill_paint);
    ganesh_flush_if_needed();
    return JS_UNDEFINED;
}

static JSValue js_stroke(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    skiac_canvas_draw_path(skia_canvas, current_path, stroke_paint);
    ganesh_flush_if_needed();
    return JS_UNDEFINED;
}

static JSValue js_clip(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    skiac_canvas_clip_path(skia_canvas, current_path);
    return JS_UNDEFINED;
}

/* Transform */
static JSValue js_save(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    skiac_canvas_save(skia_canvas);
    /* Save paint state */
    if (state_stack_idx < MAX_STATE_STACK) {
        canvas_state_t *s = &state_stack[state_stack_idx++];
        s->global_alpha = cur_global_alpha;
        s->line_width = cur_line_width;
        s->font_size = cur_font_size;
        s->fill_r = cur_fill_r; s->fill_g = cur_fill_g;
        s->fill_b = cur_fill_b; s->fill_a = cur_fill_a;
        s->stroke_r = cur_stroke_r; s->stroke_g = cur_stroke_g;
        s->stroke_b = cur_stroke_b; s->stroke_a = cur_stroke_a;
    }
    return JS_UNDEFINED;
}

static JSValue js_restore(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    skiac_canvas_restore(skia_canvas);
    /* Restore paint state */
    if (state_stack_idx > 0) {
        canvas_state_t *s = &state_stack[--state_stack_idx];
        cur_global_alpha = s->global_alpha;
        cur_line_width = s->line_width;
        cur_font_size = s->font_size;
        cur_fill_r = s->fill_r; cur_fill_g = s->fill_g;
        cur_fill_b = s->fill_b; cur_fill_a = s->fill_a;
        cur_stroke_r = s->stroke_r; cur_stroke_g = s->stroke_g;
        cur_stroke_b = s->stroke_b; cur_stroke_a = s->stroke_a;
        /* Reapply to Skia paints */
        float fa = (cur_fill_a / 255.0f) * cur_global_alpha;
        skiac_paint_set_color(fill_paint, cur_fill_r, cur_fill_g, cur_fill_b, (uint8_t)(fa * 255));
        float sa = (cur_stroke_a / 255.0f) * cur_global_alpha;
        skiac_paint_set_color(stroke_paint, cur_stroke_r, cur_stroke_g, cur_stroke_b, (uint8_t)(sa * 255));
        skiac_paint_set_stroke_width(stroke_paint, cur_line_width);
    }
    return JS_UNDEFINED;
}

static JSValue js_translate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    double x, y;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
    skiac_canvas_translate(skia_canvas, x, y);
    return JS_UNDEFINED;
}

static JSValue js_rotate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    double angle; JS_ToFloat64(ctx, &angle, argv[0]);
    skiac_canvas_rotate(skia_canvas, angle * 180.0f / M_PI);
    return JS_UNDEFINED;
}

static JSValue js_scale(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    double sx, sy;
    JS_ToFloat64(ctx, &sx, argv[0]); JS_ToFloat64(ctx, &sy, argv[1]);
    skiac_canvas_scale(skia_canvas, sx, sy);
    return JS_UNDEFINED;
}

static JSValue js_setTransform(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    if (argc >= 6) {
        double a, b, c, d, e, f;
        JS_ToFloat64(ctx, &a, argv[0]); JS_ToFloat64(ctx, &b, argv[1]);
        JS_ToFloat64(ctx, &c, argv[2]); JS_ToFloat64(ctx, &d, argv[3]);
        JS_ToFloat64(ctx, &e, argv[4]); JS_ToFloat64(ctx, &f, argv[5]);
        skiac_transform ts = {
            a * game_scale_x, b * game_scale_y,
            c * game_scale_x, d * game_scale_y,
            e * game_scale_x + game_offset_x,
            f * game_scale_y + game_offset_y
        };
        skiac_canvas_set_transform(skia_canvas, ts);
    }
    return JS_UNDEFINED;
}

static JSValue js_resetTransform(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    skiac_canvas_reset_transform(skia_canvas);
    reapply_game_scale();
    return JS_UNDEFINED;
}

/* putImageData */
static JSValue js_putImageData(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    JSValue img = argv[0];
    int32_t dx = 0, dy = 0;
    if (argc > 1) JS_ToInt32(ctx, &dx, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &dy, argv[2]);

    JSValue w_val = JS_GetPropertyStr(ctx, img, "width");
    JSValue h_val = JS_GetPropertyStr(ctx, img, "height");
    JSValue data_val = JS_GetPropertyStr(ctx, img, "data");

    int32_t w, h;
    JS_ToInt32(ctx, &w, w_val);
    JS_ToInt32(ctx, &h, h_val);

    size_t len, boff, blen;
    uint8_t *pixels = NULL;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &len, data_val);
    if (!buf) {
        JSValue ab = JS_GetTypedArrayBuffer(ctx, data_val, &boff, &blen, NULL);
        if (!JS_IsException(ab)) {
            buf = JS_GetArrayBuffer(ctx, &len, ab);
            JS_FreeValue(ctx, ab);
            if (buf) { buf += boff; len = blen; }
        }
    }
    pixels = buf;

    if (pixels) {
        skiac_canvas_put_image_data(skia_canvas, w, h, pixels,
                                     w * 4, w * h * 4,
                                     dx, dy, 0, 0, w, h, 0, 0);
    }

    JS_FreeValue(ctx, w_val);
    JS_FreeValue(ctx, h_val);
    JS_FreeValue(ctx, data_val);
    return JS_UNDEFINED;
}

/* drawImage — draws RGBA pixel data onto canvas with sub-rect + scaling support
 * Args: rgba_buffer, imgW, imgH, sx, sy, sw, sh, dx, dy, dw, dh
 * Uses a temp Skia surface to hold the source image, then draws via Skia canvas. */
static JSValue js_drawImage(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    if (argc < 5) return JS_UNDEFINED;

    /* Get pixel data */
    size_t len, boff, blen;
    uint8_t *pixels = NULL;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!buf) {
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &boff, &blen, NULL);
        if (!JS_IsException(ab)) {
            buf = JS_GetArrayBuffer(ctx, &len, ab);
            JS_FreeValue(ctx, ab);
            if (buf) { buf += boff; len = blen; }
        }
    }
    pixels = buf;
    if (!pixels) return JS_UNDEFINED;

    int32_t imgW, imgH;
    JS_ToInt32(ctx, &imgW, argv[1]);
    JS_ToInt32(ctx, &imgH, argv[2]);
    if (imgW <= 0 || imgH <= 0) return JS_UNDEFINED;

    double sx, sy, sw, sh, dx, dy, dw, dh;
    if (argc >= 11) {
        /* 9-arg form: sx,sy,sw,sh,dx,dy,dw,dh */
        JS_ToFloat64(ctx, &sx, argv[3]);
        JS_ToFloat64(ctx, &sy, argv[4]);
        JS_ToFloat64(ctx, &sw, argv[5]);
        JS_ToFloat64(ctx, &sh, argv[6]);
        JS_ToFloat64(ctx, &dx, argv[7]);
        JS_ToFloat64(ctx, &dy, argv[8]);
        JS_ToFloat64(ctx, &dw, argv[9]);
        JS_ToFloat64(ctx, &dh, argv[10]);
    } else if (argc >= 7) {
        /* 5-arg form: dx,dy,dw,dh */
        sx = 0; sy = 0; sw = imgW; sh = imgH;
        JS_ToFloat64(ctx, &dx, argv[3]);
        JS_ToFloat64(ctx, &dy, argv[4]);
        JS_ToFloat64(ctx, &dw, argv[5]);
        JS_ToFloat64(ctx, &dh, argv[6]);
    } else {
        /* 3-arg form: dx,dy */
        sx = 0; sy = 0; sw = imgW; sh = imgH;
        JS_ToFloat64(ctx, &dx, argv[3]);
        JS_ToFloat64(ctx, &dy, argv[4]);
        dw = imgW; dh = imgH;
    }

    /* Cache image surfaces by pixel data pointer — avoids per-call surface creation.
     * Images keep their _rgba buffer for their lifetime, so pointer is stable. */
    #define IMG_CACHE_SIZE 32
    static struct { uint8_t *key; skiac_surface *surf; int w, h; } img_cache[IMG_CACHE_SIZE];
    static int img_cache_count = 0;

    skiac_surface *img_surf = NULL;
    for (int i = 0; i < img_cache_count; i++) {
        if (img_cache[i].key == pixels && img_cache[i].w == imgW && img_cache[i].h == imgH) {
            img_surf = img_cache[i].surf;
            break;
        }
    }
    if (!img_surf) {
        /* First time seeing this image — create and cache surface */
        img_surf = skiac_surface_create_rgba_premultiplied(imgW, imgH, 0);
        if (!img_surf) return JS_UNDEFINED;
        skiac_canvas *img_canvas = skiac_surface_get_canvas(img_surf);
        skiac_canvas_write_pixels(img_canvas, imgW, imgH, pixels, imgW * 4, 0, 0);
        if (img_cache_count < IMG_CACHE_SIZE) {
            img_cache[img_cache_count].key = pixels;
            img_cache[img_cache_count].surf = img_surf;
            img_cache[img_cache_count].w = imgW;
            img_cache[img_cache_count].h = imgH;
            img_cache_count++;
        }
    }

    /* Draw from cached surface — no temp alloc, no flush needed */
    skiac_canvas_draw_image(skia_canvas, (void*)img_surf, 1 /* is_canvas */,
                             sx, sy, sw, sh, dx, dy, dw, dh,
                             1 /* smoothing */, 1 /* filter_quality */, fill_paint);
    return JS_UNDEFINED;
}

/* Style setters */
static JSValue js_setFillStyle(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    const char *str = JS_ToCString(ctx, argv[0]);
    if (str) {
        uint8_t r, g, b, a;
        parse_color_rgba(str, &r, &g, &b, &a);
        cur_fill_r = r; cur_fill_g = g; cur_fill_b = b; cur_fill_a = a;
        float combined = (a / 255.0f) * cur_global_alpha;
        skiac_paint_set_color(fill_paint, r, g, b, (uint8_t)(combined * 255));
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

static JSValue js_setStrokeStyle(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    const char *str = JS_ToCString(ctx, argv[0]);
    if (str) {
        uint8_t r, g, b, a;
        parse_color_rgba(str, &r, &g, &b, &a);
        cur_stroke_r = r; cur_stroke_g = g; cur_stroke_b = b; cur_stroke_a = a;
        float combined = (a / 255.0f) * cur_global_alpha;
        skiac_paint_set_color(stroke_paint, r, g, b, (uint8_t)(combined * 255));
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

static JSValue js_setGlobalAlpha(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    double a; JS_ToFloat64(ctx, &a, argv[0]);
    cur_global_alpha = (float)a;
    /* Reapply colors with new globalAlpha */
    float fa = (cur_fill_a / 255.0f) * cur_global_alpha;
    skiac_paint_set_color(fill_paint, cur_fill_r, cur_fill_g, cur_fill_b, (uint8_t)(fa * 255));
    float sa = (cur_stroke_a / 255.0f) * cur_global_alpha;
    skiac_paint_set_color(stroke_paint, cur_stroke_r, cur_stroke_g, cur_stroke_b, (uint8_t)(sa * 255));
    return JS_UNDEFINED;
}

static JSValue js_setLineWidth(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    double lw; JS_ToFloat64(ctx, &lw, argv[0]);
    cur_line_width = (float)lw;
    skiac_paint_set_stroke_width(stroke_paint, lw);
    return JS_UNDEFINED;
}

static JSValue js_setFont(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    const char *str = JS_ToCString(ctx, argv[0]);
    if (str) {
        /* Parse "16px monospace" or "bold 24px Arial" */
        float size = 10;
        const char *p = str;
        /* Skip font-style and font-weight */
        while (*p && (*p < '0' || *p > '9')) p++;
        if (*p) {
            sscanf(p, "%fpx", &size);
            cur_font_size = size;
        }
        /* Extract family after "px " */
        const char *fam = strstr(p, "px ");
        if (fam) {
            fam += 3;
            while (*fam == ' ') fam++;
            strncpy(cur_font_family, fam, sizeof(cur_font_family) - 1);
            cur_font_family[sizeof(cur_font_family) - 1] = '\0';
            /* Remove quotes */
            char *q;
            while ((q = strchr(cur_font_family, '\''))) *q = ' ';
            while ((q = strchr(cur_font_family, '"'))) *q = ' ';
        }
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

/* Font loading from .wasc assets */
static JSValue js_loadFont(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    ensure_skia_init();
    const char *family = JS_ToCString(ctx, argv[0]);
    /* argv[1] = font data (ArrayBuffer) */
    size_t len, boff, blen;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &len, argv[1]);
    if (!buf) {
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[1], &boff, &blen, NULL);
        if (!JS_IsException(ab)) {
            buf = JS_GetArrayBuffer(ctx, &len, ab);
            JS_FreeValue(ctx, ab);
            if (buf) { buf += boff; len = blen; }
        }
    }
    if (buf && family) {
        /* Register with stb_truetype (actual glyph rendering) */
        skiac_wasm_register_font_named(buf, len, family);
        JS_FreeCString(ctx, family);
        return JS_NewUint32(ctx, 1);
    }
    if (family) JS_FreeCString(ctx, family);
    return JS_NewUint32(ctx, 0);
}

/* ── Registration ────────────────────────────────────────────────── */

void register_canvas2d_native(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue c2d = JS_NewObject(ctx);

    #define REG(name, func, nargs) \
        JS_SetPropertyStr(ctx, c2d, name, JS_NewCFunction(ctx, func, name, nargs))

    REG("clearRect", js_clearRect, 4);
    REG("fillRect", js_fillRect, 4);
    REG("strokeRect", js_strokeRect, 4);
    REG("fillText", js_fillText, 3);
    REG("strokeText", js_strokeText, 3);
    REG("measureText", js_measureText, 1);
    REG("beginPath", js_beginPath, 0);
    REG("closePath", js_closePath, 0);
    REG("moveTo", js_moveTo, 2);
    REG("lineTo", js_lineTo, 2);
    REG("arc", js_arc, 5);
    REG("arcTo", js_arcTo, 5);
    REG("quadraticCurveTo", js_quadraticCurveTo, 4);
    REG("bezierCurveTo", js_bezierCurveTo, 6);
    REG("rect", js_rect, 4);
    REG("fill", js_fill, 0);
    REG("stroke", js_stroke, 0);
    REG("clip", js_clip, 0);
    REG("save", js_save, 0);
    REG("restore", js_restore, 0);
    REG("translate", js_translate, 2);
    REG("rotate", js_rotate, 1);
    REG("scale", js_scale, 2);
    REG("setTransform", js_setTransform, 6);
    REG("resetTransform", js_resetTransform, 0);
    REG("putImageData", js_putImageData, 3);
    REG("drawImage", js_drawImage, 11);
    REG("_setFillStyle", js_setFillStyle, 1);
    REG("_setStrokeStyle", js_setStrokeStyle, 1);
    REG("_setGlobalAlpha", js_setGlobalAlpha, 1);
    REG("_setLineWidth", js_setLineWidth, 1);
    REG("_setFont", js_setFont, 1);
    REG("_loadFont", js_loadFont, 2);

    #undef REG

    JS_SetPropertyStr(ctx, global, "_wcC2D", c2d);
    JS_FreeValue(ctx, global);
}
