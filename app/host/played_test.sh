#!/bin/bash
# v0.67: src/played.c on the PC
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$HOME/prism_app_host"
mkdir -p "$B"
gcc -O2 -Wall -I"$HERE/../src" "$HERE/played_test.c" "$HERE/../src/played.c" -o "$B/played_test"
"$B/played_test" "$B/played_test.txt"
