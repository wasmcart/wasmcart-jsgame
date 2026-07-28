/*
 * skia_gl_surface.cpp — Ganesh GL surface for Skia in WASM
 *
 * WASM imports can't be used as function pointers (they're not in the
 * WASM function table). Skia's GrGLInterface stores GL functions as
 * pointers. We create static wrapper functions for each GL import that
 * ARE in the function table, then give those to Skia.
 */

#include <include/core/SkSurface.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkGraphics.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/gl/GrGLDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>
#include <include/gpu/ganesh/gl/GrGLAssembleInterface.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <include/gpu/ganesh/gl/GrGLTypes.h>

#include <cstring>

#define WC_USE_GL
#include "wasmcart.h"

/* ── Additional GL imports needed by Ganesh (not in wasmcart.h) ── */

typedef void* GLsync_;
typedef unsigned long long GLuint64_;

#ifdef __wasm__
#define _GL_IMPORT_EXTRA(name) __attribute__((import_module("gl"), import_name(#name)))
#else
#define _GL_IMPORT_EXTRA(name)
#endif

_GL_IMPORT_EXTRA(glGetStringi)    extern const unsigned char* glGetStringi(GLenum name, GLuint index);
_GL_IMPORT_EXTRA(glIsEnabled)     extern GLboolean glIsEnabled(GLenum cap);
_GL_IMPORT_EXTRA(glGetBooleanv)   extern void glGetBooleanv(GLenum pname, GLboolean* data);
_GL_IMPORT_EXTRA(glGetFloatv)     extern void glGetFloatv(GLenum pname, GLfloat* data);
_GL_IMPORT_EXTRA(glMapBufferRange) extern void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
_GL_IMPORT_EXTRA(glUnmapBuffer)   extern GLboolean glUnmapBuffer(GLenum target);
_GL_IMPORT_EXTRA(glFlushMappedBufferRange) extern void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);
_GL_IMPORT_EXTRA(glRenderbufferStorageMultisample) extern void glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
_GL_IMPORT_EXTRA(glBlitFramebuffer) extern void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
_GL_IMPORT_EXTRA(glInvalidateFramebuffer) extern void glInvalidateFramebuffer(GLenum target, GLsizei numAttachments, const GLenum* attachments);
_GL_IMPORT_EXTRA(glTexImage3D)    extern void glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels);
_GL_IMPORT_EXTRA(glTexSubImage3D) extern void glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels);
_GL_IMPORT_EXTRA(glTexStorage3D)  extern void glTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth);
_GL_IMPORT_EXTRA(glFenceSync)     extern GLsync_ glFenceSync(GLenum condition, GLbitfield flags);
_GL_IMPORT_EXTRA(glDeleteSync)    extern void glDeleteSync(GLsync_ sync);
_GL_IMPORT_EXTRA(glClientWaitSync) extern GLenum glClientWaitSync(GLsync_ sync, GLbitfield flags, GLuint64_ timeout);

/* Additional Ganesh-required GL functions */
_GL_IMPORT_EXTRA(glCompressedTexImage2D) extern void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* data);
_GL_IMPORT_EXTRA(glCompressedTexSubImage2D) extern void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data);
_GL_IMPORT_EXTRA(glCopyTexSubImage2D) extern void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
_GL_IMPORT_EXTRA(glGetBufferParameteriv) extern void glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params);
_GL_IMPORT_EXTRA(glIsTexture)     extern GLboolean glIsTexture(GLuint texture);
_GL_IMPORT_EXTRA(glTexParameterf) extern void glTexParameterf(GLenum target, GLenum pname, GLfloat param);
_GL_IMPORT_EXTRA(glTexParameterfv) extern void glTexParameterfv(GLenum target, GLenum pname, const GLfloat* params);
_GL_IMPORT_EXTRA(glTexParameteriv) extern void glTexParameteriv(GLenum target, GLenum pname, const GLint* params);
_GL_IMPORT_EXTRA(glUniform2i)     extern void glUniform2i(GLint location, GLint v0, GLint v1);
_GL_IMPORT_EXTRA(glUniform3i)     extern void glUniform3i(GLint location, GLint v0, GLint v1, GLint v2);
_GL_IMPORT_EXTRA(glUniform4i)     extern void glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
_GL_IMPORT_EXTRA(glUniform2iv)    extern void glUniform2iv(GLint location, GLsizei count, const GLint* value);
_GL_IMPORT_EXTRA(glUniform3iv)    extern void glUniform3iv(GLint location, GLsizei count, const GLint* value);
_GL_IMPORT_EXTRA(glUniform4iv)    extern void glUniform4iv(GLint location, GLsizei count, const GLint* value);
_GL_IMPORT_EXTRA(glVertexAttrib1f) extern void glVertexAttrib1f(GLuint index, GLfloat x);
_GL_IMPORT_EXTRA(glVertexAttrib2fv) extern void glVertexAttrib2fv(GLuint index, const GLfloat* v);
_GL_IMPORT_EXTRA(glVertexAttrib3fv) extern void glVertexAttrib3fv(GLuint index, const GLfloat* v);
_GL_IMPORT_EXTRA(glVertexAttrib4fv) extern void glVertexAttrib4fv(GLuint index, const GLfloat* v);
_GL_IMPORT_EXTRA(glReadBuffer)    extern void glReadBuffer(GLenum mode);
_GL_IMPORT_EXTRA(glDrawRangeElements) extern void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices);
_GL_IMPORT_EXTRA(glVertexAttribIPointer) extern void glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer);
_GL_IMPORT_EXTRA(glGetFramebufferAttachmentParameteriv) extern void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint* params);
_GL_IMPORT_EXTRA(glGetRenderbufferParameteriv) extern void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint* params);
_GL_IMPORT_EXTRA(glCopyBufferSubData) extern void glCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
_GL_IMPORT_EXTRA(glIsSync)        extern GLboolean glIsSync(GLsync_ sync);
_GL_IMPORT_EXTRA(glWaitSync)      extern void glWaitSync(GLsync_ sync, GLbitfield flags, GLuint64_ timeout);
_GL_IMPORT_EXTRA(glGetInternalformativ) extern void glGetInternalformativ(GLenum target, GLenum internalformat, GLenum pname, GLsizei count, GLint* params);
_GL_IMPORT_EXTRA(glGetProgramBinary) extern void glGetProgramBinary(GLuint program, GLsizei bufSize, GLsizei* length, GLenum* binaryFormat, void* binary);
_GL_IMPORT_EXTRA(glProgramBinary) extern void glProgramBinary(GLuint program, GLenum binaryFormat, const void* binary, GLsizei length);
_GL_IMPORT_EXTRA(glProgramParameteri) extern void glProgramParameteri(GLuint program, GLenum pname, GLint value);
_GL_IMPORT_EXTRA(glBindSampler)   extern void glBindSampler(GLuint unit, GLuint sampler);
_GL_IMPORT_EXTRA(glDeleteSamplers) extern void glDeleteSamplers(GLsizei count, const GLuint* samplers);
_GL_IMPORT_EXTRA(glGenSamplers)   extern void glGenSamplers(GLsizei count, GLuint* samplers);
_GL_IMPORT_EXTRA(glSamplerParameterf) extern void glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param);
_GL_IMPORT_EXTRA(glSamplerParameteri) extern void glSamplerParameteri(GLuint sampler, GLenum pname, GLint param);
_GL_IMPORT_EXTRA(glSamplerParameteriv) extern void glSamplerParameteriv(GLuint sampler, GLenum pname, const GLint* param);
_GL_IMPORT_EXTRA(glBeginQuery)    extern void glBeginQuery(GLenum target, GLuint id);
_GL_IMPORT_EXTRA(glDeleteQueries) extern void glDeleteQueries(GLsizei n, const GLuint* ids);
_GL_IMPORT_EXTRA(glEndQuery)      extern void glEndQuery(GLenum target);
_GL_IMPORT_EXTRA(glGenQueries)    extern void glGenQueries(GLsizei n, GLuint* ids);
_GL_IMPORT_EXTRA(glGetQueryObjectuiv) extern void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params);
_GL_IMPORT_EXTRA(glGetQueryiv)    extern void glGetQueryiv(GLenum target, GLenum pname, GLint* params);
_GL_IMPORT_EXTRA(glInvalidateSubFramebuffer) extern void glInvalidateSubFramebuffer(GLenum target, GLsizei numAttachments, const GLenum* attachments, GLint x, GLint y, GLsizei width, GLsizei height);
_GL_IMPORT_EXTRA(glGetShaderPrecisionFormat) extern void glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype, GLint* range, GLint* precision);

