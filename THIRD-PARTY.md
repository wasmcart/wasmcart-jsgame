# Third-party content

This repository's own work is MIT, see [LICENSE](LICENSE): the runtime glue in
`src/`, the build and pack scripts, and the examples.

A built `cart.wasm` statically links several upstream projects. None of them
are vendored here, they are fetched at build time as sibling checkouts, but
they are inside every cart this repo produces, so their terms travel with it.
All are permissive and compatible with MIT.

| Project | Licence | Role |
|---|---|---|
| [QuickJS](https://bellard.org/quickjs/) | MIT | the JavaScript engine |
| [Skia](https://skia.org) | BSD-3-Clause, &copy; Google | Canvas 2D via the Ganesh GL backend |
| [webaudio-node](https://github.com/monteslu/webaudio-node) | ISC | Web Audio graph and audio decoders |

BSD-3-Clause carries a binary-redistribution notice requirement, so a
`.wasc` built from this repo should ship this file, or an equivalent notice,
alongside it.

## Example games

The `hello_*` and `threejs` examples are original to this repository and MIT.
`threejs` loads [Three.js](https://threejs.org) (MIT) as a game asset.

`space`, `space3d` and `adventure-ai` are built from game sources outside this
repository and keep whatever terms those carry. They are not attached to
releases here for that reason.
