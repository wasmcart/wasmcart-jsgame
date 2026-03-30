/*
 * skia_wasm_fix.cpp — Text rendering via stb_truetype for WASM
 *
 * FreeType compiled to WASM has a cmap parsing bug (unicharToGlyph returns 0
 * for all characters despite 6253 glyphs loaded). Rather than debug FreeType's
 * WASM port, we use stb_truetype for text rendering. stb_truetype is proven
 * to work in WASM across all wasmcart carts.
 *
 * Skia handles everything else (paths, gradients, transforms, compositing).
 * Text is rasterized by stb_truetype into a bitmap, then blitted to the
 * Skia surface via putImageData.
 */

#include <include/core/SkCanvas.h>
#include <include/core/SkSurface.h>
#include <include/core/SkPaint.h>
#include <include/core/SkImage.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkData.h>
#include <include/core/SkSamplingOptions.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <cstring>
#include <cstdlib>

#define MAX_FONTS 16

typedef struct {
    stbtt_fontinfo info;
    unsigned char *data;
    char family[64];
    int loaded;
} wasm_font_t;

static wasm_font_t s_fonts[MAX_FONTS];
static int s_font_count = 0;
static int s_default_font = -1; /* index of first registered font */

extern "C" {

/* Register with optional family name */
int skiac_wasm_register_font_named(const uint8_t* data, unsigned int size,
                                     const char* family) {
    if (s_font_count >= MAX_FONTS) return 0;

    /* Skip very large fonts (color emoji, etc.) — stb_truetype can't
       render CBDT/CBLC bitmap emoji anyway, and loading 10MB+ fonts
       in WASM is too slow */
    if (size > 2 * 1024 * 1024) return 0;

    int idx = s_font_count;
    s_fonts[idx].data = (unsigned char *)malloc(size);
    memcpy(s_fonts[idx].data, data, size);

    if (!stbtt_InitFont(&s_fonts[idx].info, s_fonts[idx].data, 0)) {
        free(s_fonts[idx].data);
        s_fonts[idx].data = nullptr;
        return 0;
    }

    s_fonts[idx].loaded = 1;
    if (family) {
        strncpy(s_fonts[idx].family, family, sizeof(s_fonts[idx].family) - 1);
        s_fonts[idx].family[sizeof(s_fonts[idx].family) - 1] = '\0';
    } else {
        s_fonts[idx].family[0] = '\0';
    }

    if (s_default_font < 0) s_default_font = idx;
    s_font_count++;
    return 1;
}

int skiac_wasm_register_font(const uint8_t* data, unsigned int size) {
    return skiac_wasm_register_font_named(data, size, nullptr);
}

/* Find font by family name, fallback to default */
static stbtt_fontinfo* find_font(const char* family) {
    if (!family || !family[0]) {
        return s_default_font >= 0 ? &s_fonts[s_default_font].info : nullptr;
    }

    /* Search by family name (case-insensitive substring match) */
    for (int i = 0; i < s_font_count; i++) {
        if (s_fonts[i].loaded && s_fonts[i].family[0]) {
            /* Check if requested family contains or matches the registered name */
            if (strcasestr(family, s_fonts[i].family) ||
                strcasestr(s_fonts[i].family, family)) {
                return &s_fonts[i].info;
            }
        }
    }

    /* Try finding a font that has the requested glyph */
    /* (useful for emoji — find any font that has the glyph) */
    return s_default_font >= 0 ? &s_fonts[s_default_font].info : nullptr;
}

/* Codepoint → font index cache (avoids scanning all fonts every char) */
#define GLYPH_CACHE_SIZE 256
static struct { int codepoint; int font_idx; } s_glyph_cache[GLYPH_CACHE_SIZE];
static int s_glyph_cache_init = 0;

/* Find a font that can render a specific codepoint */
static stbtt_fontinfo* find_font_for_codepoint(int codepoint) {
    if (!s_glyph_cache_init) {
        memset(s_glyph_cache, 0xFF, sizeof(s_glyph_cache)); /* -1 = uncached */
        s_glyph_cache_init = 1;
    }

    /* Check cache first */
    int slot = codepoint & (GLYPH_CACHE_SIZE - 1);
    if (s_glyph_cache[slot].codepoint == codepoint && s_glyph_cache[slot].font_idx >= 0) {
        return &s_fonts[s_glyph_cache[slot].font_idx].info;
    }

    /* Search fonts — try default first (most common case) */
    if (s_default_font >= 0) {
        int glyph = stbtt_FindGlyphIndex(&s_fonts[s_default_font].info, codepoint);
        if (glyph != 0) {
            s_glyph_cache[slot] = { codepoint, s_default_font };
            return &s_fonts[s_default_font].info;
        }
    }

    /* Search remaining fonts */
    for (int i = 0; i < s_font_count; i++) {
        if (i == s_default_font) continue;
        if (s_fonts[i].loaded) {
            int glyph = stbtt_FindGlyphIndex(&s_fonts[i].info, codepoint);
            if (glyph != 0) {
                s_glyph_cache[slot] = { codepoint, i };
                return &s_fonts[i].info;
            }
        }
    }

    /* Fallback to default */
    if (s_default_font >= 0) {
        s_glyph_cache[slot] = { codepoint, s_default_font };
        return &s_fonts[s_default_font].info;
    }
    return nullptr;
}

/*
 * Draw text using stb_truetype rasterization → Skia surface blit.
 */
float skiac_wasm_draw_text(void* canvas_ptr, void* paint_ptr,
                            const char* text, unsigned int text_len,
                            float x, float y, float font_size, int baseline) {
    if (s_font_count == 0 || !canvas_ptr || !paint_ptr || !text || text_len == 0)
        return 0;

    auto canvas = reinterpret_cast<SkCanvas*>(canvas_ptr);
    auto paint = reinterpret_cast<const SkPaint*>(paint_ptr);

    stbtt_fontinfo *default_fi = find_font(nullptr);
    if (!default_fi) return 0;

    float scale = stbtt_ScaleForPixelHeight(default_fi, font_size);

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(default_fi, &ascent, &descent, &lineGap);

    /*
     * baseline modes:
     * 0 = alphabetic (default): y IS the baseline, glyphs drawn above
     * 1 = top: y is top of em box, baseline is below
     * 2 = middle: y is vertical center of em box
     * 3 = bottom: y is bottom of em box
     *
     * stb_truetype draws glyphs relative to baseline.
     * The glyph position is: gy = draw_y - ascent*scale + glyph_y_offset
     * So for alphabetic: draw_y = y (no adjustment needed, y IS baseline)
     * For top: draw_y = y + ascent*scale (move baseline down from top)
     * For middle: draw_y = y + (ascent+descent)*scale/2
     * For bottom: draw_y = y + descent*scale (descent is negative)
     */
    float draw_y = y;
    float asc_px = ascent * scale;
    float desc_px = descent * scale; /* negative */
    float font_height = asc_px - desc_px;

    /*
     * baseline_offset converts from baseline-relative to top-of-glyph:
     *   gy = baseline_y - asc_px + glyph_y0
     *
     * For alphabetic: input y IS the baseline → baseline_y = y
     * For top: input y is top of em → baseline_y = y + asc_px
     * For middle: input y is center → baseline_y = y + asc_px/2 - desc_px/2
     *   (center of em box = ascent/2 above baseline + |descent|/2 below)
     * For bottom: input y is bottom → baseline_y = y + desc_px (desc is negative)
     */
    float baseline_y;
    switch (baseline) {
    case 1: /* top — y is top of text, baseline is ascent below */
        baseline_y = y + asc_px;
        break;
    case 2: /* middle — y is vertical center */
        baseline_y = y + (asc_px + desc_px) / 2.0f;
        break;
    case 3: /* bottom */
        baseline_y = y + desc_px;
        break;
    default: /* alphabetic */
        baseline_y = y;
        break;
    }

    /* baseline_offset: stb_truetype glyph y0 is relative to baseline,
       so final position = baseline_y + y0 (y0 is negative for ascending) */
    float baseline_offset = 0; /* we'll use baseline_y directly */

    /* Get paint color */
    SkColor4f color = paint->getColor4f();
    uint8_t cr = (uint8_t)(color.fR * 255);
    uint8_t cg = (uint8_t)(color.fG * 255);
    uint8_t cb = (uint8_t)(color.fB * 255);

    /* Render each glyph — decode UTF-8 properly */
    float xpos = x;
    for (unsigned int i = 0; i < text_len; ) {
        /* Decode UTF-8 codepoint */
        int ch;
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            ch = c; i++;
        } else if (c < 0xE0) {
            ch = (c & 0x1F) << 6;
            if (i+1 < text_len) ch |= ((unsigned char)text[i+1] & 0x3F);
            i += 2;
        } else if (c < 0xF0) {
            ch = (c & 0x0F) << 12;
            if (i+1 < text_len) ch |= (((unsigned char)text[i+1] & 0x3F) << 6);
            if (i+2 < text_len) ch |= ((unsigned char)text[i+2] & 0x3F);
            i += 3;
        } else {
            ch = (c & 0x07) << 18;
            if (i+1 < text_len) ch |= (((unsigned char)text[i+1] & 0x3F) << 12);
            if (i+2 < text_len) ch |= (((unsigned char)text[i+2] & 0x3F) << 6);
            if (i+3 < text_len) ch |= ((unsigned char)text[i+3] & 0x3F);
            i += 4;
        }
        if (ch < 32) continue;

        /* Find a font that has this glyph (emoji fallback) */
        stbtt_fontinfo *fi = find_font_for_codepoint(ch);
        if (!fi) fi = default_fi;
        float glyph_scale = stbtt_ScaleForPixelHeight(fi, font_size);

        int advance, lsb;
        stbtt_GetCodepointHMetrics(fi, ch, &advance, &lsb);

        /* Skip bitmap rendering for codepoints that won't produce outlines
           (emoji above BMP, control chars, etc.) */
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        unsigned char *bitmap = nullptr;
        int glyph_idx = stbtt_FindGlyphIndex(fi, ch);
        if (glyph_idx > 0) {
            bitmap = stbtt_GetCodepointBitmap(
                fi, glyph_scale, glyph_scale, ch, &x1, &y1, &x0, &y0);
        }

        if (bitmap && x1 > 0 && y1 > 0) {
            /* Convert alpha bitmap to RGBA premultiplied for Skia */
            int bw = x1, bh = y1;
            uint8_t *rgba = (uint8_t *)malloc(bw * bh * 4);
            for (int p = 0; p < bw * bh; p++) {
                uint8_t a = bitmap[p];
                /* Premultiplied alpha */
                rgba[p * 4 + 0] = (cr * a) / 255;
                rgba[p * 4 + 1] = (cg * a) / 255;
                rgba[p * 4 + 2] = (cb * a) / 255;
                rgba[p * 4 + 3] = a;
            }

            /* Create Skia image from RGBA data */
            auto info = SkImageInfo::Make(bw, bh,
                kRGBA_8888_SkColorType, kPremul_SkAlphaType);
            auto sk_data = SkData::MakeWithCopy(rgba, bw * bh * 4);
            auto image = SkImages::RasterFromData(info, sk_data, bw * 4);
            if (image) {
                float gx = xpos + x0;
                float gy = baseline_y + y0;
                canvas->drawImage(image, gx, gy);
            }
            free(rgba);
            stbtt_FreeBitmap(bitmap, nullptr);
        }

        xpos += advance * glyph_scale;

        /* Kerning with next codepoint */
        if (i < text_len) {
            /* Peek at next UTF-8 codepoint for kerning */
            int next_ch = (unsigned char)text[i];
            if (next_ch >= 0xC0 && i+1 < text_len) {
                if (next_ch < 0xE0) next_ch = ((next_ch & 0x1F) << 6) | ((unsigned char)text[i+1] & 0x3F);
                else next_ch = '?';
            }
            int kern = stbtt_GetCodepointKernAdvance(fi, ch, next_ch);
            xpos += kern * scale;
        }
    }

    return xpos - x;
}

