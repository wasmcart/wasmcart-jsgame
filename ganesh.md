# Ganesh GL Interface Fix — Extension Probing + Desktop GL Context Detection

## UPDATE (2026-03-31)

The original fix (hiding extensions from Ganesh) has been implemented and works for
context creation. However, **Ganesh renders only the background on RetroArch** because
RetroArch gives a Core 3.3 (desktop GL) context, not a GLES3 context. Ganesh generates
`#version 300 es` shaders (ES GLSL) which don't produce visible output on Core 3.3.

**This update adds desktop GL context detection so Ganesh uses the correct interface
builder and generates the correct GLSL.**

The fix must work on ALL four hosts:
- **Browser** (WebGL2 context) — `GrGLMakeAssembledGLESInterface`
- **Node.js host** (native-gles EGL GLES3 context) — `GrGLMakeAssembledGLESInterface`
- **wasmcart-native** (EGL GLES3 context) — `GrGLMakeAssembledGLESInterface`
- **RetroArch libretro** (GLX Core 3.3 context) — `GrGLMakeAssembledGLInterface` (desktop)

## The Two Problems

### Problem 1: Extension probing (SOLVED — already implemented)

`wc_gl_get_proc` overrides `glGetString(GL_EXTENSIONS)` → empty,
`glGetStringi(GL_EXTENSIONS)` → null, `glGetIntegerv(GL_NUM_EXTENSIONS)` → 0.
This prevents Ganesh from probing for extension function pointers not in the
WASM import table. Works on all hosts.

### Problem 2: Desktop GL vs GLES context (NEW — this update)

The host always reports `GL_VERSION = "OpenGL ES 3.0 wasmcart"`. This is correct
per the wasmcart spec — carts see ES 3.0 everywhere. But Ganesh needs to know the
ACTUAL context type to generate the right GLSL:

- **GLES context**: `#version 300 es` shaders with `precision mediump float` → works
- **Core 3.3 context**: `#version 330` shaders without precision qualifiers → works
- **GLES shaders on Core 3.3**: renders nothing (shaders compile but produce no output)

## Required Fix

### Step 1: Detect the actual GL context type

The host passes `GL_SHADING_LANGUAGE_VERSION` through real (not overridden). Use this
to detect the context type:

- GLES contexts return: `"OpenGL ES GLSL ES 3.00"` or `"OpenGL ES GLSL ES 3.20"`
- Desktop Core contexts return: `"4.60"` or `"3.30"` (no "ES" prefix)

In `skia_gl_surface.cpp`, before creating the Ganesh interface:

```cpp
// Detect context type from GL_SHADING_LANGUAGE_VERSION (host passes through real value)
static bool is_desktop_gl_context() {
    const char* glsl_ver = (const char*)glGetString(0x8B8C); // GL_SHADING_LANGUAGE_VERSION
    if (!glsl_ver) return false;
    // GLES versions contain "ES" — desktop versions don't
    return (strstr(glsl_ver, "ES") == nullptr);
}
```

### Step 2: Use the correct Ganesh interface builder

In `skia_create_gl_surface()`, replace:

```cpp
// BEFORE (always uses GLES interface):
auto interface = GrGLMakeAssembledGLESInterface(nullptr, wc_gl_get_proc);
```

With:

```cpp
// AFTER (detects context type):
sk_sp<const GrGLInterface> interface;
if (is_desktop_gl_context()) {
    gl_log("Ganesh: using desktop GL interface (Core context detected)");
    interface = GrGLMakeAssembledGLInterface(nullptr, wc_gl_get_proc);
} else {
    gl_log("Ganesh: using GLES interface");
    interface = GrGLMakeAssembledGLESInterface(nullptr, wc_gl_get_proc);
}
```

### Step 3: Override GL_VERSION in wc_gl_get_proc for desktop GL

