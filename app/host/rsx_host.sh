#!/bin/bash
# v0.67: the RSX path on the PC. Builds host/render.c twice with the SAME scenes:
#   cpu:  ui.c's CPU drawing (what render.sh builds)
#   rsx:  the real src/gfx.c + the gx_ half of ui.c, over host/fake_rsx.c (PSL1GHT's own headers)
# renders both sets and compares them picture by picture (host/rsx_compare.c).
#   bash host/rsx_host.sh <art file> <out dir> [card files...]
#   FAKE_RSX_RES=1080|480 and FAKE_RSX_SUBPIX=16 change the stand-in's TV and its vertex snapping.
# Out: <out dir>/cpu, /rsx, /diff (PPM; python3 host/ppm2png.py <dir> for PNG). Exit code = pictures failed.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/../src"
PSL="$HOME/ps3dev/ps3dev"
B="$HOME/prism_app_host/rsx_host"
ART="$1"; OUT="$2"; shift 2
mkdir -p "$B" "$OUT/cpu" "$OUT/rsx" "$OUT/diff"
SRC=("$HERE/render.c" "$S/ui.c" "$S/screens.c" "$S/mcard.c" "$S/art.c" "$S/vmc.c" "$S/vmcw.c" "$S/settings.c" "$S/help.c" "$S/icons.c" "$S/intro.c" "$S/titles.c" "$S/played.c"
     "$HERE/stub_v15.c" "$HERE/stub_v61.c" "$HERE/stub_v65.c" "$HERE/stub_v67.c")
# -DMC_LIVE_ICONS: the Memory Card screen's icons turn live in the CPU pictures too (the app's CPU path shows
# stills of them, the RSX path turns them), so the two sets differ only by how they were drawn
gcc -O2 -Wall -Wno-unused-function -DMC_LIVE_ICONS -I"$HERE/stub" -I"$S" "${SRC[@]}" -lm -o "$B/render_cpu"
# the two programs as cgcomp makes them for the console, as C arrays (the stand-in reads the attribute
# names out of them, as librsx does on the console)
LD_LIBRARY_PATH="$HOME/cg/usr/lib64" "$PSL/bin/cgcomp" -v "$S/shaders/prism_v.vcg" "$B/prism_v.vpo" > /dev/null
LD_LIBRARY_PATH="$HOME/cg/usr/lib64" "$PSL/bin/cgcomp" -f "$S/shaders/prism_f.fcg" "$B/prism_f.fpo" > /dev/null
python3 "$HERE/shader_check.py" "$B/prism_v.vpo" "$B/prism_f.fpo"
python3 - "$B" <<'PYEOF'
import sys
b = sys.argv[1]
with open(b + "/shaders.c", "w") as o:
    for name, f in (("prism_v_vpo", "prism_v.vpo"), ("prism_f_fpo", "prism_f.fpo")):
        d = open(b + "/" + f, "rb").read()
        o.write("const unsigned char %s[] __attribute__((aligned(64))) = {%s};\n" % (name, ",".join(str(x) for x in d)))
PYEOF
gcc -O2 -Wall -Wno-unused-function -Wno-unused-variable -DUI_RSX -DRENDER_RSX -I"$HERE/fake_rsx" -I"$S" -idirafter "$PSL/ppu/include" \
    "${SRC[@]}" "$S/gfx.c" "$HERE/fake_rsx.c" "$B/shaders.c" -lm -o "$B/render_rsx"
gcc -O2 -Wall "$HERE/rsx_compare.c" -o "$B/rsx_compare"
rm -f "$OUT"/cpu/*.ppm "$OUT"/rsx/*.ppm "$OUT"/diff/*.ppm
"$B/render_cpu" "$ART" "$OUT/cpu" "$@" > "$OUT/cpu.log"
"$B/render_rsx" "$ART" "$OUT/rsx" "$@" > "$OUT/rsx.log"
tail -3 "$OUT/rsx.log"
NAMES=$(cd "$OUT/cpu" && ls *.ppm | sed 's/\.ppm$//')
"$B/rsx_compare" "$OUT/cpu" "$OUT/rsx" "$OUT/diff" $NAMES
