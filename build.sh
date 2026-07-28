#!/bin/bash
#
# build.sh — Build the QuickJS jsgame cart.wasm
#
# Output: build/cart.wasm — reusable JS game runtime
#
# Phase 1: QuickJS + browser API shims (console, timers, rAF, gamepad,
#           fetch, Image, Canvas stub, localStorage, performance)
#
# Future phases add: WebGL2 shim, Canvas 2D rasterizer, Web Audio

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# ── Paths ────────────────────────────────────────────────────────

EMSDK_ROOT="$(cd ../emsdk && pwd)"
source "$EMSDK_ROOT/emsdk_env.sh" 2>/dev/null || true

QUICKJS_SRC="/tmp/quickjs"
WASMCART_H="$HERE/../wasmcart-examples/hello/wasmcart.h"
PORTING="$HERE/../wasmcart/porting"

if [ ! -f "$QUICKJS_SRC/quickjs.c" ]; then
    echo "QuickJS not found at $QUICKJS_SRC"
    echo "Run: bash setup_quickjs.sh"
    exit 1
fi

mkdir -p build obj

# ── Step 1: Compile QuickJS ──────────────────────────────────────

echo "=== Compiling QuickJS ==="

QUICKJS_CFLAGS="-O2 -DCONFIG_VERSION=\"2024\" \
    -D_GNU_SOURCE \
    -DEMSCRIPTEN \
    -Wno-implicit-function-declaration \
    -Wno-sign-compare \
    -Wno-unused-variable \
    -Wno-unused-but-set-variable"

# Core QuickJS files
emcc $QUICKJS_CFLAGS -c "$QUICKJS_SRC/quickjs.c" -o obj/quickjs.o
emcc $QUICKJS_CFLAGS -c "$QUICKJS_SRC/libregexp.c" -o obj/libregexp.o
emcc $QUICKJS_CFLAGS -c "$QUICKJS_SRC/libunicode.c" -o obj/libunicode.o
# cutils.c exists in bellard/quickjs but was merged away in quickjs-ng >=0.11.
# Conditional, like dtoa.c below, so either engine builds.
rm -f obj/cutils.o
if [ -f "$QUICKJS_SRC/cutils.c" ]; then
    emcc $QUICKJS_CFLAGS -c "$QUICKJS_SRC/cutils.c" -o obj/cutils.o
fi
if [ -f "$QUICKJS_SRC/dtoa.c" ]; then
    emcc $QUICKJS_CFLAGS -c "$QUICKJS_SRC/dtoa.c" -o obj/dtoa.o
fi

echo "  QuickJS compiled"

# ── Step 2: Compile cart shim + stubs ────────────────────────────

CART_CFLAGS="-O2 -I$QUICKJS_SRC -I$(dirname $WASMCART_H) -I$PORTING/include"

echo "=== Compiling cart main ==="
emcc $CART_CFLAGS -c src/cart_main.c -o obj/cart_main.o

echo "=== Compiling WebGL shim (Phase 1b) ==="
emcc $CART_CFLAGS -c src/webgl_shim.c -o obj/webgl_shim.o

SKIA_DIR="$HERE/../wasmcart-skia/out"

echo "=== Compiling Canvas 2D (Skia-backed, Phase 3) ==="
if [ -f "$SKIA_DIR/libskia.a" ]; then
    SKIA_CFLAGS="$CART_CFLAGS -I$SKIA_DIR/include -I$HERE/../napi-canvas/skia"
    emcc $SKIA_CFLAGS -c src/canvas2d_skia.c -o obj/canvas2d.o
    WASMCART_H_DIR="$(dirname $WASMCART_H)"
    SKIA_CXX="-O2 -std=c++20 -fno-exceptions -fno-rtti -DSK_RELEASE -DSK_DISABLE_TRACING -DSK_NO_GL -I$HERE/../napi-canvas/skia -I$PORTING/include -I$WASMCART_H_DIR"
    SKIA_GL_CXX="-O2 -std=c++20 -fno-exceptions -fno-rtti -DSK_RELEASE -DSK_DISABLE_TRACING -I$HERE/../napi-canvas/skia -I$PORTING/include -I$WASMCART_H_DIR"
    em++ $SKIA_CXX -c src/skia_wasm_fix.cpp -o obj/skia_wasm_fix.o
    em++ $SKIA_CXX -c src/skia_path_reset.cpp -o obj/skia_path_reset.o
    echo "=== Compiling Skia GL surface (Ganesh) ==="
    em++ $SKIA_GL_CXX -c src/skia_gl_surface.cpp -o obj/skia_gl_surface.o
    SKIA_LIBS="$SKIA_DIR/libskiac.a $SKIA_DIR/libskia.a $SKIA_DIR/libskshaper.a $SKIA_DIR/libskparagraph.a $SKIA_DIR/libskunicode_core.a $SKIA_DIR/libskunicode_icu.a $SKIA_DIR/libharfbuzz.a $SKIA_DIR/libicu.a $SKIA_DIR/libfreetype2.a $SKIA_DIR/libpng.a $SKIA_DIR/libjpeg.a $SKIA_DIR/libwebp.a $SKIA_DIR/libwuffs.a $SKIA_DIR/libzlib.a $SKIA_DIR/libskcms.a"
