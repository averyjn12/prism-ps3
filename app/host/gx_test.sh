#!/bin/bash
# v0.67: build and run host/gx_test.c (src/gfx.c's texture slots, buffer end and scissor over host/fake_rsx.c)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/../src"
PSL="$HOME/ps3dev/ps3dev"
B="$HOME/prism_app_host/rsx_host"
[ -f "$B/shaders.c" ] || { echo "run host/rsx_host_run.sh first (it compiles the two programs)"; exit 1; }
gcc -O2 -Wall -DUI_RSX -I"$HERE/fake_rsx" -I"$S" -idirafter "$PSL/ppu/include" "$HERE/gx_test.c" "$S/gfx.c" "$HERE/fake_rsx.c" "$B/shaders.c" -lm -o "$B/gx_test"
"$B/gx_test"
