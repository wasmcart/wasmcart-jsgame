#!/bin/bash
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
bash "$ROOT/pack_game.sh" "$HERE/" "$HERE/hello_audio.wasc" "Hello Audio"
