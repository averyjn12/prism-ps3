#!/bin/bash
# v0.67: rsx_host.sh with this PC's usual inputs: the console's art file and two of the backed-up memory cards.
#   bash host/rsx_host_run.sh [out dir]        (extra FAKE_RSX_* settings pass through)
A="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$HOME/prism_app_host/rsx_out}"
bash "$A/app/host/rsx_host.sh" "$A/app/host/prism_art_console_1124.dat" "$OUT" "$A/launcher/mc_backup/2026-11-22/OPL.VM2" "$A/launcher/mc_backup/2026-11-22/Internal Memory Card ps2.VM2"
