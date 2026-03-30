/*
 * stubs.c — WASI/syscall stubs for standalone WASM
 *
 * QuickJS is mostly self-contained, but a few libc functions need
 * stubs when targeting STANDALONE_WASM with Emscripten.
 */

#include <stdint.h>
#include <time.h>

/* QuickJS uses clock_gettime for Date.now() — we redirect to wasmcart time */
/* The actual time comes from wc_time.time_ms set by the host each frame */

/* Stub for functions that aren't available in standalone WASM */
int system(const char *cmd) { return -1; }

/* localtime/gmtime stubs — QuickJS needs these for Date */
static struct tm _stub_tm;

struct tm *localtime_r(const time_t *timep, struct tm *result) {
    if (!result) result = &_stub_tm;
    /* Minimal: just zero it out */
    __builtin_memset(result, 0, sizeof(struct tm));
    return result;
}

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    if (!result) result = &_stub_tm;
    __builtin_memset(result, 0, sizeof(struct tm));
    return result;
}
