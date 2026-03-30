/*
 * image_decode.c — Decode PNG/JPEG/BMP images via stb_image
 *
 * stb_image is proven to work in WASM across all wasmcart carts.
 * Once the Skia WASM codec global constructor issue is resolved,
 * this can be replaced with Skia's SkCodec for full format support.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

/*
 * Decode image from raw bytes → RGBA pixels.
 * Args: ArrayBuffer of raw image data (JPEG/PNG/BMP/GIF)
 * Returns: { width, height, data: ArrayBuffer(RGBA), _rgba: Uint8Array } or null
 */
static JSValue js_decode_image(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;

    size_t len, boff, blen;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!buf) {
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &boff, &blen, NULL);
        if (!JS_IsException(ab)) {
            buf = JS_GetArrayBuffer(ctx, &len, ab);
            JS_FreeValue(ctx, ab);
            if (buf) { buf += boff; len = blen; }
        }
    }
    if (!buf || len == 0) return JS_NULL;

    int w, h, channels;
    uint8_t *pixels = stbi_load_from_memory(buf, len, &w, &h, &channels, 4);
    if (!pixels) return JS_NULL;

    int size = w * h * 4;
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, result, "height", JS_NewInt32(ctx, h));

    JSValue ab_out = JS_NewArrayBufferCopy(ctx, pixels, size);
    JS_SetPropertyStr(ctx, result, "data", ab_out);

    /* Create _rgba Uint8Array view for drawImage compatibility */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue uint8_ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JSValue rgba = JS_CallConstructor(ctx, uint8_ctor, 1, &ab_out);
    JS_FreeValue(ctx, uint8_ctor);
    JS_FreeValue(ctx, global);
    JS_SetPropertyStr(ctx, result, "_rgba", rgba);

    stbi_image_free(pixels);
    return result;
}

void register_image_decode(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "_wcDecodeImage",
        JS_NewCFunction(ctx, js_decode_image, "_wcDecodeImage", 1));
    JS_FreeValue(ctx, global);
}
