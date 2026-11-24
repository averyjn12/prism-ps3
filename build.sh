#!/bin/bash
# Build a gxemu stage2 tree with the ps3dev ppu-gcc (WSL distro PSBBN, ~/ps3dev).
# usage: build.sh <tree-dir> <LR_GATE 0|1|3> <out-name>
# Output: <tree>/../out/<out-name>.{bin,elf,map}
set -e
export PS3DEV="$HOME/ps3dev/ps3dev"
export PATH="$PS3DEV/ppu/bin:/usr/bin:/bin"
TREE="$1"
GATE="${2:-0}"
NAME="${3:-stage2}"
OUT="$TREE/../out"
mkdir -p "$OUT"
cd "$TREE/ps2emu_stage2/gxemu"
rm -f *.o *.elf *.bin *.map ../../ps2emu/src/*.o ../../lv1/src/*.o ../../debug/src/*.o ../prism/*.o
make LR_GATE="$GATE" ps2gxemu_stage2.bin 2>&1 | grep -vE '^ppu-gcc |RWX permissions|type changed to PROGBITS' || true
[ -f ps2gxemu_stage2.bin ] || { echo "BUILD FAILED"; exit 1; }
cp ps2gxemu_stage2.bin "$OUT/$NAME.bin"
cp ps2gxemu_stage2.elf "$OUT/$NAME.elf"
cp ps2gxemu_stage2.map "$OUT/$NAME.map"
ls -l "$OUT/$NAME.bin"
