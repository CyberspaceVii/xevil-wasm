#!/usr/bin/env bash
# Copy the few upstream files we patch into $1 (default out-patched/) and apply
# the edits there, so ../xevil-src stays pristine. Each edit is asserted.
#   role.cpp: libc++ + `using namespace std` resolve socket bind() to
#              std::bind; qualify it ::bind.
#   ui.cpp:   browser-friendly default controls for UIlinux: player 1 (the
#              "right" side, the only one in single-player) gets XEvil's own
#              laptop nine-key pad centred on L + a s d z x c, with arrows as
#              alternates, Space as a second fire key, Return for chat;
#              player 2 takes the numeric-keypad set.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../xevil-src
P=${1:-$HERE/out-patched}
mkdir -p "$P"
put() { cmp -s "$1" "$2" 2>/dev/null || mv "$1" "$2"; rm -f "$1"; }

sed -E 's/([^:a-z_])bind\(/\1::bind(/' "$SRC/cmn/role.cpp" > "$P/role.cpp.new"
[[ $(grep -c '::bind(' "$P/role.cpp.new") == 3 ]] || { echo "role.cpp patch failed" >&2; exit 1; }
put "$P/role.cpp.new" "$P/role.cpp"

python3 - "$SRC/x11/ui.cpp" "$P/ui.cpp.new" <<'PY'
import sys
s = open(sys.argv[1]).read()
anchor = "const static UIkeymap *keymaps_preset[][2] = {"
maps = '''// cyberspace7 web build: laptop-friendly defaults (see patch-sources.sh).
const static UIkeymap keys_right_c7 = {
  {XK_l},{XK_semicolon,XK_Right},{XK_slash},{XK_period,XK_Down},
  {XK_m,XK_comma},
  {XK_k,XK_Left},{XK_i},{XK_o,XK_Up},{XK_p,XK_bracketleft},
  {XK_a,XK_space},{XK_s},{XK_d},
  {XK_z},{XK_x},{XK_c},
  {XK_Return},
};

const static UIkeymap keys_left_c7 = {
  {XK_KP_5},{XK_KP_6},{XK_KP_3},{XK_KP_2},{XK_KP_1},
  {XK_KP_4},{XK_KP_7},{XK_KP_8},{XK_KP_9},
  {XK_Insert},{XK_Home},{XK_Prior},
  {XK_Delete},{XK_End},{XK_Next},
  {XK_F3},  // Meaningless
};



'''
old_row = "{&keys_right_iris,&keys_left_all},          // UIlinux"
assert s.count(anchor) == 1 and s.count(old_row) == 1, "ui.cpp anchors moved"
s = s.replace(anchor, maps + anchor).replace(old_row, "{&keys_right_c7,&keys_left_c7},            // UIlinux")
open(sys.argv[2], "w").write(s)
PY
put "$P/ui.cpp.new" "$P/ui.cpp"
