# Ganesh Desktop GL — Deep Debug Status

## Problem
On RetroArch (GLX Core 3.3), Ganesh renders fillRect (solid color) but NOT textured
draws (drawImage, sprites, text). All other hosts (browser WebGL2, Node GLES3,
wasmcart-native GLES3) work perfectly.

## What Works
- Desktop GL interface assembly (GrGLMakeAssembledGLInterface) ✓
- Context creation ✓
- Render target (offscreen FBO) creation ✓
- Shader compilation (#version 330 via ganesh_glGetString) ✓
- Program linking ✓
- No GL errors during rendering ✓
- Texture upload via glTexSubImage2D (confirmed with logging: data=YES, sizes correct) ✓
- fillRect solid color fill ✓
- Readback from Ganesh FBO shows correct background color ✓

## What Fails
- drawImage (textured draws) — produces no fragments
- Path fills with stencil — not tested separately
- Text rendering — not tested separately
- Everything beyond solid color fillRect

## Confirmed NOT the Cause
1. **Shaders** — #version 300 es accepted by Mesa GL_ARB_ES3_compatibility, programs link OK
2. **Shader patching** — tried both #version 330 and raw #version 300 es, same result
3. **FBO binding** — verified Ganesh FBO has correct content on other hosts
4. **FBO redirect/blit** — readPixels confirms content in redirect FBO (host-side pre-blit logging)
5. **Buffer mapping** — stubbed glMapBufferRange to return null, same result
6. **Per-op flush** — kYes sync flush on every draw, same result
7. **resetContext** — tried with and without per-op resetContext, same result
8. **GL errors** — none detected after draws
9. **Texture upload** — glTexSubImage2D called with real data pointers, correct sizes
10. **layout(location=0)** — tried adding to fragment output, no effect
11. **GL version string** — tried GL 2.1 (fails MSAA), GL 3.3 works for interface

## Root Cause Analysis

The Ganesh FBO at frame 60 shows R=252 G=216 B=168 (background fill color) at
EVERY pixel position (top, center, bottom). This means:

1. The fillRect background draw executes and fills the FBO ✓
2. Subsequent textured draws (drawImage for tiles/sprites) execute (glTexSubImage2D logged)
3. But the textured draws produce NO FRAGMENTS — the background color is never overwritten

Possible remaining causes:

### A. Sampler Objects (Most Likely)
Desktop GL 3.3 enables `fUseSamplerObjects = true` (GrGLCaps.cpp line 865).
Ganesh creates sampler objects via `glGenSamplers`/`glBindSampler`/`glSamplerParameteri`.
If these functions go through the host's GL import table and the host's implementation
doesn't handle sampler objects correctly (e.g., virtual ID mapping issues), texture
sampling fails silently — the sampler doesn't filter correctly and the shader reads
(0,0,0,0) from the texture.

### B. Texture Swizzle
Desktop GL 3.3 enables `fTextureSwizzleSupport = true` (GrGLCaps.cpp line 331).
Ganesh may call `glTexParameteriv(GL_TEXTURE_SWIZZLE_RGBA, ...)` to remap color channels.
Our glTexParameteriv wrapper passes through to the host. If the host auto-stubs this
(because the cart imports it from `gl` module but host doesn't provide it), the swizzle
call silently fails and texture colors are wrong (possibly all zeros).

### C. Texture Format Mismatch
Desktop GL `glTexStorage2D` + `glTexSubImage2D` with format GL_RGBA (0x1908) should work.
But desktop GL differentiates between sized internal formats (GL_RGBA8) and unsized (GL_RGBA).
If Ganesh passes the wrong internal format, the texture might be created but empty.

### D. Core Profile VAO Requirement
Desktop GL Core 3.3 requires a VAO to be bound for ALL draws. Ganesh creates VAOs.
But if resetContext() causes Ganesh to unbind its VAO, subsequent draws silently fail.
This was tested by removing per-op resetContext — no improvement.

## Next Steps

1. **Instrument sampler object calls** — Log glGenSamplers, glBindSampler, glSamplerParameteri
   to verify they're being called and what IDs/params they use.

2. **Instrument glTexParameteriv** — Check if GL_TEXTURE_SWIZZLE_RGBA is being called
   and what values are passed.

3. **Read texture content** — After glTexSubImage2D, read the texture back with
   glGetTexImage (desktop GL only) to verify the upload succeeded.

4. **Compare GL call traces** — Capture full GL call log on web (working) vs RetroArch
   (failing) for a single drawImage operation and diff them.

5. **Debugger** — Step through GrGLGpu::onDraw or GrGLOpsRenderPass::onDraw on RetroArch
   to see exactly where the draw produces no fragments.

## Files Reference
- Cart: `wasmcart-jsgame/src/skia_gl_surface.cpp` — Ganesh setup, wc_gl_get_proc
- Cart: `wasmcart-jsgame/src/canvas2d_skia.c` — Draw dispatch, flush, blit
- Skia: `napi-canvas/skia/src/gpu/ganesh/gl/GrGLGpu.cpp` — GPU backend
- Skia: `napi-canvas/skia/src/gpu/ganesh/gl/GrGLCaps.cpp` — Capability detection
- Skia: `napi-canvas/skia/src/gpu/ganesh/gl/GrGLProgram.cpp` — Texture binding
- Host: `wasmcart-native/src/gl_imports.cpp` — GL function implementations
