#!/bin/bash
#
# build_webgpu.sh - Build the WebGPU jsgame runtime: build/cart-webgpu.wasm
#
# The same QuickJS runtime and browser shims as build.sh, with navigator.gpu
# in place of WebGL2 and Canvas 2D. The cart links Dawn's emdawnwebgpu port
# (webgpu.h); the host supplies its JavaScript half and owns the device.
# wc_info_t.gpu_api is 2.
#
# NOT in this runtime: WebGL, and Canvas 2D (getContext('2d') returns null).
# Both go through GL imports, and a WebGPU cart has none.
#
# Needs the pinned emdawnwebgpu package (the release the host's glue was
# generated from; see the wasmcart repo's scripts/wgpu/emdawnwebgpu.json):
#   EMDAWNWEBGPU_PKG=/path/to/emdawnwebgpu_pkg bash build_webgpu.sh
#
# Pack a game with it:
#   CART_WASM=build/cart-webgpu.wasm bash pack_game.sh my_game/ my_game.wasc "My Game"

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
# Sibling checkouts (emsdk, wasmcart, webaudio-node) are found under
# CLIEMU_ROOT, default this repo's parent.
CLIEMU_ROOT="${CLIEMU_ROOT:-$HERE/..}"

EMSDK_ROOT="$(cd "$CLIEMU_ROOT/emsdk" && pwd)"
source "$EMSDK_ROOT/emsdk_env.sh" >/dev/null 2>&1 || true

QUICKJS_SRC="${QUICKJS_SRC:-$HERE/vendor/quickjs}"
WASMCART_REPO="${WASMCART_REPO:-$CLIEMU_ROOT/wasmcart}"
WASMCART_H="$WASMCART_REPO/include/wasmcart.h"
EMDAWNWEBGPU_PKG="${EMDAWNWEBGPU_PKG:-$CLIEMU_ROOT/scratch/emdawnwebgpu/emdawnwebgpu_pkg}"
PORT="$EMDAWNWEBGPU_PKG/emdawnwebgpu.port.py"

for f in "$QUICKJS_SRC/quickjs.c" "$WASMCART_H" "$PORT"; do
    [ -f "$f" ] || { echo "not found: $f (see the header of this script)"; exit 1; }
done

# The binding is generated from the same package the cart links against, so
# the two cannot drift apart.
node tools/gen_webgpu_bindings.mjs "$EMDAWNWEBGPU_PKG" src/webgpu_bindings.gen.c

mkdir -p build obj-webgpu

QJS_CFLAGS="-O2 -DCONFIG_VERSION=\"2024\" -ffile-prefix-map=$QUICKJS_SRC=quickjs \
    -D_GNU_SOURCE -DEMSCRIPTEN -Wno-implicit-function-declaration -Wno-sign-compare \
    -Wno-unused-variable -Wno-unused-but-set-variable"
CART_CFLAGS="-O2 -DJSGAME_WEBGPU -I$QUICKJS_SRC -I$(dirname "$WASMCART_H") \
    -ffile-prefix-map=$QUICKJS_SRC=quickjs \
    -ffile-prefix-map=$CLIEMU_ROOT/webaudio-node=webaudio-node \
    -ffile-prefix-map=$EMDAWNWEBGPU_PKG=emdawnwebgpu \
    -ffile-prefix-map=$HERE=."

echo "=== Compiling QuickJS ==="
QJS_OBJS=""
for f in quickjs libregexp libunicode cutils dtoa; do
    if [ -f "$QUICKJS_SRC/$f.c" ]; then
        if [ ! -f "obj-webgpu/$f.o" ] || [ "$QUICKJS_SRC/$f.c" -nt "obj-webgpu/$f.o" ]; then
            emcc $QJS_CFLAGS -c "$QUICKJS_SRC/$f.c" -o "obj-webgpu/$f.o"
        fi
        QJS_OBJS="$QJS_OBJS obj-webgpu/$f.o"
    fi
done

echo "=== Compiling runtime (WebGPU) ==="
emcc $CART_CFLAGS -c src/cart_main.c -o obj-webgpu/cart_main.o
emcc $CART_CFLAGS --use-port="$PORT" -c src/webgpu_shim.c -o obj-webgpu/webgpu_shim.o
emcc $CART_CFLAGS -c src/audio_shim.c -o obj-webgpu/audio_shim.o
emcc $CART_CFLAGS -c src/image_decode.c -o obj-webgpu/image_decode.o
emcc $CART_CFLAGS -c src/worker_shim.c -o obj-webgpu/worker_shim.o
emcc -O2 -c src/stubs.c -o obj-webgpu/stubs.o

if [ ! -f obj/libwebaudio.a ]; then
    echo "=== Building libwebaudio.a ==="
    CLIEMU_ROOT="$CLIEMU_ROOT" bash build_webaudio_lib.sh
fi

echo "=== Linking cart-webgpu.wasm ==="
emcc -O2 \
    --use-port="$PORT" \
    -sSTANDALONE_WASM=1 \
    -sALLOW_MEMORY_GROWTH=1 \
    -sINITIAL_MEMORY=268435456 \
    -sMAXIMUM_MEMORY=2147483648 \
    -sERROR_ON_UNDEFINED_SYMBOLS=0 \
    -sSTACK_SIZE=8388608 \
    --no-entry $EXTRA_LDFLAGS \
    -sEXPORTED_FUNCTIONS='["_wc_get_info","_wc_init","_wc_render"]' \
    obj-webgpu/cart_main.o obj-webgpu/webgpu_shim.o obj-webgpu/audio_shim.o \
    obj-webgpu/image_decode.o obj-webgpu/worker_shim.o obj-webgpu/stubs.o \
    $QJS_OBJS obj/libwebaudio.a \
    -o build/cart-webgpu.wasm

# A GL import would make the host treat this as a dual GL/WebGPU cart.
node -e '
const m = new WebAssembly.Module(require("fs").readFileSync(process.argv[1]));
const imps = WebAssembly.Module.imports(m);
const gl = imps.filter(i => i.module === "gl" || /^(gl[A-Z]|emscripten_gl)/.test(i.name));
if (gl.length) { console.error("GL imports in the WebGPU runtime:", gl.map(i => i.name).join(", ")); process.exit(1); }
const w = imps.filter(i => /^(wgpu|emwgpu)[A-Z]|^emscripten_webgpu_/.test(i.name));
console.log("  " + w.length + " WebGPU imports, no GL imports");
' build/cart-webgpu.wasm

echo "  build/cart-webgpu.wasm ($(wc -c < build/cart-webgpu.wasm) bytes)"
