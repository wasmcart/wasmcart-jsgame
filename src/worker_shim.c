/*
 * worker_shim.c — Web Workers via cooperative multitasking
 *
 * Each Worker gets its own QuickJS runtime (real isolation).
 * Workers run cooperatively on the main thread — pumped each frame.
 * Message passing via JSON serialization between runtimes.
 *
 * This is NOT parallel — workers get CPU time during pump_workers().
 * But it IS real isolation (separate JS heaps, no shared state) and
 * real message passing (structured via JSON).
 *
 * True parallel Workers require WASI threads + shared memory, which
 * Emscripten STANDALONE_WASM doesn't support yet.
 * When wasi-sdk or Emscripten adds support, swap this for the
 * threaded version (worker_shim_threaded.c).
 */

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "quickjs.h"
#include "wasmcart.h"

/* From cart_main.c */
extern JSContext *ctx;
extern char *load_asset_string(const char *path, int *out_len);

/* ── Message queue ───────────────────────────────────────────────── */

#define MSG_QUEUE_SIZE 32
#define MSG_MAX_LEN 4096

typedef struct {
    char data[MSG_MAX_LEN];
    int len;
    int ready;
} msg_slot_t;

typedef struct {
    msg_slot_t slots[MSG_QUEUE_SIZE];
    int write_idx;
    int read_idx;
} msg_queue_t;

static void msg_queue_init(msg_queue_t *q) {
    memset(q, 0, sizeof(msg_queue_t));
}

static int msg_queue_push(msg_queue_t *q, const char *data, int len) {
    if (len > MSG_MAX_LEN) len = MSG_MAX_LEN;
    int idx = q->write_idx % MSG_QUEUE_SIZE;
    if (q->slots[idx].ready) return 0;
    memcpy(q->slots[idx].data, data, len);
    q->slots[idx].len = len;
    q->slots[idx].ready = 1;
    q->write_idx++;
    return 1;
}

/* `out` must have room for MSG_MAX_LEN + 1 bytes: the copy is NUL-terminated.
 *
 * QuickJS's JSON tokenizer reads PAST the length it is given when a value ends
 * in a number -- js_atof scans forward for more digits rather than stopping at
 * buf_end. With a reused stack buffer that meant a short message inherited the
 * tail of a longer predecessor, and JS_ParseJSON rejected perfectly valid input
 * with "SyntaxError: unexpected data at the end".
 *
 * It only bit when BOTH held: the message ended in a number, AND a longer
 * message came before it. `{"phase":"load"}` (ends in a quote) always worked,
 * which is why every worker looked fine until one posted {frame: 3}. */
static int msg_queue_pop(msg_queue_t *q, char *out, int *out_len) {
    int idx = q->read_idx % MSG_QUEUE_SIZE;
    if (!q->slots[idx].ready) return 0;
    int len = q->slots[idx].len;
    if (len < 0) len = 0;
    if (len > MSG_MAX_LEN) len = MSG_MAX_LEN;
    *out_len = len;
    memcpy(out, q->slots[idx].data, len);
    out[len] = '\0';
    q->slots[idx].ready = 0;
    q->read_idx++;
    return 1;
}

/* ── Worker state ────────────────────────────────────────────────── */

#define MAX_WORKERS 4

typedef struct {
    JSRuntime *rt;
    JSContext *wctx;
    msg_queue_t to_worker;
    msg_queue_t from_worker;
    JSValue main_callback;  /* onmessage handler on main thread */
    int active;
    int id;
} worker_state_t;

static worker_state_t workers[MAX_WORKERS];
static int next_worker_id = 1;

/* ── Worker-side postMessage (called from worker's QuickJS context) */

static JSValue js_worker_post_from_worker(JSContext *c, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    int32_t wi;
    JS_ToInt32(c, &wi, argv[0]);
    const char *json = JS_ToCString(c, argv[1]);
    if (json && wi >= 0 && wi < MAX_WORKERS) {
        msg_queue_push(&workers[wi].from_worker, json, strlen(json));
    }
    if (json) JS_FreeCString(c, json);
    return JS_UNDEFINED;
}

/* ── Create worker ───────────────────────────────────────────────── */

