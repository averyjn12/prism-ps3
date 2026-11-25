#!/bin/bash
# v0.67: one line per picture from the last rsx_host.sh comparison in <out dir>, worst first (by pixels over 32)
OUT="${1:-$HOME/prism_app_host/rsx_out}"
B="$HOME/prism_app_host/rsx_host"
NAMES=$(cd "$OUT/cpu" && ls | sed 's/\.[a-z]*$//' | sort -u)
"$B/rsx_compare" "$OUT/cpu" "$OUT/rsx" "$OUT/diff" $NAMES | sort -t: -k3 -n -r | head -${2:-12}
