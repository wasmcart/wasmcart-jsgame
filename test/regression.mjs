// Regression suite for the bugs fixed on 2026-07-28.
//
// Every one of these failed with the SAME symptom -- "memory access out of
// bounds", no hint of the cause -- so they are easy to reintroduce and hard to
// diagnose a second time. Each check below maps to one fixed bug and is written
// to fail loudly if it comes back.
//
//   node test/regression.mjs
//
// Env: WASMCART_REPO, JSGAMES (defaults: sibling checkouts)
import { readFileSync, existsSync, mkdtempSync, cpSync, rmSync, readdirSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, resolve as presolve, join } from 'node:path';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = presolve(HERE, '..');
const WASMCART = process.env.WASMCART_REPO ?? presolve(ROOT, '../wasmcart');
const JSGAMES = process.env.JSGAMES ?? presolve(ROOT, '../jsgames');

const require = createRequire(WASMCART + '/index.js');
const { CartHost } = await import(WASMCART + '/src/CartHost.js');
const { createWebGL2Context } = require('webgl-node');

let pass = 0, fail = 0, skip = 0;
const ok = (name, cond, detail = '') => {
  console.log(`  ${cond ? 'OK  ' : 'FAIL'} ${name.padEnd(46)} ${detail}`);
  cond ? pass++ : fail++;
};
const skipped = (name, why) => { console.log(`  SKIP ${name.padEnd(46)} ${why}`); skip++; };

function pack(dir, out, name) {
  execFileSync('bash', [join(ROOT, 'pack_game.sh'), dir, out, name],
    { stdio: 'ignore', cwd: ROOT });
  return out;
}

/** Run a cart N frames; returns {colours, error}. */
async function run(wasc, frames = 60, w = 800, h = 600) {
  const { gl } = createWebGL2Context(w, h);
  const host = new CartHost({});
  try {
    await host.load(readFileSync(wasc), { glBackend: gl, width: w, height: h });
    for (let i = 0; i < frames; i++) host.runFrame([]);
    const px = new Uint8Array(w * h * 4);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    const s = new Set();
    for (let i = 0; i < px.length; i += 4) s.add((px[i] << 16) | (px[i + 1] << 8) | px[i + 2]);
    return { colours: s.size, error: null };
  } catch (e) {
    return { colours: 0, error: String(e.message).slice(0, 60) };
  }
}

const tmp = mkdtempSync(join(tmpdir(), 'wcjs-regress-'));
console.log(`cart: build/cart.wasm  scratch: ${tmp}\n`);

if (!existsSync(join(ROOT, 'build/cart.wasm'))) {
  console.log('build/cart.wasm missing -- run `bash build.sh` first');
  process.exit(1);
}

/* ── 1. Skia font-directory scan (build-libcanvas/patches/wasm-no-font-dir) ──
 * Crashed on the FIRST Canvas 2D call, during lazy Skia init. Masked by
 * -sASSERTIONS=1, which is why this must run on a plain -O2 build. */
{
  const r = await run(pack(join(ROOT, 'examples/hello_canvas'), join(tmp, 'canvas.wasc'), 'c'));
  ok('canvas 2D init (Skia font dir)', !r.error && r.colours > 100,
     r.error ?? `${r.colours} colours`);
}

/* ── 2. Skia archive copy step (wasmcart-skia/build.sh) ──────────────────────
 * Copied 4 of 18 archives; consumers need 16. Produced a silent partial link
 * that faulted at runtime. A tiny cart is the sensitive case -- small assets
 * land on the wrong side of whatever is corrupt. */
{
  // Generated inline rather than checked in: the whole point is that it is
  // TINY, and a fixture directory would drift from that.
  const { mkdirSync, writeFileSync } = await import('node:fs');
  const d = join(tmp, 'tiny');
  mkdirSync(d, { recursive: true });
  writeFileSync(join(d, 'main.js'),
    `const c=document.getElementById('game');c.width=800;c.height=600;\n` +
    `const ctx=c.getContext('2d');\n` +
    `function loop(){ctx.fillStyle='#1a1a2e';ctx.fillRect(50,50,100,100);requestAnimationFrame(loop);}\n` +
    `requestAnimationFrame(loop);\n`);
  const r = await run(pack(d, join(tmp, 'tiny.wasc'), 't'), 20);
  ok('tiny cart (partial Skia link)', !r.error && r.colours > 1, r.error ?? `${r.colours} colours`);
}

