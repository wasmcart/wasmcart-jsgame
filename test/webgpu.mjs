// The WebGPU runtime end to end: examples/threejs_webgpu (three.js
// WebGPURenderer, a lit orange cube turning on a 0x203040 background) loaded
// in a WebGPU-capable wasmcart host, read back with readGpuFrame.
//
//   node test/webgpu.mjs
//
// Env: WASMCART_REPO, a wasmcart checkout whose host runs WebGPU carts
// (default: sibling ../wasmcart). Rebuild the cart first if the runtime
// changed:
//   bash build_webgpu.sh
//   CART_WASM=build/cart-webgpu.wasm bash pack_game.sh examples/threejs_webgpu \
//     examples/threejs_webgpu/threejs_webgpu.wasc "three.js WebGPU"
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const WASMCART = process.env.WASMCART_REPO ?? resolve(ROOT, '../wasmcart');
const { CartHost } = await import(WASMCART + '/index.js');

let pass = 0, fail = 0;
const ok = (name, cond, detail = '') => {
  console.log(`  ${cond ? 'OK  ' : 'FAIL'} ${name.padEnd(44)} ${detail}`);
  cond ? pass++ : fail++;
};
const tick = () => new Promise(r => setImmediate(r));
const px = (f, x, y) => { const i = (y * f.width + x) * 4; return [...f.data.slice(i, i + 4)]; };
const BG = [32, 48, 64, 255];
const isBg = p => p.every((v, i) => v === BG[i]);

const host = new CartHost();
await host.load(resolve(ROOT, 'examples/threejs_webgpu/threejs_webgpu.wasc'));
try {
  ok('host runs it on WebGPU', host.usesWgpu === true && host.usesGL === false, `usesWgpu=${host.usesWgpu} usesGL=${host.usesGL}`);
  ok('cart declares gpu_api 2', host.getInfo().gpuApi === 2, `gpu_api=${host.getInfo().gpuApi}`);

  const frames = [];
  for (let i = 1; i <= 60; i++) {
    host.runFrame();
    await tick();
    if (i % 20 === 0) frames.push(await host.readGpuFrame());
  }
  ok('frames read back', frames.every(Boolean), frames.map(f => f && `${f.width}x${f.height}`).join(' '));
  for (const [n, f] of frames.entries()) {
    const corner = px(f, 5, 5), center = px(f, f.width >> 1, f.height >> 1);
    let cube = 0;
    for (let p = 0; p < f.data.length; p += 4) if (!isBg([f.data[p], f.data[p + 1], f.data[p + 2], f.data[p + 3]])) cube++;
    ok(`frame ${(n + 1) * 20}: clear colour in the corner`, isBg(corner), corner.join());
    ok(`frame ${(n + 1) * 20}: lit orange cube in the middle`, center[0] > center[1] && center[1] > center[2] && center[0] > 100, center.join());
    ok(`frame ${(n + 1) * 20}: cube covers 10-25% of the frame`, cube > f.width * f.height * 0.10 && cube < f.width * f.height * 0.25, `${cube} px`);
  }
  // Shading: at frame 20 three faces face the camera, lit differently.
  const f20 = frames[0];
  const faces = new Set([px(f20, 400, 200), px(f20, 330, 350), px(f20, 490, 330)].map(p => p.join()));
  ok('frame 20: three faces, three shades', faces.size === 3, [...faces].join(' | '));
  const same = (a, b) => a.data.every((v, i) => v === b.data[i]);
  ok('the cube moves (frames differ)', !same(frames[0], frames[1]) && !same(frames[1], frames[2]));
} finally {
  host.destroy();
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
