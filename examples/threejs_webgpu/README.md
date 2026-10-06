# three.js WebGPU

three.js `WebGPURenderer` inside a cart: a lit orange cube (MeshPhongMaterial,
ambient + directional light) turning on a `0x203040` background. Built on the
WebGPU runtime (`build/cart-webgpu.wasm`), so it needs a host that runs WebGPU
carts.

`three.webgpu.js` and `three.core.js` are three.js r184 (npm `three@0.184.0`,
MIT), unmodified.

Rebuild the cart:

```bash
CART_WASM=build/cart-webgpu.wasm bash pack_game.sh examples/threejs_webgpu \
  examples/threejs_webgpu/threejs_webgpu.wasc "three.js WebGPU"
```

Why Phong and not MeshStandardMaterial: the Dawn inside webgpu-node (the Node
host's WebGPU) fails to compile three's standard/physical-material shader
("swizzle view instruction still has usages after lowering"), with or without
the cart in between. The cart reports the same error three reports in Node.
