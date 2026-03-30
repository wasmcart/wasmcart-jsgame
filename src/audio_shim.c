/*
 * audio_shim.c — Web Audio API shim using webaudio-node's C++ engine
 *
 * Bridges QuickJS AudioContext/AudioNode API to webaudio-node's WASM
 * audio graph (compiled as libwebaudio.a). Output mixed to wasmcart's
 * F32 stereo ring buffer each frame.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "quickjs.h"
#include "wasmcart.h"

/* From cart_main.c */
extern float        audio_ring[];
extern uint32_t     audio_write_cursor;
extern wc_time_t    time_info;
extern wc_host_info_t host_info;

#define AUDIO_CAP 8192

/* ── webaudio-node C++ API ───────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

int  createAudioGraph(int sampleRate, int channels, int bufferSize, int isRealtime);
void destroyAudioGraph(int graphId);
int  createNode(int graphId, const char *type);
void connectNodes(int graphId, int srcId, int dstId, int srcOut, int dstIn);
void startNode(int graphId, int nodeId, double when);
void stopNode(int graphId, int nodeId, double when);
void setNodeParameter(int graphId, int nodeId, int paramId, float value);
void scheduleParameterValue(int graphId, int nodeId, const char *paramName, float value, double time);
void scheduleParameterRamp(int graphId, int nodeId, const char *paramName, float value, double time, int exponential);
void processGraph(int graphId, float *output, int frameCount);
double getGraphCurrentTime(int graphId);
void setGraphCurrentTime(int graphId, double time);
void setNodeBuffer(int graphId, int nodeId, float *data, int length, int channels);
void registerBuffer(int graphId, int bufferId, float *data, int length, int channels);
void setNodeBufferId(int graphId, int nodeId, int bufferId);
void setNodeStringProperty(int graphId, int nodeId, const char *prop, const char *val);
void setNodeProperty(int graphId, int nodeId, const char *prop, float val);
int decodeWAV(const uint8_t *input, int inputSize, float **output, int *totalSamples, int *sampleRate);
int decodeMP3(const uint8_t *input, int inputSize, float **output, int *totalSamples, int *sampleRate);
int decodeAudio(const uint8_t *input, int inputSize, float **output, int *totalSamples, int *sampleRate);
void freeDecodedBuffer(float *buffer);

#ifdef __cplusplus
}
#endif

enum {
    PARAM_FREQUENCY = 0, PARAM_DETUNE = 1, PARAM_GAIN = 2,
    PARAM_Q = 3, PARAM_DELAY_TIME = 4, PARAM_PAN = 5,
    PARAM_OFFSET = 6, PARAM_TYPE = 7,
};

/* ── State ───────────────────────────────────────────────────────── */

static int graph_id = -1;
static int dest_node_id = -1;
static int sample_rate = 48000;
static int audio_initialized = 0;
static int next_buffer_id = 1;

/* Temp buffer for processGraph output */
#define MAX_RENDER_FRAMES 2048
static float render_buf[MAX_RENDER_FRAMES * 2]; /* stereo */

/* Time tracking — use absolute time like the proven sdl2_wc pattern */
static uint32_t last_audio_time_ms = 0;

/* ── pump_audio — called each frame from wc_render ───────────────── */

void pump_audio(void) {
    if (!audio_initialized || graph_id < 0) return;

    /* Time-based frame count (matches wasmcart_audio_pump pattern) */
    uint32_t now_ms = (uint32_t)time_info.time_ms;
    uint32_t delta = now_ms - last_audio_time_ms;
    if (delta == 0) return;
    if (delta > 100) delta = 100;
    last_audio_time_ms = now_ms;

    int frames = (int)((uint64_t)sample_rate * delta / 1000);
    if (frames <= 0) return;
    if (frames > MAX_RENDER_FRAMES) frames = MAX_RENDER_FRAMES;

    /* Render in 128-frame blocks (Web Audio quantum) */
    int rendered = 0;
    while (rendered < frames) {
        int chunk = frames - rendered;
        if (chunk > 128) chunk = 128;
        processGraph(graph_id, render_buf + rendered * 2, chunk);
        rendered += chunk;
    }

    /* Write to wasmcart ring buffer */
    uint32_t wc = audio_write_cursor;
    for (int f = 0; f < frames; f++) {
        uint32_t wi = wc % AUDIO_CAP;
        audio_ring[wi * 2]     = render_buf[f * 2];
        audio_ring[wi * 2 + 1] = render_buf[f * 2 + 1];
        wc++;
    }
    audio_write_cursor = wc;
}

