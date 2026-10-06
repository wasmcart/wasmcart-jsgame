#!/usr/bin/env node
// gen_webgpu_bindings.mjs - generate src/webgpu_bindings.gen.c, the bulk of
// the cart's navigator.gpu (WebGPU for JavaScript inside QuickJS).
//
// Everything here is derived from ONE pinned emdawnwebgpu release, the same
// package the WebGPU cart links against:
//
//   webgpu/include/webgpu/webgpu.h          structs, members, INIT macros,
//                                            object types, functions
//   webgpu/src/library_webgpu_enum_tables.js the JS enum strings, by C value
//                                            (the tables emdawnwebgpu itself
//                                            uses to talk to a browser)
//
// What it emits:
//   - enum converters (JS string <-> C value) for every enum it touches
//   - JS dictionary -> C struct converters for every descriptor reachable
//     from a generated method, with the header's own INIT macro supplying
//     the WebGPU defaults
//   - one QuickJS class per WebGPU object (GPUBuffer, GPUTexture, ...), whose
//     finalizer releases the handle
//   - a method for every object function whose arguments it can convert, and
//     a getter for every no-argument Get* (GPUTexture.width, GPUBuffer.size)
//
// What it leaves to src/webgpu_shim.c (handwritten), listed in SKIP below:
// promises (mapAsync, onSubmittedWorkDone, popErrorScope, requestAdapter,
// requestDevice, ...), raw-data calls (writeBuffer, writeTexture,
// getMappedRange), features/limits/info, the canvas context, and a few
// dictionaries whose JS shape differs from the C struct (OVERRIDE_STRUCTS).
//
// Usage: node tools/gen_webgpu_bindings.mjs <emdawnwebgpu_pkg dir> [out.c]

import fs from 'node:fs';
import path from 'node:path';

const pkg = process.argv[2];
if (!pkg) { console.error('usage: gen_webgpu_bindings.mjs <emdawnwebgpu_pkg> [out.c]'); process.exit(1); }
const out = process.argv[3] || new URL('../src/webgpu_bindings.gen.c', import.meta.url).pathname;
const header = fs.readFileSync(path.join(pkg, 'webgpu/include/webgpu/webgpu.h'), 'utf8');
const tablesSrc = fs.readFileSync(path.join(pkg, 'webgpu/src/library_webgpu_enum_tables.js'), 'utf8');
const version = (() => { try { return fs.readFileSync(path.join(pkg, 'VERSION.txt'), 'utf8').trim(); } catch { return 'unknown'; } })();

// ── Enum string tables ──────────────────────────────────────────────
const enumStrings = {};   // EnumName -> Map(jsString -> value)
{
  const m = tablesSrc.match(/WEBGPU_INT_TO_STRING_TABLES = `([\s\S]*?)`;/);
  const intToString = new Function(`return ({${m[1]}});`)();
  for (const [name, arr] of Object.entries(intToString)) {
    const map = new Map();
    for (let i = 0; i < arr.length; i++) if (arr[i] !== undefined) map.set(arr[i], i);
    enumStrings[name] = map;
  }
  for (const mm of tablesSrc.matchAll(/\$emwgpuStringToInt_(\w+): \\`(\{[\s\S]*?\})\\`/g)) {
    const obj = new Function(`return (${mm[2]});`)();
    const map = enumStrings[mm[1]] || new Map();
    for (const [k, v] of Object.entries(obj)) if (k !== 'undefined') map.set(k, v);
    enumStrings[mm[1]] = map;
  }
}

