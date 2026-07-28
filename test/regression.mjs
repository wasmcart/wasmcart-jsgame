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

rmSync(tmp, { recursive: true, force: true });
console.log(`\n${pass} passed, ${fail} failed${skip ? `, ${skip} skipped` : ''}`);
process.exit(fail ? 1 : 0);