/* Desktop GL functions (Core 3.3 — needed for GrGLMakeAssembledGLInterface) */
_GL_IMPORT_EXTRA(glDrawBuffer)     extern void glDrawBuffer(GLenum buf);
_GL_IMPORT_EXTRA(glPolygonMode)    extern void glPolygonMode(GLenum face, GLenum mode);
_GL_IMPORT_EXTRA(glGetTexLevelParameteriv) extern void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params);
_GL_IMPORT_EXTRA(glTexBuffer)      extern void glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer);
_GL_IMPORT_EXTRA(glMapBuffer)      extern void* glMapBuffer(GLenum target, GLenum access);
_GL_IMPORT_EXTRA(glBindFragDataLocation) extern void glBindFragDataLocation(GLuint program, GLuint color, const char* name);
_GL_IMPORT_EXTRA(glGetMultisamplefv) extern void glGetMultisamplefv(GLenum pname, GLuint index, GLfloat* val);
_GL_IMPORT_EXTRA(glTexBufferRange) extern void glTexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size);
_GL_IMPORT_EXTRA(glPatchParameteri) extern void glPatchParameteri(GLenum pname, GLint value);
_GL_IMPORT_EXTRA(glMemoryBarrier)  extern void glMemoryBarrier(GLbitfield barriers);

/* ── Static wrapper functions ────────────────────────────────────── */
/* Each wraps a WASM GL import so it has a proper function table entry */

#define W0(ret, fn)          static ret w_##fn() { return fn(); }
#define W1(ret, fn, t1)      static ret w_##fn(t1 a) { return fn(a); }
#define W2(ret, fn, t1,t2)   static ret w_##fn(t1 a, t2 b) { return fn(a,b); }
#define W3(ret, fn, t1,t2,t3) static ret w_##fn(t1 a, t2 b, t3 c) { return fn(a,b,c); }
#define W4(ret, fn, t1,t2,t3,t4) static ret w_##fn(t1 a, t2 b, t3 c, t4 d) { return fn(a,b,c,d); }
#define W5(ret, fn, t1,t2,t3,t4,t5) static ret w_##fn(t1 a, t2 b, t3 c, t4 d, t5 e) { return fn(a,b,c,d,e); }
#define W6(ret, fn, t1,t2,t3,t4,t5,t6) static ret w_##fn(t1 a, t2 b, t3 c, t4 d, t5 e, t6 f) { return fn(a,b,c,d,e,f); }
#define W7(ret, fn, t1,t2,t3,t4,t5,t6,t7) static ret w_##fn(t1 a, t2 b, t3 c, t4 d, t5 e, t6 f, t7 g) { return fn(a,b,c,d,e,f,g); }
#define W9(ret, fn, t1,t2,t3,t4,t5,t6,t7,t8,t9) static ret w_##fn(t1 a, t2 b, t3 c, t4 d, t5 e, t6 f, t7 g, t8 h, t9 i) { return fn(a,b,c,d,e,f,g,h,i); }

#define V0(fn)          static void w_##fn() { fn(); }
#define V1(fn, t1)      static void w_##fn(t1 a) { fn(a); }
#define V2(fn, t1,t2)   static void w_##fn(t1 a, t2 b) { fn(a,b); }
#define V3(fn, t1,t2,t3) static void w_##fn(t1 a, t2 b, t3 c) { fn(a,b,c); }
#define V4(fn, t1,t2,t3,t4) static void w_##fn(t1 a, t2 b, t3 c, t4 d) { fn(a,b,c,d); }
#define V5(fn, t1,t2,t3,t4,t5) static void w_##fn(t1 a, t2 b, t3 c, t4 d, t5 e) { fn(a,b,c,d,e); }
#define V6(fn, t1,t2,t3,t4,t5,t6) static void w_##fn(t1 a, t2 b, t3 c, t4 d, t5 e, t6 f) { fn(a,b,c,d,e,f); }
#define V7(fn, t1,t2,t3,t4,t5,t6,t7) static void w_##fn(t1 a, t2 b, t3 c, t4 d, t5 e, t6 f, t7 g) { fn(a,b,c,d,e,f,g); }
#define V9(fn, t1,t2,t3,t4,t5,t6,t7,t8,t9) static void w_##fn(t1 a, t2 b, t3 c, t4 d, t5 e, t6 f, t7 g, t8 h, t9 i) { fn(a,b,c,d,e,f,g,h,i); }

typedef unsigned int GLenum_;
typedef int GLint_;
typedef unsigned int GLuint_;
typedef int GLsizei_;
typedef float GLfloat_;
typedef unsigned char GLboolean_;
typedef unsigned int GLbitfield_;
typedef signed long GLsizeiptr_;
typedef signed long GLintptr_;

/* Logging helper — available throughout the file */
__attribute__((import_module("env"), import_name("wc_log")))
extern void _wc_log_fn(const char *msg, unsigned int len);
static void _log(const char *m) { unsigned int l=0; while(m[l])l++; _wc_log_fn(m,l); }

#include "gl_trace.h"

/* Desktop GL detection (set during Ganesh init, used by shader patching) */
static bool _is_desktop_context = false;
static bool _use_desktop_gl = false;

/* State */
V1(glEnable, GLenum_)
V1(glDisable, GLenum_)
W0(GLenum_, glGetError)
V0(glFinish)
V0(glFlush)
V2(glHint, GLenum_, GLenum_)
V2(glPixelStorei, GLenum_, GLint_)
V2(glGetIntegerv, GLenum_, GLint_*)
W1(const unsigned char*, glGetString, GLenum_)

/* Viewport / Clear */
V4(glViewport, GLint_, GLint_, GLsizei_, GLsizei_)
V4(glScissor, GLint_, GLint_, GLsizei_, GLsizei_)
V1(glClear, GLbitfield_)
V4(glClearColor, GLfloat_, GLfloat_, GLfloat_, GLfloat_)
V1(glClearDepthf, GLfloat_)
V1(glClearStencil, GLint_)

/* Blending */
V2(glBlendFunc, GLenum_, GLenum_)
V4(glBlendFuncSeparate, GLenum_, GLenum_, GLenum_, GLenum_)
V1(glBlendEquation, GLenum_)
V2(glBlendEquationSeparate, GLenum_, GLenum_)
V4(glBlendColor, GLfloat_, GLfloat_, GLfloat_, GLfloat_)
V4(glColorMask, GLboolean_, GLboolean_, GLboolean_, GLboolean_)