/* ── 3. Microtask DoS bound (cart_main.c MAX_JOB_MS) ─────────────────────────
 * `function spin(){Promise.resolve().then(spin);}` used to hang wc_render
 * forever. A count-based bound still handed a hostile cart a fixed slice of
 * every frame, so the bound is TIME-based. */
{
  const { mkdirSync, writeFileSync } = await import('node:fs');
  const d = join(tmp, 'spin');
  mkdirSync(d, { recursive: true });
  writeFileSync(join(d, 'main.js'),
    `function spin(){Promise.resolve().then(spin);}\nspin();\n` +
    `function loop(){requestAnimationFrame(loop);}\nrequestAnimationFrame(loop);\n`);
  const wasc = pack(d, join(tmp, 'spin.wasc'), 's');
  const { gl } = createWebGL2Context(320, 240);
  const host = new CartHost({});
  await host.load(readFileSync(wasc), { glBackend: gl, width: 320, height: 240 });
  for (let i = 0; i < 5; i++) host.runFrame([]);
  const t = [];
  for (let i = 0; i < 40; i++) { const a = Date.now(); host.runFrame([]); t.push(Date.now() - a); }
  t.sort((a, b) => a - b);
  // The budget is 4ms; allow generous headroom for a slow machine, but a
  // REGRESSION to unbounded draining would blow past this by orders of magnitude.
  ok('hostile microtask spin is bounded', t[39] < 100, `max ${t[39]}ms/frame (budget 4ms)`);
}

/* ── 4. Promise job pump (cart_main.c pump_jobs) ─────────────────────────────
 * Nothing drained the microtask queue, so NO promise in ANY game ever
 * resolved. hello_fetch awaits its assets, so it is the canary. */
{
  const r = await run(pack(join(ROOT, 'examples/hello_fetch'), join(tmp, 'fetch.wasc'), 'f'), 60);
  ok('promises resolve (fetch + await)', !r.error, r.error ?? 'ran 60 frames');
}

/* ── 5. MAXIMUM_MEMORY ceiling (build.sh) ────────────────────────────────────
 * `space` decodes 21MB of .ogg to ~500MB of f32 PCM and blew a 1GB ceiling
 * during load. Needs the jsgames checkout; skipped if absent. */
{
  const src = join(JSGAMES, 'space');
  if (!existsSync(src)) {
    skipped('large audio load (memory ceiling)', 'jsgames/space not found');
  } else {
    const d = join(tmp, 'space');
    const { mkdirSync } = await import('node:fs');
    mkdirSync(d, { recursive: true });
    // node_modules/dist would make a 150MB cart -- see REBUILD_STATUS.md
    for (const f of readdirSync(src)) {
      if (['node_modules', 'dist', 'package-lock.json'].includes(f)) continue;
      cpSync(join(src, f), join(d, f), { recursive: true });
    }
    const r = await run(pack(d, join(tmp, 'space.wasc'), 'sp'), 120);
    ok('large audio load (memory ceiling)', !r.error && r.colours > 50,
       r.error ?? `${r.colours} colours`);
  }
}