When using `GrGLMakeAssembledGLInterface` (desktop), Ganesh expects the version
string to be desktop format (e.g., "3.3"). Our `ganesh_glGetString` override
currently returns `"OpenGL ES 3.0 wasmcart"`. For the desktop path, it needs to
return `"3.3.0 wasmcart"`.

Update the `ganesh_glGetString` wrapper in `wc_gl_get_proc`:

```cpp
static bool _use_desktop_gl = false; // set before creating interface

static const unsigned char* ganesh_glGetString(GLenum_ name) {
    if (name == 0x1F02) { // GL_VERSION
        if (_use_desktop_gl) {
            static const unsigned char ver[] = "3.3.0 wasmcart";
            return ver;
        } else {
            static const unsigned char ver[] = "OpenGL ES 3.0 wasmcart";
            return ver;
        }
    }
    if (name == 0x1F03) { // GL_EXTENSIONS
        static const unsigned char empty[] = "";
        return empty;
    }
    return glGetString(name);
}
```

And set `_use_desktop_gl` before calling the interface builder:

```cpp
_use_desktop_gl = is_desktop_gl_context();
sk_sp<const GrGLInterface> interface;
if (_use_desktop_gl) {
    interface = GrGLMakeAssembledGLInterface(nullptr, wc_gl_get_proc);
} else {
    interface = GrGLMakeAssembledGLESInterface(nullptr, wc_gl_get_proc);
}
```

## What NOT to change

- **Do NOT change the host's `gl_imports.cpp`.** The host always reports
  `"OpenGL ES 3.0 wasmcart"` for `GL_VERSION`. This is the wasmcart spec.
- **Do NOT change other WASM GL imports.** The actual GL calls (draw, texture, etc.)
  are the same on GLES3 and Core 3.3. Only the GLSL shader generation differs.
- **Do NOT remove the extension hiding** from `wc_gl_get_proc`. It's still needed
  to prevent Ganesh from probing for missing function pointers on all hosts.

## How it works per host

| Host | Real GL context | GL_SHADING_LANGUAGE_VERSION | Interface builder | Ganesh sees |
|------|----------------|---------------------------|-------------------|-------------|
| Browser | WebGL2 | "OpenGL ES GLSL ES 3.00" | GLES | ES 3.0, #version 300 es |
| Node.js | EGL GLES3 | "OpenGL ES GLSL ES 3.20" | GLES | ES 3.0, #version 300 es |
| wasmcart-native | EGL GLES3 | "OpenGL ES GLSL ES 3.20" | GLES | ES 3.0, #version 300 es |
| RetroArch | GLX Core 3.3 | "4.60" | Desktop GL | GL 3.3, #version 330 |

All four hosts produce correct rendering because Ganesh generates GLSL matching
the actual context's compiler.

## Testing

After making the changes, rebuild the cart and test on ALL hosts:

1. **wasmcart-native**: `wasmcart-run adventure-ai.wasc --fps --uncapped --res 1920x1080`
   - Should show `Ganesh: using GLES interface`
   - Should show `Ganesh: GL surface created successfully!`
   - Should render game content at 500+ FPS uncapped

2. **RetroArch**: `flatpak run ... org.libretro.RetroArch -L .../wasmcart_libretro.so adventure-ai.wasc`
   - Should show `Ganesh: using desktop GL interface (Core context detected)`
   - Should show `Ganesh: GL surface created successfully!`
   - Should render game content (not just background)

3. **Node.js host**: `node --experimental-wasm-exnref retroemu/bin/cli.js adventure-ai.wasc --video sdl`
   - Should show `Ganesh: GL surface created successfully!`
   - Should render game content

4. **Browser**: Load adventure-ai.wasc in wasmcart-web-test
   - Should render game content
   - Console should show `Ganesh: GL surface created successfully!`

