#!/bin/bash
# Build and run host/bench.c (plain, then with gprof); see bench.c.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/../src"
B="$HOME/prism_app_host/bench"
mkdir -p "$B"
SRCS=("$HERE/bench.c" "$S/ui.c" "$S/screens.c" "$S/mcard.c" "$S/art.c" "$S/vmc.c" "$S/vmcw.c" "$S/vmcnew.c" "$S/settings.c" "$S/icons.c" "$S/intro.c" "$HERE/stub_v15.c")
gcc -O2 -Wall -Wno-unused-function -I"$HERE/stub" -I"$S" "${SRCS[@]}" -lm -o "$B/bench"
"$B/bench" "$1" "${2:-600}"
if [ -n "${PROF:-}" ]; then
  gcc -O2 -pg -fno-inline -Wno-unused-function -I"$HERE/stub" -I"$S" "${SRCS[@]}" -lm -o "$B/bench_pg"
  (cd "$B" && ./bench_pg "$1" "${2:-600}" > /dev/null && gprof -b -p ./bench_pg gmon.out | head -25)
fi