/* ── 6. Frame watchdog (cart_main.c JS_SetInterruptHandler) ──────────────────
 * `while(true){}` used to hang wc_render, and the host with it, forever. The
 * microtask bound cannot catch it: a synchronous loop never returns to the job
 * queue. QuickJS's interrupt handler can, because the interpreter calls it. */
{
  const { mkdirSync, writeFileSync } = await import('node:fs');
  const d = join(tmp, 'busy');
  mkdirSync(d, { recursive: true });
  writeFileSync(join(d, 'main.js'),
    `function loop(){ while(true){} }\nrequestAnimationFrame(loop);\n`);
  const wasc = pack(d, join(tmp, 'busy.wasc'), 'b');
  const { gl } = createWebGL2Context(320, 240);
  const host = new CartHost({});
  await host.load(readFileSync(wasc), { glBackend: gl, width: 320, height: 240 });
  const t0 = Date.now();
  host.runFrame([]);
  const spent = Date.now() - t0;
  // Budget is 2000ms. Anything under ~5s means it fired; a REGRESSION to no
  // handler at all would never return and this line would never run.
  ok('infinite loop is interrupted', spent < 5000, `frame returned after ${spent}ms`);
  // And the host must still be usable afterwards, not left wedged.
  const t1 = Date.now();
  for (let i = 0; i < 3; i++) host.runFrame([]);
  ok('host survives an interrupted frame', Date.now() - t1 < 1000,
     `3 more frames in ${Date.now() - t1}ms`);
}

/* ── 7. fetch() 404 response shape (cart_main.c finish_response) ─────────────
 * A miss used to return a bare {ok:false,status:404} with no body methods, so
 * `fetch(missing).then(...)` threw "not a function" and `.text()` was absent.
 * `await fetch(...)` worked, which is why hello_fetch never caught it. */
{
  const { mkdirSync, writeFileSync } = await import('node:fs');
  const d = join(tmp, 'f404');
  mkdirSync(d, { recursive: true });
  writeFileSync(join(d, 'main.js'), `
const c=document.getElementById('game'); c.width=320; c.height=240;
const ctx=c.getContext('2d');
globalThis.r = [];
fetch('does-not-exist.json')
  .then(res => r.push('then:' + res.ok + ':' + (typeof res.text)))
  .catch(e => r.push('threw'));
let n=0;
function loop(){ if(++n===30 && !globalThis.p){ globalThis.p=1;
  console.log('F404 ' + JSON.stringify(r)); }
  ctx.fillStyle='#111'; ctx.fillRect(0,0,320,240); requestAnimationFrame(loop); }
requestAnimationFrame(loop);
`);
  const wasc = pack(d, join(tmp, 'f404.wasc'), 'f');
  const { gl } = createWebGL2Context(320, 240);
  const host = new CartHost({});
  const cap = [];
  const real = console.error;
  console.error = (...a) => cap.push(a.join(' '));
  await host.load(readFileSync(wasc), { glBackend: gl, width: 320, height: 240 });
  for (let i = 0; i < 60; i++) host.runFrame([]);
  console.error = real;
  const line = cap.find((l) => l.includes('F404 ')) ?? '';
  // Want then:false:function -- .then resolved AND .text() exists on a miss.
  ok('fetch 404 keeps Response shape', /then:false:function/.test(line),
     line.replace(/^\[cart\] F404 /, '').slice(0, 40) || '(no output)');
}

/* ── 8. Gamepad axis range (cart_main.c js_get_gamepads) ─────────────────────
 * The wire type is int16_t, so full negative deflection is -32768, and
 * -32768/32767.0 = -1.0000305 -- outside the -1..1 the Gamepad API promises.
 * Checks BOTH ends: the clamp must not cost 32767 its exact 1.0. */
{
  const { mkdirSync, writeFileSync } = await import('node:fs');
  const d = join(tmp, 'pad');
  mkdirSync(d, { recursive: true });
  writeFileSync(join(d, 'main.js'), `
const c=document.getElementById('game'); c.width=320; c.height=240;
const ctx=c.getContext('2d');
let lo=0, hi=0, n=0;
function loop(){
  const g = (navigator.getGamepads()||[])[0];
  if (g) for (const v of g.axes) { if (v<lo) lo=v; if (v>hi) hi=v; }
  if (++n===40 && !globalThis.p){ globalThis.p=1;
    console.log('PAD ' + lo.toFixed(7) + ' ' + hi.toFixed(7)); }
  ctx.fillStyle='#111'; ctx.fillRect(0,0,320,240); requestAnimationFrame(loop);
}
requestAnimationFrame(loop);
`);
  const wasc = pack(d, join(tmp, 'pad.wasc'), 'p');
  const { gl } = createWebGL2Context(320, 240);
  const host = new CartHost({});
  const cap = [];
  const real = console.error;
  console.error = (...a) => cap.push(a.join(' '));
  await host.load(readFileSync(wasc), { glBackend: gl, width: 320, height: 240 });
  // Extremes on the same frame: min on the left stick, max on the right.
  const pad = { connected: true, buttons: 0,
                leftX: -32768, leftY: -32768, rightX: 32767, rightY: 32767 };
  for (let i = 0; i < 50; i++) host.runFrame([pad]);
  console.error = real;
  const line = (cap.find((l) => l.includes('PAD ')) ?? '').replace(/^.*PAD /, '');
  const [lo, hi] = line.split(' ').map(Number);
  ok('gamepad axes stay in -1..1', lo === -1 && hi === 1, `lo=${lo} hi=${hi}`);
}

