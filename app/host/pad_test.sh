#!/bin/bash
# v0.72: src/pad.c on the PC (host/pad_test.c). First v0.71's buttons() must FAIL the test (the fault is
# reproduced), then the new one must pass.
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$HOME/prism_app_host"
mkdir -p "$B"
gcc -O2 -Wall -DOLD_BUTTONS -I"$HERE/fake_pad" -I"$HERE/../src" "$HERE/pad_test.c" -o "$B/pad_test_old" || exit 2
"$B/pad_test_old" > "$B/pad_old.log"
grep -q "^FAIL: one Cross press" "$B/pad_old.log" || { echo "BROKEN: v0.71's buttons() passes: the test does not show the fault"; exit 2; }
echo "v0.71's buttons(): $(grep -m1 '^FAIL' "$B/pad_old.log")"
gcc -O2 -Wall -I"$HERE/fake_pad" -I"$HERE/../src" "$HERE/pad_test.c" "${PADC:-$HERE/../src/pad.c}" -o "$B/pad_test" || exit 2
"$B/pad_test"
