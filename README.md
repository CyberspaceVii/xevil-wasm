# XEvil (WebAssembly)

XEvil 2.1 beta in the browser. The original X11 backend is compiled unmodified
against a small in-process Xlib-on-SDL2 shim, then built to WebAssembly with
emscripten. Single player against the game's AI. Runs at https://cyberspace7.org/.

## Layout

- `xevil-src/` upstream XEvil (lvella/xevil), vendored unmodified at commit a016022
- `xevil-web/` the port: the Xlib/SDL2 shim (`xshim.cpp` + `xinc/X11/*.h`), a
  frame-driven main loop (`xevil_main.cpp`), the emscripten glue (`glue.js`),
  and the build scripts

## Build

    git clone https://github.com/CyberspaceVii/xevil-wasm
    cd xevil-wasm/xevil-web
    ./build-wasm.sh          # emscripten, output out-wasm/xevil.{js,wasm}
    ./build-native-sdl.sh    # g++ + SDL2, output out-native/xevil-sdl

The wasm build needs emscripten (`emcc` on your PATH). The native build, a
faster loop for checking rendering, needs SDL2 and a C++14 compiler.

## Running

Serve `out-wasm/` over HTTP (wasm will not load from `file://`) and load a page
that calls `__C7xevil.run(canvas, { wasmUrl })`. See the header of `glue.js` for
the full interface, including `quit()` and `pause()`.

## How it works

XEvil's `x11/` backend uses a thin slice of Xlib (about 40 functions), so rather
than rewrite it, the headers under `xevil-web/xinc/X11/` plus `xshim.cpp` provide
an SDL2-backed Xlib and the existing `x11/*.cpp` compile against them. Each
drawable owns a 32-bit buffer and all rasterising (clip-mask sprites, tiled
backgrounds, stippled panels, the baked bitmap fonts) is done in the shim, so the
picture matches X where the game depends on it. Only `main.cpp`'s blocking event
loop is replaced, with a frame callback driven at a fixed step.

## License

GPL v2 or later; see `LICENSE`. XEvil is Copyright (C) 1994, 2000 Steve Hardt and
Michael Judge. The port files in `xevil-web/` are combined with it to form the
distributed program and are released under the same terms; see
`xevil-web/COPYING-port.txt`.