/* ── 9+10. getImageData readback, and putImageData bounds ────────────────────
 * getImageData was never registered natively, so the JS shim's try/catch
 * returned a zero-filled buffer of the right SHAPE every time -- every read in
 * every game silently came back black, indistinguishable from a black canvas.
 * With readback fixed, the second bug became testable: putImageData trusted
 * the JS-supplied width/height over the real buffer length, so a 1-pixel
 * buffer claiming 64x64 made Skia read ~16KB of adjacent cart heap and paint
 * it on screen (61 distinct colours out of one pixel). */
{
  const { mkdirSync, writeFileSync } = await import('node:fs');
  const d = join(tmp, 'pixels');
  mkdirSync(d, { recursive: true });
  writeFileSync(join(d, 'main.js'), `
const c=document.getElementById('game'); c.width=64; c.height=64;
const ctx=c.getContext('2d');
function colours(){
  const d=ctx.getImageData(0,0,64,64).data, s=new Set();
  for(let i=0;i<d.length;i+=4) s.add((d[i]<<16)|(d[i+1]<<8)|d[i+2]);
  return {n:s.size, px:[d[0],d[1],d[2]].join(',')};
}
let n=0;
function loop(){
  if(++n===20 && !globalThis.p){ globalThis.p=1;
    ctx.fillStyle='#ff0000'; ctx.fillRect(0,0,64,64);
    ctx.fillStyle='#00ff00'; ctx.fillRect(0,0,8,8);
    const rb = ctx.getImageData(0,0,64,64).data;
    const mid=(32*64+32)*4;
    console.log('RB ' + [rb[0],rb[1],rb[2]].join(',') + ' ' + [rb[mid],rb[mid+1],rb[mid+2]].join(','));
    // Honest data must still land.
    ctx.fillStyle='#000000'; ctx.fillRect(0,0,64,64);
    const real=new Uint8ClampedArray(64*64*4);
    for(let i=0;i<real.length;i+=4){real[i+2]=255;real[i+3]=255;}
    ctx.putImageData({width:64,height:64,data:real},0,0);
    const ctl=colours();
    // Lying data must leak nothing.
    ctx.fillStyle='#000000'; ctx.fillRect(0,0,64,64);
    const tiny=new Uint8ClampedArray(4); tiny[0]=255; tiny[3]=255;
    try { ctx.putImageData({width:64,height:64,data:tiny},0,0); } catch(e){}
    const lie=colours();
    console.log('PX ctl=' + ctl.n + ':' + ctl.px + ' lie=' + lie.n);
    // drawImage carried the SAME trust bug: imgW/imgH from JS, buffer length
    // never checked, so a short buffer painted adjacent heap (538 colours).
    ctx.fillStyle='#000000'; ctx.fillRect(0,0,64,64);
    const dreal=new Uint8Array(64*64*4);
    for(let i=0;i<dreal.length;i+=4){dreal[i+2]=255;dreal[i+3]=255;}
    _wcC2D.drawImage(dreal,64,64,0,0,64,64,0,0,64,64);
    const dctl=colours().n;
    ctx.fillStyle='#000000'; ctx.fillRect(0,0,64,64);
    const dtiny=new Uint8Array(4); dtiny[0]=255; dtiny[3]=255;
    let dlie;
    try { _wcC2D.drawImage(dtiny,64,64,0,0,64,64,0,0,64,64); dlie=colours().n; }
    catch(e){ dlie='threw'; }
    console.log('DI ctl=' + dctl + ' lie=' + dlie);
  }
  requestAnimationFrame(loop);
}
requestAnimationFrame(loop);
`);
  const wasc = pack(d, join(tmp, 'pixels.wasc'), 'px');
  const { gl } = createWebGL2Context(64, 64);
  const host = new CartHost({});
  const cap = [];
  const real = console.error;
  console.error = (...a) => cap.push(a.join(' '));
  await host.load(readFileSync(wasc), { glBackend: gl, width: 64, height: 64 });
  for (let i = 0; i < 30; i++) host.runFrame([]);
  console.error = real;
  const rb = (cap.find((l) => l.includes('RB ')) ?? '').replace(/^.*RB /, '').trim();
  // Green square at the origin, red at the centre -- proves a REAL read, not zeros.
  ok('getImageData reads back real pixels', rb === '0,255,0 255,0,0', rb || '(none)');

  const px = (cap.find((l) => l.includes('PX ')) ?? '').replace(/^.*PX /, '').trim();
  const m = /ctl=(\d+):([\d,]+) lie=(\d+)/.exec(px);
  // ctl: honest put lands as flat blue. lie: short buffer paints NOTHING.
  ok('putImageData rejects short buffers',
     !!m && m[1] === '1' && m[2] === '0,0,255' && m[3] === '1', px || '(none)');

  const di = (cap.find((l) => l.includes('DI ')) ?? '').replace(/^.*DI /, '').trim();
  const dm = /ctl=(\d+) lie=(.+)/.exec(di);
  // Honest draw lands flat (1 colour); short buffer must THROW, not paint heap.
  ok('drawImage rejects short buffers',
     !!dm && dm[1] === '1' && dm[2] === 'threw', di || '(none)');
}

