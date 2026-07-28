# Rebuild status — the tree does not currently rebuild correctly

**The committed `build/cart.wasm` works. A fresh `./build.sh` produces a binary
that crashes.** Source changes are safe to make; they just cannot be shipped
until this is resolved. Written 2026-07-28 after bisecting.

## Symptom

A rebuilt cart dies with `RuntimeError: memory access out of bounds` on
**frame 0**, in the Skia Canvas 2D path. `hello_audio` and `hello_fetch` are
fine; `hello_canvas` is not.

## What the bisection established

Each of these was tested, not assumed:

| Suspect | Verdict |
|---|---|
| QuickJS version drift | **NOT it.** Crashes identically on bellard 2026-06 and quickjs-ng 0.15.1 |
| `stb_truetype.h` / `stb_image.h` | **NOT it.** Byte-identical (md5 `ce249f35`, v1.26) across wasmcart-lua, -mruby, -sdl2, -examples |
| Skia library staleness | **NOT it.** Every Skia object is rebuilt by `build.sh`; only `emstubs.o` and `libwebaudio.a` are stale, and those are the audio path, which works |
| emsdk drift | **NOT it.** Unchanged since May 18 (emscripten 5.0.0) |
| The packer | **NOT it.** Same `pack_game.sh`, same deflate ratios either side of the boundary |
| The game's drawing code | **NOT it.** See below |

## The decisive test

Take one game. Pack it twice — once against the **shipped** `cart.wasm`, once
against a **rebuilt** one. Nothing else differs:

```
SHIPPED runtime + 228-byte game -> OK
REBUILT runtime + same game     -> CRASH
```

So the fault is in what `./build.sh` produces today, not in games, assets, the
packer, or the engine.

## The red herring worth recording

Bisecting by drawing primitive produced a bizarre-looking result: `fillRect`
crashed while `arc` and `strokeRect` — which do strictly more Skia work —
passed. Then `fillRect(10,10,100,100)` passed while `fillRect(9,9,100,100)`
crashed.

It is not the geometry. Holding the drawing code **identical** and varying only
the file length with a padding comment gives an exact threshold:

```
227 bytes  CRASH      229 bytes  OK
228 bytes  CRASH      230 bytes  OK
```

Small assets simply land on the wrong side of whatever is corrupt. Anyone
re-bisecting this should vary one thing at a time and pad to fixed sizes, or
they will chase Canvas 2D primitives for an hour like I did.

## FIXED 2026-07-28: the Skia copy step was incomplete

`wasmcart-skia/build.sh` copied only FOUR archives by name
(libskia/libskshaper/libskparagraph/libskunicode_icu), so a fresh
`rm -rf out && ./build.sh` produced **5 archives where the working tree had
18**. The other 13 -- freetype, harfbuzz, icu, png, jpeg, webp, zlib, skcms,
wuffs, skunicode_core -- had been copied by hand at some point and never
scripted. The link here needs 16 of them.

Fixed to copy every `.a` the Skia build produces, with a hard failure if
`libskia.a` is absent. That turned a silent partial link into either a correct
build or a loud error.

**This fixed a real class of crash.** The 228-byte cart from the bisection below
now runs 60 frames clean where it previously died on frame 0.

## Still broken: hello_canvas

`hello_canvas` STILL crashes after the Skia fix, while `hello_audio`,
`hello_fetch` and the small synthetic carts all pass. So there is a second,
independent bug -- narrower than the first, and specific to something
hello_canvas does that the others do not (it is the only example using
`fillText` + `arc` + `strokeRect` together against the Ganesh GPU path).

Next step: bisect hello_canvas's own draw calls against the FIXED build, which
is now a meaningful exercise -- before the Skia fix the results were noise, as
the "red herring" section explains.

## What is left to check

The remaining unverified variable is the **Skia link**: `libskia.a` and friends
are from May 18, while the working `cart.wasm` was committed July 27. Every
`.o` gets rebuilt, but the prebuilt Skia archives do not. If Skia sources under
`../wasmcart-skia` changed after May 18, the archives are stale relative to the
headers `skia_c.hpp` exposes, and a fresh compile against newer headers linking
older archives is exactly this class of fault.

Next step: rebuild `../wasmcart-skia` (`rm -rf out && ./build.sh`, ~10-20 min,
needs the `napi-canvas` Skia checkout and depot_tools), then rebuild here.

## Note on the missing headers

`src/skia_wasm_fix.cpp` and `src/image_decode.c` include `stb_truetype.h` and
`stb_image.h`. Neither was in the tree, and the `-I../napi-canvas/skia` include
path does not contain them either — that is a full Skia checkout with no stb.
They are now vendored in `src/` (v1.26, byte-identical to every other wasmcart
repo). Without them a clean checkout cannot compile at all, which means the
committed binary was built on a machine where they existed somewhere else.