static JSValue js_worker_create(JSContext *c, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    const char *path = JS_ToCString(c, argv[0]);
    if (!path) return JS_NewInt32(c, -1);

    /* Find free slot */
    int wi = -1;
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (!workers[i].active) { wi = i; break; }
    }
    if (wi < 0) { JS_FreeCString(c, path); return JS_NewInt32(c, -1); }

    worker_state_t *w = &workers[wi];
    memset(w, 0, sizeof(worker_state_t));
    w->id = next_worker_id++;
    msg_queue_init(&w->to_worker);
    msg_queue_init(&w->from_worker);

    /* Store main thread callback */
    if (argc > 1 && JS_IsFunction(c, argv[1]))
        w->main_callback = JS_DupValue(c, argv[1]);
    else
        w->main_callback = JS_UNDEFINED;

    /* Create isolated QuickJS runtime */
    w->rt = JS_NewRuntime();
    if (!w->rt) { JS_FreeCString(c, path); return JS_NewInt32(c, -1); }
    JS_SetMemoryLimit(w->rt, 64 * 1024 * 1024);
    JS_SetMaxStackSize(w->rt, 512 * 1024);

    w->wctx = JS_NewContext(w->rt);
    if (!w->wctx) {
        JS_FreeRuntime(w->rt);
        JS_FreeCString(c, path);
        return JS_NewInt32(c, -1);
    }

    /* Set up worker globals */
    JSValue global = JS_GetGlobalObject(w->wctx);
    JS_SetPropertyStr(w->wctx, global, "self", JS_DupValue(w->wctx, global));

    /* console.log → wc_log */
    JSValue console_obj = JS_NewObject(w->wctx);
    /* For now, worker console is silent. Could route to wc_log. */
    const char *noop_src = "function(){}";
    JSValue noop = JS_Eval(w->wctx, noop_src, strlen(noop_src), "<n>", JS_EVAL_TYPE_GLOBAL);
    JS_SetPropertyStr(w->wctx, console_obj, "log", JS_DupValue(w->wctx, noop));
    JS_SetPropertyStr(w->wctx, console_obj, "warn", JS_DupValue(w->wctx, noop));
    JS_SetPropertyStr(w->wctx, console_obj, "error", JS_DupValue(w->wctx, noop));
    JS_FreeValue(w->wctx, noop);
    JS_SetPropertyStr(w->wctx, global, "console", console_obj);

    /* postMessage → push to from_worker queue */
    JS_SetPropertyStr(w->wctx, global, "_workerIdx", JS_NewInt32(w->wctx, wi));
    JS_SetPropertyStr(w->wctx, global, "_wcWorkerPostFromThread",
        JS_NewCFunction(w->wctx, js_worker_post_from_worker, "_wcWorkerPostFromThread", 2));

    const char *pm_js =
        "globalThis.postMessage = function(data) {\n"
        "  _wcWorkerPostFromThread(_workerIdx, JSON.stringify(data));\n"
        "};\n";
    JS_Eval(w->wctx, pm_js, strlen(pm_js), "<w>", JS_EVAL_TYPE_GLOBAL);

    JS_FreeValue(w->wctx, global);

    /* Load and eval worker script */
    int script_len = 0;
    char *script = load_asset_string(path, &script_len);
    JS_FreeCString(c, path);

    if (script) {
        JSValue result = JS_Eval(w->wctx, script, script_len,
                                  "worker.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(w->wctx);
            JS_FreeValue(w->wctx, exc);
        }
        JS_FreeValue(w->wctx, result);
        free(script);
    }

    w->active = 1;
    return JS_NewInt32(c, w->id);
}

/* ── Post message to worker ──────────────────────────────────────── */

static JSValue js_worker_post(JSContext *c, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    int32_t id;
    JS_ToInt32(c, &id, argv[0]);
    const char *json = JS_ToCString(c, argv[1]);
    if (!json) return JS_UNDEFINED;

    for (int i = 0; i < MAX_WORKERS; i++) {
        if (workers[i].active && workers[i].id == id) {
            msg_queue_push(&workers[i].to_worker, json, strlen(json));
            break;
        }
    }
    JS_FreeCString(c, json);
    return JS_UNDEFINED;
}

/* ── Terminate worker ────────────────────────────────────────────── */

