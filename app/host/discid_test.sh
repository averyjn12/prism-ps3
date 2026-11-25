#!/bin/bash
# v0.72: src/discid.c on the PC (host/discid_test.c). Arguments: pairs of a real image and its serial.
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$HOME/prism_app_host"
mkdir -p "$B"
gcc -O2 -Wall -I"$HERE/../src" "$HERE/discid_test.c" "${DISCID:-$HERE/../src/discid.c}" -o "$B/discid_test" || exit 2
"$B/discid_test" "$@"
