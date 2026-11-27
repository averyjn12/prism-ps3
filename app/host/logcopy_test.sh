#!/bin/bash
# v0.81: PC test of src/logcopy.c (host/logcopy_test.c). Runs the clean build, then the two known-bad runs, which
# must FAIL on the test's own FAIL lines (exit 1; a crash is BROKEN, never a catch). Exit 0 only if all three behave.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/../src"
B="$HOME/prism_app_host/logcopy"
mkdir -p "$B"
FL=(-O2 -Wall -Wextra -Wno-format-truncation -Wno-stringop-truncation -I"$HERE/stub_sysfs" -I"$HERE/stub" -I"$S")
gcc "${FL[@]}" -Dfopen=fake_fopen -c "$S/logcopy.c" -o "$B/logcopy.o"	# only the app's file sees the fake fopen
gcc "${FL[@]}" -c "$S/dirlist.c" -o "$B/dirlist.o"
gcc "${FL[@]}" "$HERE/logcopy_test.c" "$B/logcopy.o" "$B/dirlist.o" -o "$B/logcopy_test"
export LOGCOPY_ROOT="$B/root"
set +e
"$B/logcopy_test" > "$B/clean.txt"; rc=$?
tail -1 "$B/clean.txt"
[ $rc -eq 0 ] || { grep '^FAIL' "$B/clean.txt"; echo "CLEAN RUN FAILED (exit $rc)"; exit 1; }
for k in 1 2; do
	"$B/logcopy_test" "$k" > "$B/bad$k.txt"; rc=$?
	n=$(grep -c '^FAIL' "$B/bad$k.txt")
	if [ $rc -eq 1 ] && [ "$n" -gt 0 ]; then
		echo "known-bad $k caught: $n FAIL line(s)"
	else
		echo "KNOWN-BAD $k NOT CAUGHT (exit $rc, $n FAIL lines): BROKEN"; exit 1
	fi
done
echo "logcopy test: ok"