static JSValue js_worker_terminate(JSContext *c, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    int32_t id;
    JS_ToInt32(c, &id, argv[0]);
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (workers[i].active && workers[i].id == id) {
            workers[i].active = 0;
            JS_FreeContext(workers[i].wctx);
            JS_FreeRuntime(workers[i].rt);
            workers[i].wctx = NULL;
            workers[i].rt = NULL;
            if (!JS_IsUndefined(workers[i].main_callback))
                JS_FreeValue(c, workers[i].main_callback);
            workers[i].main_callback = JS_UNDEFINED;
            break;
        }
    }
    return JS_UNDEFINED;
}

/* ── Pump workers — called from wc_render each frame ─────────────── */

void pump_workers(void) {
    if (!ctx) return;

    for (int i = 0; i < MAX_WORKERS; i++) {
        if (!workers[i].active) continue;
        worker_state_t *w = &workers[i];

        /* Deliver messages from main → worker */
        char msg_buf[MSG_MAX_LEN + 1];
        int msg_len;
        while (msg_queue_pop(&w->to_worker, msg_buf, &msg_len)) {
            JSValue global = JS_GetGlobalObject(w->wctx);
            JSValue onmsg = JS_GetPropertyStr(w->wctx, global, "onmessage");
            if (JS_IsFunction(w->wctx, onmsg)) {
                JSValue evt = JS_NewObject(w->wctx);
                JSValue data = JS_ParseJSON(w->wctx, msg_buf, msg_len, "<msg>");
                JS_SetPropertyStr(w->wctx, evt, "data", data);
                JSValue ret = JS_Call(w->wctx, onmsg, global, 1, &evt);
                JS_FreeValue(w->wctx, ret);
                JS_FreeValue(w->wctx, evt);
            }
            JS_FreeValue(w->wctx, onmsg);
            JS_FreeValue(w->wctx, global);
        }

        /* Run worker's pending microtasks -- TIME-bounded, same reasoning as
         * the main pump in cart_main.c: a worker that reschedules a microtask
         * from inside a microtask would otherwise hang the host forever, and a
         * pure count still hands it a fixed slice of every frame. */
        JSContext *pctx;
        {
            struct timespec ts;
            double deadline = 0;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
                deadline = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6 + 2.0;
            for (int job = 0; job < 4096; job++) {
                if (JS_ExecutePendingJob(w->rt, &pctx) <= 0) break;
                if ((job & 63) == 63 && deadline > 0 &&
                    clock_gettime(CLOCK_MONOTONIC, &ts) == 0 &&
                    ((double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6) >= deadline) break;
            }
        }

        /* Deliver messages from worker → main */
        while (msg_queue_pop(&w->from_worker, msg_buf, &msg_len)) {
            if (!JS_IsUndefined(w->main_callback) &&
                JS_IsFunction(ctx, w->main_callback)) {
                JSValue evt = JS_NewObject(ctx);
                JSValue data = JS_ParseJSON(ctx, msg_buf, msg_len, "<wmsg>");
                JS_SetPropertyStr(ctx, evt, "data", data);
                JSValue ret = JS_Call(ctx, w->main_callback,
                                      JS_UNDEFINED, 1, &evt);
                JS_FreeValue(ctx, ret);
                JS_FreeValue(ctx, evt);
            }
        }
    }
}

/* ── Registration ────────────────────────────────────────────────── */

void register_worker_api(JSContext *c) {
    memset(workers, 0, sizeof(workers));
    for (int i = 0; i < MAX_WORKERS; i++)
        workers[i].main_callback = JS_UNDEFINED;

    JSValue global = JS_GetGlobalObject(c);
    JS_SetPropertyStr(c, global, "_wcWorkerCreate",
        JS_NewCFunction(c, js_worker_create, "_wcWorkerCreate", 2));
    JS_SetPropertyStr(c, global, "_wcWorkerPost",
        JS_NewCFunction(c, js_worker_post, "_wcWorkerPost", 2));
    JS_SetPropertyStr(c, global, "_wcWorkerTerminate",
        JS_NewCFunction(c, js_worker_terminate, "_wcWorkerTerminate", 1));
    JS_FreeValue(c, global);
}
