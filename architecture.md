# wasmcart-jsgame Architecture

Detailed technical documentation for the QuickJS + Skia + WebGL2 + Web Audio game runtime.

## Overview

A single `cart.wasm` (~5MB) contains:
- **QuickJS** — ES2023 JavaScript engine
- **Skia** (via @napi-rs/canvas) — Canvas 2D with Ganesh GL GPU backend
- **webaudio-node** C++ engine — 16 AudioNode types, 5 audio decoders
- **WebGL2 passthrough** — direct GL imports to host GPU
- **Browser API shims** — document, window, fetch, Image, localStorage, timers, events, etc.

Games are packed as `.wasc` archives (ZIP with manifest.json + cart.wasm + assets/). The game developer writes standard browser JavaScript — no C, no WASM toolchain.

## Rendering Pipeline

### Canvas 2D (Ganesh GL)

```
Game JS draws (fillRect, fill, stroke, drawImage, fillText)
    ↓
skiac_* C API (Skia canvas operations)
    ↓
Ganesh GL backend (queues GPU commands to offscreen FBO)
    ↓  flushAndSubmit(kNo) per draw op
GPU executes draws
    ↓  flushAndSubmit(kYes) end of frame
glBlitFramebuffer (Ganesh FBO → host display FBO)
    ↓
resetContext() (tell Ganesh GL state changed)
    ↓
Host swapBuffers → screen
```

### WebGL2 (Direct)

```
Game JS calls gl.bindBuffer, gl.drawArrays, etc.
    ↓
webgl_shim.c (QuickJS C functions → wasmcart GL imports)
    ↓
Host GPU (via webgl_imports.js)
    ↓
Host swapBuffers → screen
```

WebGL games set `game_uses_webgl = 1` which skips Ganesh entirely.

### Desktop GL Core 3.3 (RetroArch)

On RetroArch's Core 3.3 context, two cart-side workarounds are needed:

1. **VAO 0 redirect**: Core Profile doesn't allow VAO 0. Ganesh's GLES path
   uses VAO 0 as the default. `wc_gl_get_proc` intercepts `glBindVertexArray(0)`
   and redirects to a real VAO created via `glGenVertexArrays`.

2. **GLES interface on desktop**: Ganesh uses `GrGLMakeAssembledGLESInterface`
   even on Core 3.3. Mesa accepts `#version 300 es` shaders via
   `GL_ARB_ES3_compatibility`. The desktop GL interface
   (`GrGLMakeAssembledGLInterface`) requires too many desktop-only function
   stubs and produces the same rendering output.

3. **Extension hiding**: `ganesh_glGetString(GL_EXTENSIONS)` returns empty,
   `ganesh_glGetIntegerv(GL_NUM_EXTENSIONS)` returns 0. Prevents Ganesh from
   probing for extension function pointers not in the WASM import table.

These workarounds are transparent — the same `.wasc` file works on all hosts.

## Ganesh GL Setup

### The Problem
WASM imports are not in the function table — they can't be used as function pointers. Skia's `GrGLInterface` stores GL functions as pointers. Solution: static wrapper functions for every GL import.

### skia_gl_surface.cpp

Creates ~120 wrapper functions using macros:
```c
#define W1(ret, fn, t1)  static ret w_##fn(t1 a) { return fn(a); }
#define V2(fn, t1, t2)   static void w_##fn(t1 a, t2 b) { fn(a, b); }
// ... W0-W9, V0-V9 for different arg counts
```

The `wc_gl_get_proc` resolver maps function names to wrappers:
```c
static GrGLFuncPtr wc_gl_get_proc(void* ctx, const char name[]) {
    #define MAP(fn) if (strcmp(name, #fn) == 0) return (GrGLFuncPtr)w_##fn
    MAP(glEnable); MAP(glDisable); MAP(glGetError);
    // ... ~120 functions
    MAP(glGetInternalformativ); MAP(glGetShaderPrecisionFormat);
    #undef MAP
    return nullptr;
}
```

### Context and Surface Creation

```c
// 1. Create GL interface from wrapper functions
auto interface = GrGLMakeAssembledGLESInterface(nullptr, wc_gl_get_proc);

// 2. Create Ganesh direct context
s_grContext = GrDirectContexts::MakeGL(interface).release();

// 3. Create offscreen render target (NOT FBO 0 — needs stencil for paths)
auto imageInfo = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
s_glSurface = SkSurfaces::RenderTarget(
    s_grContext, skgpu::Budgeted::kNo, imageInfo, 0,
    kTopLeft_GrSurfaceOrigin, nullptr).release();
```

