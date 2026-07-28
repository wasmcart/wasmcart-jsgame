#!/bin/bash
#
# setup_quickjs.sh — Download and prepare QuickJS source
#
# QuickJS is compiled directly into cart.wasm (it's small enough).
# No separate .a build step needed — just include the source files.
#

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"

# QuickJS lives in the repo, NOT /tmp: /tmp wipes on reboot and took the whole
# JS engine source with it. Override with QUICKJS_DIR=/path for a shared checkout.
QUICKJS_DIR="${QUICKJS_DIR:-$HERE/vendor/quickjs}"
# NOTE: this was previously set to "2024-01-13" and NEVER USED -- the clone
# below always took HEAD, so nothing was actually pinned to it. Set
# QUICKJS_REF to a tag/commit for a real pin, or leave empty to track HEAD.
QUICKJS_REF="${QUICKJS_REF:-}"

if [ -d "$QUICKJS_DIR" ] && [ -f "$QUICKJS_DIR/quickjs.c" ]; then
    echo "QuickJS already at $QUICKJS_DIR"
    exit 0
fi

# NOTE: the previous version listed four "fallbacks" that were all the SAME
# dead URL (nicbarker/nicbarker-quickjs, which 404s -- git prompts for a
# username, GitHub's 404 for a missing repo). bellard/quickjs is upstream and
# live. Verified 2026-07-28.
echo "=== Cloning QuickJS ==="
rm -rf "$QUICKJS_DIR"
git clone --depth 1 https://github.com/bellard/quickjs.git "$QUICKJS_DIR"
if [ -n "$QUICKJS_REF" ]; then
    ( cd "$QUICKJS_DIR" && git fetch --depth 1 origin "$QUICKJS_REF" && git checkout -q FETCH_HEAD )
fi

echo "QuickJS source ready at $QUICKJS_DIR"
echo "Key files: quickjs.c quickjs.h quickjs-libc.c libregexp.c libunicode.c"
