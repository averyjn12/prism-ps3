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
# v0.57: BearSSL (third_party/build_bearssl.sh) for the Game covers page's HTTPS (src/net.c)
TP="$HERE/../third_party"
[ -f "$TP/bearssl_ppu/libbearssl.a" ] && [ -f "$TP/bearssl_ppu/ta_mozilla.c" ] || { echo "BearSSL is not built: bash third_party/build_bearssl.sh"; exit 1; }
# v0.60: -DLR_APP adds lr_fs.c's lr_fs_run (the open file's sector runs) to the app's copy only
CFLAGS=(-O2 -Wall -mcpu=cell -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections -DPRISM_LR=5 -DPS2GXEMU -DLR_APP
 "-I$PSL1GHT/ppu/include" "-I$HERE/src" "-I$HERE/src/stub" "-I$TREE" "-I$TP/BearSSL/inc" "-I$TP/bearssl_ppu")
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
ppu-gcc -mhard-float -Wl,--gc-sections -Wl,-Map,"$B/prism_app.map" "$B"/*.o "$TP/bearssl_ppu/libbearssl.a" -L"$PSL1GHT/ppu/lib" \
    -lrsx -lgcm_sys -lio -ljpgdec -lpngdec -lnet -lnetctl -lsysutil -lsysmodule -lsysfs -laudio -lrt -llv2 -lm -o "$B/prism_app.elf"
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
# v0.54: the A01 helper (CECHA / CECHB, ps2_emu): only the build the A01 scenario suite passed
WANT_A="$(cat "$HERE/../out/lr_hw5k11.tested")"
HAVE_A="$(sha256sum "$HERE/../out/lr_hw5k11.bin" | cut -c1-64)"
if [ "$WANT_A" != "$HAVE_A" ]; then echo "lr_hw5k11.bin is not the tested build ($HAVE_A vs $WANT_A)"; exit 1; fi
cp "$HERE/../out/lr_hw5k11.bin" "$B/pkg/USRDIR/setup/prism_stage2_a01.bin"
# v0.55: the two webMAN shortcut templates (setup.c wm_shortcuts_add) = bench/lr_title.py build_template
python3 "$HERE/../app_a01/tools/mk_templates.py" "$B/pkg/USRDIR/setup"
for t in tpl_dvd.iso tpl_cd.iso; do
    [ "$(dd if="$B/pkg/USRDIR/setup/$t" bs=1 skip=$((0x7800)) count=8 2>/dev/null)" = "PRISMLR1" ] || { echo "template $t has no marker"; exit 1; }
done
cp "$HERE/setup/carrier.iso" "$HERE/setup/carrier.png" "$HERE/setup/tinytees.iso" "$B/pkg/USRDIR/setup/"
# v0.57: where the Game covers page looks (src/covers.c); no cover images are packaged, ever
cp "$HERE/setup/cover_sources.txt" "$B/pkg/USRDIR/cover_sources.txt"
# v0.59: the one exception: Tiny Tees' own cover, original art (tools/mk_tinytees_cover.py). setup.c names
# its serial (TT_SERIAL): the shipped disc must name the same, or the cover lands on no game
python3 - "$HERE/setup/tinytees.iso" "$HERE/src/setup.c" <<'PYEOF' || exit 1
import re, struct, sys
d = open(sys.argv[1], "rb").read()
pvd = d[16 * 2048:17 * 2048]
lba, ln = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
r, i, cnf = d[lba * 2048:lba * 2048 + ln], 0, ""
while i < len(r) and r[i]:
    if r[i + 33:i + 33 + r[i + 32]].upper().startswith(b"SYSTEM.CNF"):
        a, n = struct.unpack_from("<I", r, i + 2)[0], struct.unpack_from("<I", r, i + 10)[0]
        cnf = d[a * 2048:a * 2048 + n].decode("ascii", "replace")
    i += r[i]
disc = re.search(r"cdrom0:\\([A-Z]{4}_\d{3}\.\d{2})", cnf)
want = re.search(r'#define TT_SERIAL\s+"([^"]+)"', open(sys.argv[2]).read())
if not disc or not want or disc.group(1) != want.group(1):
    print("Tiny Tees' serial: the disc says %s, setup.c TT_SERIAL says %s" % (disc and disc.group(1), want and want.group(1)))
    sys.exit(1)
print("Tiny Tees' serial %s: the disc and setup.c agree" % disc.group(1))
PYEOF
[ "$(stat -c %s "$HERE/setup/tinytees_cover.rgba")" = 65536 ] || { echo "tinytees_cover.rgba is not 128 x 128 x 4"; exit 1; }
cp "$HERE/setup/tinytees_cover.png" "$HERE/setup/tinytees_cover.rgba" "$B/pkg/USRDIR/setup/"
if find "$B/pkg" -iname '*_COV.JPG' -o -ipath '*covers*' -iname '*.jpg' | grep -q .; then echo "a cover image is in the package: refused"; exit 1; fi
# v0.43: game titles by title ID, for discs and copies without a cover entry (src/titles.c)
python3 "$HERE/tools/mk_titles.py" "$HERE/db" "$B/pkg/USRDIR/titles.txt"
sfo --title "$TITLE" --appid "$APPID" -f "$PS3DEV/bin/sfo.xml" "$B/pkg/PARAM.SFO"
pkg --contentid "$CONTENTID" "$B/pkg/" "$OUT/prism_app.pkg" > /dev/null
ls -l "$B/prism_app.elf" "$B/pkg/USRDIR/EBOOT.BIN" "$OUT/prism_app.pkg"
sha256sum "$OUT/prism_app.pkg" | cut -c1-16