5. **Verify Godot carts still work** (they don't use Ganesh, unaffected by this change):
   - `wasmcart-run roboblast.wasc --fps` — PNG textures must load without errors
   - RetroArch with warlords.wasc — must render correctly

## Key principle

The HOST always reports `GL_VERSION = "OpenGL ES 3.0 wasmcart"`. That's the wasmcart
spec. The CART detects the actual context type via `GL_SHADING_LANGUAGE_VERSION`
(passed through real) and adapts internally. The host is a transparent ES 3.0 surface.
The cart is responsible for generating correct shaders for the underlying context.

## UPDATE 2 (2026-03-31): Desktop GL interface MUST NOT fall back to GLES

The cart now detects Core GL context and tries `GrGLMakeAssembledGLInterface` (desktop).
It fails because some desktop GL 3.3 functions are missing from `wc_gl_get_proc`.
The cart falls back to `GrGLMakeAssembledGLESInterface` (GLES). The GLES interface
generates `#version 300 es` shaders that compile on Core 3.3 but produce NO visible
output — the draw calls are silently discarded.

**The fallback to GLES MUST BE REMOVED.** If desktop GL interface fails, the cart
should log which functions are missing and NOT create a Ganesh context at all (fall
back to CPU Skia which works everywhere).

**The fix: make the desktop GL interface work.** Add ALL required GL 3.3 core functions
to the `wc_gl_get_proc` MAP table. The functions that desktop Ganesh needs beyond what
GLES Ganesh needs are:

- `glDrawBuffer` — select front/back draw buffer (Core only, not in ES)
- `glPolygonMode` — wireframe mode (Core only)
- `glTexImage1D` / `glTexSubImage1D` — 1D textures (Core only)
- `glGetTexImage` — read texture data back (Core only)
- Possibly others

To find the exact missing functions, add logging to `wc_gl_get_proc` for the desktop
path and check what returns nullptr that Ganesh considers required:

```cpp
if (result == nullptr) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Ganesh desktop: MISSING %s", name);
    gl_log(msg);
}
```

These functions can be stubbed as no-ops since we're running ES 3.0 content — Ganesh
won't actually call them for ES-level rendering. They just need to be non-null to pass
validation.

**IMPORTANT:** Do NOT fall back to GLES interface on desktop. Either desktop works or
use CPU Skia. GLES shaders on Core 3.3 silently fail to render.

## UPDATE 3 (2026-03-31): VAO 0 FIX MUST BE CART-SIDE

The VAO 0 redirect in gl_imports.cpp fixed Ganesh but broke Three.js. Three.js uses
VAO 0 legitimately with its own vertex attributes. Redirecting VAO 0 globally breaks
all carts that use VAO 0 directly.

**The fix MUST be in the cart's `wc_gl_get_proc` callback**, not in the host. The host
is a transparent passthrough — it cannot change GL behavior for one engine.

In `skia_gl_surface.cpp`, the `wc_gl_get_proc` wrapper for `glBindVertexArray` should
redirect VAO 0 to a Ganesh-owned VAO:

```cpp
static GLuint _ganesh_default_vao = 0;

static void ganesh_glBindVertexArray(GLuint_ vao) {
    if (vao == 0) {
        if (!_ganesh_default_vao) glGenVertexArrays(1, &_ganesh_default_vao);
        vao = _ganesh_default_vao;
    }
    glBindVertexArray(vao);
}

// In wc_gl_get_proc:
if (strcmp(name, "glBindVertexArray") == 0) return (GrGLFuncPtr)ganesh_glBindVertexArray;
```

This way Ganesh always gets a real VAO (required by Core 3.3), while Three.js and other
carts that use VAO 0 directly continue to work through the host's transparent passthrough.

**Why this is cart-side:** Only Ganesh has this problem. Three.js creates its own VAOs.
Godot creates its own VAOs. OpenArena uses gl4es which creates VAOs. Ganesh is the only
engine that binds VAO 0 and expects it to work like GLES (where VAO 0 is always valid).
The cart knows it's using Ganesh and can intercept this one function.
