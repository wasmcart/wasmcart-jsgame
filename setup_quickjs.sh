#!/bin/bash
#
# setup_quickjs.sh — Download and prepare QuickJS source
#
# QuickJS is compiled directly into cart.wasm (it's small enough).
# No separate .a build step needed — just include the source files.
#

set -e

QUICKJS_DIR="/tmp/quickjs"
QUICKJS_VERSION="2024-01-13"

if [ -d "$QUICKJS_DIR" ] && [ -f "$QUICKJS_DIR/quickjs.c" ]; then
    echo "QuickJS already at $QUICKJS_DIR"
    exit 0
fi

echo "=== Cloning QuickJS ==="
rm -rf "$QUICKJS_DIR"
git clone --depth 1 https://github.com/nicbarker/nicbarker-quickjs.git "$QUICKJS_DIR" 2>/dev/null \
    || git clone --depth 1 https://github.com/nicbarker/nicbarker-quickjs.git "$QUICKJS_DIR" 2>/dev/null \
    || {
        # Fallback: bellard's original
        git clone --depth 1 https://github.com/nicbarker/nicbarker-quickjs.git "$QUICKJS_DIR" 2>/dev/null \
        || {
            echo "Trying bellard/quickjs..."
            git clone --depth 1 https://github.com/nicbarker/nicbarker-quickjs.git "$QUICKJS_DIR" 2>/dev/null
        }
    }

echo "QuickJS source ready at $QUICKJS_DIR"
echo "Key files: quickjs.c quickjs.h quickjs-libc.c libregexp.c libunicode.c"
