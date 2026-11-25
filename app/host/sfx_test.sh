#!/bin/bash
# v0.72: src/sfx.c on the PC over a pretend audio port (host/sfx_test.c, host/fake_audio/)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$HOME/prism_app_host"
mkdir -p "$B"
gcc -O2 -Wall -I"$HERE/fake_audio" -I"$HERE/../src" "$HERE/sfx_test.c" "${SFX:-$HERE/../src/sfx.c}" -lm -lpthread -o "$B/sfx_test"
"$B/sfx_test"