// ── Header model ────────────────────────────────────────────────────
const objects = new Set([...header.matchAll(/typedef struct WGPU(\w+)Impl\* WGPU\1 WGPU_OBJECT_ATTRIBUTE;/g)].map(m => m[1]));
const flagTypes = new Set([...header.matchAll(/typedef WGPUFlags WGPU(\w+);/g)].map(m => m[1]));
const enumTypes = new Set([...header.matchAll(/typedef enum WGPU(\w+) \{/g)].map(m => m[1]));
const initMacro = {};
for (const m of header.matchAll(/#define (WGPU_\w+_INIT) _wgpu_MAKE_INIT_STRUCT\(WGPU(\w+),/g)) initMacro[m[2]] = m[1];

function parseDecl(text) {
  const nullable = /WGPU_NULLABLE/.test(text);
  const t = text.trim().replace(/;$/, '').replace(/WGPU_NULLABLE\s+/g, '').replace(/\bstruct\s+/g, '');
  const m = t.match(/^(.*?)([A-Za-z_]\w*)$/);
  return { type: m[1].trim().replace(/\s+/g, ' '), name: m[2], nullable };
}

const structs = {};
for (const m of header.matchAll(/typedef struct WGPU(\w+) \{\n([\s\S]*?)\n\} WGPU\1 WGPU_STRUCTURE_ATTRIBUTE;/g)) {
  structs[m[1]] = m[2].split('\n').map(l => l.trim()).filter(l => l && !l.startsWith('//')).map(parseDecl);
}

const functions = [];
for (const m of header.matchAll(/^WGPU_EXPORT (.*?)\b(wgpu\w+)\((.*?)\) WGPU_FUNCTION_ATTRIBUTE;$/gm)) {
  const ret = m[1].replace(/WGPU_NULLABLE\s+/, '').trim();
  const params = m[3].trim() === 'void' || !m[3].trim() ? [] : m[3].split(',').map(parseDecl);
  functions.push({ ret, name: m[2], params });
}

// ── Classification ──────────────────────────────────────────────────
const strip = t => t.replace(/^WGPU/, '');
function kind(type) {
  const t = type;
  if (t === 'WGPUStringView') return { k: 'sv' };
  if (t === 'WGPUBool') return { k: 'bool' };
  if (t === 'WGPUOptionalBool') return { k: 'obool' };
  if (/^(u?int(8|16|32|64)_t|size_t)$/.test(t)) return { k: 'int', c: t };
  if (t === 'float' || t === 'double') return { k: 'float', c: t };
  if (/^WGPU\w+$/.test(t)) {
    const n = strip(t);
    if (objects.has(n)) return { k: 'obj', n };
    if (flagTypes.has(n)) return { k: 'flags', n };
    if (enumTypes.has(n)) return enumStrings[n] ? { k: 'enum', n } : { k: 'unsupported' };
    if (structs[n]) return { k: 'struct', n };
  }
  let m = t.match(/^WGPU(\w+) const \*$/);
  if (m && structs[m[1]]) return { k: 'structptr', n: m[1] };
  if (m && (objects.has(m[1]) || enumTypes.has(m[1]))) return { k: 'elemptr', n: m[1] };
  if (t === 'uint32_t const *') return { k: 'elemptr', n: 'uint32_t' };
  return { k: 'unsupported' };
}

// Dictionaries whose JS shape is not the C struct's: converters handwritten
// in webgpu_shim.c (same signature as generated ones).
const OVERRIDE_STRUCTS = new Set([
  'Extent3D',            // [w, h, d] or {width, height, depthOrArrayLayers}
  'Origin3D',            // [x, y, z] or {x, y, z}
  'Color',               // [r, g, b, a] or {r, g, b, a}
  'TexelCopyBufferInfo', // JS flattens .layout into the dictionary
  'BindGroupEntry',      // JS .resource is a union
  'ShaderModuleDescriptor', // JS .code becomes a chained WGSL source
]);

// C run after a generated converter, for C defaults JS does not share.
const POST = {
  // INIT leaves depthClearValue NaN ("undefined"), which the glue passes to
  // JS as-is and a real GPURenderPassEncoder rejects ("not finite") even
  // though it is ignored unless depthLoadOp is 'clear'. JS leaves it out in
  // exactly that case, so only keep NaN where it would be a real error.
  RenderPassDepthStencilAttachment:
    'if (out->depthClearValue != out->depthClearValue && out->depthLoadOp != WGPULoadOp_Clear) out->depthClearValue = 0.0f;',
};

// Object functions handled in webgpu_shim.c (or deliberately absent).
const SKIP = new Set([
  // promises
  'wgpuAdapterRequestDevice', 'wgpuBufferMapAsync', 'wgpuDevicePopErrorScope',
  'wgpuDeviceCreateComputePipelineAsync', 'wgpuDeviceCreateRenderPipelineAsync',
  'wgpuQueueOnSubmittedWorkDone', 'wgpuShaderModuleGetCompilationInfo',
  'wgpuInstanceRequestAdapter', 'wgpuDeviceGetLostFuture',
  // raw memory / mapping
  'wgpuQueueWriteBuffer', 'wgpuQueueWriteTexture', 'wgpuBufferGetMappedRange',
  'wgpuBufferGetConstMappedRange', 'wgpuBufferReadMappedRange', 'wgpuBufferWriteMappedRange',
  'wgpuBufferUnmap', 'wgpuBufferDestroy',
  // attributes built by hand (features, limits, info, queue)
  'wgpuAdapterGetFeatures', 'wgpuAdapterGetInfo', 'wgpuAdapterGetLimits', 'wgpuAdapterHasFeature',
  'wgpuDeviceGetAdapterInfo', 'wgpuDeviceGetFeatures', 'wgpuDeviceGetLimits', 'wgpuDeviceHasFeature',
  'wgpuDeviceGetQueue', 'wgpuDeviceDestroy', // the host owns the device
  // the cart never presents; the canvas context owns the surface
  'wgpuSurfaceConfigure', 'wgpuSurfaceGetCapabilities', 'wgpuSurfaceGetCurrentTexture',
  'wgpuSurfacePresent', 'wgpuSurfaceUnconfigure',
  'wgpuInstanceCreateSurface', 'wgpuInstanceWaitAny', 'wgpuInstanceProcessEvents',
  'wgpuInstanceGetWGSLLanguageFeatures', 'wgpuInstanceHasWGSLLanguageFeature',
]);

// JS default values for trailing optional numeric arguments (WebGPU IDL).
const ARG_DEFAULTS = {
  instanceCount: '1', workgroupCountY: '1', workgroupCountZ: '1',
};
function argDefault(fn, p) {
  if (ARG_DEFAULTS[p.name]) return ARG_DEFAULTS[p.name];
  if (p.name === 'size' && p.type === 'uint64_t') return 'WGPU_WHOLE_SIZE';
  return '0';
}

// ── Reachability ────────────────────────────────────────────────────
const usedStructs = new Set(), usedEnums = new Set();
function useStruct(n) {
  if (usedStructs.has(n) || /CallbackInfo$/.test(n)) return;
  usedStructs.add(n);
  for (const mem of structs[n] || []) {
    const k = kind(mem.type);
    if (k.k === 'enum') usedEnums.add(k.n);
    if (k.k === 'struct' || k.k === 'structptr') useStruct(k.n);
    if (k.k === 'elemptr' && enumTypes.has(k.n)) usedEnums.add(k.n);
  }
}

// Plan methods.
const methods = {};   // obj -> [{js, fn, getter}]
const skipped = [];
for (const fn of functions) {
  const owner = [...objects].filter(o => fn.name.startsWith('wgpu' + o)).sort((a, b) => b.length - a.length)[0];
  if (!owner || !fn.params.length || fn.params[0].type !== 'WGPU' + owner) continue;
  const rest = fn.name.slice(4 + owner.length);
  if (/^(AddRef|Release|SetLabel)$/.test(rest)) continue;
  if (SKIP.has(fn.name)) { skipped.push(`${fn.name} (handwritten or n/a)`); continue; }
  const params = fn.params.slice(1);
  let ok = true;
  for (let i = 0; i < params.length; i++) {
    const k = kind(params[i].type);
    if (params[i].type === 'size_t' && /Count$/.test(params[i].name) && params[i + 1] && kind(params[i + 1].type).k === 'elemptr') { i++; continue; }
    if (params[i].type === 'size_t' && /Count$/.test(params[i].name) && params[i + 1] && kind(params[i + 1].type).k === 'structptr') { i++; continue; }
    if (!['sv', 'bool', 'int', 'float', 'obj', 'flags', 'enum', 'structptr'].includes(k.k)) { ok = false; break; }
  }
  const rk = kind(fn.ret);
  if (!(fn.ret === 'void' || ['obj', 'bool', 'int', 'float', 'enum', 'flags'].includes(rk.k))) ok = false;
  if (!ok) { skipped.push(`${fn.name} (argument or result not convertible)`); continue; }
  const getter = /^Get[A-Z]/.test(rest) && params.length === 0 && rk.k !== 'obj';
  const js = getter ? rest[3].toLowerCase() + rest.slice(4) : rest[0].toLowerCase() + rest.slice(1);
  (methods[owner] ||= []).push({ js, fn, getter, params });
  for (const p of params) {
    const k = kind(p.type);
    if (k.k === 'structptr') useStruct(k.n);
    if (k.k === 'enum') usedEnums.add(k.n);
    if (k.k === 'elemptr' && enumTypes.has(k.n)) usedEnums.add(k.n);
  }
  if (rk.k === 'enum') usedEnums.add(rk.n);
}
// Structs/enums the handwritten half needs too.
for (const s of ['DeviceDescriptor', 'RequestAdapterOptions', 'TexelCopyTextureInfo', 'TexelCopyBufferLayout',
  'Extent3D', 'Origin3D', 'Color', 'RenderPipelineDescriptor', 'ComputePipelineDescriptor', 'Limits']) useStruct(s);
for (const e of ['TextureFormat', 'FeatureName', 'CompositeAlphaMode', 'ErrorFilter', 'CompilationMessageType',
  'BufferMapState', 'TextureDimension', 'TextureViewDimension', 'QueryType']) usedEnums.add(e);

// ── Emit ────────────────────────────────────────────────────────────
const L = [];
const emit = s => L.push(s);
emit(`/* GENERATED by tools/gen_webgpu_bindings.mjs from emdawnwebgpu ${version}. DO NOT EDIT. */`);
emit(`/* Included by src/webgpu_shim.c, which provides the helpers used here. */`);
emit('');

// Object type indices.
const objList = [...objects].sort();
emit('enum {');
for (const o of objList) emit(`    WGJ_T_${o},`);
emit('    WGJ_T_COUNT');
emit('};');
emit(`static const char *const wgj_class_names[WGJ_T_COUNT] = { ${objList.map(o => `"GPU${o}"`).join(', ')} };`);
for (const o of objList) emit(`static void wgj_release_${o}(void *h) { wgpu${o}Release((WGPU${o})h); }`);
emit(`static void (*const wgj_release_fns[WGJ_T_COUNT])(void *) = { ${objList.map(o => `wgj_release_${o}`).join(', ')} };`);
emit('');

// Enums.
for (const e of [...usedEnums].sort()) {
  const map = enumStrings[e];
  if (!map) throw new Error(`no JS strings for enum ${e}`);
  const entries = [...map].filter(([s]) => s !== '');
  emit(`static const wgj_enum_entry wgj_enum_tab_${e}[] = {`);
  for (const [s, v] of entries) emit(`    { ${JSON.stringify(s)}, ${v} },`);
  emit('    { 0, 0 }');
  emit('};');
  emit(`static int wgj_enum_${e}(JSContext *ctx, JSValueConst v, WGPU${e} *out) {`);
  emit(`    uint32_t x; if (wgj_enum_lookup(ctx, v, wgj_enum_tab_${e}, "GPU${e}", &x)) return -1;`);
  emit(`    *out = (WGPU${e})x; return 0;`);
  emit('}');
  emit(`static JSValue wgj_enum_${e}_to_js(JSContext *ctx, WGPU${e} v) { return wgj_enum_name(ctx, wgj_enum_tab_${e}, (uint32_t)v); }`);
}
emit('');

// Struct converter prototypes.
const structList = [...usedStructs].filter(s => structs[s]).sort();
for (const s of structList) emit(`static int wgj_conv_${s}(JSContext *ctx, JSValueConst o, WGPU${s} *out);`);
emit('');

function convValue(k, dst, v, label) {
  switch (k.k) {
    case 'sv': return `if (wgj_to_sv(ctx, ${v}, &${dst})) goto fail;`;
    case 'bool': return `${dst} = JS_ToBool(ctx, ${v}) ? 1 : 0;`;
    case 'obool': return `${dst} = JS_ToBool(ctx, ${v}) ? WGPUOptionalBool_True : WGPUOptionalBool_False;`;
    case 'int': return `{ double d_; if (JS_ToFloat64(ctx, &d_, ${v})) goto fail; ${dst} = (${k.c})wgj_d2i(d_); }`;
    case 'float': return `{ double d_; if (JS_ToFloat64(ctx, &d_, ${v})) goto fail; ${dst} = (${k.c})d_; }`;
    case 'flags': return `{ double d_; if (JS_ToFloat64(ctx, &d_, ${v})) goto fail; ${dst} = (WGPUFlags)wgj_d2i(d_); }`;
    case 'enum': return `if (wgj_enum_${k.n}(ctx, ${v}, &${dst})) goto fail;`;
    case 'obj': return `if (wgj_unwrap(ctx, ${v}, WGJ_T_${k.n}, (void **)&${dst}, ${JSON.stringify(label)})) goto fail;`;
    case 'struct': return `if (wgj_conv_${k.n}(ctx, ${v}, &${dst})) goto fail;`;
    default: throw new Error('convValue ' + k.k);
  }
}

// Array of elements: JS array (or typed array) -> arena C array.
function convArray(elemType, dstPtr, dstCount, v, label) {
  const ek = elemType === 'uint32_t' ? { k: 'int', c: 'uint32_t' } : kind('WGPU' + elemType);
  const ctype = elemType === 'uint32_t' ? 'uint32_t' : 'WGPU' + elemType;
  const lines = [];
  lines.push(`{ uint32_t n_; if (wgj_array_len(ctx, ${v}, &n_, ${JSON.stringify(label)})) goto fail;`);
  lines.push(`  ${ctype} *a_ = n_ ? (${ctype} *)wgj_alloc(sizeof(${ctype}) * n_) : NULL;`);
  lines.push(`  for (uint32_t i_ = 0; i_ < n_; i_++) {`);
  lines.push(`    JSValue e_ = JS_GetPropertyUint32(ctx, ${v}, i_);`);
  if (ek.k === 'struct') {
    lines.push(`    if (JS_IsNull(e_) || JS_IsUndefined(e_)) { memset(&a_[i_], 0, sizeof a_[i_]); continue; }`);
    lines.push(`    if (wgj_conv_${ek.n}(ctx, e_, &a_[i_])) { JS_FreeValue(ctx, e_); goto fail; }`);
  } else {
    const c = convValue(ek, 'a_[i_]', 'e_', label + '[]').replace(/goto fail;/g, '{ JS_FreeValue(ctx, e_); goto fail; }');
    lines.push(`    ${c}`);
  }
  lines.push(`    JS_FreeValue(ctx, e_);`);
  lines.push(`  }`);
  lines.push(`  ${dstPtr} = a_; ${dstCount} = n_; }`);
  return lines.join('\n        ');
}

for (const s of structList) {
  if (OVERRIDE_STRUCTS.has(s)) continue;
  const mem = structs[s];
  emit(`static int wgj_conv_${s}(JSContext *ctx, JSValueConst o, WGPU${s} *out) {`);
  emit(initMacro[s] ? `    *out = (WGPU${s})${initMacro[s]};` : `    memset(out, 0, sizeof *out);`);
  emit(`    if (JS_IsUndefined(o) || JS_IsNull(o)) return 0;`);
  emit(`    if (!JS_IsObject(o)) { JS_ThrowTypeError(ctx, "GPU${s}: expected a dictionary"); return -1; }`);
  emit(`    JSValue v = JS_UNDEFINED;`);
  for (let i = 0; i < mem.length; i++) {
    const m = mem[i];
    if (m.name === 'nextInChain' || m.name === 'chain' || /^userdata/.test(m.name)) continue;
    if (/CallbackInfo$/.test(m.type) || /Callback$/.test(m.type)) continue;
    const next = mem[i + 1];
    if (m.type === 'size_t' && /Count$/.test(m.name) && next && /const \*$/.test(next.type)) {
      const nk = kind(next.type);
      const label = `GPU${s}.${next.name}`;
      emit(`    v = JS_GetPropertyStr(ctx, o, "${next.name}");`);
      emit(`    if (JS_IsException(v)) goto fail;`);
      if (next.name === 'constants' && nk.n === 'ConstantEntry') {
        emit(`    if (!JS_IsUndefined(v) && wgj_conv_constants(ctx, v, &out->constants, &out->constantCount)) goto fail;`);
      } else if (nk.k === 'structptr' || nk.k === 'elemptr') {
        emit(`    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {`);
        emit(`        ${convArray(nk.n, 'out->' + next.name, 'out->' + m.name, 'v', label)}`);
        emit(`    }`);
      } else {
        emit(`    /* ${label}: unsupported element type ${next.type} */`);
      }
      emit(`    JS_FreeValue(ctx, v); v = JS_UNDEFINED;`);
      i++;
      continue;
    }
    const k = kind(m.type);
    const label = `GPU${s}.${m.name}`;
    if (k.k === 'unsupported' || k.k === 'elemptr') { emit(`    /* ${label}: ${m.type} not exposed to JS */`); continue; }
    emit(`    v = JS_GetPropertyStr(ctx, o, "${m.name}");`);
    emit(`    if (JS_IsException(v)) goto fail;`);
    if (k.k === 'structptr') {
      emit(`    if (!JS_IsUndefined(v) && !JS_IsNull(v)) { WGPU${k.n} *p_ = (WGPU${k.n} *)wgj_alloc(sizeof *p_); if (wgj_conv_${k.n}(ctx, v, p_)) goto fail; out->${m.name} = p_; }`);
    } else if (k.k === 'obj') {
      emit(`    if (!JS_IsUndefined(v)) { ${convValue(k, 'out->' + m.name, 'v', label)} }`);
    } else {
      emit(`    if (!JS_IsUndefined(v)) { ${convValue(k, 'out->' + m.name, 'v', label)} }`);
    }
    emit(`    JS_FreeValue(ctx, v); v = JS_UNDEFINED;`);
  }
  if (POST[s]) emit(`    ${POST[s]}`);
  emit(`    return 0;`);
  emit(`fail:`);
  emit(`    JS_FreeValue(ctx, v);`);
  emit(`    return -1;`);
  emit(`}`);
  emit('');
}

// C -> JS for results the shim hands back as plain objects (limits, adapter
// info): every numeric, string and enum member, under its C name.
const OUT_STRUCTS = ['Limits', 'AdapterInfo'];
for (const s of OUT_STRUCTS) {
  emit(`static JSValue wgj_out_${s}(JSContext *ctx, const WGPU${s} *in) {`);
  emit(`    JSValue o = JS_NewObject(ctx);`);
  for (const m of structs[s]) {
    const k = kind(m.type);
    if (k.k === 'enum') usedEnums.add(k.n);
    let val = null;
    if (k.k === 'sv') val = `JS_NewStringLen(ctx, in->${m.name}.data ? in->${m.name}.data : "", in->${m.name}.data ? (in->${m.name}.length == WGPU_STRLEN ? strlen(in->${m.name}.data) : in->${m.name}.length) : 0)`;
    else if (k.k === 'int' || k.k === 'float') val = `JS_NewFloat64(ctx, (double)in->${m.name})`;
    else if (k.k === 'enum' && enumStrings[k.n]) val = `wgj_enum_${k.n}_to_js(ctx, in->${m.name})`;
    if (val) emit(`    JS_SetPropertyStr(ctx, o, "${m.name}", ${val});`);
  }
  emit(`    return o;`);
  emit(`}`);
}
emit('');

// Methods and getters.
let methodCount = 0, getterCount = 0;
for (const o of objList) {
  const list = methods[o] || [];
  for (const mth of list) {
    const { fn, params } = mth;
    const cname = `wgj_m_${o}_${mth.js}`;
    if (mth.getter) {
      getterCount++;
      const rk = kind(fn.ret);
      emit(`static JSValue ${cname}(JSContext *ctx, JSValueConst this_val) {`);
      emit(`    WGPU${o} self; if (wgj_unwrap_this(ctx, this_val, WGJ_T_${o}, (void **)&self)) return JS_EXCEPTION;`);
      emit(`    ${retToJs(rk, `${fn.name}(self)`)}`);
      emit(`}`);
      continue;
    }
    methodCount++;
    emit(`static JSValue ${cname}(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {`);
    emit(`    WGPU${o} self; if (wgj_unwrap_this(ctx, this_val, WGJ_T_${o}, (void **)&self)) return JS_EXCEPTION;`);
    emit(`    size_t mark = wgj_arena_mark();`);
    const callArgs = ['self'];
    let j = 0;
    for (let i = 0; i < params.length; i++) {
      const p = params[i];
      const label = `GPU${o}.${mth.js}: ${p.name}`;
      const next = params[i + 1];
      if (p.type === 'size_t' && /Count$/.test(p.name) && next && ['elemptr', 'structptr'].includes(kind(next.type).k)) {
        const nk = kind(next.type);
        const ctype = nk.n === 'uint32_t' ? 'uint32_t' : 'WGPU' + nk.n;
        emit(`    const ${ctype} *${next.name} = NULL; size_t ${p.name} = 0;`);
        emit(`    if (argc > ${j} && !JS_IsUndefined(argv[${j}]) && !JS_IsNull(argv[${j}])) {`);
        emit(`        ${convArray(nk.n, next.name, p.name, `argv[${j}]`, label)}`);
        emit(`    }`);
        callArgs.push(p.name, next.name);
        i++; j++;
        continue;
      }
      const k = kind(p.type);
      if (k.k === 'structptr') {
        emit(`    WGPU${k.n} *${p.name} = NULL;`);
        // A nullable descriptor left out is NULL; a required one left out
        // is converted from undefined (its INIT defaults) for Dawn to judge.
        emit(p.nullable ? `    if (argc > ${j} && !JS_IsUndefined(argv[${j}])) {` : `    {`);
        emit(`        ${p.name} = (WGPU${k.n} *)wgj_alloc(sizeof *${p.name});`);
        emit(`        if (wgj_conv_${k.n}(ctx, argc > ${j} ? argv[${j}] : JS_UNDEFINED, ${p.name})) goto fail;`);
        emit(`    }`);
      } else if (k.k === 'obj') {
        emit(`    WGPU${k.n} ${p.name} = NULL;`);
        emit(`    if (argc > ${j}) { ${convValue(k, p.name, `argv[${j}]`, label)} }`);
      } else if (k.k === 'sv') {
        emit(`    WGPUStringView ${p.name} = { NULL, 0 };`);
        emit(`    if (argc > ${j} && !JS_IsUndefined(argv[${j}])) { ${convValue(k, p.name, `argv[${j}]`, label)} }`);
      } else if (k.k === 'enum') {
        emit(`    WGPU${k.n} ${p.name} = (WGPU${k.n})0;`);
        emit(`    if (argc > ${j} && !JS_IsUndefined(argv[${j}])) { ${convValue(k, p.name, `argv[${j}]`, label)} }`);
      } else {
        const ctype = k.k === 'flags' ? 'WGPU' + k.n : (k.c || p.type);
        emit(`    ${ctype} ${p.name} = (${ctype})${argDefault(fn, p)};`);
        emit(`    if (argc > ${j} && !JS_IsUndefined(argv[${j}])) { ${convValue(k, p.name, `argv[${j}]`, label)} }`);
      }
      callArgs.push(p.name);
      j++;
    }
    const call = `${fn.name}(${callArgs.join(', ')})`;
    if (fn.ret === 'void') {
      emit(`    ${call};`);
      emit(`    wgj_arena_release(mark);`);
      emit(`    return JS_UNDEFINED;`);
    } else {
      const rk = kind(fn.ret);
      emit(`    ${fn.ret} r_ = ${call};`);
      emit(`    wgj_arena_release(mark);`);
      emit(`    ${retToJs(rk, 'r_')}`);
    }
    emit(`fail:`);
    emit(`    wgj_arena_release(mark);`);
    emit(`    return JS_EXCEPTION;`);
    emit(`}`);
  }
  emit(`static const JSCFunctionListEntry wgj_gen_proto_${o}[] = {`);
  for (const mth of list) {
    if (mth.getter) emit(`    JS_CGETSET_DEF("${mth.js}", wgj_m_${o}_${mth.js}, NULL),`);
    else emit(`    JS_CFUNC_DEF("${mth.js}", ${mth.params.length}, wgj_m_${o}_${mth.js}),`);
  }
  emit(`    JS_CGETSET_MAGIC_DEF("label", wgj_label_get, wgj_label_set, WGJ_T_${o}),`);
  emit(`};`);
  emit('');
}

function retToJs(rk, expr) {
  switch (rk.k) {
    case 'obj': return `return wgj_wrap(ctx, WGJ_T_${rk.n}, (void *)(${expr}));`;
    case 'bool': return `return JS_NewBool(ctx, (${expr}) != 0);`;
    case 'enum': return `return wgj_enum_${rk.n}_to_js(ctx, ${expr});`;
    case 'int': case 'float': case 'flags': return `return JS_NewFloat64(ctx, (double)(${expr}));`;
    default: throw new Error('retToJs ' + rk.k);
  }
}

// Registration table.
emit(`static const JSCFunctionListEntry *const wgj_gen_protos[WGJ_T_COUNT] = { ${objList.map(o => `wgj_gen_proto_${o}`).join(', ')} };`);
emit(`static const int wgj_gen_proto_lens[WGJ_T_COUNT] = { ${objList.map(o => `countof(wgj_gen_proto_${o})`).join(', ')} };`);
emit('');
emit(`/* Coverage: ${methodCount} methods, ${getterCount} getters, ${structList.length} dictionaries, ${usedEnums.size} enums, ${objList.length} classes. */`);
emit(`/* Not generated (see webgpu_shim.c):`);
for (const s of skipped) emit(` *   ${s}`);
emit(` */`);

fs.writeFileSync(out, L.join('\n') + '\n');
console.log(`wrote ${out}: ${methodCount} methods, ${getterCount} getters, ${structList.length} dictionaries, ${usedEnums.size} enums, ${objList.length} classes; ${skipped.length} functions left to the shim`);