/* ── 13. GL pixel paths bound to the real buffer (webgl_shim.c) ──────────────
 * texImage2D/texSubImage2D/readPixels each fetched the buffer length and then
 * DISCARDED it, trusting JS-supplied width/height. readPixels is the severe
 * one -- GL WRITES w*h*bpp bytes, so 64x64 RGBA into a 16-byte Uint8Array was
 * a heap overflow that killed the whole runtime, reachable from cart JS. */
{
  const { mkdirSync, writeFileSync } = await import('node:fs');
  const d = join(tmp, 'glbounds');
  mkdirSync(d, { recursive: true });
  writeFileSync(join(d, 'main.js'), `
const c=document.getElementById('game'); c.width=64; c.height=64;
const gl=c.getContext('webgl2');
/* uniformMatrix4fv used "if (count < 1) count = 1", forcing GL to read a FULL
 * matrix from a shorter buffer -- up to 60 bytes of adjacent heap uploaded as
 * a transform. Seed m[0][0]=7, do a short upload, then check it survived.
 * The shader paints green when the seed is intact, red when it was clobbered. */
function sh(t,src){const s=gl.createShader(t);gl.shaderSource(s,src);gl.compileShader(s);return s;}
const _vs=sh(gl.VERTEX_SHADER,'#version 300 es\\nuniform highp mat4 m;\\nvoid main(){gl_Position=vec4(m[0][0]*0.0,0,0,1);gl_PointSize=64.0;}');
const _fs=sh(gl.FRAGMENT_SHADER,'#version 300 es\\nprecision highp float;\\nuniform highp mat4 m;\\nout vec4 o;\\nvoid main(){o=(m[0][0]==7.0)?vec4(0,1,0,1):vec4(1,0,0,1);}');
const _p=gl.createProgram();gl.attachShader(_p,_vs);gl.attachShader(_p,_fs);gl.linkProgram(_p);
const _loc=gl.getUniformLocation(_p,'m');
const _known=new Float32Array(16); _known[0]=7;
let n=0;
function loop(){
  gl.clearColor(1,0,0,1); gl.clear(gl.COLOR_BUFFER_BIT);
  if(++n===20 && !globalThis.p){ globalThis.p=1;
    const R=[];
    const tex=gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D,tex);
    const ok=new Uint8Array(8*8*4);
    try { gl.readPixels(0,0,8,8,gl.RGBA,gl.UNSIGNED_BYTE,ok);
          R.push('rp_ctl:'+(ok[0]===255&&ok[1]===0?'red':'wrong')); }
    catch(e){ R.push('rp_ctl:THREW'); }
    try { gl.readPixels(0,0,64,64,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(16));
          R.push('rp_lie:ok'); } catch(e){ R.push('rp_lie:threw'); }
    try { gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,8,8,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(8*8*4));
          R.push('ti_ctl:ok'); } catch(e){ R.push('ti_ctl:THREW'); }
    try { gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,64,64,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(16));
          R.push('ti_lie:ok'); } catch(e){ R.push('ti_lie:threw'); }
    console.log('GLB ' + R.join(' '));
    /* Matrix probe: blue would mean nothing drew, so a broken probe cannot pass. */
    gl.useProgram(_p);
    gl.uniformMatrix4fv(_loc,false,_known);
    gl.clearColor(0,0,1,1); gl.clear(gl.COLOR_BUFFER_BIT);
    gl.uniformMatrix4fv(_loc,false,new Float32Array(1));   // short upload
    gl.drawArrays(gl.POINTS,0,1);
    const _px=new Uint8Array(4);
    gl.readPixels(32,32,1,1,gl.RGBA,gl.UNSIGNED_BYTE,_px);
    console.log('UM ' + [_px[0],_px[1],_px[2]].join(','));
  }
  requestAnimationFrame(loop);
}
requestAnimationFrame(loop);
`);
  const wasc = pack(d, join(tmp, 'glbounds.wasc'), 'gb');
  const { gl } = createWebGL2Context(64, 64);
  const host = new CartHost({});
  const cap = [];
  const real = console.error;
  console.error = (...a) => cap.push(a.join(' '));
  let survived = true;
  try {
    await host.load(readFileSync(wasc), { glBackend: gl, width: 64, height: 64 });
    for (let i = 0; i < 30; i++) host.runFrame([]);
  } catch (e) { survived = false; }
  console.error = real;
  const g = (cap.find((l) => l.includes('GLB ')) ?? '').replace(/^.*GLB /, '').trim();
  // Honest calls work; both lying calls throw; and the runtime is still alive.
  ok('GL pixel calls bound to buffer length',
     survived && g === 'rp_ctl:red rp_lie:threw ti_ctl:ok ti_lie:threw',
     survived ? (g || '(none)') : 'runtime died (heap overflow)');

  const um = (cap.find((l) => l.includes('UM ')) ?? '').replace(/^.*UM /, '').trim();
  const umLabel = um === '0,0,255' ? 'nothing drawn (probe broken)'
                : um === '0,255,0' ? 'seeded matrix preserved' : `clobbered (${um})`;
  ok('uniformMatrix4fv ignores short buffers', um === '0,255,0', umLabel);
}

