/* --js-library for the XEvil wasm build: the game must never touch the host page's
   tab title (or favicon, nothing in the build does). SDL's emscripten backend maps
   SDL_SetWindowTitle → emscripten_set_window_title → document.title; xshim.cpp already
   avoids both calls, and this override makes the import a no-op so no code path can. */
addToLibrary({
    emscripten_set_window_title: function (title) {}
});
