#!/usr/bin/env bash
# Native SDL2 build of XEvil against the xshim fake-Xlib: the fast correctness
# loop before emscripten. Output: out-native/xevil-sdl
set -euo pipefail
# -w: the vendored upstream XEvil tree (xevil-src) is warning-heavy on modern toolchains;
# the port's own files (xshim.cpp, xevil_main.cpp) build clean under -Wall -Wextra.

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../xevil-src
OUT=$HERE/out-native
export TMPDIR=${TMPDIR:-$HERE/.tmp}
mkdir -p "$OUT/obj" "$TMPDIR"
CXX=${CXX:-g++}
FLAGS=(-O2 -g -std=gnu++14 -fpermissive -w
       -DX11=1 -DUSE_RANDOM -DXEVIL_KEYSET=UIlinux -DUSE_UINT_NET_LENGTH -DNO_PRAGMAS
       -I"$HERE/xinc" -I"$HERE" -I"$SRC/cmn" -I"$SRC/x11" -I"$SRC/x11/bitmaps"
       $(sdl2-config --cflags))
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
  (( stale )) && { "$CXX" "${FLAGS[@]}" -c "$s" -o "$o" 2>"$o.log" & pids+=($!); }
done
fail=0; for p in "${pids[@]}"; do wait "$p" || fail=1; done
if (( fail )); then
  for l in "$OUT"/obj/*.log; do [[ -s $l ]] && { echo "== $l"; head -30 "$l"; }; done
  exit 1
fi
"$CXX" -o "$OUT/xevil-sdl" "${objs[@]}" $(sdl2-config --libs) -lm
echo "built $OUT/xevil-sdl"
