# Rebuild notes — RESOLVED 2026-07-28

**The tree rebuilds cleanly and all 8 examples work.** `rm -rf obj build/cart.wasm
&& bash build.sh` at plain `-O2`, no assertions crutch.

| example | colours | example | colours |
|---|---|---|---|
| hello_audio | 569 | threejs | 5039 |
| hello_canvas | 340 (text renders) | space | 537 |
| hello_fetch | 1 (solid bg by design) | space3d | 208 |
| hello_webgl | 47902 | adventure-ai | 28 |

These counts are a **liveness signal, not a fixture**. They are sampled at a
particular animation frame and drift between runs — space3d has read anywhere
from 35 to 262 depending on where its nebulae are. Treat `> 1` as "it drew";
only `1` (or an error) is a real failure. `hello_fetch` legitimately reads 1
because it has no draw calls at all.

Colour counts also cannot see orientation: every one of these was identical
while all 2D frames shipped upside down. Look at the PNG, not the number —
`node ../wasmcart/bin/wasmcart-play.js examples/hello_canvas/hello_canvas.wasc
--frames 60 --shot /tmp/x.png`.

## CORRECTION (verified by reverting each fix in turn)

The Canvas 2D crash was fixed by **the complete Skia archive link (#2)**, not by
the font-directory change (#1). Proven by reverting fixes one at a time against
the regression suite: with the font fix reverted AND `STACK_SIZE` reverted to
the ignored `TOTAL_STACK`, hello_canvas still renders 341 colours.

The font fix is still correct in principle -- scanning `/fonts/`, which nothing
creates, is wrong -- but it was NOT what unblocked the build, and the commit
that landed it claims otherwise. ASAN pointed at `skiac_font_collection` because
that constructor is simply where lazy Skia init happens to touch memory first,
under a partial link.

Lesson: I confirmed a fix by observing the symptom disappear, without checking
whether an earlier fix in the same session had already done it.

## Two more build bugs, 2026-07-28 (later session)

**`worker_shim.o` was compiled and never linked.** `build.sh` built it, then
the `OBJS` list didn't mention it, so the entire Worker implementation was
absent from `cart.wasm` and `new Worker(...)` threw `_wcWorkerCreate is not
defined`. `-sERROR_ON_UNDEFINED_SYMBOLS=0` is required for the Skia/GL stubs,
so an orphaned object produces no diagnostic at all. Audit with:

```bash
for o in obj/*.o; do
  grep -q "$(basename $o)" <<<"$(grep '^OBJS=' -A10 build.sh)" || echo "ORPHAN: $o"
done
# the three skia_*.o arrive via $SKIA_FIX/$SKIA_GL and are expected
```

**QuickJS lived in `/tmp/quickjs`.** Both `build.sh` and `setup_quickjs.sh`
pointed there, so a reboot silently deleted the JS engine source and left the
tree unbuildable with only "QuickJS not found" to go on. Now defaults to
`vendor/quickjs` (gitignored), overridable via `QUICKJS_DIR`/`QUICKJS_SRC`.
`setup_quickjs.sh` also referenced `$HERE` without defining it, which the old
absolute path had masked.

## A third trap: the packed `.wasc` is a second staleness layer

A `.wasc` embeds `cart.wasm`. Rebuilding the runtime does **nothing** to an
already-packed cart, so `bash build.sh && node run-my-probe.js` can test a cart
from an hour ago. This produced three separate false conclusions in one session,
including "the blit arrives un-flipped" and "`skia_flush_to_framebuffer` is
never called" — both of which reversed after repacking.

Before trusting any negative result, confirm the string you just added is
actually in the binary *and* in the cart:

```bash
strings build/cart.wasm | grep -c "MY_NEW_LOG_STRING"
python3 -c "import zipfile;z=zipfile.ZipFile('x.wasc');print([i.file_size for i in z.infolist() if i.filename=='cart.wasm'])"
stat -c%s build/cart.wasm    # must match the number above
```

## The five bugs this file records

1. **Skia scanned a font directory that does not exist.**
   `skiac_font_collection` called `SkFontMgr_New_Custom_Directory("/fonts/")`, a
   MEMFS path nothing creates. Out-of-bounds write on the FIRST Canvas 2D call.
   Fixed in `build-libcanvas/patches/wasm-no-font-dir.patch`.

2. **`wasmcart-skia/build.sh` copied 4 archives, consumers need 16.** Silent
   partial link → runtime memory fault. Fixed there; that repo is now tracked.

3. **`build_webaudio_lib.sh` missed the opus/ogg include dirs.** From-scratch
   builds died on `opus_multistream.h: file not found`; it only ever worked
   because `obj/libwebaudio.a` was a stale prebuilt.

4. **`MAXIMUM_MEMORY=1GB` was too low.** `space` decodes 21 MB of .ogg to
   ~500 MB of f32 PCM. Raised to 2 GB.

5. **`TOTAL_STACK` → `STACK_SIZE`.** Renamed in emscripten 3.1.27 and silently
   ignored since, so the intended 8 MB stack was never applied.

## Two traps that cost the most time

**`wasmcart-skia/out/include/skia_c.{cpp,hpp}` are COPIES.** The wrapper
compiles from `napi-canvas/skia-c/`, and `wasmcart-skia/build.sh` short-circuits
when `out/libskia.a` exists — so editing the copies changes nothing AND the copy
step never reruns. Three consecutive "that fix did not work" results were
measuring a stale `libskiac.a`, which talked me out of the correct hypothesis.
Source changes go in `build-libcanvas/patches/`.

**Every one of these failures looks identical:** `memory access out of bounds`,
no hint of the cause. A missing archive, a nonexistent font path, and an
exhausted memory ceiling all produce the same trap. Bisect by *removing things*,
and confirm each fix actually reached the binary before concluding it failed.

---

# Appendix: the investigation, in the order it happened

Kept because the dead ends are the useful part — several conclusions below were
WRONG and are marked where they were corrected later.

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


## space / space3d: it was the memory ceiling, not the games

Both crashed during load with `memory access out of bounds`. Not a rendering
bug and not a regression -- the SHIPPED carts (old runtime) failed identically.

Bisected by stripping the game to nothing and adding assets back:

```
js only (60KB)            OK
js + public/ (33MB)       OK
js + music/  (21MB)       CRASH
  2 ogg files (5.5MB)     OK
  4 ogg files (10.8MB)    OK
  7 ogg files (16.1MB)    OK
  8 ogg files (20.9MB)    CRASH
```

21MB of Vorbis decoded to f32 PCM is roughly 25x, ~500MB, and the cart was
linked with `-sMAXIMUM_MEMORY=1073741824`. Raising it to 2GB fixes both:
space renders 500 colours (clouds, ship, "Score: 100 / Max: 100 / Level: 1"),
space3d renders 35.

The failure gives no hint that memory is the issue -- `ALLOW_MEMORY_GROWTH`
just fails the growth request and the next write traps. Worth remembering for
any cart that loads a lot of compressed audio.

### Packing gotcha, again

`jsgames/space` is 171MB because of `node_modules`. Packing the game root
sweeps it all in -- 1421 files, a 150MB cart. Exclude `node_modules`, `dist`
and `package-lock.json`; the real game is 41 files.
