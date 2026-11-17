#!/bin/bash
#
# pack_game.sh — Pack a JS game into a .wasc cart
#
# Usage: bash pack_game.sh <game_dir> <output.wasc> [name]
#
# Bundles: cart.wasm + game JS/assets
# The game developer just writes standard JS — no C, no WASM toolchain.
#

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
# Sibling checkouts (emsdk, wasmcart, webaudio-node, wasmcart-skia,
# napi-canvas) are found under CLIEMU_ROOT, default this repo's parent.
# Set it when building from a git worktree that lives elsewhere.
CLIEMU_ROOT="${CLIEMU_ROOT:-$HERE/..}"

GAME_DIR="${1:?Usage: pack_game.sh <game_dir> <output.wasc> [name]}"
OUTPUT="${2:?Usage: pack_game.sh <game_dir> <output.wasc> [name]}"
NAME="${3:-JS Game}"

CART_WASM="${CART_WASM:-$HERE/build/cart.wasm}"   # build/cart-webgpu.wasm for a WebGPU game

if [ ! -f "$CART_WASM" ]; then
    echo "ERROR: cart.wasm not found. Run build.sh first."
    exit 1
fi

# Create temp directory with combined assets
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Copy game files (exclude .wasc and pack.sh)
cp -r "$GAME_DIR"/* "$TMPDIR/"
rm -f "$TMPDIR"/*.wasc "$TMPDIR"/pack.sh

# Bundle default font if game doesn't have one
if [ ! -f "$TMPDIR/fonts/DejaVuSans.ttf" ] && [ ! -f "$TMPDIR/DejaVuSans.ttf" ]; then
    mkdir -p "$TMPDIR/fonts"
    for f in /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
             /usr/share/fonts/truetype/freefont/FreeSans.ttf; do
        if [ -f "$f" ]; then
            cp "$f" "$TMPDIR/fonts/"
            break
        fi
    done
fi

echo "Game assets: $(find "$TMPDIR" -type f | wc -l) files"

# The cart's version is the GAME's: CART_VERSION if set, else the "version"
# in the game's package.json, else (wasmcart-pack 0.40.0 or later) the game
# directory's git checkout: the tag at a clean v<semver> tag, else
# <last tag>-dev.g<commit>. Never this SDK's version. Not semver = no cart.
VERSION_ARGS=(--version-from "$GAME_DIR")
if [ -n "${CART_VERSION:-}" ]; then
    VERSION_ARGS=(--version "$CART_VERSION")
elif [ -f "$GAME_DIR/package.json" ]; then
    PKG_VERSION="$(node -e "process.stdout.write(String(JSON.parse(require('fs').readFileSync(process.argv[1], 'utf8')).version ?? ''))" "$GAME_DIR/package.json")"
    [ -n "$PKG_VERSION" ] && VERSION_ARGS=(--version "$PKG_VERSION")
fi

# Pack with a sibling wasmcart checkout when there is one, else the
# published packer from npm (a fresh clone of this repo alone).
PACK=(npx -y wasmcart pack)
[ -f "$CLIEMU_ROOT/wasmcart/bin/wasmcart-pack.js" ] && PACK=(node "$CLIEMU_ROOT/wasmcart/bin/wasmcart-pack.js")
"${PACK[@]}" \
    --wasm "$CART_WASM" \
    --assets "$TMPDIR/" \
    --name "$NAME" \
    "${VERSION_ARGS[@]}" \
    -o "$OUTPUT"
