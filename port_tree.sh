#!/bin/bash
# Make a fresh copy of Cobra 8.5 4.93 CEX SRC buildable AND CORRECT with ps3dev nightly-2026-10-11
# (ppu-gcc 13.2.0 / binutils 2.42). Run from Git Bash:  port_tree.sh <new-tree-dir>
#
# Cobra was written for a much older ppu-gcc. Workarounds 1-4 only make it compile and link.
# Workarounds 5-7 fix code that links cleanly and then crashes: each one was found by executing
# the rebuilt stock stage2 in emu/run_tests.py on 2026-11-19, before any console run.
#
#   1. <memory.h> shim            - newlib dropped the header
#   2. -mno-pltseq                - gcc 13 ICEs on longcall PLT sequences for this target
#   3. -Os -> -O2                 - gcc 13 ICEs on out-of-line register save/restore at -Os
#   4. @ha -> @higha, @h -> @high - binutils >= 2.25 overflow-checks ADDR16_HA/HI; the stage2
#                                   links at 0x0BADCAFE00000008 on purpose (relocation tag)
#   5. ALIGN(8) before __reloc_start - start.S walks the relocation range in 8-byte steps from
#      __reloc_start; gcc 13 leaves .rodata ending on an odd offset (0x27f6 in the first build),
#      so every relocation would have been missed.
#   6. ALIGN(256) before __toc_start - binutils >= 2.25 rounds its own TOC base (.TOC., used by
#      every compiler-generated toc access and stored in every .opd entry) DOWN to a multiple of
#      256, while start.S loads r2 from the script symbol __toc. With .toc at ...2ca0 the two
#      differed by 0xA0 and the first call through the toc jumped into string data.
#   7. volatile members in f_desc_t + -fno-strict-aliasing - crt0 builds a function descriptor
#      in a local struct and calls through a cast of its address. gcc 13 sees no read of the
#      struct and deletes both stores; the call then loads entry 0 / toc 0 from the stack.
#   8. -mno-altivec -mno-vsx -msoft-float - gcc 13 at -O2 auto-vectorises small copies with
#      lvx/stvx. Nothing says MSR[VEC] is on in the context stage2 runs in, and Cobra's own
#      builds never contained a vector or FP instruction.
set -e
SRC="/path/to/PS3 software/src/Cobra-PS3/8.5/4.93/CEX/SRC"
DST="$1"
[ -n "$DST" ] || { echo "usage: port_tree.sh <dir>"; exit 2; }
[ -e "$DST" ] && { echo "$DST exists"; exit 2; }
mkdir -p "$DST"
cp -r "$SRC"/{ps2emu,ps2emu_stage2,lv1,lv2,debug} "$DST"/

# git may have checked the sources out with CRLF; normalise the files we edit
MK="$DST/ps2emu_stage2/gxemu/Makefile"
LDS="$DST/ps2emu_stage2/gxemu/ldscript.ld"
PH="$DST/ps2emu/include/ps2emu/patch.h"
sed -i 's/\r$//' "$MK" "$LDS" "$PH"

printf '/* shim: newlib in ps3dev nightly-2026-10-11 has no <memory.h> */\n#include <string.h>\n' > "$DST/ps2emu/include/memory.h"

sed -i 's/^CFLAGS += --std=gnu99.*$/CFLAGS += --std=gnu99 -mno-pltseq -fno-strict-aliasing -mno-altivec -mno-vsx -msoft-float/; s/-Os/-O2/g' "$MK"
grep -q -- '-mno-pltseq -fno-strict-aliasing -mno-altivec -mno-vsx -msoft-float' "$MK"

grep -rlE '@(ha|h)\b' "$DST/ps2emu" "$DST/ps2emu_stage2/gxemu" "$DST/ps2emu_stage2/common" "$DST/lv1" "$DST/debug" \
  | while read -r f; do sed -i -E 's/@ha\b/@higha/g; s/@h\b/@high/g' "$f"; done

sed -i 's/^\t__reloc_start = \.;$/\t. = ALIGN(8);\n\t__reloc_start = .;/' "$LDS"
grep -q 'ALIGN(8)' "$LDS"

# the ALIGN(32) directly in front of __toc_start becomes ALIGN(256)
sed -i -z 's/\t\. = ALIGN(32);\n\t__toc_start = \.;/\t. = ALIGN(256);\n\t__toc_start = .;/' "$LDS"
grep -q 'ALIGN(256)' "$LDS"

sed -i -E 's/^\tvoid[ \t]+\*addr;[ \t]*$/\tvoid * volatile addr;/; s/^\tvoid[ \t]+\*toc;[ \t]*$/\tvoid * volatile toc;/' "$PH"
[ "$(grep -c 'volatile' "$PH")" = "2" ] || { echo "f_desc_t patch did not apply"; exit 1; }

echo "ported: $DST"
