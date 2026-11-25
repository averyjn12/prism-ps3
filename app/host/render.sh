#!/bin/bash
# Build the PC renderer of the app's screens and render them (see render.c).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/../src"
B="$HOME/prism_app_host/render"
mkdir -p "$B"
gcc -O2 -Wall -Wno-unused-function -I"$HERE/stub" -I"$S" "$HERE/render.c" "$S/ui.c" "$S/screens.c" "$S/mcard.c" "$S/art.c" "$S/vmc.c" "$S/vmcw.c" "$S/settings.c" "$S/help.c" "$S/icons.c" "$S/intro.c" "$S/titles.c" "$HERE/stub_v15.c" "$HERE/stub_v61.c" "$HERE/stub_v65.c" -lm -o "$B/render"
"$B/render" "$@"