/* ── QuickJS bindings ────────────────────────────────────────────── */

static JSValue js_audio_ctx_new(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    if (!audio_initialized) {
        sample_rate = host_info.audio_sample_rate;
        if (sample_rate == 0) sample_rate = 48000;

        graph_id = createAudioGraph(sample_rate, 2, 128, 1);
        if (graph_id < 0) {
            WC_LOG("ERROR: createAudioGraph failed");
            return JS_NULL;
        }
        /* Destination node ID is 1 (next_id starts at 1 in audio_graph_simple.cpp) */
        dest_node_id = 1;
        audio_initialized = 1;
        last_audio_time_ms = (uint32_t)time_info.time_ms;
    }

    JSValue ac = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ac, "sampleRate", JS_NewInt32(ctx, sample_rate));
    JS_SetPropertyStr(ctx, ac, "_destNodeId", JS_NewInt32(ctx, dest_node_id));
    return ac;
}

static JSValue js_audio_current_time(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    if (graph_id < 0) return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, getGraphCurrentTime(graph_id));
}

static JSValue js_create_node(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    if (graph_id < 0) return JS_NULL;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_NULL;
    int nodeId = createNode(graph_id, type);
    JS_FreeCString(ctx, type);
    if (nodeId < 0) return JS_NULL;
    return JS_NewInt32(ctx, nodeId);
}

static JSValue js_connect_nodes(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    int32_t srcId, dstId;
    JS_ToInt32(ctx, &srcId, argv[0]);
    JS_ToInt32(ctx, &dstId, argv[1]);
    connectNodes(graph_id, srcId, dstId, 0, 0);
    return JS_UNDEFINED;
}

static JSValue js_start_node(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    int32_t nodeId; JS_ToInt32(ctx, &nodeId, argv[0]);
    double when = 0;
    if (argc > 1) JS_ToFloat64(ctx, &when, argv[1]);
    startNode(graph_id, nodeId, when);
    return JS_UNDEFINED;
}

static JSValue js_stop_node(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    int32_t nodeId; JS_ToInt32(ctx, &nodeId, argv[0]);
    double when = 0;
    if (argc > 1) JS_ToFloat64(ctx, &when, argv[1]);
    stopNode(graph_id, nodeId, when);
    return JS_UNDEFINED;
}

static JSValue js_set_param(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    int32_t nodeId, paramId;
    double value;
    JS_ToInt32(ctx, &nodeId, argv[0]);
    JS_ToInt32(ctx, &paramId, argv[1]);
    JS_ToFloat64(ctx, &value, argv[2]);
    setNodeParameter(graph_id, nodeId, paramId, (float)value);
    return JS_UNDEFINED;
}

static JSValue js_schedule_param_value(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    int32_t nodeId; JS_ToInt32(ctx, &nodeId, argv[0]);
    const char *param = JS_ToCString(ctx, argv[1]);
    double value, time;
    JS_ToFloat64(ctx, &value, argv[2]);
    JS_ToFloat64(ctx, &time, argv[3]);
    scheduleParameterValue(graph_id, nodeId, param, (float)value, time);
    JS_FreeCString(ctx, param);
    return JS_UNDEFINED;
}

static JSValue js_schedule_param_ramp(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    int32_t nodeId; JS_ToInt32(ctx, &nodeId, argv[0]);
    const char *param = JS_ToCString(ctx, argv[1]);
    double value, time;
    JS_ToFloat64(ctx, &value, argv[2]);
    JS_ToFloat64(ctx, &time, argv[3]);
    int exponential = 0;
    if (argc > 4) exponential = JS_ToBool(ctx, argv[4]);
    scheduleParameterRamp(graph_id, nodeId, param, (float)value, time, exponential);
    JS_FreeCString(ctx, param);
    return JS_UNDEFINED;
}

