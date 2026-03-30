# Three.js Demo

GPU-accelerated 3D scene with PBR materials, texture mapping, dynamic lights, and audio — running inside a sandboxed WASM cart.

```bash
node retroemu/bin/cli.js examples/threejs/threejs.wasc --video sdl --res 1920x1080
```

## What's in it

- **PBR materials** — metallic cube, glossy sphere, emissive torus knot (MeshStandardMaterial)
- **Texture mapping** — JPEG floor texture decoded via stb_image, uploaded as DataTexture
- **Dynamic lighting** — ambient light, directional light, orbiting point light with additive glow sprite
- **Audio** — laser sound effect on gamepad button press (MP3 decoded via webaudio-node)
- **Gamepad camera** — left stick orbits, right stick zooms, face buttons trigger sounds

## Controls

| Input | Action |
|-------|--------|
| Left stick X | Orbit camera horizontally |
| Left stick Y | Raise/lower camera |
| Right stick Y | Zoom in/out |
| A / B / X / Y | Play laser sound |
| Guide + Start | Exit |

## Performance

~1800fps uncapped at 1920x1080 (AMD Radeon 890M). WebGL2 calls go directly to the host GPU — no Skia, no pixel copies.

## How it works

Three.js ESM source is included directly (not bundled). The game imports from `./three.js` which resolves from the .wasc assets. All GL calls pass through the cart's WebGL2 shim straight to the host GPU.

To make your own Three.js game:
```bash
cp node_modules/three/build/three.module.js my_game/three.js
cp node_modules/three/build/three.core.js my_game/
# Change: import * as THREE from 'three'
# To:     import * as THREE from './three.js'
bash pack_game.sh my_game/ my_game.wasc "My 3D Game"
```
