# wasmcart-jsgame

A sandboxed JavaScript game runtime in a 5MB WASM binary. Write a standard browser game — Canvas 2D, WebGL2, Web Audio, ES modules, gamepad — pack it, run it anywhere. Desktop, browser, ARM handheld, [RetroArch](https://www.retroarch.com). No code changes.

**GPU-accelerated Canvas 2D** via [Skia](https://skia.org) Ganesh GL. **Direct WebGL2** passthrough to the host GPU. **Full Web Audio** with 16 node types and 5 audio decoders, via [webaudio-node](https://github.com/monteslu/webaudio-node). **~50 browser API globals** shimmed (plus their methods) so real games work unmodified.

Runs on every [wasmcart](https://github.com/wasmcart/wasmcart) host: Node.js (SDL), browser (WebGL2), [wasmcart-native](https://github.com/wasmcart/wasmcart-native) (EGL), RetroArch ([wasmcart-libretro](https://github.com/wasmcart/wasmcart-libretro)). Same `.wasc` file, same game, everywhere.

**More secure than a browser.** Games run inside [QuickJS](https://bellard.org/quickjs/) inside WASM — no DOM access, no XSS, no arbitrary network, no filesystem. You can safely run untrusted JavaScript games the way you'd run a SNES ROM.

## Make a Game

You don't need to build anything. Grab the pre-built `build/cart.wasm`, write your game, pack it.

### 1. Write standard browser JS

```javascript
// main.js
const canvas = document.getElementById('game');
canvas.width = 800;
canvas.height = 600;
const ctx = canvas.getContext('2d');

function gameLoop(timestamp) {
    ctx.clearRect(0, 0, 800, 600);
    ctx.fillStyle = '#ff0000';
    ctx.fillRect(100, 100, 200, 150);
    requestAnimationFrame(gameLoop);
}
requestAnimationFrame(gameLoop);
```

### 2. Pack it

```bash
bash pack_game.sh my_game/ my_game.wasc "My Game"
```

### 3. Run it

These are GL carts, so they need a GL-capable host:

```bash
# SDL window at 1080p
npx retroemu my_game.wasc --video sdl --res 1920x1080

# Fullscreen
npx retroemu my_game.wasc --video sdl -f

# Terminal (chafa rendering)
npx retroemu my_game.wasc
```

The same `.wasc` runs on any wasmcart host —
[retroemu](https://github.com/monteslu/retroemu),
[wasmcart-native](https://github.com/wasmcart/wasmcart-native), browser,
RetroArch, [Knulli](https://knulli.org) handhelds.

## Supported APIs

| API | Notes |
|-----|-------|
| `canvas.getContext('2d')` | GPU-accelerated via Skia Ganesh GL |
| `canvas.getContext('webgl2')` | Direct GPU passthrough, [Three.js](https://threejs.org) works |
| `new AudioContext()` | 16 node types, MP3/WAV/OGG/FLAC/AAC decoding |
| `navigator.getGamepads()` | 4 players, standard W3C layout |
| `addEventListener('keydown/keyup')` | Full keyboard with HID mapping |
| `addEventListener('mousedown/pointermove/...)` | Mouse + pointer events |
| `fetch('file.json')` | Loads from .wasc assets |
| `new Image(); img.src = 'sprite.png'` | JPEG/PNG/BMP/GIF via [stb_image](https://github.com/nothings/stb) |
| `import { x } from './module.js'` | ES modules resolve from .wasc |
| `requestAnimationFrame` | 60fps game loop |
| `setTimeout / setInterval` | Timer scheduling |
| `performance.now() / Date.now()` | High-resolution timing |
| `localStorage` | Persists via wasmcart save region (64KB, host-owned) |
| `new FontFace(...)` | TrueType fonts from assets |
| `ctx.drawImage(img, sx,sy,sw,sh, dx,dy,dw,dh)` | Full 9-arg sprite sheet support |
| `console.log` | Outputs to host terminal |
| `crypto.getRandomValues` | RNG for game IDs |
| `atob / btoa` | Base64 encode/decode |
| `Worker` | Cooperative (separate QuickJS runtimes), JSON message passing |
| `WebSocket` | Via wasmcart WS ABI (manifest allowlist) |

`localStorage` **persists**, backed by the wasmcart save region
(`wc_info_t.save_ptr`/`save_size`). The host loads any existing bytes before
`wc_init` and reads them back to store however it likes — the CLI player keeps
a `.sav` next to the cart. Budget is 64KB; exceeding it throws a real
`QuotaExceededError` and leaves the existing store untouched rather than
truncating it. A corrupt or foreign save is detected and ignored, so a bad
`.sav` starts the game fresh instead of bricking it.

~50 browser globals are shimmed (document, window, navigator, Blob, URL, Event,
MutationObserver, localStorage, performance, screen, crypto, atob/btoa, …), each
with the methods a game actually reaches for. Counted from `src/cart_main.c`;
see [architecture.md](architecture.md) for the breakdown.

## Canvas 2D — GPU Accelerated

Canvas 2D rendering uses Skia's Ganesh GL backend. All drawing happens on the GPU with zero CPU pixel copies. Direct FBO blit to display.

**Implemented:** fillRect, clearRect, strokeRect, fillText, strokeText, measureText, drawImage (3/5/9-arg), beginPath, closePath, moveTo, lineTo, arc, arcTo, quadraticCurveTo, bezierCurveTo, rect, roundRect, ellipse, fill, stroke, clip, save, restore, translate, rotate, scale, setTransform, resetTransform, getTransform, setLineDash, getLineDash, isPointInPath, isPointInStroke, putImageData, getImageData, createImageData, createLinearGradient, createRadialGradient, createConicGradient, createPattern

`putImageData`, `drawImage` and `getImageData` validate the pixel buffer
against the width/height you pass. A buffer shorter than `w * h * 4` throws a
RangeError naming both sizes rather than reading past the end of it.

Games with fixed canvas size (e.g. 640x480) automatically scale to fill the host window with letterboxing, like CSS scaling in a browser.

## WebGL2 — [Three.js](https://threejs.org) and Beyond

WebGL calls go directly to the host GPU. No framebuffer copy. No software rendering.

```bash
# Copy three.js ESM source into your game directory
cp node_modules/three/build/three.module.js my_game/three.js
# Change imports: from 'three' → from './three.js'
bash pack_game.sh my_game/ my_game.wasc "My 3D Game"
```

## Examples

| Example | Type | Description |
|---------|------|-------------|
| [`examples/hello_canvas/`](examples/hello_canvas) | Canvas 2D | Bouncing ball, text, colors |
| [`examples/hello_audio/`](examples/hello_audio) | Web Audio | Oscillator tones |
| [`examples/hello_fetch/`](examples/hello_fetch) | fetch/modules | Asset loading, ES imports |
| [`examples/hello_webgl/`](examples/hello_webgl) | WebGL2 | Raw GL triangle |
| [`examples/threejs/`](examples/threejs) | WebGL2 + Audio | Three.js 3D scene — PBR materials, textures, lights, gamepad camera orbit, sound effects |

The `hello_*` examples include ready-to-run `.wasc` files. The Three.js demo includes source + `.wasc`.

Larger game examples (space shooter, adventure RPG) are in the [jsgames](https://github.com/monteslu/jsgames) repo. Pack them with:

```bash
bash pack_game.sh /path/to/jsgames/space/ space.wasc "Space Game"
```

## Performance (AMD Radeon 890M, 1920x1080 uncapped)

| Game | Type | FPS |
|------|------|-----|
| Space shooter | Canvas 2D + Audio | ~850 |
| Adventure AI | Canvas 2D + drawImage sprites | ~450 |
| Three.js demo | WebGL2 + Audio | ~1800 |

## Runs Everywhere

| Host | Context | Canvas 2D | WebGL2 | Status |
|------|---------|-----------|--------|--------|
| Browser | WebGL2 | Ganesh GL (GPU) | Direct | Working |
| Node.js + SDL | [native-gles](https://github.com/monteslu/native-gles) EGL | Ganesh GL (GPU) | Direct | Working |
| [wasmcart-native](https://github.com/wasmcart/wasmcart-native) | EGL GLES3 | Ganesh GL (GPU) | Direct | Working |
| RetroArch ([libretro](https://github.com/wasmcart/wasmcart-libretro)) | GLX Core 3.3 | Ganesh GL (GPU) | Direct | Working |

The same `.wasc` file runs on all hosts. Canvas 2D games get GPU acceleration via Skia Ganesh on every platform — including RetroArch's desktop GL Core 3.3 context, which required cart-side shader compatibility and VAO workarounds for Core Profile.

## Security — Safer Than a Browser

The game runs inside QuickJS (JS interpreter) inside WASM (memory sandbox). This is **more secure than running the same game in a browser**:

| Threat | Browser | wasmcart-jsgame |
|--------|---------|-----------------|
| XSS | Vulnerable | Impossible — no real DOM |
| Cookie/token theft | Vulnerable | Impossible — no cookies |
| Arbitrary network | Same-origin policy (bypassable) | No network — assets from .wasc only |
| Filesystem access | Blocked by sandbox | Impossible — WASM linear memory only |
| Code injection | eval/innerHTML | QuickJS eval stays inside WASM sandbox |
| Supply chain | npm packages have full access | No npm at runtime — all code in .wasc |

Games are isolated like ROM files. You can download a `.wasc` from anyone and run it safely — the game can't steal data, can't phone home, can't escape the sandbox. It can only render pixels and play audio.

See [architecture.md](architecture.md) for full security comparison and feature parity tables.

## Testing

```bash
node test/regression.mjs        # 17 checks, ~8s (needs build/cart.wasm)
```

Every check maps to one fixed bug. They exist because this class of failure is
almost impossible to diagnose twice: a corrupted QuickJS heap, an unlinked
object file, and an out-of-bounds write all surface as the same bare
`memory access out of bounds`, with nothing pointing at the cause.

Each one was verified in **both** directions — the fix reverted, rebuilt, and
the check confirmed to go red. A check that has never failed is not known to
work.

Two conventions worth keeping if you add to it:

- **Don't validate rendering with an in-process GL readback.** Sixteen checks
  read pixels back through the same GL path the cart draws into, so a
  whole-frame vertical flip cancelled out and every one stayed green while
  every 2D frame shipped upside down. The orientation check goes through the
  shipped player and decodes the PNG instead.
- **Assert the probe can move.** More than one "bug found" here was a broken
  harness — a probe that embedded empty base64, a shader that failed to link,
  a stale `.wasc`. Pair every hostile case with an honest control that must
  visibly succeed, and give the probe a sentinel value for "nothing drew".

The suite needs `../wasmcart` for the host and picks up `../jsgames` if present
(one check packs `space`, and skips cleanly if it isn't there).

## Build the Runtime

Only needed if you're modifying the runtime itself. Game developers just use the pre-built `build/cart.wasm`.

**Prerequisites**, as sibling checkouts next to this repo:

| | what | why |
|---|---|---|
| [emsdk](https://emscripten.org) | `../emsdk`, or `emcc` on PATH | the compiler |
| [QuickJS](https://bellard.org/quickjs/) | `bash setup_quickjs.sh` | the JS engine. Builds against bellard/quickjs or [quickjs-ng](https://github.com/quickjs-ng/quickjs) — `cutils.c` exists only in the former and is handled conditionally |
| [Skia](https://skia.org) | `cd ../wasmcart-skia && bash build.sh` | Canvas 2D. Needs `../napi-canvas` (the Skia C wrapper) and pulls Skia itself (~2GB, 10–20 min first run) |
| [build-libcanvas](https://github.com/monteslu/build-libcanvas) | applies `patches/` to napi-canvas | **required** — one patch fixes a wasm crash on the first Canvas 2D call |
| [webaudio-node](https://github.com/monteslu/webaudio-node) | `../webaudio-node` | audio decoders |

```bash
bash build.sh          # → build/cart.wasm (~5MB)
```

**Editing the Skia wrapper?** `wasmcart-skia/out/include/skia_c.{cpp,hpp}` are
build *outputs*, copied from `napi-canvas/skia-c/`. Editing them does nothing.
Source changes belong in `build-libcanvas/patches/`. See
[REBUILD_STATUS.md](REBUILD_STATUS.md), which records that trap and four build
bugs fixed in July 2026.

## Architecture

See [architecture.md](architecture.md) for:
- Ganesh GL pipeline (how GPU-accelerated Canvas 2D works in WASM)
- Skia build requirements
- Feature parity tables (Browser vs jsgamelauncher vs wasmcart-jsgame)
- Security model comparison
- Native library integration (WASM-in-WASM problem)
- Upcoming networking API (wc_fetch)