static JSValue js_set_string_prop(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    int32_t nodeId; JS_ToInt32(ctx, &nodeId, argv[0]);
    const char *prop = JS_ToCString(ctx, argv[1]);
    const char *val = JS_ToCString(ctx, argv[2]);
    setNodeStringProperty(graph_id, nodeId, prop, val);
    JS_FreeCString(ctx, prop);
    JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

static JSValue js_decode_audio(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    size_t len, boff, blen;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!buf) {
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &boff, &blen, NULL);
        if (!JS_IsException(ab)) {
            buf = JS_GetArrayBuffer(ctx, &len, ab);
            JS_FreeValue(ctx, ab);
            if (buf) { buf += boff; len = blen; }
        }
    }
    if (!buf) return JS_ThrowTypeError(ctx, "decodeAudioData requires ArrayBuffer");

    float *decoded = NULL;
    int total_samples = 0, decoded_rate = 0;
    int channels = decodeAudio(buf, len, &decoded, &total_samples, &decoded_rate);
    if (channels <= 0 || !decoded)
        channels = decodeWAV(buf, len, &decoded, &total_samples, &decoded_rate);
    if (channels <= 0 || !decoded)
        return JS_ThrowTypeError(ctx, "Failed to decode audio data");

    int length_frames = total_samples / channels;
    int buf_id = next_buffer_id++;
    /* registerBuffer stores a POINTER, not a copy.
     * Do NOT call freeDecodedBuffer — the graph keeps the reference. */
    registerBuffer(graph_id, buf_id, decoded, length_frames, channels);

    JSValue audio_buf = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, audio_buf, "numberOfChannels", JS_NewInt32(ctx, channels));
    JS_SetPropertyStr(ctx, audio_buf, "length", JS_NewInt32(ctx, length_frames));
    JS_SetPropertyStr(ctx, audio_buf, "sampleRate", JS_NewInt32(ctx, decoded_rate));
    JS_SetPropertyStr(ctx, audio_buf, "duration", JS_NewFloat64(ctx, (double)length_frames / decoded_rate));
    JS_SetPropertyStr(ctx, audio_buf, "_bufferId", JS_NewInt32(ctx, buf_id));

    /* DO NOT free — registerBuffer keeps the pointer alive */

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
    JSValue resolved = JS_Call(ctx, resolve_fn, promise_ctor, 1, &audio_buf);
    JS_FreeValue(ctx, resolve_fn);
    JS_FreeValue(ctx, promise_ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, audio_buf);
    return resolved;
}

static JSValue js_set_node_buffer(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    int32_t nodeId, bufferId;
    JS_ToInt32(ctx, &nodeId, argv[0]);
    JS_ToInt32(ctx, &bufferId, argv[1]);
    setNodeBufferId(graph_id, nodeId, bufferId);
    return JS_UNDEFINED;
}

/* ── Registration ────────────────────────────────────────────────── */

