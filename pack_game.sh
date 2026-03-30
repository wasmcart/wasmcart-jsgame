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

GAME_DIR="${1:?Usage: pack_game.sh <game_dir> <output.wasc> [name]}"
OUTPUT="${2:?Usage: pack_game.sh <game_dir> <output.wasc> [name]}"
NAME="${3:-JS Game}"

CART_WASM="$HERE/build/cart.wasm"

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

# Pack
node "$HERE/../wasmcart/bin/wasmcart-pack.js" \
    --wasm "$CART_WASM" \
    --assets "$TMPDIR/" \
    --name "$NAME" \
    -o "$OUTPUT"
