#!/bin/bash
S="$HOME/ps3dev/src/PSL1GHT/samples"
for d in graphics/videoTest graphics/blitting input/padtest; do
    echo "=== $d"; ls "$S/$d" "$S/$d/source" 2>/dev/null
done
for f in "$S/graphics/videoTest/source/"*.c; do echo "=== $f"; cat "$f"; done
echo "=== padtest"; cat "$S/input/padtest/source/"*.c | head -80