void register_audio_api(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);

    #define REG(name, func, nargs) \
        JS_SetPropertyStr(ctx, global, name, JS_NewCFunction(ctx, func, name, nargs))

    REG("_wcAudioInit", js_audio_ctx_new, 0);
    REG("_wcAudioTime", js_audio_current_time, 0);
    REG("_wcCreateNode", js_create_node, 1);
    REG("_wcConnectNodes", js_connect_nodes, 2);
    REG("_wcStartNode", js_start_node, 2);
    REG("_wcStopNode", js_stop_node, 2);
    REG("_wcSetParam", js_set_param, 3);
    REG("_wcScheduleParamValue", js_schedule_param_value, 4);
    REG("_wcScheduleParamRamp", js_schedule_param_ramp, 5);
    REG("_wcSetStringProp", js_set_string_prop, 3);
    REG("_wcDecodeAudio", js_decode_audio, 1);
    REG("_wcSetNodeBuffer", js_set_node_buffer, 2);

    JS_SetPropertyStr(ctx, global, "_PARAM_FREQUENCY", JS_NewInt32(ctx, PARAM_FREQUENCY));
    JS_SetPropertyStr(ctx, global, "_PARAM_DETUNE", JS_NewInt32(ctx, PARAM_DETUNE));
    JS_SetPropertyStr(ctx, global, "_PARAM_GAIN", JS_NewInt32(ctx, PARAM_GAIN));
    JS_SetPropertyStr(ctx, global, "_PARAM_Q", JS_NewInt32(ctx, PARAM_Q));
    JS_SetPropertyStr(ctx, global, "_PARAM_DELAY_TIME", JS_NewInt32(ctx, PARAM_DELAY_TIME));
    JS_SetPropertyStr(ctx, global, "_PARAM_PAN", JS_NewInt32(ctx, PARAM_PAN));
    JS_SetPropertyStr(ctx, global, "_PARAM_OFFSET", JS_NewInt32(ctx, PARAM_OFFSET));

    #undef REG

    const char *src =
        "const _paramMap = {\n"
        "  frequency: _PARAM_FREQUENCY, detune: _PARAM_DETUNE,\n"
        "  gain: _PARAM_GAIN, Q: _PARAM_Q, delayTime: _PARAM_DELAY_TIME,\n"
        "  pan: _PARAM_PAN, offset: _PARAM_OFFSET\n"
        "};\n"
        "\n"
        "class _WCAudioParam {\n"
        "  constructor(nodeId, paramName) {\n"
        "    this._nodeId = nodeId;\n"
        "    this._paramName = paramName;\n"
        "    this._paramId = _paramMap[paramName] !== undefined ? _paramMap[paramName] : -1;\n"
        "    this._value = paramName === 'gain' ? 1.0 : (paramName === 'frequency' ? 440 : 0);\n"
        "  }\n"
        "  get value() { return this._value; }\n"
        "  set value(v) {\n"
        "    this._value = v;\n"
        "    if (this._paramId >= 0) _wcSetParam(this._nodeId, this._paramId, v);\n"
        "  }\n"
        "  setValueAtTime(v, t) {\n"
        "    _wcScheduleParamValue(this._nodeId, this._paramName, v, t);\n"
        "    return this;\n"
        "  }\n"
        "  linearRampToValueAtTime(v, t) {\n"
        "    _wcScheduleParamRamp(this._nodeId, this._paramName, v, t, false);\n"
        "    return this;\n"
        "  }\n"
        "  exponentialRampToValueAtTime(v, t) {\n"
        "    _wcScheduleParamRamp(this._nodeId, this._paramName, v, t, true);\n"
        "    return this;\n"
        "  }\n"
        "  setTargetAtTime(target, startTime, timeConstant) {\n"
        "    _wcScheduleParamRamp(this._nodeId, this._paramName, target, startTime + timeConstant * 5, true);\n"
        "    return this;\n"
        "  }\n"
        "  cancelScheduledValues(t) { return this; }\n"
        "}\n"
        "\n"
        "class _WCAudioNode {\n"
        "  constructor(nodeId) { this._nodeId = nodeId; }\n"
        "  connect(dest) {\n"
        "    const dstId = dest._nodeId !== undefined ? dest._nodeId : dest;\n"
        "    _wcConnectNodes(this._nodeId, dstId);\n"
        "    return dest;\n"
        "  }\n"
        "  disconnect() { /* TODO */ }\n"
        "}\n"
        "\n"
        "class AudioContext {\n"
        "  constructor(opts) {\n"
        "    const ac = _wcAudioInit();\n"
        "    this.sampleRate = ac.sampleRate;\n"
        "    this.destination = { _nodeId: ac._destNodeId };\n"
        "    this.state = 'running';\n"
        "  }\n"
        "  get currentTime() { return _wcAudioTime(); }\n"
        "\n"
        "  createOscillator() {\n"
        "    const id = _wcCreateNode('oscillator');\n"
        "    const n = new _WCAudioNode(id);\n"
        "    n.frequency = new _WCAudioParam(id, 'frequency');\n"
        "    n.detune = new _WCAudioParam(id, 'detune');\n"
        "    n._type = 'sine';\n"
        "    Object.defineProperty(n, 'type', {\n"
        "      get() { return this._type; },\n"
        "      set(v) { this._type = v; _wcSetStringProp(this._nodeId, 'type', v); }\n"
        "    });\n"
        "    n.start = function(when) { _wcStartNode(id, when || 0); };\n"
        "    n.stop = function(when) { _wcStopNode(id, when || 0); };\n"
        "    return n;\n"
        "  }\n"
        "\n"
        "  createGain() {\n"
        "    const id = _wcCreateNode('gain');\n"
        "    const n = new _WCAudioNode(id);\n"
        "    n.gain = new _WCAudioParam(id, 'gain');\n"
        "    return n;\n"
        "  }\n"
        "\n"
        "  createBufferSource() {\n"
        "    const id = _wcCreateNode('bufferSource');\n"
        "    const n = new _WCAudioNode(id);\n"
        "    n.playbackRate = new _WCAudioParam(id, 'playbackRate');\n"
        "    n._buffer = null;\n"
        "    n.loop = false;\n"
        "    Object.defineProperty(n, 'buffer', {\n"
        "      get() { return this._buffer; },\n"
        "      set(b) {\n"
        "        this._buffer = b;\n"
        "        if (b && b._bufferId) _wcSetNodeBuffer(id, b._bufferId);\n"
        "      }\n"
        "    });\n"
        "    n.start = function(when) { _wcStartNode(id, when || 0); };\n"
        "    n.stop = function(when) { _wcStopNode(id, when || 0); };\n"
        "    n.onended = null;\n"
        "    return n;\n"
        "  }\n"
        "\n"
        "  createBiquadFilter() {\n"
        "    const id = _wcCreateNode('biquadFilter');\n"
        "    const n = new _WCAudioNode(id);\n"
        "    n.frequency = new _WCAudioParam(id, 'frequency');\n"
        "    n.Q = new _WCAudioParam(id, 'Q');\n"
        "    n.gain = new _WCAudioParam(id, 'gain');\n"
        "    n.detune = new _WCAudioParam(id, 'detune');\n"
        "    n._type = 'lowpass';\n"
        "    Object.defineProperty(n, 'type', {\n"
        "      get() { return this._type; },\n"
        "      set(v) { this._type = v; _wcSetStringProp(this._nodeId, 'type', v); }\n"
        "    });\n"
        "    return n;\n"
        "  }\n"
        "\n"
        "  createDelay(maxDelay) {\n"
        "    const id = _wcCreateNode('delay');\n"
        "    const n = new _WCAudioNode(id);\n"
        "    n.delayTime = new _WCAudioParam(id, 'delayTime');\n"
        "    return n;\n"
        "  }\n"
        "\n"
        "  createStereoPanner() {\n"
        "    const id = _wcCreateNode('stereoPanner');\n"
        "    const n = new _WCAudioNode(id);\n"
        "    n.pan = new _WCAudioParam(id, 'pan');\n"
        "    return n;\n"
        "  }\n"
        "\n"
        "  createDynamicsCompressor() {\n"
        "    const id = _wcCreateNode('dynamicsCompressor');\n"
        "    const n = new _WCAudioNode(id);\n"
        "    n.threshold = new _WCAudioParam(id, 'threshold');\n"
        "    n.knee = new _WCAudioParam(id, 'knee');\n"
        "    n.ratio = new _WCAudioParam(id, 'ratio');\n"
        "    n.attack = new _WCAudioParam(id, 'attack');\n"
        "    n.release = new _WCAudioParam(id, 'release');\n"
        "    return n;\n"
        "  }\n"
        "\n"
        "  createConstantSource() {\n"
        "    const id = _wcCreateNode('constantSource');\n"
        "    const n = new _WCAudioNode(id);\n"
        "    n.offset = new _WCAudioParam(id, 'offset');\n"
        "    n.start = function(when) { _wcStartNode(id, when || 0); };\n"
        "    n.stop = function(when) { _wcStopNode(id, when || 0); };\n"
        "    return n;\n"
        "  }\n"
        "\n"
        "  createBuffer(channels, length, rate) {\n"
        "    return { numberOfChannels: channels, length: length,\n"
        "             sampleRate: rate, duration: length / rate, _bufferId: 0 };\n"
        "  }\n"
        "\n"
        "  decodeAudioData(data) { return _wcDecodeAudio(data); }\n"
        "  resume() { return Promise.resolve(); }\n"
        "  suspend() { return Promise.resolve(); }\n"
        "  close() { return Promise.resolve(); }\n"
        "}\n"
        "\n"
        "globalThis.AudioContext = AudioContext;\n"
        "globalThis.webkitAudioContext = AudioContext;\n";

    JS_Eval(ctx, src, strlen(src), "<audio>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, global);
}
