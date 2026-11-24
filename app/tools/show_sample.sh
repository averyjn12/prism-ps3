#!/bin/bash
# Show the PSL1GHT sample layout and one sample Makefile (read-only).
S="$HOME/ps3dev/src/PSL1GHT/samples"
ls "$S"; ls "$S"/* | head -60
echo "=== ppu_rules head"; sed -n 1,80p "$HOME/ps3dev/ps3dev/ppu_rules" 2>/dev/null || ls "$HOME/ps3dev/ps3dev"
M=$(ls "$S"/*/*/Makefile 2>/dev/null | grep -i -m1 "graphics\|video\|sys" )
echo "=== $M"; cat "$M"
grep -rn "lv2syscall7\|define lv2syscall" "$HOME/ps3dev/ps3dev/ppu/include/ppu-lv2.h" | head -5
ls "$HOME/ps3dev/ps3dev/ppu/include/sys" | head -60
