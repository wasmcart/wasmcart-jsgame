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

## Still broken: hello_canvas — bisected to a draw-call COUNT

Bisected against the fixed build. Cumulative, one draw call added at a time:

```
clearRect                            OK
+ fillRect (background)              OK
+ fillRect (title bar)               OK
+ fillText 20px                      OK
+ arc / fill                         OK
+ strokeRect                         OK
+ fillText 14px                      CRASH   <- frame 0
```

Then removing calls from that crashing set:

```
minus arc                            OK
minus strokeRect                     OK
minus the FIRST fillText             CRASH   (so not the text)
minus the title-bar fillRect         OK      <- the one that matters
```

**It is the SECOND fillRect.** With `arc` + `strokeRect` + `fillText` present,
one `fillRect` is fine and two crash. Geometry is irrelevant -- 800x40,
800x100, 800x300, 800x600 and 400x40 all crash identically. So this is not a
size, coordinate or shape bug.

Also NOT the cause, each tested in isolation and all passing: two `fillText`
at the same size, two at different sizes, text that changes every frame (200
frames clean), `arc`+`strokeRect`+`fillText` together without the second
fillRect, and `clearRect` with or without.

Crashes on **frame 0**, so it is a setup-time fault, not a leak or an
unbounded cache.

That shape -- a specific COUNT of one primitive tipping it over while
geometry is irrelevant -- points at a fixed-size buffer or a draw-op batch
limit in the cart-side Skia wrapper (`src/canvas2d_skia.c`) or in
`skia_c.cpp`, rather than at Skia itself. That is where to look next.

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


## Hunting the UB behind `-sASSERTIONS=1` (2026-07-28)

Narrowed a long way, not caught. What is now established:

- **It is a build flag, not source.** With every source fix in place, the
  pristine `build.sh` crashes and the modified one passes.
- **The flag rename is NOT the cause.** Changing only `TOTAL_STACK` ->
  `STACK_SIZE` still crashes. (It is still worth fixing -- `TOTAL_STACK` has
  been silently ignored since emscripten 3.1.27 -- but it is not this bug.)
- **Stack SIZE is not the variable.** 4 MB, 8 MB, 12 MB, 16 MB and 32 MB all
  behave the same once `-sASSERTIONS=1` is present, and 32 MB alone (no
  assertions) also passes -- which is what a stack overflow would look like,
  except that 4 MB *with* assertions passes too. Both cannot be true of a
  simple overflow, so size is a red herring.
- **`-sSTACK_OVERFLOW_CHECK=1` alone fixes it** -- and that is the strongest
  clue. It reserves a cookie at the top of the stack and checks writes to
  address zero, which SHIFTS THE MEMORY LAYOUT. A bug that disappears when
  layout shifts is an out-of-bounds write landing somewhere harmless rather
  than a genuine overflow.
- `-sSAFE_HEAP=1` and `-sCHECK_NULL_WRITES=1` do NOT fix it, and
  `-sSTACK_OVERFLOW_CHECK=2` fails during `load` instead, so neither gives a
  clean diagnostic.

**Where to look next:** something writes just past a buffer, and the cookie
reservation moves the target out of harm's way. The Skia/Ganesh path is the
place to start, since only Canvas 2D carts are affected -- `hello_audio` and
`hello_fetch` pass in every configuration.

The productive next step is probably an emscripten build with `-fsanitize=address`
(ASAN works under wasm and would name the write), not more flag bisection.


## ASAN named the crash site (2026-07-28)

An `-fsanitize=address` build with `-g` did not produce a sanitizer *report*
-- the fault is a raw wasm trap inside Skia, which is a prebuilt archive and so
was never instrumented. But the debug symbols gave the exact call chain, which
is what mattered:

```
skiac_font_collection::skiac_font_collection()   <- crashes here
  <- skiac_font_collection_create
  <- ensure_skia_init
  <- js_clearRect                                <- first Canvas 2D call
```

So it is not a draw call at all. It is **lazy Skia init on the first Canvas 2D
operation**, which is why every earlier per-primitive bisection gave nonsense:
whichever draw happened first paid for the init, and everything else looked
innocent.

**Prime suspect, from reading the constructor** (`wasmcart-skia/out/include/skia_c.hpp`):

```cpp
font_mgr(SkFontMgr_New_Custom_Directory(SK_FONT_FILE_PREFIX))
```

`SK_FONT_FILE_PREFIX` has branches for Windows, Apple and `__linux__` -- and
**none for wasm**, so a cart compiles to `/usr/share/fonts/`, a host path that
does not exist inside the sandbox. Same class of bug as retroemu's
GET_SYSTEM_DIRECTORY: a host path is meaningless in wasm.