/* Depth / Stencil */
V1(glDepthFunc, GLenum_)
V1(glDepthMask, GLboolean_)
V2(glDepthRangef, GLfloat_, GLfloat_)
V3(glStencilFunc, GLenum_, GLint_, GLuint_)
V4(glStencilFuncSeparate, GLenum_, GLenum_, GLint_, GLuint_)
V3(glStencilOp, GLenum_, GLenum_, GLenum_)
V4(glStencilOpSeparate, GLenum_, GLenum_, GLenum_, GLenum_)
V1(glStencilMask, GLuint_)
V2(glStencilMaskSeparate, GLenum_, GLuint_)

/* Face culling */
V1(glCullFace, GLenum_)
V1(glFrontFace, GLenum_)
V2(glPolygonOffset, GLfloat_, GLfloat_)
V1(glLineWidth, GLfloat_)

/* Buffers */
V2(glGenBuffers, GLsizei_, GLuint_*)
V2(glDeleteBuffers, GLsizei_, const GLuint_*)
V2(glBindBuffer, GLenum_, GLuint_)
V4(glBufferData, GLenum_, GLsizeiptr_, const void*, GLenum_)
V4(glBufferSubData, GLenum_, GLintptr_, GLsizeiptr_, const void*)

/* Textures */
V2(glGenTextures, GLsizei_, GLuint_*)
V2(glDeleteTextures, GLsizei_, const GLuint_*)
V2(glBindTexture, GLenum_, GLuint_)
V1(glActiveTexture, GLenum_)
static void w_glTexImage2D(GLenum_ t, GLint_ l, GLint_ i, GLsizei_ w, GLsizei_ h,
                            GLint_ b, GLenum_ f, GLenum_ ty, const void* p) {
    GL_TRACE("glTexImage2D(0x%x, %d, 0x%x, %d, %d, %d, 0x%x, 0x%x, %s)",
             t, l, i, w, h, b, f, ty, p ? "data" : "NULL");
    glTexImage2D(t, l, i, w, h, b, f, ty, p);
}
static void w_glTexSubImage2D(GLenum_ t, GLint_ l, GLint_ x, GLint_ y,
                               GLsizei_ w, GLsizei_ h, GLenum_ f, GLenum_ ty, const void* p) {
    GL_TRACE("glTexSubImage2D(0x%x, %d, %d, %d, %d, %d, 0x%x, 0x%x, %s)",
             t, l, x, y, w, h, f, ty, p ? "data" : "NULL");
    glTexSubImage2D(t, l, x, y, w, h, f, ty, p);
}
V3(glTexParameteri, GLenum_, GLenum_, GLint_)
V1(glGenerateMipmap, GLenum_)

/* Shaders */
W1(GLuint_, glCreateShader, GLenum_)
V1(glDeleteShader, GLuint_)
V4(glShaderSource, GLuint_, GLsizei_, const char* const*, const GLint_*)
/* _log already defined at file scope above */

static void w_glCompileShader(GLuint_ shader) {
    glCompileShader(shader);
    if (_is_desktop_context) {
        GLint_ status = 0;
        glGetShaderiv(shader, 0x8B81, &status); /* GL_COMPILE_STATUS */
        if (!status) {
            static int fail_count = 0;
            fail_count++;
            if (fail_count <= 3) {
                char log[512] = {0};
                GLsizei_ len = 0;
                glGetShaderInfoLog(shader, sizeof(log)-1, &len, log);
                char msg[640];
                snprintf(msg, sizeof(msg), "Ganesh: shader compile FAILED: %.500s", log);
                _log(msg);
            }
        }
    }
}
V3(glGetShaderiv, GLuint_, GLenum_, GLint_*)
V4(glGetShaderInfoLog, GLuint_, GLsizei_, GLsizei_*, char*)

/* Programs */
W0(GLuint_, glCreateProgram)
V1(glDeleteProgram, GLuint_)
V2(glAttachShader, GLuint_, GLuint_)
V2(glDetachShader, GLuint_, GLuint_)
static void w_glLinkProgram(GLuint_ program) {
    glLinkProgram(program);
    if (_is_desktop_context) {
        GLint_ status = 0;
        glGetProgramiv(program, 0x8B82, &status); /* GL_LINK_STATUS */
        if (!status) {
            static int fail_count = 0;
            fail_count++;
            if (fail_count <= 3) {
                char log[512] = {0};
                GLsizei_ len = 0;
                glGetProgramInfoLog(program, sizeof(log)-1, &len, log);
                char msg[640];
                snprintf(msg, sizeof(msg), "Ganesh: program link FAILED: %.500s", log);
                _log(msg);
            }
        } else {
            static int link_ok = 0;
            link_ok++;
            if (link_ok <= 5) {
                _log("Ganesh: program linked OK");
            }
        }
    }
}
V1(glUseProgram, GLuint_)
V3(glGetProgramiv, GLuint_, GLenum_, GLint_*)
V4(glGetProgramInfoLog, GLuint_, GLsizei_, GLsizei_*, char*)
V1(glValidateProgram, GLuint_)
V3(glBindAttribLocation, GLuint_, GLuint_, const char*)
W2(GLint_, glGetAttribLocation, GLuint_, const char*)
W2(GLint_, glGetUniformLocation, GLuint_, const char*)
V7(glGetActiveAttrib, GLuint_, GLuint_, GLsizei_, GLsizei_*, GLint_*, GLenum_*, char*)
V7(glGetActiveUniform, GLuint_, GLuint_, GLsizei_, GLsizei_*, GLint_*, GLenum_*, char*)

/* Uniforms */
V2(glUniform1i, GLint_, GLint_)
V2(glUniform1f, GLint_, GLfloat_)
V3(glUniform2f, GLint_, GLfloat_, GLfloat_)
V4(glUniform3f, GLint_, GLfloat_, GLfloat_, GLfloat_)
V5(glUniform4f, GLint_, GLfloat_, GLfloat_, GLfloat_, GLfloat_)
V3(glUniform1iv, GLint_, GLsizei_, const GLint_*)
V3(glUniform1fv, GLint_, GLsizei_, const GLfloat_*)
V3(glUniform2fv, GLint_, GLsizei_, const GLfloat_*)
V3(glUniform3fv, GLint_, GLsizei_, const GLfloat_*)
V3(glUniform4fv, GLint_, GLsizei_, const GLfloat_*)
V4(glUniformMatrix2fv, GLint_, GLsizei_, GLboolean_, const GLfloat_*)
V4(glUniformMatrix3fv, GLint_, GLsizei_, GLboolean_, const GLfloat_*)
V4(glUniformMatrix4fv, GLint_, GLsizei_, GLboolean_, const GLfloat_*)

/* Vertex attribs */
V1(glEnableVertexAttribArray, GLuint_)
V1(glDisableVertexAttribArray, GLuint_)
V6(glVertexAttribPointer, GLuint_, GLint_, GLenum_, GLboolean_, GLsizei_, const void*)

/* Drawing */
V3(glDrawArrays, GLenum_, GLint_, GLsizei_)
V4(glDrawElements, GLenum_, GLsizei_, GLenum_, const void*)

/* FBOs */
V2(glGenFramebuffers, GLsizei_, GLuint_*)
V2(glDeleteFramebuffers, GLsizei_, const GLuint_*)
V2(glBindFramebuffer, GLenum_, GLuint_)
W1(GLenum_, glCheckFramebufferStatus, GLenum_)
V5(glFramebufferTexture2D, GLenum_, GLenum_, GLenum_, GLuint_, GLint_)
V4(glFramebufferRenderbuffer, GLenum_, GLenum_, GLenum_, GLuint_)

