/**
 * hello_audio — Web Audio API test for wasmcart-jsgame
 *
 * Plays a continuous tone that changes pitch every second.
 */

console.log('hello_audio started');

const canvas = document.getElementById('game');
canvas.width = 800;
canvas.height = 600;
const ctx = canvas.getContext('2d');

let audioCtx = null;
let osc = null;
let gain = null;
let noteIndex = 0;
const notes = [262, 294, 330, 349, 392, 440, 494, 523];
const noteNames = ['C4', 'D4', 'E4', 'F4', 'G4', 'A4', 'B4', 'C5'];
let lastNoteTime = 0;
let frameCount = 0;

function gameLoop(timestamp) {
    frameCount++;

    // Start audio on first frame
    if (!audioCtx) {
        try {
            audioCtx = new AudioContext();
            console.log('AudioContext created, rate:', audioCtx.sampleRate);

            gain = audioCtx.createGain();
            gain.gain.setValueAtTime(0.25, 0);
            gain.connect(audioCtx.destination);

            osc = audioCtx.createOscillator();
            osc.type = 'square';
            osc.frequency.value = notes[0];
            osc.connect(gain);
            osc.start(0);
            console.log('Oscillator started at', notes[0], 'Hz');
        } catch (e) {
            console.error('Audio error:', e);
        }
    }

    // Change note every ~60 frames
    if (osc && frameCount % 60 === 0) {
        noteIndex = (noteIndex + 1) % notes.length;
        // Create new oscillator for new note
        osc.stop(audioCtx.currentTime);
        osc = audioCtx.createOscillator();
        osc.type = 'square';
        osc.frequency.value = notes[noteIndex];
        osc.connect(gain);
        osc.start(audioCtx.currentTime);
    }

    // Draw
    ctx.fillStyle = '#0a0a1a';
    ctx.fillRect(0, 0, 800, 600);

    ctx.fillStyle = '#00ff88';
    ctx.font = '24px monospace';
    ctx.fillText('Web Audio Test', 300, 40);

    ctx.fillStyle = '#ffffff';
    ctx.font = '64px monospace';
    ctx.fillText(noteNames[noteIndex], 350, 320);

    ctx.fillStyle = '#888888';
    ctx.font = '18px monospace';
    ctx.fillText(notes[noteIndex] + ' Hz', 360, 370);

    ctx.fillStyle = '#00ff88';
    ctx.font = '16px monospace';
    ctx.fillText(audioCtx ? 'Audio: Playing' : 'Audio: Not available', 300, 550);

    ctx.fillStyle = '#666666';
    ctx.fillText('frame: ' + frameCount, 20, 590);

    requestAnimationFrame(gameLoop);
}

requestAnimationFrame(gameLoop);