### Attempted and NOT sufficient

Two fixes were tried and neither made plain `-O2` pass:

1. `#define SK_FONT_FILE_PREFIX ""` for wasm -- still crashes (an empty prefix
   still enters the directory scanner).
2. `font_mgr(SkFontMgr_New_Custom_Empty())` on wasm -- still crashes.

So the font path is very likely *a* bug, but something else in that constructor
is also unhappy. The remaining members are worth checking in order:
`sk_make_sp<FontCollection>()`, `TypefaceFontProviderCustom(font_mgr)`,
`SkFontMgr_New_Custom_Empty()` for the default manager, and
`enableFontFallback()` -- fallback with no fonts registered is a plausible
next suspect.

**To reproduce the diagnosis:** `bash build-asan.sh` (committed), pack
hello_canvas against `build/cart-asan.wasm`, and read the stack. That is the
whole reason to keep that script.

`-sASSERTIONS=1` remains in `build.sh` as the working mitigation.


## FIXED 2026-07-28: SkFontMgr_New_Custom_Directory on a path that does not exist

`skiac_font_collection`'s constructor calls

```cpp
font_mgr(SkFontMgr_New_Custom_Directory(SK_FONT_FILE_PREFIX))
```

and `wasmcart-skia/build.sh` compiles it with `-DSK_FONT_FILE_PREFIX="/fonts/"`
-- a MEMFS path that **nothing ever creates**. The directory scan walks it and
writes out of bounds. On wasm the constructor now uses
`SkFontMgr_New_Custom_Empty()` instead: no scan, and fonts arrive through the
dynamic provider exactly as they already did.

`-sASSERTIONS=1` is no longer needed. Plain `-O2` renders hello_canvas at 338
colours, with text.

### The mistake that cost the most time here

I spent several bisection rounds editing `wasmcart-skia/out/include/skia_c.hpp`.
**That file is a build OUTPUT, copied from `napi-canvas/skia-c/skia_c.hpp`.**
Editing it changes nothing, and `wasmcart-skia/build.sh` additionally
short-circuits when `out/libskia.a` already exists, so even the copy never
reran. Three "the fix did not work" results in a row were measuring a stale
`libskiac.a`.

Edit `napi-canvas/skia-c/skia_c.hpp`, then rebuild the wrapper:

```bash
cd wasmcart-skia
rm -f out/libskiac.a out/skia_c.o
em++ -O2 -std=c++20 -fno-exceptions -fno-rtti -DSK_RELEASE -DSK_DISABLE_TRACING \
  '-DSK_FONT_FILE_PREFIX="/fonts/"' -I../napi-canvas/skia -I../napi-canvas/skia-c \
  -c ../napi-canvas/skia-c/skia_c.cpp -o out/skia_c.o
emar rcs out/libskiac.a out/skia_c.o
```

The ASAN stack that named `skiac_font_collection` was right all along -- I
disbelieved it when my (ineffective) edits did not change the outcome.


## From-scratch rebuild verified (2026-07-28)

`rm -rf obj build/cart.wasm && bash build.sh` now completes and every example
that can be rebuilt from source works.

One more thing had to be fixed to get there: `build_webaudio_lib.sh` did not
pass `-I.../vendor/opus/include -I.../vendor/ogg/include`, so a from-scratch
build died with `opus_multistream.h: file not found` (opusfile.h includes it).
It only ever worked because `obj/libwebaudio.a` was a stale prebuilt artifact
that the script skips when present. webaudio-node's own
`scripts/build-unified-real.sh` passes those include dirs; this one now does too.

### Results, 60 frames each, at plain -O2 with no assertions

| example | colours | note |
|---|---|---|
| hello_audio | 572 | |
| hello_canvas | 345 | text renders |
| hello_fetch | 1 | draws a solid background by design |
| hello_webgl | 47960 | |
| threejs | 5188 | |
| adventure-ai | 233 | loading screen, text correct |
| space | — | **crashes, PRE-EXISTING** |
| space3d | — | **crashes, PRE-EXISTING** |

`space` and `space3d` are NOT regressions: the carts monteslu shipped, which
embed the OLD runtime, fail identically (`memory access out of bounds` during
load). Verified by running `examples/space/space.wasc` as committed.

### Packing gotcha

`examples/space`, `examples/space3d` and `examples/adventure-ai` contain ONLY a
prebuilt `.wasc` -- no source. Packing those directories produces a cart with no
game (renders 1 colour, "passes" every count-based check). The real sources are
in the `jsgames` repo, and they must be packed from the game's ROOT, not its
`dist/`: packing `adventure-ai/dist` gave 1 colour, packing `adventure-ai/` gave
233 and a correct loading screen.
