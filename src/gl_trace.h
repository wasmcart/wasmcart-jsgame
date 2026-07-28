/* gl_trace.h — GL call tracing for Ganesh debug
 *
 * Include in skia_gl_surface.cpp. Enable with gl_trace_enable().
 * Logs every GL call Ganesh makes during one frame to wc_log.
 * Compare traces between working (Node) and failing (RetroArch) hosts.
 */

#ifndef GL_TRACE_H
#define GL_TRACE_H

static int _gl_trace_active = 0;
static int _gl_trace_frame = 0;
static int _gl_trace_call = 0;

static void gl_trace_enable(void) { _gl_trace_active = 1; _gl_trace_call = 0; }
static void gl_trace_disable(void) { _gl_trace_active = 0; }

/* Log a GL call if tracing is active */
#define GL_TRACE(fmt, ...) do { \
    if (_gl_trace_active) { \
        char _msg[256]; \
        snprintf(_msg, sizeof(_msg), "GLTRACE[%d]: " fmt, _gl_trace_call++, ##__VA_ARGS__); \
        _log(_msg); \
    } \
} while(0)

/* Wrapper generators for traced functions */
#define TRACE_V1(fn, t1, fmt1) \
    static void w_traced_##fn(t1 a) { \
        GL_TRACE(#fn "(" fmt1 ")", a); \
        fn(a); \
    }

#define TRACE_V2(fn, t1, t2, fmt) \
    static void w_traced_##fn(t1 a, t2 b) { \
        GL_TRACE(#fn "(" fmt ")", a, b); \
        fn(a, b); \
    }

#define TRACE_V3(fn, t1, t2, t3, fmt) \
    static void w_traced_##fn(t1 a, t2 b, t3 c) { \
        GL_TRACE(#fn "(" fmt ")", a, b, c); \
        fn(a, b, c); \
    }

#define TRACE_V4(fn, t1, t2, t3, t4, fmt) \
    static void w_traced_##fn(t1 a, t2 b, t3 c, t4 d) { \
        GL_TRACE(#fn "(" fmt ")", a, b, c, d); \
        fn(a, b, c, d); \
    }

#endif /* GL_TRACE_H */
