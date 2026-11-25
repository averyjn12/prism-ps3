#!/bin/bash
# v0.67: the stand-in's other settings: vertices snapped to 1/16 pixel (as a real rasteriser does), and a
# 1920 x 1080 and a 720 x 480 TV (their pictures have another size, so they are only looked at).
H="$(cd "$(dirname "$0")" && pwd)"
echo "== snapped to 1/16 pixel"
FAKE_RSX_SUBPIX=16 bash "$H/rsx_host_run.sh" "$HOME/prism_app_host/rsx_out_sub16" 2>/dev/null | tail -1
bash "$H/rsx_host_sum.sh" "$HOME/prism_app_host/rsx_out_sub16" 6
for r in 1080 480; do
    echo "== TV $r"
    FAKE_RSX_RES=$r bash "$H/rsx_host_run.sh" "$HOME/prism_app_host/rsx_out_$r" 2>/dev/null | grep -c "sizes differ"
    tail -2 "$HOME/prism_app_host/rsx_out_$r/rsx.log"
    python3 "$H/ppm2png.py" "$HOME/prism_app_host/rsx_out_$r/rsx" > /dev/null
    D="/path/to/AppData/Local/Temp/claude/d313a4004095d210/cc427217-c2d2-40de-a116-b9bf919e69d5/scratchpad/cmp"
    cp "$HOME/prism_app_host/rsx_out_$r/rsx/games_ps2.png" "$D/res${r}_games_ps2.png"
    cp "$HOME/prism_app_host/rsx_out_$r/rsx/search.png" "$D/res${r}_search.png"
done