/* RBOs */
V2(glGenRenderbuffers, GLsizei_, GLuint_*)
V2(glDeleteRenderbuffers, GLsizei_, const GLuint_*)
V2(glBindRenderbuffer, GLenum_, GLuint_)
V4(glRenderbufferStorage, GLenum_, GLenum_, GLsizei_, GLsizei_)

/* Readback */
V7(glReadPixels, GLint_, GLint_, GLsizei_, GLsizei_, GLenum_, GLenum_, void*)

/* VAO */
V2(glGenVertexArrays, GLsizei_, GLuint_*)
V2(glDeleteVertexArrays, GLsizei_, const GLuint_*)
V1(glBindVertexArray, GLuint_)

/* WebGL2 extras */
V3(glBindBufferBase, GLenum_, GLuint_, GLuint_)
V5(glBindBufferRange, GLenum_, GLuint_, GLuint_, GLintptr_, GLsizeiptr_)
W2(GLuint_, glGetUniformBlockIndex, GLuint_, const char*)
V3(glUniformBlockBinding, GLuint_, GLuint_, GLuint_)
V5(glTexStorage2D, GLenum_, GLsizei_, GLenum_, GLsizei_, GLsizei_)
V2(glDrawBuffers, GLsizei_, const GLenum_*)

/* Additional wrappers needed by Ganesh GL context creation */
static const unsigned char* w_glGetStringi(GLenum_ name, GLuint_ index) { return glGetStringi(name, index); }
W1(GLboolean_, glIsEnabled, GLenum_)
V2(glGetBooleanv, GLenum_, GLboolean_*)
V2(glGetFloatv, GLenum_, GLfloat_*)

/* Buffer mapping */
static void* w_glMapBufferRange(GLenum_ t, GLintptr_ o, GLsizeiptr_ l, GLbitfield_ a) { return glMapBufferRange(t,o,l,a); }
static GLboolean_ w_glUnmapBuffer(GLenum_ t) { return glUnmapBuffer(t); }
V3(glFlushMappedBufferRange, GLenum_, GLintptr_, GLsizeiptr_)

/* Renderbuffer multisample */
V5(glRenderbufferStorageMultisample, GLenum_, GLsizei_, GLenum_, GLsizei_, GLsizei_)

/* Instanced drawing */
V4(glDrawArraysInstanced, GLenum_, GLint_, GLsizei_, GLsizei_)
V5(glDrawElementsInstanced, GLenum_, GLsizei_, GLenum_, const void*, GLsizei_)
V2(glVertexAttribDivisor, GLuint_, GLuint_)

/* Blit framebuffer (10 args — manual wrapper) */
static void w_glBlitFramebuffer(GLint_ sx0, GLint_ sy0, GLint_ sx1, GLint_ sy1,
                                 GLint_ dx0, GLint_ dy0, GLint_ dx1, GLint_ dy1,
                                 GLbitfield_ mask, GLenum_ filter) {
    glBlitFramebuffer(sx0,sy0,sx1,sy1,dx0,dy0,dx1,dy1,mask,filter);
}

/* Invalidate framebuffer */
V3(glInvalidateFramebuffer, GLenum_, GLsizei_, const GLenum_*)

/* Texture 3D (10-11 args — manual wrappers) */
static void w_glTexImage3D(GLenum_ t, GLint_ l, GLint_ i, GLsizei_ w, GLsizei_ h,
                            GLsizei_ d, GLint_ b, GLenum_ f, GLenum_ ty, const void* p) {
    glTexImage3D(t,l,i,w,h,d,b,f,ty,p);
}
static void w_glTexSubImage3D(GLenum_ t, GLint_ l, GLint_ x, GLint_ y, GLint_ z,
                               GLsizei_ w, GLsizei_ h, GLsizei_ d,
                               GLenum_ f, GLenum_ ty, const void* p) {
    glTexSubImage3D(t,l,x,y,z,w,h,d,f,ty,p);
}
V6(glTexStorage3D, GLenum_, GLsizei_, GLenum_, GLsizei_, GLsizei_, GLsizei_)

/* Sync */
static void* w_glFenceSync(GLenum_ c, GLbitfield_ f) { return (void*)glFenceSync(c,f); }
static void w_glDeleteSync(void* s) { glDeleteSync((GLsync_)s); }
static GLenum_ w_glClientWaitSync(void* s, GLbitfield_ f, unsigned long long t) { return glClientWaitSync((GLsync_)s,f,t); }

/* Additional Ganesh wrappers */
static void w_glCompressedTexImage2D(GLenum_ t, GLint_ l, GLenum_ f, GLsizei_ w, GLsizei_ h, GLint_ b, GLsizei_ s, const void* d) { glCompressedTexImage2D(t,l,f,w,h,b,s,d); }
static void w_glCompressedTexSubImage2D(GLenum_ t, GLint_ l, GLint_ x, GLint_ y, GLsizei_ w, GLsizei_ h, GLenum_ f, GLsizei_ s, const void* d) { glCompressedTexSubImage2D(t,l,x,y,w,h,f,s,d); }
static void w_glCopyTexSubImage2D(GLenum_ t, GLint_ l, GLint_ x, GLint_ y, GLint_ sx, GLint_ sy, GLsizei_ w, GLsizei_ h) { glCopyTexSubImage2D(t,l,x,y,sx,sy,w,h); }
V3(glGetBufferParameteriv, GLenum_, GLenum_, GLint_*)
W1(GLboolean_, glIsTexture, GLuint_)
V3(glTexParameterf, GLenum_, GLenum_, GLfloat_)
V3(glTexParameterfv, GLenum_, GLenum_, const GLfloat_*)
V3(glTexParameteriv, GLenum_, GLenum_, const GLint_*)
V3(glUniform2i, GLint_, GLint_, GLint_)
V4(glUniform3i, GLint_, GLint_, GLint_, GLint_)
V5(glUniform4i, GLint_, GLint_, GLint_, GLint_, GLint_)
V3(glUniform2iv, GLint_, GLsizei_, const GLint_*)
V3(glUniform3iv, GLint_, GLsizei_, const GLint_*)
V3(glUniform4iv, GLint_, GLsizei_, const GLint_*)
V2(glVertexAttrib1f, GLuint_, GLfloat_)
V2(glVertexAttrib2fv, GLuint_, const GLfloat_*)
V2(glVertexAttrib3fv, GLuint_, const GLfloat_*)
V2(glVertexAttrib4fv, GLuint_, const GLfloat_*)
V1(glReadBuffer, GLenum_)
V6(glDrawRangeElements, GLenum_, GLuint_, GLuint_, GLsizei_, GLenum_, const void*)
V5(glVertexAttribIPointer, GLuint_, GLint_, GLenum_, GLsizei_, const void*)
V4(glGetFramebufferAttachmentParameteriv, GLenum_, GLenum_, GLenum_, GLint_*)
V3(glGetRenderbufferParameteriv, GLenum_, GLenum_, GLint_*)
V5(glCopyBufferSubData, GLenum_, GLenum_, GLintptr_, GLintptr_, GLsizeiptr_)
static GLboolean_ w_glIsSync(void* s) { return glIsSync((GLsync_)s); }
static void w_glWaitSync(void* s, GLbitfield_ f, unsigned long long t) { glWaitSync((GLsync_)s,f,t); }
V5(glGetInternalformativ, GLenum_, GLenum_, GLenum_, GLsizei_, GLint_*)
static void w_glGetProgramBinary(GLuint_ p, GLsizei_ b, GLsizei_* l, GLenum_* bf, void* bin) { glGetProgramBinary(p,b,l,bf,bin); }
V4(glProgramBinary, GLuint_, GLenum_, const void*, GLsizei_)
V3(glProgramParameteri, GLuint_, GLenum_, GLint_)
V2(glBindSampler, GLuint_, GLuint_)
V2(glDeleteSamplers, GLsizei_, const GLuint_*)
V2(glGenSamplers, GLsizei_, GLuint_*)
V3(glSamplerParameterf, GLuint_, GLenum_, GLfloat_)
V3(glSamplerParameteri, GLuint_, GLenum_, GLint_)
V3(glSamplerParameteriv, GLuint_, GLenum_, const GLint_*)
V2(glBeginQuery, GLenum_, GLuint_)
V2(glDeleteQueries, GLsizei_, const GLuint_*)
V1(glEndQuery, GLenum_)
V2(glGenQueries, GLsizei_, GLuint_*)
V3(glGetQueryObjectuiv, GLuint_, GLenum_, GLuint_*)
V3(glGetQueryiv, GLenum_, GLenum_, GLint_*)
static void w_glInvalidateSubFramebuffer(GLenum_ t, GLsizei_ n, const GLenum_* a, GLint_ x, GLint_ y, GLsizei_ w, GLsizei_ h) { glInvalidateSubFramebuffer(t,n,a,x,y,w,h); }
V4(glGetShaderPrecisionFormat, GLenum_, GLenum_, GLint_*, GLint_*)