/* ── 15. Workers: linked at all, and messages survive JSON round-trip ────────
 * Two bugs stacked here. worker_shim.o was compiled by build.sh but left out
 * of OBJS, so the whole implementation never linked and `new Worker(...)`
 * threw "_wcWorkerCreate is not defined" -- hidden because
 * ERROR_ON_UNDEFINED_SYMBOLS=0 is needed for the Skia/GL stubs.
 *
 * Underneath that: msg_queue_pop copied len bytes into a REUSED stack buffer
 * without terminating it, and QuickJS's JSON tokenizer reads past the length
 * it is given when a value ends in a number (js_atof scans for more digits).
 * So a short message inherited the tail of a longer predecessor and valid
 * JSON was rejected with "unexpected data at the end". The reply below ends
 * in a NUMBER on purpose -- with a string-terminated reply the bug is
 * invisible, which is exactly why it survived this long. */
{
  const { mkdirSync, writeFileSync } = await import('node:fs');
  const d = join(tmp, 'worker');
  mkdirSync(d, { recursive: true });
  writeFileSync(join(d, 'worker.js'), `
postMessage({phase:'load'});
onmessage = function(e){ postMessage({phase:'recv', frame:(e&&e.data)?e.data.frame:-1}); };
`);
  writeFileSync(join(d, 'main.js'), `
const c=document.getElementById('game'); c.width=64; c.height=64;
const ctx=c.getContext('2d');
const R=[];
let ctorErr='';
let w=null;
try { w=new Worker('worker.js'); } catch(e){ ctorErr=e.message; }
if (w) w.onmessage=(e)=>{ try { R.push(e.data.phase + ':' + (e.data.frame ?? '')); }
                          catch(err){ R.push('cbthrew'); } };
let n=0;
function loop(){
  ctx.fillStyle='#111'; ctx.fillRect(0,0,64,64);
  if(n===3 && w) w.postMessage({payload:'x'.repeat(8), frame:3});
  if(n===10 && w) w.postMessage({payload:'y'.repeat(64), frame:10});
  if(++n===35 && !globalThis.p){ globalThis.p=1;
    console.log('WRK ' + (ctorErr ? 'ctor:'+ctorErr : R.join(','))); }
  requestAnimationFrame(loop);
}
requestAnimationFrame(loop);
`);
  const wasc = pack(d, join(tmp, 'worker.wasc'), 'wk');
  const { gl } = createWebGL2Context(64, 64);
  const host = new CartHost({});
  const cap = [];
  const real = console.error;
  console.error = (...a) => cap.push(a.join(' '));
  await host.load(readFileSync(wasc), { glBackend: gl, width: 64, height: 64 });
  for (let i = 0; i < 40; i++) host.runFrame([]);
  console.error = real;
  const wk = (cap.find((l) => l.includes('WRK ')) ?? '').replace(/^.*WRK /, '').trim();
  // load fires at worker startup; both posts must round-trip with their frame number.
  ok('workers link and round-trip messages',
     wk === 'load:,recv:3,recv:10', wk || '(no output)');
}

