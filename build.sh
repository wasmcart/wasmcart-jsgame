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
# Sibling checkouts (emsdk, wasmcart, webaudio-node, wasmcart-skia,
# napi-canvas) are found under CLIEMU_ROOT, default this repo's parent.
# Set it when building from a git worktree that lives elsewhere.
CLIEMU_ROOT="${CLIEMU_ROOT:-$HERE/..}"
cd "$HERE"

# ── Paths ────────────────────────────────────────────────────────

EMSDK_ROOT="$(cd "$CLIEMU_ROOT/emsdk" && pwd)"
source "$EMSDK_ROOT/emsdk_env.sh" 2>/dev/null || true

# QuickJS lives in the repo, NOT /tmp: /tmp wipes on reboot and took the whole
# JS engine source with it. Override with QUICKJS_SRC=/path for a shared checkout.
QUICKJS_SRC="${QUICKJS_SRC:-$HERE/vendor/quickjs}"
# The ABI header comes from a wasmcart checkout's include/ (WASMCART_REPO
# overrides; default sibling). It used to be read out of wasmcart-examples'
# hello cart -- a copy of a copy that happened to be current.
WASMCART_REPO="${WASMCART_REPO:-$CLIEMU_ROOT/wasmcart}"
WASMCART_H="$WASMCART_REPO/include/wasmcart.h"
if [ ! -f "$WASMCART_H" ]; then
    echo "wasmcart ABI header not found at $WASMCART_H (set WASMCART_REPO)"
    exit 1
fi

if [ ! -f "$QUICKJS_SRC/quickjs.c" ]; then
    echo "QuickJS not found at $QUICKJS_SRC"
    echo "Run: bash setup_quickjs.sh"
    exit 1
fi

mkdir -p build obj

# ── Step 1: Compile QuickJS ──────────────────────────────────────

echo "=== Compiling QuickJS ==="

QUICKJS_CFLAGS="-O2 -DCONFIG_VERSION=\"2024\" \
    `# keep the builder's absolute paths out of the wasm -- see CART_CFLAGS` \
    -ffile-prefix-map=$QUICKJS_SRC=quickjs \
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

# -ffile-prefix-map keeps the BUILDER'S absolute paths out of the shipped
# binary. assert() and __FILE__ bake the full source path into cart.wasm, so
# without this every .wasc published from this tree leaks a local directory
# layout (/home/<user>/code/...). Maps to short logical roots instead, which
# also makes the wasm byte-identical across machines.
CART_CFLAGS="-O2 -I$QUICKJS_SRC -I$(dirname $WASMCART_H) \
  -ffile-prefix-map=$QUICKJS_SRC=quickjs \
  -ffile-prefix-map=$CLIEMU_ROOT/webaudio-node=webaudio-node \
  -ffile-prefix-map=$HERE=."

echo "=== Compiling cart main ==="
emcc $CART_CFLAGS -c src/cart_main.c -o obj/cart_main.o

echo "=== Compiling WebGL shim (Phase 1b) ==="
emcc $CART_CFLAGS -c src/webgl_shim.c -o obj/webgl_shim.o

SKIA_DIR="$CLIEMU_ROOT/wasmcart-skia/out"

echo "=== Compiling Canvas 2D (Skia-backed, Phase 3) ==="
if [ -f "$SKIA_DIR/libskia.a" ]; then
    SKIA_CFLAGS="$CART_CFLAGS -I$SKIA_DIR/include -I$CLIEMU_ROOT/napi-canvas/skia"
    emcc $SKIA_CFLAGS -c src/canvas2d_skia.c -o obj/canvas2d.o
    WASMCART_H_DIR="$(dirname $WASMCART_H)"
    SKIA_CXX="-O2 -std=c++20 -fno-exceptions -fno-rtti -DSK_RELEASE -DSK_DISABLE_TRACING -DSK_NO_GL -I$CLIEMU_ROOT/napi-canvas/skia -I$WASMCART_H_DIR"
    SKIA_GL_CXX="-O2 -std=c++20 -fno-exceptions -fno-rtti -DSK_RELEASE -DSK_DISABLE_TRACING -I$CLIEMU_ROOT/napi-canvas/skia -I$WASMCART_H_DIR"
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

# ── Step 3: Link ─────────────────────────────────────────────────

echo "=== Linking cart.wasm ==="

SKIA_FIX=""
if [ -f obj/skia_wasm_fix.o ]; then SKIA_FIX="obj/skia_wasm_fix.o obj/skia_path_reset.o "; fi
SKIA_GL=""
if [ -f obj/skia_gl_surface.o ]; then SKIA_GL="obj/skia_gl_surface.o "; fi
# worker_shim.o was compiled above but left OUT of this list, so the whole
# Worker implementation never linked: `new Worker(...)` threw
# "_wcWorkerCreate is not defined" at runtime. ERROR_ON_UNDEFINED_SYMBOLS=0
# (needed for the Skia/GL stubs) is why the link stayed silent about it.
OBJS="obj/cart_main.o obj/webgl_shim.o obj/canvas2d.o $SKIA_FIX $SKIA_GL obj/image_decode.o obj/audio_shim.o obj/worker_shim.o obj/quickjs.o obj/libregexp.o obj/libunicode.o obj/stubs.o obj/libwebaudio.a $SKIA_LIBS"
if [ -f obj/cutils.o ]; then
    OBJS="$OBJS obj/cutils.o"
fi
if [ -f obj/dtoa.o ]; then
    OBJS="$OBJS obj/dtoa.o"
fi

emcc -O2 \
    -sSTANDALONE_WASM=1 \
    \
    -sALLOW_MEMORY_GROWTH=1 \
    -sINITIAL_MEMORY=268435456 \
    `# 2GB, not 1GB: space decodes 21MB of .ogg to f32 PCM (~25x expansion),` \
    `# which blew a 1GB ceiling during load with a bare "memory access out of` \
    `# bounds". Bisected file-by-file: 7 music files loaded, the 8th did not.` \
    -sMAXIMUM_MEMORY=2147483648 \
    -sERROR_ON_UNDEFINED_SYMBOLS=0 \
    -sSTACK_SIZE=8388608 \
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
