/**
 * hello_canvas — Basic Canvas 2D test for wasmcart-jsgame
 *
 * Tests: requestAnimationFrame, console.log, document shim,
 *        Canvas 2D context, gamepad input, performance.now()
 */

const canvas = document.getElementById('game');
canvas.width = 800;
canvas.height = 600;
const ctx = canvas.getContext('2d');

console.log('hello_canvas started');
console.log('canvas size:', canvas.width, 'x', canvas.height);

let ballX = 400;
let ballY = 300;
let ballDX = 4;
let ballDY = 3;
let frameCount = 0;

function gameLoop(timestamp) {
    frameCount++;

    // Check gamepad
    const gamepads = navigator.getGamepads();
    if (gamepads[0] && gamepads[0].connected) {
        const pad = gamepads[0];
        // D-pad
        if (pad.buttons[14] && pad.buttons[14].pressed) ballDX = -Math.abs(ballDX);
        if (pad.buttons[15] && pad.buttons[15].pressed) ballDX = Math.abs(ballDX);
        if (pad.buttons[12] && pad.buttons[12].pressed) ballDY = -Math.abs(ballDY);
        if (pad.buttons[13] && pad.buttons[13].pressed) ballDY = Math.abs(ballDY);

        // Left stick
        if (Math.abs(pad.axes[0]) > 0.3) ballDX = pad.axes[0] * 6;
        if (Math.abs(pad.axes[1]) > 0.3) ballDY = pad.axes[1] * 6;
    }

    // Update
    ballX += ballDX;
    ballY += ballDY;
    if (ballX < 20 || ballX > 780) { ballDX = -ballDX; ballX += ballDX; }
    if (ballY < 20 || ballY > 580) { ballDY = -ballDY; ballY += ballDY; }

    // Draw
    ctx.clearRect(0, 0, 800, 600);

    // Background
    ctx.fillStyle = '#1a1a2e';
    ctx.fillRect(0, 0, 800, 600);

    // Title bar
    ctx.fillStyle = '#e94560';
    ctx.fillRect(0, 0, 800, 40);

    // Title text
    ctx.fillStyle = '#ffffff';
    ctx.font = '20px monospace';
    ctx.fillText('jsgame on wasmcart', 300, 28);

    // Bouncing ball
    ctx.fillStyle = '#0f3460';
    ctx.beginPath();
    ctx.arc(ballX, ballY, 20, 0, Math.PI * 2);
    ctx.fill();

    // Border lines
    ctx.strokeStyle = '#16213e';
    ctx.lineWidth = 2;
    ctx.strokeRect(5, 45, 790, 550);

    // Frame counter
    ctx.fillStyle = '#533483';
    ctx.font = '14px monospace';
    ctx.fillText('frame: ' + frameCount + '  time: ' + Math.round(timestamp) + 'ms', 20, 590);

    requestAnimationFrame(gameLoop);
}

requestAnimationFrame(gameLoop);