/* ── 16. arcTo exposed, and roundRect draws real geometry ───────────────────
 * arcTo was registered natively in canvas2d_skia.c but never exposed on the JS
 * context, so ctx.arcTo was undefined for every cart -- and roundRect (which
 * needs it) did not exist at all. Samples three pixels: a filled centre proves
 * it drew, a BLACK corner proves the corners were actually rounded rather than
 * a plain rect, and a filled top edge proves it is not just a small box. */
{
  const { mkdirSync, writeFileSync } = await import('node:fs');
  const d = join(tmp, 'roundrect');
  mkdirSync(d, { recursive: true });
  writeFileSync(join(d, 'main.js'), `
const c=document.getElementById('game'); c.width=64; c.height=64;
const ctx=c.getContext('2d');
let n=0;
function loop(){
  ctx.fillStyle='#000000'; ctx.fillRect(0,0,64,64);
  if(++n===20 && !globalThis.p){ globalThis.p=1;
    let out;
    try {
      ctx.fillStyle='#ff0000';
      ctx.beginPath(); ctx.roundRect(8,8,48,48,16); ctx.fill();
      const d=ctx.getImageData(0,0,64,64).data;
      const at=(x,y)=>{const i=(y*64+x)*4; return d[i]+','+d[i+1]+','+d[i+2];};
      out = at(32,32)+' '+at(9,9)+' '+at(32,9);
    } catch(e){ out = 'THREW:'+e.message; }
    console.log('RR ' + (typeof ctx.arcTo) + ' ' + out);
  }
  requestAnimationFrame(loop);
}
requestAnimationFrame(loop);
`);
  const wasc = pack(d, join(tmp, 'roundrect.wasc'), 'rr');
  const { gl } = createWebGL2Context(64, 64);
  const host = new CartHost({});
  const cap = [];
  const real = console.error;
  console.error = (...a) => cap.push(a.join(' '));
  await host.load(readFileSync(wasc), { glBackend: gl, width: 64, height: 64 });
  for (let i = 0; i < 30; i++) host.runFrame([]);
  console.error = real;
  const rr = (cap.find((l) => l.includes('RR ')) ?? '').replace(/^.*RR /, '').trim();
  ok('arcTo exposed, roundRect rounds corners',
     rr === 'function 255,0,0 0,0,0 255,0,0', rr || '(no output)');
}