/* Shader source patching — convert GLES shaders to desktop GL when on Core context.
 * Ganesh with GLES interface generates #version 300 es shaders. These compile on
 * Core 3.3 (via GL_ARB_ES3_compatibility) but may not produce correct output.
 * Patch to #version 330 and strip precision qualifiers. */
static char _shader_patch_buf[16384];

static void w_glShaderSource_patched(GLuint_ shader, GLsizei_ count,
                                      const char* const* strings, const GLint_* lengths) {
    if (!_is_desktop_context) {
        glShaderSource(shader, count, strings, lengths);
        return;
    }

    /* Concatenate all source strings */
    int total = 0;
    for (int i = 0; i < count; i++) {
        int len = (lengths && lengths[i] > 0) ? lengths[i] : strlen(strings[i]);
        if (total + len < (int)sizeof(_shader_patch_buf) - 1) {
            memcpy(_shader_patch_buf + total, strings[i], len);
            total += len;
        }
    }
    _shader_patch_buf[total] = 0;

    /* Don't patch #version 300 es → 330. Mesa Core 3.3 accepts ES shaders
     * natively via GL_ARB_ES3_compatibility. Patching causes more problems
     * (missing layout qualifiers, precision differences) than it solves.
     * Only strip extensions that Core 3.3 doesn't support as ES extensions. */
    {
        char *p;
        while ((p = strstr(_shader_patch_buf, "#extension GL_OES_standard_derivatives")) != NULL) {
            /* Blank the entire #extension line */
            char *end = strchr(p, '\n');
            if (end) memset(p, ' ', end - p);
            else memset(p, ' ', strlen(p));
        }
    }

    const char *patched = _shader_patch_buf;
    int patched_len = total;
    glShaderSource(shader, 1, &patched, &patched_len);
}

/* Desktop GL wrappers (Core 3.3) */
V1(glDrawBuffer, GLenum_)
V2(glPolygonMode, GLenum_, GLenum_)
V4(glGetTexLevelParameteriv, GLenum_, GLint_, GLenum_, GLint_*)
V3(glTexBuffer, GLenum_, GLenum_, GLuint_)
static void* w_glMapBuffer(GLenum_ t, GLenum_ a) { return glMapBuffer(t, a); }
V3(glBindFragDataLocation, GLuint_, GLuint_, const char*)
V3(glGetMultisamplefv, GLenum_, GLuint_, GLfloat_*)
V5(glTexBufferRange, GLenum_, GLenum_, GLuint_, GLintptr_, GLsizeiptr_)
V2(glPatchParameteri, GLenum_, GLint_)
V1(glMemoryBarrier, GLbitfield_)

