/**
 * hello_fetch — Tests asset loading and ES module imports
 */

import { greet, VERSION } from './helper.js';

console.log(greet('wasmcart'));
console.log('Version:', VERSION);

// Test fetch with JSON asset
async function loadData() {
    try {
        const resp = await fetch('data.json');
        console.log('fetch ok:', resp.ok);
        console.log('fetch status:', resp.status);

        const data = await resp.json();
        console.log('message:', data.message);
        console.log('items:', JSON.stringify(data.items));
        console.log('nested.works:', data.nested.works);
    } catch (e) {
        console.error('fetch error:', e);
    }
}

// Test fetch with text
async function loadSelf() {
    try {
        const resp = await fetch('helper.js');
        const text = await resp.text();
        console.log('helper.js loaded, length:', text.length);
    } catch (e) {
        console.error('load error:', e);
    }
}

// Test 404
async function testNotFound() {
    try {
        const resp = await fetch('nonexistent.txt');
        console.log('404 test ok:', resp.ok);
    } catch (e) {
        console.error('404 error:', e);
    }
}

// Test performance.now
console.log('performance.now():', performance.now());

// Test setTimeout
setTimeout(() => {
    console.log('setTimeout fired!');
}, 100);

// Test gamepad API shape
const pads = navigator.getGamepads();
console.log('gamepads:', pads.length, 'slots');

// Test localStorage
localStorage.setItem('test', 'hello');
console.log('localStorage:', localStorage.getItem('test'));

await loadData();
await loadSelf();
await testNotFound();

let frame = 0;
function loop(ts) {
    frame++;
    if (frame <= 3) {
        console.log('frame', frame, 'at', Math.round(ts), 'ms');
    }
    requestAnimationFrame(loop);
}
requestAnimationFrame(loop);
