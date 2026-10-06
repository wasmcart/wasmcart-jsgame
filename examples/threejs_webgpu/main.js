// three.js WebGPURenderer inside a wasmcart cart (the WebGPU jsgame runtime).
//
// A lit, coloured cube turning on a dark blue background. Nothing here is
// cart-specific: it is the code a browser page would run, against the
// navigator.gpu and canvas.getContext('webgpu') the runtime provides.
//
// The cube turns a fixed step per frame (not per millisecond), so a given
// frame number always shows the same picture.
import * as THREE from './three.webgpu.js';

const canvas = document.getElementById('canvas');
const renderer = new THREE.WebGPURenderer({ canvas, antialias: false });
await renderer.init();
console.log(`three r${THREE.REVISION} on ${renderer.backend.isWebGPUBackend ? 'WebGPU' : 'a fallback backend'}`);

renderer.setSize(canvas.width, canvas.height, false);
renderer.setClearColor(0x203040, 1);

const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(50, canvas.width / canvas.height, 0.1, 100);
camera.position.set(0, 0, 4);

// MeshPhongMaterial, not MeshStandardMaterial: the Dawn in webgpu-node (the
// Node host's WebGPU) fails to compile three's standard-material shader
// ("swizzle view instruction still has usages after lowering"); a browser's
// does not. Phong is lit the same way, by the same lights.
const cube = new THREE.Mesh(
  new THREE.BoxGeometry(1.4, 1.4, 1.4),
  new THREE.MeshPhongMaterial({ color: 0xff6020, shininess: 40 }));
scene.add(cube);
scene.add(new THREE.AmbientLight(0xffffff, 0.6));
const sun = new THREE.DirectionalLight(0xffffff, 3);
sun.position.set(2, 3, 4);
scene.add(sun);

let frame = 0;
function tick() {
  frame++;
  cube.rotation.x = frame * 0.03;
  cube.rotation.y = frame * 0.05;
  renderer.render(scene, camera);
  if (frame === 1) console.log('first frame rendered');
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);
