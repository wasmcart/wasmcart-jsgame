/**
 * hello_webgl — WebGL2 triangle test for wasmcart-jsgame
 *
 * Tests: getContext('webgl2'), shader compile/link, draw call,
 *        requestAnimationFrame, uniform updates
 *
 * This is the simplest possible WebGL2 program — a rotating colored triangle.
 * When the WebGL shim is wired to wasmcart GL imports, these calls go
 * directly to the host GPU with near-zero overhead.
 */

const canvas = document.getElementById('game');
canvas.width = 800;
canvas.height = 600;
const gl = canvas.getContext('webgl2');

if (!gl) {
    console.log('WebGL2 not available yet (Phase 1 — needs webgl_shim.c)');
    // Fall back to a rAF loop that just logs
    let frame = 0;
    function fallback(ts) {
        if (frame % 60 === 0) console.log('hello_webgl frame', frame);
        frame++;
        requestAnimationFrame(fallback);
    }
    requestAnimationFrame(fallback);
} else {
    console.log('WebGL2 context acquired');

    const vsSource = `#version 300 es
    in vec2 aPosition;
    in vec3 aColor;
    out vec3 vColor;
    uniform float uAngle;
    void main() {
        float c = cos(uAngle);
        float s = sin(uAngle);
        vec2 p = vec2(
            aPosition.x * c - aPosition.y * s,
            aPosition.x * s + aPosition.y * c
        );
        gl_Position = vec4(p, 0.0, 1.0);
        vColor = aColor;
    }`;

    const fsSource = `#version 300 es
    precision mediump float;
    in vec3 vColor;
    out vec4 fragColor;
    void main() {
        fragColor = vec4(vColor, 1.0);
    }`;

    function createShader(type, source) {
        const shader = gl.createShader(type);
        gl.shaderSource(shader, source);
        gl.compileShader(shader);
        if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
            console.error('Shader error:', gl.getShaderInfoLog(shader));
            gl.deleteShader(shader);
            return null;
        }
        return shader;
    }

    const vs = createShader(gl.VERTEX_SHADER, vsSource);
    const fs = createShader(gl.FRAGMENT_SHADER, fsSource);

    const program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);

    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        console.error('Program link error:', gl.getProgramInfoLog(program));
    }

    // Triangle vertices: position (x,y) + color (r,g,b)
    const vertices = new Float32Array([
        // x,    y,     r,   g,   b
         0.0,  0.6,   1.0, 0.0, 0.0,
        -0.5, -0.4,   0.0, 1.0, 0.0,
         0.5, -0.4,   0.0, 0.0, 1.0,
    ]);

    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);

    const vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);

    const aPos = gl.getAttribLocation(program, 'aPosition');
    gl.enableVertexAttribArray(aPos);
    gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 20, 0);

    const aCol = gl.getAttribLocation(program, 'aColor');
    gl.enableVertexAttribArray(aCol);
    gl.vertexAttribPointer(aCol, 3, gl.FLOAT, false, 20, 8);

    const uAngle = gl.getUniformLocation(program, 'uAngle');

    function render(timestamp) {
        const angle = timestamp * 0.001;

        gl.viewport(0, 0, 800, 600);
        gl.clearColor(0.1, 0.1, 0.15, 1.0);
        gl.clear(gl.COLOR_BUFFER_BIT);

        gl.useProgram(program);
        gl.bindVertexArray(vao);
        gl.uniform1f(uAngle, angle);
        gl.drawArrays(gl.TRIANGLES, 0, 3);

        requestAnimationFrame(render);
    }

    requestAnimationFrame(render);
}
