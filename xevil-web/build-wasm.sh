#!/usr/bin/env bash
# Emscripten build of XEvil + xshim → out-wasm/xevil.js + xevil.wasm.
# Same sources/defines as build-native-sdl.sh; SDL2 comes from emscripten's
# port (-sUSE_SDL=2). The wasm stays a separate file (compiled-in sprites make
# it large; base64 SINGLE_FILE would add a third).
set -euo pipefail
# -w: the vendored upstream XEvil tree (xevil-src) is warning-heavy on modern toolchains;
# the port's own files (xshim.cpp, xevil_main.cpp) build clean under -Wall -Wextra.

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../xevil-src
OUT=${1:-$HERE/out-wasm}
export TMPDIR=${TMPDIR:-$HERE/.tmp}
mkdir -p "$OUT/obj" "$TMPDIR"
CFLAGS=(-O2 -std=gnu++14 -w -sUSE_SDL=2
        -DX11=1 -DUSE_RANDOM -DXEVIL_KEYSET=UIlinux -DUSE_UINT_NET_LENGTH -DNO_PRAGMAS
        -I"$HERE/xinc" -I"$HERE" -I"$SRC/cmn" -I"$SRC/x11" -I"$SRC/x11/bitmaps")
"$HERE/patch-sources.sh" "$OUT/patched"
SRCS=("$SRC"/cmn/{actual,area,coord,game,game_style,id,intel,l_agreement,locator,physical,sound_cmn,streams,ui_cmn,utils,world,xetp_basic,xetp}.cpp
      "$OUT/patched/role.cpp"
      "$SRC"/x11/{xdata,panel,l_agreement_dlg,viewport,draw}.cpp "$OUT/patched/ui.cpp"
      "$HERE"/xshim.cpp "$HERE"/xevil_main.cpp)
pids=(); objs=()
for s in "${SRCS[@]}"; do
  o=$OUT/obj/$(basename "${s%.cpp}").o; objs+=("$o")
  dep=$s; [[ $s == "$HERE"/* ]] && dep="$s $HERE/xshim.h $HERE/xfonts.h"
  stale=0; [[ -f $o ]] || stale=1
  for d in $dep; do [[ $d -nt $o ]] && stale=1; done
  (( stale )) && { em++ "${CFLAGS[@]}" -c "$s" -o "$o" 2>"$o.log" & pids+=($!); }
done
fail=0; for p in "${pids[@]}"; do wait "$p" || fail=1; done
if (( fail )); then
  for l in "$OUT"/obj/*.log; do [[ -s $l ]] && { echo "== $l"; head -30 "$l"; }; done
  exit 1
fi
em++ -O2 -sUSE_SDL=2 -sMODULARIZE=1 -sEXPORT_NAME=c7xevil -sENVIRONMENT=web \
     -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sEXIT_RUNTIME=1 \
     -sEXPORTED_FUNCTIONS=_main,_xshim_quit,_xevil_set_paused -sEXPORTED_RUNTIME_METHODS=specialHTMLTargets \
     --js-library "$HERE/nohijack.js" --extern-post-js "$HERE/glue.js" \
     -o "$OUT/xevil.js" "${objs[@]}"
ls -l "$OUT"/xevil.{js,wasm}
