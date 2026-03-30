# wasmcart-jsgame

Run standard JavaScript browser games as sandboxed wasmcart `.wasc` carts. Canvas 2D, WebGL2, Web Audio, ES modules, gamepad — it all works. No code changes to your game.

A single reusable `cart.wasm` (~5MB) that is essentially a browser runtime in WASM: QuickJS + Skia (GPU-accelerated via Ganesh GL) + webaudio-node + WebGL2 passthrough + 120+ browser API shims.

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

```bash
# SDL window at 1080p
node retroemu/bin/cli.js my_game.wasc --video sdl --res 1920x1080

# Fullscreen
node retroemu/bin/cli.js my_game.wasc --video sdl -f

# Terminal (chafa rendering)
node retroemu/bin/cli.js my_game.wasc
```

The same `.wasc` runs on any wasmcart host — retroemu, wasmcart-native, browser, RetroArch, Knulli handhelds.

## Supported APIs

| API | Notes |
|-----|-------|
| `canvas.getContext('2d')` | GPU-accelerated via Skia Ganesh GL |
| `canvas.getContext('webgl2')` | Direct GPU passthrough, Three.js works |
| `new AudioContext()` | 16 node types, MP3/WAV/OGG/FLAC/AAC decoding |
| `navigator.getGamepads()` | 4 players, standard W3C layout |
| `addEventListener('keydown/keyup')` | Full keyboard with HID mapping |
| `addEventListener('mousedown/pointermove/...)` | Mouse + pointer events |
| `fetch('file.json')` | Loads from .wasc assets |
| `new Image(); img.src = 'sprite.png'` | JPEG/PNG/BMP/GIF via stb_image |
| `import { x } from './module.js'` | ES modules resolve from .wasc |
| `requestAnimationFrame` | 60fps game loop |
| `setTimeout / setInterval` | Timer scheduling |
| `performance.now() / Date.now()` | High-resolution timing |
| `localStorage` | In-memory (persistent save planned) |
| `new FontFace(...)` | TrueType fonts from assets |
| `ctx.drawImage(img, sx,sy,sw,sh, dx,dy,dw,dh)` | Full 9-arg sprite sheet support |
| `console.log` | Outputs to host terminal |
| `crypto.getRandomValues` | RNG for game IDs |
| `atob / btoa` | Base64 encode/decode |
| `Worker` | Cooperative (separate QuickJS runtimes) |
| `WebSocket` | Via wasmcart WS ABI (manifest allowlist) |

120+ additional DOM/browser APIs shimmed (document, window, navigator, Blob, URL, Event, MutationObserver, etc.). See [architecture.md](architecture.md) for the full list.

## Canvas 2D — GPU Accelerated

Canvas 2D rendering uses Skia's Ganesh GL backend. All drawing happens on the GPU with zero CPU pixel copies. Direct FBO blit to display.

**Implemented:** fillRect, clearRect, strokeRect, fillText, strokeText, measureText, drawImage (3/5/9-arg), beginPath, closePath, moveTo, lineTo, arc, arcTo, quadraticCurveTo, bezierCurveTo, rect, fill, stroke, clip, save, restore, translate, rotate, scale, setTransform, resetTransform, putImageData, getImageData, createLinearGradient, createRadialGradient

Games with fixed canvas size (e.g. 640x480) automatically scale to fill the host window with letterboxing, like CSS scaling in a browser.

## WebGL2 — Three.js and Beyond

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
| `examples/hello_canvas/` | Canvas 2D | Bouncing ball, text, colors |
| `examples/hello_audio/` | Web Audio | Oscillator tones |
| `examples/hello_fetch/` | fetch/modules | Asset loading, ES imports |
| `examples/hello_webgl/` | WebGL2 | Raw GL triangle |
| `examples/threejs/` | WebGL2 + Audio | Three.js 3D scene — PBR materials, textures, lights, gamepad camera orbit, sound effects |

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

## Security

The game runs in WASM linear memory inside QuickJS. More secure than running the same game in a browser:

- No real DOM access (XSS impossible)
- No arbitrary network requests (fetch loads from .wasc only)
- No filesystem or process access
- No cookies or cross-origin data
- Code can't escape the WASM sandbox

All I/O goes through the wasmcart ABI: pixels out (GPU), audio out (ring buffer), input in (shared memory). See [architecture.md](architecture.md) for detailed security comparison.

## Build the Runtime

Only needed if you're modifying the runtime itself. Game developers just use the pre-built `build/cart.wasm`.

**Prerequisites:** Emscripten SDK, QuickJS (`bash setup_quickjs.sh`), Skia bitcode (`cd ../wasmcart-skia && bash build.sh`), webaudio-node sources

```bash
bash build.sh          # → build/cart.wasm (~5MB)
```

## Architecture

See [architecture.md](architecture.md) for:
- Ganesh GL pipeline (how GPU-accelerated Canvas 2D works in WASM)
- Skia build requirements
- Feature parity tables (Browser vs jsgamelauncher vs wasmcart-jsgame)
- Security model comparison
- Native library integration (WASM-in-WASM problem)
- Upcoming networking API (wc_fetch)
