# Ganesh RetroArch Debug — Complete State (2026-03-31)

## The Core Mystery

Three.js (WebGL2 direct) renders perfectly on RetroArch. Same GL context, same GL imports,
same driver. Ganesh (Skia's GPU backend) on the SAME context: only fillRect works. Textured
draws produce zero fragments. No GL errors. Shaders compile. Programs link. Textures upload.
Samplers bind.

## What This Proves

- The GL context works (Three.js proves it)
- The GL import table works (Three.js uses the same `gl.*` WASM imports)
- The host's FBO redirect works (Three.js renders through it)
- GLES shaders work on Core 3.3 (Three.js uses `#version 300 es`)
- Texture sampling works on this GPU (Three.js textures render)
- The driver is fine

## What's Different About Ganesh

Three.js calls GL functions DIRECTLY through WASM imports:
```
Game JS → webgl_shim.c → WASM import (gl.glDrawArrays) → host GL
```

Ganesh calls GL functions through `wc_gl_get_proc` wrapper functions:
```
Skia canvas op → Ganesh → wc_gl_get_proc function pointer → wrapper → WASM import → host GL
```

The wrapper functions (`w_glDrawArrays`, `w_glBindTexture`, etc.) are simple pass-throughs.
They call the EXACT same WASM imports. But Ganesh also calls functions through its
GrGLInterface function table, which includes functions that might be stubs.

## Tested Approaches (ALL FAILED)

| Approach | Result |
|----------|--------|
| Desktop GL interface (GrGLMakeAssembledGLInterface) | fillRect only |
| GLES interface (GrGLMakeAssembledGLESInterface) on desktop | fillRect only |
| Shader patching (#version 300 es → #version 330) | fillRect only |
| No shader patching (raw #version 300 es on Core 3.3) | fillRect only |
| Per-op flushAndSubmit(kYes) | fillRect only |
| Per-op flushAndSubmit(kNo) | fillRect only |
| No per-op flush (batch all, flush at end of frame) | fillRect only |
| Per-op resetContext | fillRect only |
| No per-op resetContext | fillRect only |
| glMapBufferRange stub returning null | fillRect only |
| Force Core Profile detection | fillRect only |
| Force GLES version string on desktop | fillRect only |
| layout(location=0) on fragment output | fillRect only |
| 24 correctly-typed desktop GL function stubs | fillRect only |

## Verified Data at Frame 60

```
Ganesh FBO GL-bottom(y=10):  R=252 G=216 B=168  ← background fill color
Ganesh FBO GL-center(y=540): R=252 G=216 B=168  ← background fill color
Ganesh FBO GL-top(y=1070):   R=252 G=216 B=168  ← background fill color
```

Every pixel is the background fill. The fillRect covers the entire canvas (640x480 scaled).
No tile, sprite, or text content is visible. The content never renders into the FBO.

## What We Know Works

- `glTexSubImage2D` called with real data: 19x31, 21x25, 20x25, 21x33, 5x32 (font glyphs)
- `glGenSamplers` creates IDs 1, 2
- `glBindSampler` binds to unit 0
- 5 shader programs link OK
- fillRect (solid color, no texture) renders correctly
- The readback path (glReadPixels → glTexImage2D → draw quad) works
- The host's redirect FBO receives content from the readback path

## Root Cause Hypothesis

Ganesh's function pointer table (`wc_gl_get_proc`) wraps GL functions. For MOST functions,
the wrappers are pass-throughs. But some functions have the WRONG behavior:

1. **Cached state mismatch**: Ganesh caches GL state (bound FBO, bound textures, active
   texture unit, bound program). After a draw, Ganesh assumes state hasn't changed. But
   the host's GL import interceptors might change state (e.g., the FBO redirect interceptor
   changes the REAL FBO but Ganesh doesn't know). On the next textured draw, Ganesh skips
   rebinding because its cache says "already bound" → draws go to wrong FBO or with wrong
   texture.

2. **FBO identity collision**: Host redirect FBO and Ganesh render target may have the same
   virtual ID in the host's GL import table. Ganesh binds "its" FBO but actually binds the
   host's redirect FBO. fillRect fills the redirect FBO (visible). Textured draws also go
   there but get overwritten by the NEXT frame's background fill.

3. **A specific GL function stub** that Ganesh calls during textured draw setup (not during
   fill setup) is returning wrong data. This function isn't called during fillRect because
   fillRect doesn't use textures. The wrong return value causes Ganesh to set up incorrect
   GL state for the textured draw.

## Next Steps

1. **GL call trace**: Use apitrace or similar to capture the EXACT GL calls Ganesh makes
   during one frame on RetroArch vs one frame on wasmcart-native. Diff the traces to find
   the divergence point.

2. **Instrument ALL Ganesh GL calls**: Replace every function in `wc_gl_get_proc` with a
   logging wrapper that prints the function name and arguments. Compare the call sequence
   between working (Node) and failing (RetroArch) hosts.

3. **Check FBO binding during textured draw**: Add a `glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING)`
   check IMMEDIATELY before each `glDrawArrays`/`glDrawElements` call inside Ganesh (via
   wrapper) to see if Ganesh is drawing to the correct FBO.

4. **Check if the issue is the readback path**: On RetroArch, the desktop path uses
   readback (glReadPixels → texture upload → draw quad to FBO 0). Test if the readback
   itself produces correct data by logging the first few pixels.

## Files

- `wasmcart-jsgame/src/skia_gl_surface.cpp` — Ganesh setup, wc_gl_get_proc, stubs
- `wasmcart-jsgame/src/canvas2d_skia.c` — Flush, readback, blit
- `napi-canvas/skia/src/gpu/ganesh/gl/GrGLGpu.cpp` — GPU backend (5000+ lines)
- `napi-canvas/skia/src/gpu/ganesh/gl/GrGLCaps.cpp` — Capability detection (5000+ lines)
- `napi-canvas/skia/src/gpu/ganesh/gl/GrGLProgram.cpp` — Texture binding
- `napi-canvas/skia/src/gpu/ganesh/gl/GrGLOpsRenderPass.cpp` — Draw dispatch

## DEFINITIVE FINDING (2026-03-31)

Full GL call traces captured on both Node (working) and RetroArch (failing).
The traces are FUNCTIONALLY IDENTICAL — same calls, same order, same parameters.
762 calls on RetroArch including ~67 textured glDrawArrays calls.

The cart sends correct GL commands. The host's gl_imports.cpp must debug:

1. Add logging INSIDE gl_imports.cpp for glBindTexture showing both virtual ID
   and the REAL GL texture ID being bound
2. Add glGetError() checks after EACH real glDrawArrays call
3. After glTexSubImage2D, read back the texture with glGetTexImage to verify
   the upload actually stored pixel data in the real texture
4. After each glBindFramebuffer, verify with real glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING)
   that the correct real FBO is bound

The cart-side wc_gl_get_proc wrappers are pass-throughs. The only way the
draws can fail is if the host's translation from virtual IDs to real GL objects
is broken.

## Key Insight

The GLES interface works on desktop Core 3.3 (Mesa GL_ARB_ES3_compatibility). Both
GLES and desktop interfaces produce the same failure. This means the issue is NOT in
the interface type or shader generation. It's in the RUNTIME draw dispatch — something
Ganesh does differently when drawing textured geometry vs solid fills, regardless of
interface type.

Three.js bypasses Ganesh entirely and works. Ganesh adds its own state management
layer that has a bug specific to the RetroArch/libretro GL context. The most likely
cause is an FBO binding state mismatch caused by the host's GL import interceptors
conflicting with Ganesh's cached state assumptions.
