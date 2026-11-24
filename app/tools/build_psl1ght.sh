#!/bin/bash
# Fetch and build PSL1GHT (the open PS3 SDK) against the ppu-gcc 13.2 already in ~/ps3dev.
# Run as the normal WSL user. Sources go to ~/ps3dev/src (outside the project); the SDK
# installs into $PS3DEV (libraries under ppu/, tools under bin/).
set -e
export PS3DEV="$HOME/ps3dev/ps3dev"
export PSL1GHT="$PS3DEV"
export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin:/usr/bin:/bin"
SRC="$HOME/ps3dev/src"
mkdir -p "$SRC"
cd "$SRC"
if [ ! -d PSL1GHT ]; then
    git clone -q https://github.com/ps3dev/PSL1GHT.git
fi
cd PSL1GHT
echo "PSL1GHT at $(git rev-parse --short HEAD) $(git log -1 --format=%cd --date=short)"
make install-ctrl > ../psl1ght_ctrl.log 2>&1
make -j8 > ../psl1ght_make.log 2>&1 || { echo "make FAILED"; tail -30 ../psl1ght_make.log; exit 1; }
make install > ../psl1ght_install.log 2>&1 || { echo "install FAILED"; tail -30 ../psl1ght_install.log; exit 1; }
echo "installed:"
ls "$PS3DEV/bin" | head -30
ls "$PS3DEV/ppu/lib" 2>/dev/null | head -40
