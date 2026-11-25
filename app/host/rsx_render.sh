#!/bin/bash
# v0.67: build host/render.c FOR THE PS3 with the real src/gfx.c, so the RSX draws the same scenes the
# PC renderer draws (render.sh) and each is read back into a PPM (render.c RENDER_PS3).
#   bash host/rsx_render.sh            -> ~/prism_app_host/rsx_render/rsx_render.self (+ .elf)
# Run it in RPCS3 (tools/rsx_bench.ps1) or on a console; then host/rsx_compare.py judges the two sets.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/../src"
export PS3DEV="$HOME/ps3dev/ps3dev"
export PSL1GHT="$PS3DEV"
export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:/usr/bin:/bin"
B="$HOME/prism_app_host/rsx_render"
rm -rf "$B"; mkdir -p "$B"
CGLIB="$HOME/cg/usr/lib64"
CFLAGS=(-O2 -Wall -Wno-unused-function -Wno-unused-variable -Wno-format-truncation -mcpu=cell -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections
 -DUI_RSX -DRENDER_PS3 "-I$PSL1GHT/ppu/include" "-I$S")
for f in "$HERE/render.c" "$S/gfx.c" "$S/ui.c" "$S/screens.c" "$S/mcard.c" "$S/art.c" "$S/vmc.c" "$S/vmcw.c" "$S/settings.c" "$S/help.c" "$S/icons.c" \
         "$S/intro.c" "$S/titles.c" "$S/played.c" "$HERE/stub_v15.c" "$HERE/stub_v61.c" "$HERE/stub_v65.c" "$HERE/stub_v67.c"; do
    ppu-gcc "${CFLAGS[@]}" -c "$f" -o "$B/$(basename "${f%.c}").o"
done
LD_LIBRARY_PATH="$CGLIB" cgcomp -v "$S/shaders/prism_v.vcg" "$B/prism_v.vpo" > /dev/null
LD_LIBRARY_PATH="$CGLIB" cgcomp -f "$S/shaders/prism_f.fcg" "$B/prism_f.fpo" > /dev/null
(cd "$B" && bin2s -a 64 prism_v.vpo | ppu-as -o prism_v_vpo.o && bin2s -a 64 prism_f.fpo | ppu-as -o prism_f_fpo.o)
ppu-gcc -mhard-float -Wl,--gc-sections "$B"/*.o -L"$PSL1GHT/ppu/lib" -lrsx -lgcm_sys -lio -lsysutil -lsysmodule -lsysfs -lrt -llv2 -lm -o "$B/rsx_render.elf"
ppu-strip "$B/rsx_render.elf" -o "$B/rsx_render_s.elf"
sprxlinker "$B/rsx_render_s.elf"
fself "$B/rsx_render_s.elf" "$B/rsx_render.self" > /dev/null
ls -l "$B/rsx_render.elf" "$B/rsx_render.self"