extern "C" {

/* wc_log import */
__attribute__((import_module("env"), import_name("wc_log")))
extern void wc_log_raw(const char *msg, unsigned int len);
static void gl_log(const char *msg) {
    unsigned int len = 0;
    while (msg[len]) len++;
    wc_log_raw(msg, len);
}

static GrDirectContext* s_grContext = nullptr;
static SkSurface* s_glSurface = nullptr;

/* Detect desktop GL vs GLES from GL_SHADING_LANGUAGE_VERSION (host passes through real) */
static bool is_desktop_gl_context() {
    const char* glsl_ver = (const char*)glGetString(0x8B8C); /* GL_SHADING_LANGUAGE_VERSION */
    if (!glsl_ver) return false;
    /* GLES versions contain "ES" — desktop versions don't */
    return (strstr(glsl_ver, "ES") == nullptr);
}

/* Ganesh extension filtering + GL version override.
 * Reports zero extensions so Ganesh only uses core functions.
 * Overrides GL_VERSION to match actual context type (ES 3.0 or GL 3.3). */
static const unsigned char* ganesh_glGetString(GLenum_ name) {
    if (name == 0x1F02) { /* GL_VERSION */
        if (_use_desktop_gl) {
            static const unsigned char ver[] = "3.3.0 wasmcart";
            return ver;
        } else {
            static const unsigned char ver[] = "OpenGL ES 3.0 wasmcart";
            return ver;
        }
    }
    if (name == 0x8B8C) { /* GL_SHADING_LANGUAGE_VERSION */
        if (_use_desktop_gl) {
            static const unsigned char ver[] = "3.30";
            return ver;
        } else {
            static const unsigned char ver[] = "OpenGL ES GLSL ES 3.00";
            return ver;
        }
    }
    if (name == 0x1F03) { /* GL_EXTENSIONS */
        static const unsigned char empty[] = "";
        return empty;
    }
    return glGetString(name);
}

static const unsigned char* ganesh_glGetStringi(GLenum_ name, GLuint_ index) {
    if (name == 0x1F03) return nullptr; /* GL_EXTENSIONS — none */
    return glGetStringi(name, index);
}

static void ganesh_glGetIntegerv(GLenum_ pname, GLint_* data) {
    if (pname == 0x821D) { *data = 0; return; } /* GL_NUM_EXTENSIONS */
    /* GL_CONTEXT_PROFILE_MASK (0x9126) — pass through to real GL */
    glGetIntegerv(pname, data);
}

/* Traced GL wrappers — log function name + key args when tracing is active */
static void w_traced_glBindFramebuffer(GLenum_ t, GLuint_ f) {
    GL_TRACE("glBindFramebuffer(0x%x, %d)", t, f); glBindFramebuffer(t, f); }
static void w_traced_glBindTexture(GLenum_ t, GLuint_ tex) {
    GL_TRACE("glBindTexture(0x%x, %d)", t, tex); glBindTexture(t, tex); }
static void w_traced_glActiveTexture(GLenum_ u) {
    GL_TRACE("glActiveTexture(0x%x)", u); glActiveTexture(u); }
static void w_traced_glUseProgram(GLuint_ p) {
    GL_TRACE("glUseProgram(%d)", p); glUseProgram(p); }
/* On Core 3.3, VAO 0 is invalid. Create a default VAO and redirect VAO 0 to it. */
static GLuint_ _default_vao = 0;
static void w_traced_glBindVertexArray(GLuint_ v) {
    if (v == 0 && _is_desktop_context) {
        if (!_default_vao) {
            glGenVertexArrays(1, &_default_vao);
            _log("Ganesh: created default VAO for Core 3.3");
        }
        v = _default_vao;
    }
    GL_TRACE("glBindVertexArray(%d)", v); glBindVertexArray(v); }
static void w_traced_glDrawArrays(GLenum_ m, GLint_ f, GLsizei_ c) {
    GL_TRACE("glDrawArrays(0x%x, %d, %d)", m, f, c); glDrawArrays(m, f, c); }
static void w_traced_glDrawElements(GLenum_ m, GLsizei_ c, GLenum_ t, const void* i) {
    GL_TRACE("glDrawElements(0x%x, %d, 0x%x, %p)", m, c, t, i); glDrawElements(m, c, t, i); }
static void w_traced_glEnable(GLenum_ c) {
    GL_TRACE("glEnable(0x%x)", c); glEnable(c); }
static void w_traced_glDisable(GLenum_ c) {
    GL_TRACE("glDisable(0x%x)", c); glDisable(c); }
static void w_traced_glViewport(GLint_ x, GLint_ y, GLsizei_ w, GLsizei_ h) {
    GL_TRACE("glViewport(%d, %d, %d, %d)", x, y, w, h); glViewport(x, y, w, h); }
static void w_traced_glScissor(GLint_ x, GLint_ y, GLsizei_ w, GLsizei_ h) {
    GL_TRACE("glScissor(%d, %d, %d, %d)", x, y, w, h); glScissor(x, y, w, h); }
static void w_traced_glBindBuffer(GLenum_ t, GLuint_ b) {
    GL_TRACE("glBindBuffer(0x%x, %d)", t, b); glBindBuffer(t, b); }
static void w_traced_glBlendFunc(GLenum_ s, GLenum_ d) {
    GL_TRACE("glBlendFunc(0x%x, 0x%x)", s, d); glBlendFunc(s, d); }
static void w_traced_glClear(GLbitfield_ m) {
    GL_TRACE("glClear(0x%x)", m); glClear(m); }
static void w_traced_glClearColor(GLfloat_ r, GLfloat_ g, GLfloat_ b, GLfloat_ a) {
    GL_TRACE("glClearColor(%.2f, %.2f, %.2f, %.2f)", r, g, b, a); glClearColor(r, g, b, a); }
static void w_traced_glColorMask(GLboolean_ r, GLboolean_ g, GLboolean_ b, GLboolean_ a) {
    GL_TRACE("glColorMask(%d,%d,%d,%d)", r, g, b, a); glColorMask(r, g, b, a); }
static void w_traced_glDepthMask(GLboolean_ f) {
    GL_TRACE("glDepthMask(%d)", f); glDepthMask(f); }
static void w_traced_glStencilFunc(GLenum_ f, GLint_ r, GLuint_ m) {
    GL_TRACE("glStencilFunc(0x%x, %d, 0x%x)", f, r, m); glStencilFunc(f, r, m); }
static void w_traced_glStencilOp(GLenum_ sf, GLenum_ df, GLenum_ dp) {
    GL_TRACE("glStencilOp(0x%x, 0x%x, 0x%x)", sf, df, dp); glStencilOp(sf, df, dp); }
static void w_traced_glStencilMask(GLuint_ m) {
    GL_TRACE("glStencilMask(0x%x)", m); glStencilMask(m); }
static void w_traced_glPixelStorei(GLenum_ p, GLint_ v) {
    GL_TRACE("glPixelStorei(0x%x, %d)", p, v); glPixelStorei(p, v); }

static GrGLFuncPtr wc_gl_get_proc(void* ctx, const char name[]) {
    #define MAP(fn) if (strcmp(name, #fn) == 0) return (GrGLFuncPtr)w_##fn
    #define TMAP(fn) if (strcmp(name, #fn) == 0) return (GrGLFuncPtr)w_traced_##fn

    /* Traced versions of draw-critical functions */
    TMAP(glActiveTexture); MAP(glAttachShader); TMAP(glBindBuffer);
    TMAP(glBindFramebuffer); MAP(glBindRenderbuffer); TMAP(glBindTexture);
    TMAP(glBindVertexArray); MAP(glBlendColor); MAP(glBlendEquation);
    MAP(glBlendEquationSeparate); TMAP(glBlendFunc); MAP(glBlendFuncSeparate);
    MAP(glBufferData); MAP(glBufferSubData); MAP(glCheckFramebufferStatus);
    TMAP(glClear); TMAP(glClearColor); MAP(glClearStencil);
    TMAP(glColorMask);
    if (strcmp(name, "glCompileShader") == 0) return (GrGLFuncPtr)w_glCompileShader;
    MAP(glCreateProgram);
    MAP(glCreateShader); MAP(glCullFace); MAP(glDeleteBuffers);
    MAP(glDeleteFramebuffers); MAP(glDeleteProgram); MAP(glDeleteRenderbuffers);
    MAP(glDeleteShader); MAP(glDeleteTextures); MAP(glDeleteVertexArrays);
    MAP(glDepthFunc); TMAP(glDepthMask); MAP(glDepthRangef);
    TMAP(glDisable); MAP(glDisableVertexAttribArray); TMAP(glDrawArrays);
    TMAP(glDrawElements); TMAP(glEnable); MAP(glEnableVertexAttribArray);
    MAP(glFinish); MAP(glFlush); MAP(glFramebufferRenderbuffer);
    MAP(glFramebufferTexture2D); MAP(glFrontFace); MAP(glGenBuffers);
    MAP(glGenFramebuffers); MAP(glGenRenderbuffers); MAP(glGenTextures);
    MAP(glGenVertexArrays); MAP(glGenerateMipmap); MAP(glGetError);
    /* glGetIntegerv/glGetString/glGetStringi intercepted to filter extensions */
    if (strcmp(name, "glGetIntegerv") == 0) return (GrGLFuncPtr)ganesh_glGetIntegerv;
    if (strcmp(name, "glGetString") == 0) return (GrGLFuncPtr)ganesh_glGetString;
    MAP(glGetProgramInfoLog); MAP(glGetProgramiv);
    MAP(glGetShaderInfoLog); MAP(glGetShaderiv);
    MAP(glGetUniformLocation); MAP(glGetAttribLocation);
    MAP(glGetActiveAttrib); MAP(glGetActiveUniform);
    MAP(glHint); MAP(glLineWidth);
    if (strcmp(name, "glLinkProgram") == 0) return (GrGLFuncPtr)w_glLinkProgram;
    TMAP(glPixelStorei); MAP(glPolygonOffset); MAP(glReadPixels);
    MAP(glRenderbufferStorage); TMAP(glScissor);
    if (strcmp(name, "glShaderSource") == 0) return (GrGLFuncPtr)w_glShaderSource_patched;
    TMAP(glStencilFunc); MAP(glStencilFuncSeparate);
    MAP(glStencilMask); MAP(glStencilMaskSeparate);
    TMAP(glStencilOp); MAP(glStencilOpSeparate);
    MAP(glTexImage2D); MAP(glTexParameteri); MAP(glTexSubImage2D);
    MAP(glUniform1f); MAP(glUniform1i);
    MAP(glUniform2f); MAP(glUniform3f); MAP(glUniform4f);
    MAP(glUniform1fv); MAP(glUniform1iv);
    MAP(glUniform2fv); MAP(glUniform3fv); MAP(glUniform4fv);
    MAP(glUniformMatrix2fv); MAP(glUniformMatrix3fv); MAP(glUniformMatrix4fv);
    TMAP(glUseProgram); MAP(glValidateProgram);
    MAP(glVertexAttribPointer); TMAP(glViewport);
    MAP(glBindAttribLocation); MAP(glClearDepthf);
    MAP(glBindBufferBase); MAP(glBindBufferRange);
    MAP(glGetUniformBlockIndex); MAP(glUniformBlockBinding);
    MAP(glTexStorage2D); MAP(glDrawBuffers);
    MAP(glDetachShader);

    /* Ganesh-required extras */
    if (strcmp(name, "glGetStringi") == 0) return (GrGLFuncPtr)ganesh_glGetStringi;
    MAP(glIsEnabled);
    MAP(glGetBooleanv); MAP(glGetFloatv);
    /* On desktop GL, provide MapBuffer stubs that always return NULL.
     * This forces Ganesh to fall back to glTexSubImage2D for uploads
     * instead of using mapped buffers (which may not work through WASM). */
    if (_is_desktop_context) {
        if (strcmp(name, "glMapBufferRange") == 0) {
            static void* (*fn)(unsigned int, long, long, unsigned int) =
                [](unsigned int, long, long, unsigned int) -> void* { return nullptr; };
            return (GrGLFuncPtr)fn;
        }
        if (strcmp(name, "glMapBuffer") == 0) {
            static void* (*fn)(unsigned int, unsigned int) =
                [](unsigned int, unsigned int) -> void* { return nullptr; };
            return (GrGLFuncPtr)fn;
        }
    } else {
        MAP(glMapBufferRange);
    }
    MAP(glUnmapBuffer); MAP(glFlushMappedBufferRange);
    MAP(glRenderbufferStorageMultisample);
    MAP(glDrawArraysInstanced); MAP(glDrawElementsInstanced); MAP(glVertexAttribDivisor);
    MAP(glBlitFramebuffer); MAP(glInvalidateFramebuffer);
    MAP(glTexImage3D); MAP(glTexSubImage3D); MAP(glTexStorage3D);
    MAP(glFenceSync); MAP(glDeleteSync); MAP(glClientWaitSync);

    /* Additional Ganesh extras */
    MAP(glCompressedTexImage2D); MAP(glCompressedTexSubImage2D); MAP(glCopyTexSubImage2D);
    MAP(glGetBufferParameteriv); MAP(glIsTexture);
    MAP(glTexParameterf); MAP(glTexParameterfv); MAP(glTexParameteriv);
    MAP(glUniform2i); MAP(glUniform3i); MAP(glUniform4i);
    MAP(glUniform2iv); MAP(glUniform3iv); MAP(glUniform4iv);
    MAP(glVertexAttrib1f); MAP(glVertexAttrib2fv); MAP(glVertexAttrib3fv); MAP(glVertexAttrib4fv);
    MAP(glReadBuffer); MAP(glDrawRangeElements); MAP(glVertexAttribIPointer);
    MAP(glGetFramebufferAttachmentParameteriv); MAP(glGetRenderbufferParameteriv);
    MAP(glCopyBufferSubData); MAP(glIsSync); MAP(glWaitSync);
    MAP(glGetInternalformativ); MAP(glGetProgramBinary); MAP(glProgramBinary); MAP(glProgramParameteri);
    /* Sampler objects — instrument on desktop to debug texture binding */
    if (strcmp(name, "glGenSamplers") == 0) {
        static void (*fn)(GLsizei_, GLuint_*) = [](GLsizei_ n, GLuint_* ids) {
            glGenSamplers(n, ids);
            if (_is_desktop_context && n > 0) {
                char msg[64]; snprintf(msg, sizeof(msg), "Ganesh: glGenSamplers(%d) → id=%d", n, ids[0]);
                _log(msg);
            }
        };
        return (GrGLFuncPtr)fn;
    }
    if (strcmp(name, "glBindSampler") == 0) {
        static void (*fn)(GLuint_, GLuint_) = [](GLuint_ unit, GLuint_ sampler) {
            glBindSampler(unit, sampler);
            if (_is_desktop_context) {
                static int c = 0;
                if (c++ < 5) { char msg[64]; snprintf(msg, sizeof(msg), "Ganesh: glBindSampler(unit=%d, sampler=%d)", unit, sampler); _log(msg); }
            }
        };
        return (GrGLFuncPtr)fn;
    }
    MAP(glDeleteSamplers);
    MAP(glSamplerParameterf); MAP(glSamplerParameteri); MAP(glSamplerParameteriv);
    MAP(glBeginQuery); MAP(glDeleteQueries); MAP(glEndQuery); MAP(glGenQueries);
    MAP(glGetQueryObjectuiv); MAP(glGetQueryiv);
    MAP(glInvalidateSubFramebuffer); MAP(glGetShaderPrecisionFormat);

    /* Desktop GL (Core 3.3) */
    MAP(glDrawBuffer); MAP(glPolygonMode); MAP(glGetTexLevelParameteriv);
    MAP(glTexBuffer); MAP(glMapBuffer); MAP(glBindFragDataLocation);
    MAP(glGetMultisamplefv); MAP(glTexBufferRange);
    MAP(glPatchParameteri); MAP(glMemoryBarrier);

    /* Desktop GL stubs — correctly typed no-ops for functions Ganesh's desktop
     * interface builder requires but we don't need for ES 3.0 rendering.
     * WASM enforces strict function signature matching — generic stubs crash. */
    if (_is_desktop_context) {
        #define STUB_V0(n) if(strcmp(name,#n)==0){static void(*f)()=[](){};return(GrGLFuncPtr)f;}
        #define STUB_V1(n,t1) if(strcmp(name,#n)==0){static void(*f)(t1)=[](t1){};return(GrGLFuncPtr)f;}
        #define STUB_V2(n,t1,t2) if(strcmp(name,#n)==0){static void(*f)(t1,t2)=[](t1,t2){};return(GrGLFuncPtr)f;}
        #define STUB_V3(n,t1,t2,t3) if(strcmp(name,#n)==0){static void(*f)(t1,t2,t3)=[](t1,t2,t3){};return(GrGLFuncPtr)f;}
        #define STUB_V4(n,t1,t2,t3,t4) if(strcmp(name,#n)==0){static void(*f)(t1,t2,t3,t4)=[](t1,t2,t3,t4){};return(GrGLFuncPtr)f;}
        #define STUB_V5(n,t1,t2,t3,t4,t5) if(strcmp(name,#n)==0){static void(*f)(t1,t2,t3,t4,t5)=[](t1,t2,t3,t4,t5){};return(GrGLFuncPtr)f;}
        #define STUB_R(n,r,...) if(strcmp(name,#n)==0){static r(*f)(__VA_ARGS__)=[](__VA_ARGS__)->r{return(r)0;};return(GrGLFuncPtr)f;}

        /* Draw indirect */
        STUB_V2(glDrawArraysIndirect, unsigned int, const void*)
        STUB_V2(glDrawElementsIndirect, unsigned int, const void*)
        STUB_V4(glDrawArraysInstancedBaseInstance, unsigned int, int, int, int)
        STUB_V5(glDrawElementsInstancedBaseVertexBaseInstance, unsigned int, int, unsigned int, const void*, int)
        STUB_V4(glMultiDrawArraysIndirect, unsigned int, const void*, int, int)
        STUB_V4(glMultiDrawElementsIndirect, unsigned int, const void*, int, int)

        /* Fragment data */
        STUB_V3(glBindFragDataLocationIndexed, unsigned int, unsigned int, unsigned int)

        /* Texture clear/invalidate */
        STUB_V5(glClearTexImage, unsigned int, int, unsigned int, unsigned int, const void*)
        if(strcmp(name,"glClearTexSubImage")==0){static void(*f)(unsigned int,int,int,int,int,int,int,int,unsigned int,unsigned int,const void*)=[](unsigned int,int,int,int,int,int,int,int,unsigned int,unsigned int,const void*){};return(GrGLFuncPtr)f;}
        STUB_V1(glInvalidateTexImage, unsigned int)
        STUB_V5(glInvalidateTexSubImage, unsigned int, int, int, int, int)
        STUB_V1(glInvalidateBufferData, unsigned int)
        STUB_V3(glInvalidateBufferSubData, unsigned int, long, long)

        /* Debug */
        STUB_V2(glDebugMessageCallback, void*, const void*)
        STUB_V5(glDebugMessageControl, unsigned int, unsigned int, unsigned int, int, const unsigned int*)
        STUB_V5(glDebugMessageInsert, unsigned int, unsigned int, unsigned int, unsigned int, int)
        STUB_R(glGetDebugMessageLog, unsigned int, unsigned int, int, unsigned int*, unsigned int*, unsigned int*, unsigned int*, int*, char*)
        STUB_V4(glObjectLabel, unsigned int, unsigned int, int, const char*)
        STUB_V3(glPushDebugGroup, unsigned int, unsigned int, int)
        STUB_V0(glPopDebugGroup)

        /* Query */
        STUB_V2(glQueryCounter, unsigned int, unsigned int)
        STUB_V3(glGetQueryObjecti64v, unsigned int, unsigned int, long long*)
        STUB_V3(glGetQueryObjectui64v, unsigned int, unsigned int, unsigned long long*)

        /* Texture barrier */
        STUB_V0(glTextureBarrier)

        #undef STUB_V0
        #undef STUB_V1
        #undef STUB_V2
        #undef STUB_V3
        #undef STUB_V4
        #undef STUB_V5
        #undef STUB_R
    }

    /* EGL stubs — Ganesh optionally queries EGL */
    if (strcmp(name, "eglQueryString") == 0) {
        static const char* (*egl_fn)(void*, int) = [](void*, int) -> const char* { return ""; };
        return (GrGLFuncPtr)egl_fn;
    }
    if (strcmp(name, "eglGetCurrentDisplay") == 0) {
        static void* (*egl_fn2)() = []() -> void* { return (void*)1; }; /* non-null = valid display */
        return (GrGLFuncPtr)egl_fn2;
    }

    #undef MAP
    /* Log missing GL function so we can add wrapper */
    {
        char msg[128] = "Ganesh: MISSING GL proc: ";
        int i = 0;
        while (msg[i]) i++;
        for (int j = 0; name[j] && i < 120; j++) msg[i++] = name[j];
        msg[i] = 0;
        gl_log(msg);
    }
    return nullptr;
}

void* skia_create_gl_surface(int width, int height) {
    gl_log("Ganesh: SkGraphics::Init...");
    SkGraphics::Init();
    gl_log("Ganesh: Init done, detecting GL context type...");

    _is_desktop_context = is_desktop_gl_context();
    _use_desktop_gl = _is_desktop_context;

    sk_sp<const GrGLInterface> interface;
    if (_use_desktop_gl) {
        /* Use GLES interface even on desktop GL. Ganesh's desktop GL rendering
         * path has incompatibilities with our function stubs that cause textured
         * draws to silently fail. The GLES interface generates shaders and state
         * that work on desktop GL via GL_ARB_ES3_compatibility.
         * We still detect desktop context for the readback/blit path. */
        gl_log("Ganesh: Core context detected, trying GLES interface on desktop...");
        _use_desktop_gl = false;
        interface = GrGLMakeAssembledGLESInterface(nullptr, wc_gl_get_proc);
        if (interface) {
            gl_log("Ganesh: GLES interface on desktop OK!");
        } else {
            gl_log("Ganesh: GLES interface failed on desktop, trying desktop interface...");
            _use_desktop_gl = true;
            interface = GrGLMakeAssembledGLInterface(nullptr, wc_gl_get_proc);
            gl_log(interface ? "Ganesh: desktop interface OK" : "Ganesh: desktop interface NULL");
        }
    } else {
        gl_log("Ganesh: using GLES interface");
        interface = GrGLMakeAssembledGLESInterface(nullptr, wc_gl_get_proc);
    }
    if (!interface) {
        gl_log("Ganesh: interface assembly returned null");
        return nullptr;
    }
    gl_log("Ganesh: GL interface assembled, creating context...");

    s_grContext = GrDirectContexts::MakeGL(interface).release();
    if (!s_grContext) {
        gl_log("Ganesh: GrDirectContexts::MakeGL returned null");
        return nullptr;
    }
    gl_log("Ganesh: context created!");

    /* Log what Ganesh detected about GL */
    {
        int maxSamples = s_grContext->maxSurfaceSampleCountForColorType(kRGBA_8888_SkColorType);
        char msg[128];
        int n = 0;
        const char *p = "Ganesh: maxSamples(RGBA8)=";
        while (p[n]) { msg[n] = p[n]; n++; }
        msg[n++] = '0' + (maxSamples / 10);
        msg[n++] = '0' + (maxSamples % 10);
        msg[n] = 0;
        gl_log(msg);
    }

    gl_log("Ganesh: creating render target...");

    /* Use offscreen render target — Ganesh creates its own FBO with
     * color + stencil + depth attachments. Stencil is required for
     * path rendering (polygons). We blit to FBO 0 in skia_gl_flush(). */
    auto imageInfo = SkImageInfo::Make(width, height,
        kRGBA_8888_SkColorType, kPremul_SkAlphaType);

    s_glSurface = SkSurfaces::RenderTarget(
        s_grContext, skgpu::Budgeted::kNo, imageInfo, 0,
        kTopLeft_GrSurfaceOrigin, nullptr).release();

    if (!s_glSurface) {
        gl_log("Ganesh: offscreen render target failed");
        s_grContext->abandonContext();
        delete s_grContext;
        s_grContext = nullptr;
        return nullptr;
    }
    gl_log("Ganesh: GL surface created successfully!");
    return s_glSurface;
}

void skia_gl_flush(void) {
    if (!s_grContext || !s_glSurface) return;
    /* Full sync — ensures GPU is done before FBO blit */
    s_grContext->flushAndSubmit(GrSyncCpu::kYes);
}

void skia_gl_flush_nosync(void) {
    if (!s_grContext) return;
    /* Submit queued commands to GPU without waiting — fast per-op flush */
    s_grContext->flushAndSubmit(GrSyncCpu::kNo);
}

int skia_has_gl_surface(void) {
    return s_glSurface != nullptr ? 1 : 0;
}

void gl_trace_start(void) {
    _log("=== GL TRACE START (frame 5) ===");
    gl_trace_enable();
}
void gl_trace_stop(void) {
    gl_trace_disable();
    char msg[64];
    snprintf(msg, sizeof(msg), "=== GL TRACE END (%d calls) ===", _gl_trace_call);
    _log(msg);
}

void skia_gl_reset_context(void) {
    if (s_grContext) s_grContext->resetContext();
}

int _skia_is_desktop_context(void) {
    return _is_desktop_context ? 1 : 0;
}

} /* extern "C" */
