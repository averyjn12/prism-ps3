#!/bin/bash
# What the WSL distro PSBBN has for building PS3 (PSL1GHT) homebrew.
P="$HOME/ps3dev/ps3dev"
echo "PS3DEV=$P"
ls "$P"
ls "$P/ppu/bin" | head -8
"$P/ppu/bin/ppu-gcc" --version | head -1
ls "$P/ppu/powerpc64-ps3-elf/include/stdio.h" "$P/ppu/powerpc64-ps3-elf/lib/libc.a" 2>&1
find "$P/ppu" -maxdepth 3 -name "libc.a" 2>/dev/null | head
for t in git make gcc g++ python3 pkg-config autoconf automake libtool bison flex wget curl; do
    printf "%-10s %s\n" "$t" "$(command -v $t || echo MISSING)"
done
dpkg -l 2>/dev/null | awk '/^ii/ {print $2}' | grep -E "^(libelf-dev|zlib1g-dev|libssl-dev|libgmp-dev|libmpfr-dev|libmpc-dev|texinfo|python3-dev)"
