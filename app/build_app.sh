#!/bin/bash
# Build the Prism PS3 app (PSL1GHT, WSL distro PSBBN) into out/prism_app.pkg.
#   bash build_app.sh            (from WSL; called from Windows as wsl -d PSBBN -- bash <this file>)
# The card reader is lr/tree/ps2emu_stage2/prism/lr_fs.c compiled UNCHANGED (one source for the
# stage2 and the app). Do not build while emu/mutate5.py is running: it plants bugs in that file.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$HERE/../tree/ps2emu_stage2/prism"
export PS3DEV="$HOME/ps3dev/ps3dev"
export PSL1GHT="$PS3DEV"
export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:/usr/bin:/bin"
TITLE="Prism Launcher"
APPID="PRSM00001"
CONTENTID="UP0001-${APPID}_00-0000000000000000"
B="$HERE/build"
OUT="$HERE/out"
rm -rf "$B"; mkdir -p "$B/pkg/USRDIR" "$OUT"

# an array: the project path holds a space ("PS3 software")
CFLAGS=(-O2 -Wall -mcpu=cell -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections -DPRISM_LR=5 -DPS2GXEMU
 "-I$PSL1GHT/ppu/include" "-I$HERE/src" "-I$HERE/src/stub" "-I$TREE")
for f in "$HERE"/src/*.c "$TREE/lr_fs.c"; do
    ppu-gcc "${CFLAGS[@]}" -c "$f" -o "$B/$(basename "${f%.c}").o"
done
# v0.26: import stub for cellGameExec (sprx/gameexec), built the way PSL1GHT builds its own stubs
GX="$HERE/sprx/gameexec"
ppu-gcc -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections -mregnames -mcpu=cell -D__ASSEMBLY__ \
    -Wa,-mcell -iquote "$GX" "-I$PSL1GHT/ppu/include" -c "$GX/exports.S" -o "$B/gx_exports.o"
ppu-gcc -S -m32 -iquote "$GX" "-I$PSL1GHT/ppu/include" "$GX/libexport.c" -o "$B/gx_libexport.S"
ppu-gcc -c "$B/gx_libexport.S" -o "$B/gx_libexport.o"
ppu-ld -r "$B/gx_exports.o" "$B/gx_libexport.o" -o "$B/gx_sprx.o"
rm -f "$B/gx_exports.o" "$B/gx_libexport.o" "$B/gx_libexport.S"
ppu-gcc -mhard-float -Wl,--gc-sections -Wl,-Map,"$B/prism_app.map" "$B"/*.o -L"$PSL1GHT/ppu/lib" \
    -lrsx -lgcm_sys -lio -lnet -lsysutil -lsysmodule -lsysfs -laudio -lrt -llv2 -lm -o "$B/prism_app.elf"
ppu-strip "$B/prism_app.elf" -o "$B/prism_app_s.elf"
sprxlinker "$B/prism_app_s.elf"
make_self_npdrm "$B/prism_app_s.elf" "$B/pkg/USRDIR/EBOOT.BIN" "$CONTENTID" > /dev/null
cp "$HERE/../launcher/art/prism_launcher_icon.png" "$B/pkg/ICON0.PNG"
# v0.36: the start-up feature's boot disc and autostart.bat are no longer built (removed; boot/ keeps
# their sources). BD still holds the helper tools below.
BD="$B/bootdisc"
mkdir -p "$BD"
# v0.32: the intro's sound, rendered here by the same code (src/intro_synth.c): the console took
# 1.2 s to compute it before the intro could start (intro_sound.c loads this file instead)
gcc -O2 -I"$HERE/src" "$HERE/host/introwav.c" "$HERE/src/intro_synth.c" -lm -o "$BD/introwav"
"$BD/introwav" "$B/pkg/USRDIR/intro.wav" > /dev/null
# v0.15: the files the in-app PS2 setup installs (setup.c). The helper must be the stage2 build the
# scenario suite last passed (lr/out/lr_g5.tested holds its sha256): refuse to package anything else.
mkdir -p "$B/pkg/USRDIR/setup"
WANT="$(cat "$HERE/../out/lr_g5.tested")"
HAVE="$(sha256sum "$HERE/../out/lr_g5.bin" | cut -c1-64)"
if [ "$WANT" != "$HAVE" ]; then echo "lr_g5.bin is not the tested build ($HAVE vs $WANT)"; exit 1; fi
cp "$HERE/../out/lr_g5.bin" "$B/pkg/USRDIR/setup/prism_stage2.bin"
cp "$HERE/setup/carrier.iso" "$HERE/setup/carrier.png" "$HERE/setup/tinytees.iso" "$B/pkg/USRDIR/setup/"
# v0.43: game titles by title ID, for discs and copies without a cover entry (src/titles.c)
python3 "$HERE/tools/mk_titles.py" "$HERE/db" "$B/pkg/USRDIR/titles.txt"
sfo --title "$TITLE" --appid "$APPID" -f "$PS3DEV/bin/sfo.xml" "$B/pkg/PARAM.SFO"
pkg --contentid "$CONTENTID" "$B/pkg/" "$OUT/prism_app.pkg" > /dev/null
ls -l "$B/prism_app.elf" "$B/pkg/USRDIR/EBOOT.BIN" "$OUT/prism_app.pkg"
sha256sum "$OUT/prism_app.pkg" | cut -c1-16
