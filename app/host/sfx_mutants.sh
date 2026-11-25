#!/bin/bash
# v0.72: planted bugs in a copy of src/sfx.c against host/sfx_test.c. Caught = the test's own FAIL line.
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$HOME/prism_app_host/sfx_mut"
mkdir -p "$B"
bash "$HERE/sfx_test.sh" > "$B/base.log" 2>&1 || { echo "BROKEN: the unmutated test fails"; tail -4 "$B/base.log"; exit 2; }
echo "baseline: $(tail -1 "$B/base.log")"
caught=0; total=0
mut() {
    total=$((total + 1))
    sed "$2" "$HERE/../src/sfx.c" > "$B/sfx.c"
    cmp -s "$B/sfx.c" "$HERE/../src/sfx.c" && { echo "BROKEN: $1: nothing changed"; exit 2; }
    SFX="$B/sfx.c" timeout 120 bash "$HERE/sfx_test.sh" > "$B/run.log" 2>&1; rc=$?
    grep -q "error:" "$B/run.log" && { echo "BROKEN: $1 does not compile"; exit 2; }
    if grep -q "^FAIL:" "$B/run.log"; then caught=$((caught + 1)); echo "caught   $1 ($(grep -m1 "^FAIL:" "$B/run.log" | cut -c1-76))"; else echo "SURVIVED $1 (exit $rc)"; fi
}
mut "a voice never ends" 's/^\t\t\t\tvoice\[k\].pos = p;$/\t\t\t\t;/'
mut "left and right of the music swapped" 's/b\[2 \* i\] += music\[2 \* p\] \* music_gain;/b[2 * i] += music[2 * p + 1] * music_gain;/'
mut "written two blocks farther ahead (later sounds)" 's/^#define AHEAD	3	/#define AHEAD	5	/'
mut "written into the block that is playing" 's/while ((next + nb - cur) % nb <= ahead)/while ((next + nb - cur) % nb < 1)/'
mut "a fade that never fades" 's/music_gain -= 1.0f \/ (RATE \* 0.15f);/music_gain -= 0.0f;/'
mut "blocks not wiped before mixing" 's/^\t\t\tmemset(b, 0, sizeof(float) \* 2 \* AUDIO_BLOCK_SAMPLES);$/\t\t\t;/'
mut "the tick ten times too loud" 's/note(snd\[which\], n, 0.0f, 1318.5f, 0.16f, 0.012f);/note(snd[which], n, 0.0f, 1318.5f, 1.6f, 0.012f);/'
mut "music skips a frame a block" 's/music_pos = (p >= n || music_gain <= 0.0f) ? n : p;/music_pos = (p >= n || music_gain <= 0.0f) ? n : p + 1;/'
echo "sfx_mutants: $caught of $total caught"
[ "$caught" = "$total" ]