### Why Offscreen FBO (Not FBO 0)
The host's FBO redirect doesn't include a stencil attachment. Ganesh uses stencil-based path rendering for `beginPath`/`fill` polygons. Without stencil, only `fillRect` and text work — all path fills silently fail.

### Per-Op Flush (Critical)
Ganesh's WASM/GLES path drops batched draw commands without intermediate flushes. Every draw operation calls:
```c
s_grContext->flushAndSubmit(GrSyncCpu::kNo);
```
`kNo` submits commands to the GPU without CPU stall. The end-of-frame flush uses `kYes` to ensure everything completes before the FBO blit.

### Direct FBO Blit
```c
// Get Ganesh's FBO
_gl_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &ganesh_fbo);

// Blit: Ganesh FBO → host display FBO (0 = host redirect)
_gl_BindFramebuffer(GL_READ_FRAMEBUFFER, ganesh_fbo);
_gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
_gl_BlitFramebuffer(
    0, cur_height, cur_width, 0,   // src flipped Y (Skia top-down → GL bottom-up)
    0, 0, cur_width, cur_height,   // dst
    GL_COLOR_BUFFER_BIT, GL_NEAREST);

// Restore Ganesh's FBO and reset its state tracking
_gl_BindFramebuffer(GL_FRAMEBUFFER, ganesh_fbo);
skia_gl_reset_context();
```

## Skia Build (wasmcart-skia/)

Skia is compiled from @napi-rs/canvas source as LLVM bitcode.

### Critical GN Args
```
skia_enable_ganesh = true
skia_use_gl = true
skia_use_webgl = false     # Prevents Skia from importing GL directly (module conflict)
skia_gl_standard = "gles"  # Use GLES path, not WebGL
```

### Critical CFlags
- NO `-DSK_NO_GL` — this was the original blocker. It disabled GL at the preprocessor level, making all Ganesh GL code dead.
- `skia_gl_surface.cpp` compiled with separate `SKIA_GL_CXX` flags (without `-DSK_NO_GL`)