/* ── 17. Canvas2D frames are not upside down (KNOWN FAILING) ────────────────
 * Every other check in this file reads pixels back through the same GL path
 * the cart renders into, so a whole-frame vertical flip cancels out and is
 * invisible to all of them -- 16/16 green while every 2D frame ships inverted.
 * This one goes through the SHIPPED player instead and inspects the PNG, which
 * is what a user actually sees.
 *
 * Measured: a cart filling canvas-top green and canvas-bottom red produces a
 * PNG with red on top. Text renders upside down in hello_canvas and mirrored
 * in space. A pure-WebGL control cart through the SAME player is upright, so
 * the host is fine and the fault is in our Canvas2D presentation.
 *
 * Not yet root-caused: neither the GLES blit's src-Y inversion nor Ganesh's
 * kTopLeft/kBottomLeft origin changes the output, so the flip enters
 * somewhere downstream of both. Left FAILING on purpose rather than deleted --
 * a skipped check is a forgotten check. */
{
  const { mkdirSync, writeFileSync } = await import('node:fs');
  const { execFileSync } = await import('node:child_process');
  const d = join(tmp, 'orient');
  mkdirSync(d, { recursive: true });
  writeFileSync(join(d, 'main.js'), `
const c=document.getElementById('game'); c.width=200; c.height=100;
const ctx=c.getContext('2d');
function loop(){
  ctx.fillStyle='#000000'; ctx.fillRect(0,0,200,100);
  ctx.fillStyle='#00ff00'; ctx.fillRect(0,0,200,20);    // canvas TOP
  ctx.fillStyle='#ff0000'; ctx.fillRect(0,80,200,20);   // canvas BOTTOM
  requestAnimationFrame(loop);
}
requestAnimationFrame(loop);
`);
  const wasc = pack(d, join(tmp, 'orient.wasc'), 'or');
  const png = join(tmp, 'orient.png');
  const player = presolve(WASMCART, 'bin/wasmcart-play.js');
  let top = '(not run)';
  if (!existsSync(player)) {
    skipped('Canvas2D frames are upright', 'wasmcart player not found');
  } else {
    try {
      execFileSync('node', [player, wasc, '--frames', '30', '--shot', png],
        { stdio: 'ignore', timeout: 180000 });
      // Decode just enough PNG to sample the top row: zlib-inflate the IDAT
      // and read scanline 2, which sits inside the green band if upright.
      const { inflateSync } = await import('node:zlib');
      const buf = readFileSync(png);
      let idat = [], off = 8;
      while (off < buf.length) {
        const len = buf.readUInt32BE(off);
        const type = buf.toString('ascii', off + 4, off + 8);
        if (type === 'IDAT') idat.push(buf.subarray(off + 8, off + 8 + len));
        off += 12 + len;
      }
      const raw = inflateSync(Buffer.concat(idat));
      const stride = 200 * 3 + 1;           // RGB8 + per-row filter byte
      const row = 2, base = row * stride + 1 + 100 * 3;
      top = `${raw[base]},${raw[base + 1]},${raw[base + 2]}`;
    } catch (e) { top = 'ERR:' + String(e.message).slice(0, 40); }
    ok('Canvas2D frames are upright', top === '0,255,0', `top row = ${top} (want 0,255,0)`);
  }
}

rmSync(tmp, { recursive: true, force: true });
console.log(`\n${pass} passed, ${fail} failed${skip ? `, ${skip} skipped` : ''}`);
process.exit(fail ? 1 : 0);