float skiac_wasm_measure_text(const char* text, unsigned int text_len,
                               float font_size) {
    if (s_font_count == 0 || !text || text_len == 0)
        return text_len * font_size * 0.6f;

    stbtt_fontinfo *fi = find_font(nullptr);
    if (!fi) return text_len * font_size * 0.6f;
    float scale = stbtt_ScaleForPixelHeight(fi, font_size);
    float width = 0;

    for (unsigned int i = 0; i < text_len; ) {
        /* Decode UTF-8 */
        int ch;
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) { ch = c; i++; }
        else if (c < 0xE0) { ch = (c&0x1F)<<6; if(i+1<text_len) ch|=((unsigned char)text[i+1]&0x3F); i+=2; }
        else if (c < 0xF0) { ch = (c&0x0F)<<12; if(i+1<text_len) ch|=(((unsigned char)text[i+1]&0x3F)<<6); if(i+2<text_len) ch|=((unsigned char)text[i+2]&0x3F); i+=3; }
        else { ch = (c&0x07)<<18; if(i+1<text_len) ch|=(((unsigned char)text[i+1]&0x3F)<<12); if(i+2<text_len) ch|=(((unsigned char)text[i+2]&0x3F)<<6); if(i+3<text_len) ch|=((unsigned char)text[i+3]&0x3F); i+=4; }

        int advance, lsb;
        stbtt_fontinfo *gfi = find_font_for_codepoint(ch);
        if (!gfi) gfi = fi;
        stbtt_GetCodepointHMetrics(gfi, ch, &advance, &lsb);
        width += advance * scale;
    }

    return width;
}

} /* extern "C" */