### Host GL Requirements (webgl_imports.js)
- `glGetInternalformativ`: synthesize `GL_NUM_SAMPLE_COUNTS` from `GL_SAMPLES` (WebGL2 doesn't support the former). Without this, Ganesh thinks RGBA8 is not renderable → surface creation fails.
- `glGetShaderPrecisionFormat`: return real values from `ctx.getShaderPrecisionFormat()`
- Auto-stub: `CartHost.js` stubs any `gl.*` imports not in webgl_imports.js to `() => 0`

## Canvas 2D Implementation (canvas2d_skia.c)

### Paint State
- Anti-aliasing disabled on Ganesh (`skiac_paint_set_anti_alias(0)`) — AA causes strokes and small geometry to vanish on GPU
- `fill_paint` (style=fill) and `stroke_paint` (style=stroke) are global
- Color set via `skiac_paint_set_color(paint, r, g, b, alpha * globalAlpha)`
- `save()`/`restore()` preserves fill/stroke colors, globalAlpha, lineWidth, fontSize

### drawImage with Surface Caching
Images decoded by stb_image have stable `_rgba` pixel buffer pointers. `drawImage` caches a raster Skia surface per unique pointer:
```c
static struct { uint8_t *key; skiac_surface *surf; int w, h; } img_cache[32];
```
First call: create surface + writePixels. Subsequent calls: reuse cached surface. No per-call allocation, no flush needed for surface lifetime.

### Game Resolution Scaling
Games with fixed canvas size (e.g. 640x480) get automatic CSS-like scaling:
```c
float scale = min(surface_width / game_width, surface_height / game_height);
canvas_translate(offset_x, offset_y);  // center
canvas_scale(scale, scale);            // uniform scale
canvas_clip_rect(0, 0, game_width, game_height);  // prevent bleed into letterbox
```

Only the main canvas (from `getElementById`) triggers scaling. Offscreen canvases (from `createElement('canvas')`) are flagged `isMain=false` and don't affect the global transform.

`setTransform()` and `resetTransform()` incorporate the game scale so games that reset their transform don't lose the scaling.

## Audio Pipeline (audio_shim.c)

- webaudio-node C++ engine compiled as `libwebaudio.a`
- 128-frame quantum rendering (Web Audio standard)
- Time-based frame count using `wc_time.time_ms`
- Ring buffer: 8192 frames, F32 stereo
- `decodeAudioData`: MP3/WAV/FLAC/OGG/AAC via webaudio-node decoders
- Critical: `registerBuffer` keeps decoded audio pointer alive — do NOT call `freeDecodedBuffer`

## Text Rendering (skia_wasm_fix.cpp)

- stb_truetype (not FreeType — Skia's FreeType has a cmap bug in WASM where `unicharToGlyph` returns 0)
- Multi-font support (MAX_FONTS=16) with per-codepoint fallback
- UTF-8 decoding for unicode/emoji
- Glyph cache (256 entries)
- textBaseline modes: alphabetic, top, middle, bottom

## Worker Shim (worker_shim.c)

- Cooperative multitasking (not parallel — Emscripten STANDALONE_WASM doesn't support shared memory)
- Each Worker gets its own QuickJS runtime (real isolation, separate JS heaps)
- Message passing via JSON serialization through ring buffers
- Workers pumped each frame in `pump_workers()`

## Browser API Shims (cart_main.c)

All shims are inline JS strings evaluated during `wc_init`:

- **DOM:** document, window, navigator, screen, location, history
- **Events:** Event, CustomEvent, PointerEvent, TouchEvent, WheelEvent, EventTarget
- **Timing:** setTimeout, setInterval, requestAnimationFrame, requestIdleCallback, performance.now, Date.now
- **Storage:** localStorage, sessionStorage (in-memory)
- **Network:** fetch (via wc_load_asset), XMLHttpRequest, WebSocket (wasmcart WS ABI)
- **Data:** Blob, URL, TextEncoder, TextDecoder, DOMParser, FormData
- **Observers:** MutationObserver, ResizeObserver, IntersectionObserver (stubs)
- **Crypto:** crypto.getRandomValues, crypto.randomUUID
- **Encoding:** atob, btoa
- **Modern:** queueMicrotask, structuredClone, matchMedia, AbortController, MessageChannel
- **Input:** Gamepad API (wc_pads), keyboard/mouse/pointer events (wc_keys + wc_pointers)
- **Modules:** ES module imports via QuickJS module loader, CommonJS require() shim

## File Structure

```
src/
  cart_main.c          — wasmcart ABI + QuickJS boot + all browser shims
  canvas2d_skia.c      — Canvas 2D backed by Skia (CPU raster or Ganesh GL)
  skia_gl_surface.cpp  — Ganesh GL context + ~120 GL wrapper functions
  skia_wasm_fix.cpp    — Text rendering via stb_truetype
  stb_truetype.h       — vendored single-header (v1.26); skia_wasm_fix.cpp
                         defines STB_TRUETYPE_IMPLEMENTATION
  gl_trace.h           — optional GL call tracing for Ganesh debugging
  skia_path_reset.cpp  — Skia path reset helper
  webgl_shim.c         — WebGL2 → wasmcart GL imports (~80 functions)
  audio_shim.c         — Web Audio API → webaudio-node C++ engine
  image_decode.c       — Image decoding via stb_image
  worker_shim.c        — Cooperative Workers via separate QuickJS runtimes
  stubs.c              — libc stubs for standalone WASM
build.sh               — Full build script (QuickJS + Skia + webaudio + link)
pack_game.sh           — Pack JS game + cart.wasm into .wasc archive
```

## Feature Parity: Browser vs jsgamelauncher vs wasmcart-jsgame

### Quick Status (matching jsgamelauncher's feature list)

| API | jsgamelauncher | wasmcart-jsgame | Delta |
|-----|---------------|-----------------|-------|
| Canvas 2D | @napi-rs/canvas (Skia CPU) | Skia Ganesh GL (GPU) | Faster — GPU-accelerated |
| WebGL | webgl-node (EGL pbuffer) | GL imports → host GPU | Same — direct GPU |
| Web Audio | webaudio-node (C++ WASM) | webaudio-node (C++ linked) | Same engine |
| Keyboard | SDL keyboard events | wc_keys bitmask | Same |
| Gamepad | gamepad-node + SDL | wc_pads shared memory | Same |
| FontFace | GlobalFonts (FreeType) | stb_truetype | Different backend, same API |
| localStorage | lowdb (file-backed) | In-memory Map | Reduced — no persistence yet |
| Web Assembly | Node.js WASM | Not exposed to games | Missing |
| Web Workers | Node.js worker_threads | Cooperative (same thread) | Reduced — not parallel |
| WebSockets | ws package | Shim (wasmcart WS ABI) | Reduced — needs host manifest |
| Peer Connection | Not implemented | Not implemented | Same |
| Phaser support | Tested | Untested | Unknown |
| Three.js support | Tested | Working (1800fps) | Same |

### Detailed Breakdown

#### Graphics

| Feature | Browser | jsgamelauncher | wasmcart-jsgame | Notes |
|---------|---------|----------------|-----------------|-------|
| Canvas 2D fillRect/strokeRect | Native | @napi-rs/canvas (Skia CPU) | Skia Ganesh GL (GPU) | GPU-accelerated in wasmcart |
| Canvas 2D paths (fill/stroke) | Native | @napi-rs/canvas | Skia Ganesh GL | beginPath/moveTo/lineTo/arc/bezier/quadratic |
| Canvas 2D drawImage (3/5/9-arg) | Native | @napi-rs/canvas | Skia + cached raster surfaces | Surface cached per image pointer |
| Canvas 2D text (fillText/strokeText) | Native | @napi-rs/canvas (FreeType) | stb_truetype | FreeType cmap bug in WASM |
| Canvas 2D gradients | Native | @napi-rs/canvas | First color stop only | API present, flat color fallback |
| Canvas 2D shadows | Native | @napi-rs/canvas | Not rendered | API present, no-op |
| Canvas 2D patterns | Native | @napi-rs/canvas | Not implemented | |
| Canvas 2D compositing | Native | @napi-rs/canvas | Default only | globalCompositeOperation ignored |
| Canvas 2D transforms | Native | @napi-rs/canvas | Full (translate/rotate/scale/setTransform) | Game scale transform auto-applied |
| Canvas 2D save/restore | Native | @napi-rs/canvas | Full (matrix + paint state) | |
| Canvas 2D clip | Native | @napi-rs/canvas | Full (rect + path) | |
| Canvas 2D getImageData/putImageData | Native | @napi-rs/canvas | Working | |
| Canvas 2D measureText | Native | @napi-rs/canvas | Width only | No font metrics |
| WebGL2 | Native | webgl-node (EGL) | GL imports → host GPU | ~80 GL functions + auto-stub |
| Image decoding | Native | @napi-rs/canvas (SkCodec) | stb_image | SkCodec crashes in WASM |
| CSS canvas scaling | Native CSS | Browser handles | Uniform scale + letterbox + clip | Automatic for fixed-res games |

#### Audio

| Feature | Browser | jsgamelauncher | wasmcart-jsgame | Notes |
|---------|---------|----------------|-----------------|-------|
| AudioContext | Native | webaudio-node | webaudio-node C++ (libwebaudio.a) | Same engine, compiled to WASM |
| OscillatorNode | Native | webaudio-node | Working | |
| GainNode | Native | webaudio-node | Working | |
| BufferSourceNode | Native | webaudio-node | Working | |
| BiquadFilterNode | Native | webaudio-node | Working | |
| DelayNode | Native | webaudio-node | Working | |
| AnalyserNode | Native | webaudio-node | Working | |
| PannerNode | Native | webaudio-node | Working | |
| DynamicsCompressorNode | Native | webaudio-node | Working | |
| StereoPannerNode | Native | webaudio-node | Working | |
| ConvolverNode | Native | webaudio-node | Working | |
| WaveShaperNode | Native | webaudio-node | Working | |
| ConstantSourceNode | Native | webaudio-node | Working | |
| ChannelSplitter/Merger | Native | webaudio-node | Working | |
| IIRFilterNode | Native | webaudio-node | Working | |
| decodeAudioData | Native | webaudio-node | MP3/WAV/FLAC/OGG/AAC | 5 decoder formats |
| AudioWorklet | Native | Not implemented | Not implemented | |

#### Input

| Feature | Browser | jsgamelauncher | wasmcart-jsgame | Notes |
|---------|---------|----------------|-----------------|-------|
| Gamepad API | Native | gamepad-node + SDL | wc_pads shared memory | W3C Gamepad mapping |
| Keyboard events | Native | SDL keyboard | wc_keys bitmask + edge detection | keydown/keyup with HID→key mapping |
| Mouse events | Native | SDL mouse | wc_pointers + event dispatch | click/mousedown/mouseup/mousemove |
| Pointer events | Native | N/A | Auto-fired alongside mouse events | pointerdown/pointerup/pointermove |
| Touch events | Native | N/A | Event classes defined | TouchEvent/Touch constructors available |
| Wheel events | Native | N/A | WheelEvent class defined | Routed through mouse listeners |

#### Platform APIs

| Feature | Browser | jsgamelauncher | wasmcart-jsgame | Notes |
|---------|---------|----------------|-----------------|-------|
| fetch | Native | fs.readFile | wc_load_asset from .wasc | Local assets only |
| Image (src loading) | Native | @napi-rs/canvas | fetch + stb_image | Async load + decode |
| FontFace | Native | GlobalFonts | fetch + stb_truetype register | |
| localStorage | Native | lowdb / file | In-memory Map | Save ABI planned |
| sessionStorage | Native | N/A | Alias to localStorage | |
| performance.now | Native | Node.js | wc_time.time_ms | Wasmcart time source |
| Date.now | Native | Node.js | Overridden to use performance.now | QuickJS built-in returns 0 in WASM |
| console.log | Native | Node.js | wc_log | |
| setTimeout/setInterval | Native | Node.js | Internal timer slots (max 64) | Pumped each frame |
| requestAnimationFrame | Native | Custom loop | Fires one callback per wc_render | |
| crypto.getRandomValues | Native | Node.js crypto | Math.random-based | |
| atob/btoa | Native | Node.js Buffer | Pure JS implementation | |
| TextEncoder/TextDecoder | Native | Node.js | Pure JS implementation | ASCII subset |
| Blob/URL | Native | Node.js | In-memory with blob registry | |
| XMLHttpRequest | Native | N/A | Shim via fetch | GET only |
| WebSocket | Native | ws package | Shim (wasmcart WS ABI) | Needs host manifest allowlist |
| Worker | Native | Node.js worker_threads | Cooperative (separate QuickJS runtimes) | Not parallel — WASM standalone limitation |
| ES modules (import) | Native | Node.js ESM | QuickJS module loader via wc_load_asset | |
| CommonJS (require) | Native | Node.js CJS | Shim (returns {} for builtins) | Limited |

#### DOM Shims

| Feature | Browser | jsgamelauncher | wasmcart-jsgame | Notes |
|---------|---------|----------------|-----------------|-------|
| document.getElementById | Native | N/A (direct canvas) | Returns main _WCCanvas | |
| document.createElement | Native | N/A | Returns _WCCanvas or stub element | |
| document.querySelector | Native | N/A | Alias to getElementById | |
| document.addEventListener | Native | N/A | Routes key/mouse/pointer events | |
| window.innerWidth/Height | Native | N/A | Set from host preferred resolution | |
| window.devicePixelRatio | Native | N/A | Always 1 | |
| screen.width/height | Native | N/A | Dynamic from canvas size | |
| location/history | Native | N/A | Stub objects | |
| MutationObserver | Native | N/A | No-op stub | Three.js requires it |
| ResizeObserver | Native | N/A | No-op stub | |
| IntersectionObserver | Native | N/A | No-op stub | |
| matchMedia | Native | N/A | Returns {matches: false} | |
| getComputedStyle | Native | N/A | Returns empty strings via Proxy | |
| Event/CustomEvent | Native | N/A | Full implementation | |
| EventTarget | Native | N/A | addEventListener/removeEventListener/dispatchEvent | |
| AbortController | Native | N/A | Working (signal.aborted) | |
| MessageChannel | Native | N/A | Synchronous port messaging | |
| DOMParser | Native | N/A | Returns empty document | |
| requestIdleCallback | Native | N/A | Shim via setTimeout | |
| queueMicrotask | Native | N/A | Shim via Promise.resolve | |
| structuredClone | Native | N/A | Shim via JSON round-trip | |

## Upcoming: wc_fetch ABI

See [wasmcart/docs/fetch.md](../wasmcart/docs/fetch.md) for the full design.

Currently, the jsgame cart handles fetch internally — relative paths go through `wc_load_asset`, absolute URLs fail gracefully. This means games can load assets from the .wasc bundle but can't make network requests.

A `wc_fetch` wasmcart ABI would change this:

- **Cart becomes simpler** — passes ALL fetch URLs to the host, no URL routing in the cart
- **Host handles everything** — relative paths → .wasc bundle, absolute URLs → network proxy (if manifest allows)
- **Games get real networking** — `fetch('https://api.example.com/scores').then(r => r.json())` just works
- **Security preserved** — host checks manifest allowlist, rejects unlisted domains
- **Browser hosts get CORS for free** — the host's native `fetch` handles it

This is the missing piece for multiplayer games, leaderboards, dynamic content loading, and CDN-hosted assets. The game code wouldn't change — same `fetch()` calls, just more of them work.

## Native Library Integration (WASM-in-WASM Problem)

### The Problem
Browser JS games often load native libraries as WASM modules:
```javascript
const box2d = await WebAssembly.instantiate(box2dWasmBytes, imports);
```
This won't work in wasmcart-jsgame because:
- QuickJS has no `WebAssembly` API
- WASM can't instantiate WASM modules inside itself
- The host's WASM runtime owns the single WASM instance

### The Solution: Link at Build Time
Compile the native library to LLVM bitcode and link it directly into cart.wasm. Expose the API to QuickJS as C functions. The JS game calls the same API — it doesn't know the library is linked in rather than loaded dynamically.

This is the same pattern used for the existing native libraries:

| Library | Source | Linked As | JS API |
|---------|--------|-----------|--------|
| Skia (Canvas 2D) | @napi-rs/canvas | libskia.a + libskiac.a | `ctx.fillRect()`, `ctx.drawImage()`, etc. |
| webaudio-node | webaudio-node C++ | libwebaudio.a | `new AudioContext()`, `oscillator.start()`, etc. |
| QuickJS | bellard/quickjs | quickjs.o | The JS engine itself |
| stb_image | stb single-header | image_decode.o | `new Image(); img.src = '...'` |
| stb_truetype | stb single-header | skia_wasm_fix.o | `ctx.fillText()` |

### Adding a New Library (e.g. box2d3)

1. **Compile to object files:**
   ```bash
   emcc -O2 -c box2d3/src/*.c -I box2d3/include -o obj/box2d3.o
   ```

2. **Write a QuickJS binding shim** (e.g. `box2d_shim.c`):
   ```c
   #include "box2d/box2d.h"
   #include "quickjs.h"

   static JSValue js_create_world(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
       double gx, gy;
       JS_ToFloat64(ctx, &gx, argv[0]);
       JS_ToFloat64(ctx, &gy, argv[1]);
       b2WorldDef worldDef = b2DefaultWorldDef();
       worldDef.gravity = (b2Vec2){gx, gy};
       b2WorldId world = b2CreateWorld(&worldDef);
       return JS_NewInt32(ctx, world.index1);
   }

   void register_box2d_api(JSContext *ctx) {
       JSValue global = JS_GetGlobalObject(ctx);
       JSValue b2d = JS_NewObject(ctx);
       JS_SetPropertyStr(ctx, b2d, "createWorld",
           JS_NewCFunction(ctx, js_create_world, "createWorld", 2));
       // ... more bindings
       JS_SetPropertyStr(ctx, global, "_wcBox2D", b2d);
       JS_FreeValue(ctx, global);
   }
   ```

3. **Add JS compatibility shim** so existing game code works:
   ```javascript
   // Game originally does: const box2d = await import('box2d-wasm');
   // Shim intercepts the import and returns linked bindings:
   globalThis.Box2D = {
       createWorld: (gx, gy) => _wcBox2D.createWorld(gx, gy),
       // ...
   };
   ```

4. **Link into cart.wasm:**
   ```bash
   emcc ... obj/box2d3.o obj/box2d_shim.o ... -o build/cart.wasm
   ```

### Modular Carts

Not every game needs every library. Instead of one bloated cart.wasm, build specialized variants:

| Cart | Size | Includes |
|------|------|----------|
| `cart.wasm` | ~5MB | QuickJS + Skia/Ganesh + webaudio (base) |
| `cart_box2d.wasm` | ~5.5MB | Base + box2d3 |
| `cart_rapier.wasm` | ~6MB | Base + Rapier physics |
| `cart_matter.wasm` | ~5.3MB | Base + Matter.js (pure JS, no native) |

The game's `manifest.json` specifies which cart it needs, or the pack tool selects automatically based on the game's imports.

### Intercepting WebAssembly.instantiate

For maximum compatibility, the cart could intercept `WebAssembly.instantiate` calls and return pre-linked bindings for known libraries:

```javascript
globalThis.WebAssembly = {
    async instantiate(bytes, imports) {
        // Check if bytes match a known library (by hash or magic bytes)
        const hash = hashBytes(bytes);
        if (hash === BOX2D_HASH) return { instance: { exports: _wcBox2D } };
        throw new Error('Dynamic WASM loading not supported');
    }
};
```

This lets games that load box2d.wasm via `WebAssembly.instantiate` work without code changes. The bytes are ignored — the pre-linked native implementation is returned instead.

## Security Model

### Comparison: Browser vs jsgamelauncher vs wasmcart-jsgame

| Threat | Browser | jsgamelauncher | wasmcart-jsgame |
|--------|---------|----------------|-----------------|
| XSS (cross-site scripting) | Vulnerable — game JS has DOM access | N/A — no DOM | Immune — no real DOM, JS runs in QuickJS inside WASM |
| Arbitrary network access | Same-origin policy (bypassable) | Full Node.js network stack | Impossible — no network imports except declared WebSocket endpoints |
| Filesystem access | Blocked by sandbox | Full Node.js fs access | Impossible — WASM linear memory only, assets via wc_load_asset |
| Process spawning | Blocked by sandbox | Full Node.js child_process | Impossible — no system calls |
| Cookie/token theft | Vulnerable via JS | N/A | Impossible — no cookies, no document.cookie |
| Memory isolation | Shared browser process | Shared Node.js process | WASM linear memory — separate address space per cart |
| Code injection | eval/innerHTML/script injection | eval available | QuickJS eval runs inside WASM sandbox — can't escape to host |
| Supply chain (malicious deps) | npm packages have full browser access | npm packages have full Node.js access | No npm at runtime — all code bundled in .wasc, runs in WASM sandbox |

### What this does NOT protect against: availability

The table above is about *confidentiality and integrity*, and those claims hold.
It says nothing about **availability**, and a cart can trivially deny it:

```js
function loop(){ while(true){} }
requestAnimationFrame(loop);
```

Three lines, no exotic APIs. `wc_render` never returns and the host hangs with
it. Verified 2026-07-28: `runFrame` did not return in 25 seconds.

A promise-based variant (`function spin(){Promise.resolve().then(spin);}`) IS
bounded -- `cart_main.c` caps microtask draining at `MAX_JOB_MS` per frame -- but
that bound cannot see a synchronous loop, because control never returns to the
job queue at all.

The mitigation QuickJS provides is `JS_SetInterruptHandler`, which is **not
currently used**. Wiring it to a per-frame deadline would let the runtime abort
a game that overruns instead of taking the host down. Until then:

> A malicious cart cannot read your files, reach the network, or escape the
> sandbox -- but it CAN freeze the process running it. Treat "safe to run
> untrusted games" as a statement about your data, not about your uptime.

### Why wasmcart-jsgame is more secure than a browser

A JavaScript game running in a browser has access to the full DOM, cookies, fetch with credentials, and can execute arbitrary code that interacts with the page. Even in an iframe sandbox, escapes and side-channel attacks exist.

A wasmcart-jsgame cart runs the same JavaScript game code but inside QuickJS compiled to WASM. The game cannot:
- Access real DOM (all DOM APIs are inert shims)
- Make network requests (fetch only reads from .wasc assets)
- Read cookies or localStorage from other origins
- Inject scripts into a host page
- Access the host filesystem or spawn processes
- Escape the WASM linear memory sandbox

All I/O goes through the wasmcart ABI: pixels out (GPU), audio out (ring buffer), input in (shared memory), and optionally declared WebSocket endpoints. The game is as sandboxed as a native WASM module — more restricted than a browser tab.

This makes wasmcart-jsgame ideal for **safely sharing and running untrusted JavaScript games** — game marketplaces, retro device game stores, or embedding third-party games without security risk.
