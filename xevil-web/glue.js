/* --extern-post-js for the XEvil wasm build: wraps the MODULARIZE factory
   (c7xevil) as globalThis.__C7xevil = { run(canvas, opts) }.

   run() boots a fresh instance on `canvas` and resolves with the exit code
   when the game quits (its own Quit button, or handle.quit()). opts:
     wasmUrl   where xevil.wasm lives (hex-named asset path, injected by host)
     argv      extra XEvil switches
     log(s)    optional line sink for stdout/stderr
     timeoutLoop  pace with setTimeout instead of rAF (headless tests only)
   Returns { done: Promise<code>, quit(), pause(on) }. quit() delivers
   WM_DELETE so the game shuts down through its normal path (the instance is
   then garbage); pause() halts/resumes the main loop (host minimize). */
(function () {
    "use strict";
    const factory = c7xevil;
    function run(canvas, opts) {
        opts = opts || {};
        let mod = null, resolveDone;
        const done = new Promise(r => { resolveDone = r; });
        let finished = false, quitPending = false, pausePending = null;
        const finish = code => { if (!finished) { finished = true; resolveDone(code | 0); } };
        const log = opts.log || (() => {});
        factory({
            canvas,
            arguments: opts.argv || [],
            c7timeoutLoop: !!opts.timeoutLoop,
            locateFile: (p, dir) => (p.endsWith(".wasm") && opts.wasmUrl) ? opts.wasmUrl : dir + p,
            print: s => log(s),
            printErr: s => log(s),
            // SDL's emscripten port addresses its canvas (sizing + the
            // keyboard-element hint) by the selector "#canvas"; bind that
            // name to the host's element whatever its id is.
            preRun: [m => { m.specialHTMLTargets["#canvas"] = canvas; }],
            onExit: code => finish(code),
            onAbort: w => { log("abort: " + w); finish(134); }
        }).then(m => {
            mod = m;
            // Apply anything the host asked for before init finished (quit wins).
            if (quitPending) mod._xshim_quit();
            else if (pausePending !== null) mod._xevil_set_paused(pausePending ? 1 : 0);
        }).catch(e => { log(String(e)); finish(1); });
        return {
            done,
            quit() { if (finished) return; if (mod) mod._xshim_quit(); else quitPending = true; },
            pause(on) { if (finished) return; if (mod) mod._xevil_set_paused(on ? 1 : 0); else pausePending = on; }
        };
    }
    globalThis.__C7xevil = { run };
})();