else
    echo "  WARNING: Skia libs not found, using fallback bitmap rasterizer"
    emcc $CART_CFLAGS -c src/canvas2d.c -o obj/canvas2d.o
    SKIA_LIBS=""
fi

echo "=== Compiling Audio shim (Phase 2) ==="
emcc $CART_CFLAGS -c src/audio_shim.c -o obj/audio_shim.o

echo "=== Compiling image decoder (stb_image) ==="
emcc $CART_CFLAGS -c src/image_decode.c -o obj/image_decode.o

echo "=== Compiling Worker shim (WASI threads) ==="
emcc $CART_CFLAGS -c src/worker_shim.c -o obj/worker_shim.o

echo "=== Compiling stubs ==="
emcc -O2 -c src/stubs.c -o obj/stubs.o

# ── Step 3b: Build webaudio lib if needed ────────────────────────

if [ ! -f obj/libwebaudio.a ]; then
    echo "=== Building libwebaudio.a ==="
    bash build_webaudio_lib.sh
fi

# emstubs for Emscripten runtime stubs
if [ -f "$PORTING/emstubs.c" ]; then
    emcc -O2 -c "$PORTING/emstubs.c" -o obj/emstubs.o
fi

# ── Step 3: Link ─────────────────────────────────────────────────

echo "=== Linking cart.wasm ==="

SKIA_FIX=""
if [ -f obj/skia_wasm_fix.o ]; then SKIA_FIX="obj/skia_wasm_fix.o obj/skia_path_reset.o "; fi
SKIA_GL=""
if [ -f obj/skia_gl_surface.o ]; then SKIA_GL="obj/skia_gl_surface.o "; fi
OBJS="obj/cart_main.o obj/webgl_shim.o obj/canvas2d.o $SKIA_FIX $SKIA_GL obj/image_decode.o obj/audio_shim.o obj/quickjs.o obj/libregexp.o obj/libunicode.o obj/stubs.o obj/libwebaudio.a $SKIA_LIBS"
if [ -f obj/cutils.o ]; then
    OBJS="$OBJS obj/cutils.o"
fi
if [ -f obj/dtoa.o ]; then
    OBJS="$OBJS obj/dtoa.o"
fi
if [ -f obj/emstubs.o ]; then
    OBJS="$OBJS obj/emstubs.o"
fi

# -sASSERTIONS=1 is LOAD-BEARING, not a debug leftover. Without it a -O2 build
# faults with "memory access out of bounds" on frame 0 of any cart that draws a
# second fillRect alongside arc/strokeRect/fillText -- hello_canvas, for one.
# Bisected: -O2 alone crashes, -O2 -sASSERTIONS=1 does not, and it is neither
# the link level (-O1 link still crashes) nor the stack (16MB still crashes).
# That pattern -- behaviour changing with optimisation -- means undefined
# behaviour somewhere in the C/C++ that ASSERTIONS happens to mask. The real
# bug is NOT fixed; this keeps the tree buildable while it is hunted.
emcc -O2 -sASSERTIONS=1 \
    -sSTANDALONE_WASM=1 \
    \
    -sALLOW_MEMORY_GROWTH=1 \
    -sINITIAL_MEMORY=268435456 \
    -sMAXIMUM_MEMORY=1073741824 \
    -sERROR_ON_UNDEFINED_SYMBOLS=0 \
    -sTOTAL_STACK=8388608 \
    `# was TOTAL_STACK, renamed in emscripten 3.1.27 and silently ignored since` \
    --no-entry \
    -sEXPORTED_FUNCTIONS='["_wc_get_info","_wc_init","_wc_render"]' \
    $OBJS \
    -o build/cart.wasm

WASM_SIZE=$(wc -c < build/cart.wasm)
echo ""
echo "=== Build complete ==="
echo "  build/cart.wasm ($WASM_SIZE bytes, $(( WASM_SIZE / 1024 / 1024 )) MB)"
echo ""
echo "Pack a game:"
echo "  bash pack_game.sh my_game/ my_game.wasc \"My Game\""
