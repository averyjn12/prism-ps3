# Prism app (PS3 side) - started 2026-11-22

Owner's idea, 2026-11-22 ~16:10 CST: a launcher like IRISMAN, started from the XMB Game column,
running on the PS3 side, showing PS1, PS2 and PS3 games and the memory cards. PS1 / PS3 games start
straight from it; a PS2 game is handed to the PS2 emulator, whose stage2 reads it from the card.
Owner's decisions the same evening: **make the app read exFAT** (do not wipe the card to all-FAT32
until it is certain the app cannot); download whatever is needed; the saves on the console are not
important, so memory-card writes may be tested on them.

## Why this shape

| Need | How |
|---|---|
| PS1 / PS3 start at once | the app asks webMAN on 127.0.0.1 (`/mount_ps3/<path>`, the call the XMB icon makes; proven for PS1 from `/dev_sd` on 2026-11-22) |
| PS2 from the card | the bench path, proven R5-R7: write the one-shot trigger `/dev_hdd0/tmp/prism_lr.cfg` (`mode=iso path=...`), mount a carrier title, start it; the stage2 serves the card file |
| exFAT, which GameOS cannot see (it mounts only the first partition, FAT32, as `/dev_sd`) | read the card as raw sectors (lv2 syscalls 600/602/609, SD device `0x103000100000010`, psdevwiki DeviceID) with the stage2's own reader `lr_fs.c`, compiled unchanged |
| PS3 / PS1 games ON exFAT (later) | webMAN's `rawseciso` mounts ISOs from raw sector lists (how it plays from NTFS/exFAT USB drives); the app can make those lists with the same reader |
| the 64 KB stage2 limit | the menus leave the stage2; it stays a "serve this file" engine |

## Layout

- `src/main.c` - the loop: pad, screens, starts (`launch.c`), the probe on START (`probe.c`)
- `src/ui.c`, `src/screens.c`, `src/look.h` - the PS2 launcher's look, ported: a software
  rasterizer in the launcher's own 640 x 448 coordinates scaled onto a 1280 x 720 canvas
  (`gfx.c` scales to the TV), DejaVu at 22/26/31/42 px (`tools/gen_font_ps3.py`)
- `src/art.c` - covers from `/dev_hdd0/tmp/prism_art.dat` (the launcher's art file)
- `src/vmc.c` - reads the virtual memory cards in `/dev_hdd0/savedata/vmc` (PS2 `.VM2`, PS1 `.VM1`),
  saves, titles, icons (PS2 3D models, PS1 16 x 16 frames)
- `src/vmcw.c` - copies / deletes saves; ECC per page; keeps `<card>.orig` (before Prism ever
  changed it) and `<card>.last` (before the latest change) in `/dev_hdd0/tmp/prism_vmc_bak`
- `src/mcard.c` - the MEMORY CARD screen
- `src/fsport_ps3.c`, `src/storage.h` - lr_fs.c's block layer on the raw SD device (read-only)
- `src/stub/ps2emu/*.h` - empty stand-ins so `lr/tree/ps2emu_stage2/prism/lr_fs.c` compiles as is
- `host/` - the same reader built for the PC (`fsprobe`), tested against the Linux-made cards in
  `emu/cards` (`run_host_tests.sh` -> `test_fsport.py`, ground truth = `emu/cards/manifest.txt`)
- `build_app.sh` - PSL1GHT build -> `out/prism_app.pkg` (title ID `PRSM00001`, icon = the launcher's)
- `tools/` - toolchain scripts (PSL1GHT `dd4e8c9`, 2026-11-04, built in WSL PSBBN into ~/ps3dev),
  `expect_probe.py` (what the probe must read, from the PC copies of the games)
- `host/render.sh` - draws every screen on the PC into `host/shots/*.ppm` from the real art file
  and real card files (same ui.c / screens.c / mcard.c / vmc.c as the console build)
- `host/vmc_test.sh` - the card writer on COPIES of the owner's cards, judged by **mymcplus**
  (an outside PS2 card reader, `pip install mymcplus`; its `check` + a read of every file with ECC
  checking, through `host/mc_compare.py`), `host/vmc_inspect.py` (ECC of every page) and
  `host/ps1_card.py` (PS1 fixtures + an independent check). Calibrated: mymcplus `check` alone
  passes a card with bad ECC (it does not read file data) - the per-file read is what catches it.
  Mutants that must fail it (2026-11-22, all three do): no ECC written; a chain one cluster short;
  a delete that frees nothing. Run `VMCW=<mutant.c> host/vmc_test.sh ...` to plant one.

## Rules

- Do not build the app or run the host tests while `emu/mutate5.py` runs (it plants bugs in lr_fs.c).
- The card is only ever READ by the app until a step says otherwise.
- Nothing is copied to the hard disk; nothing is written to flash.

## Steps

1. Probe: raw SD open, both partitions mounted, list, known bytes in GTASA and the split MGS2, raw
   read speed. **PASSED on the console 2026-11-22 16:31 CST (Amendment G5): exFAT read, 8/8 byte checks, 16.1 MB/s raw.**
2. A picture: home tiles, lists (PS1 / PS2 / PS3), covers. (v0.2-v0.5: one list screen, tabs,
   DejaVu text drawn in software; the full launcher look is still to port.)
3. **Direct starts, no XMB in between - DONE in v0.5 (2026-11-22 19:07 CST):** PS1 = webMAN
   `/mount_ps3` + exit-spawn of `ps1_emu.self` (owner: Metal Gear Solid); PS2 = trigger + carrier
   mount + `sys_sm_shutdown(0x8202, block, 0x4F0)` with the block this console's XMB sends
   (`src/ps2_block.h`, captured from inside ps2_gxemu; owner: San Andreas off exFAT); PS3 =
   `/mount_ps3` + exit-spawn of the disc's EBOOT (not run yet). Why every other route failed, and
   the firmware addresses: Amendment G5b. webMAN Auto-Play stays OFF (owner).
4. PS3 / PS1 from exFAT through rawseciso sector lists. (PS3 on FAT32 already works as a folder:
   Borderlands BLUS30386 ripped 2026-11-22 over FTP from /dev_bdvd to the PC (`~/ps3_rips`, sizes =
   the disc's listing) and copied to `/dev_sd/GAMES/BLUS30386`; all 550 files read back = PC copy,
   SHA-256. Not started from the app yet. Building an ISO needs makeps3iso: running the downloaded
   tool was refused by the permission check, so folders until the owner allows it.)
5. Memory cards (`.VM2` / `.VM1` on the hard disk): view, then copy / delete. **Built in v0.8
   (2026-11-22 19:43 CST), not yet run on the console.** Every card, L1 / R1 between them; each
   save floats and turns by its own icon; X: Copy to <card of the same kind> / Delete / Cancel,
   then a question that starts on "No". What the writer copies from real cards (measured with
   `host/vmc_inspect.py` on the owner's three cards): ECC 3 bytes per 128-byte chunk + 4 zero bytes,
   6327 of 6327 written pages; the PS2 deletes by clearing the entry's 0x8000 and each FAT entry's
   allocated bit, keeping the links; a save's "." holds length 0 (the count is in its root entry).
   PC result: `host/vmc_test.sh` 0 failures (13 saves compared byte for byte through mymcplus).

## Versions

| | built | on the console | what |
|---|---|---|---|
| 0.5 | 19:07 | yes | direct PS1 / PS2 / PS3 starts |
| 0.6 | 19:13 | yes (installed) | passes `PRISM_PS1_1.VM1` to the PS1 emulator |
| 0.7 | 19:25 | install not confirmed (console went to PS2 mode during it) | the launcher's look: home tiles, carousel with covers, A-Z, search, starting screen |
| 0.8 | 19:43 | yes, 20:08 (EBOOT read back = build) | MEMORY CARD screen: view, copy, delete |
| 0.9 | 20:42 | yes, 20:45 (EBOOT read back = build) | PS3 game FOLDERS in /dev_sd/GAMES (named by PARAM.SFO TITLE); **PS1 card passed by NAME** (`PRISM_PS1_1.VM1`): v0.4-v0.8 passed the full path and MGS said "no memory card" (owner, 20:4x). IRISMAN's psx_launch passes names; the emulator opens them in /dev_hdd0/savedata/vmc |
| 0.10 | 21:40 | yes, 21:4x (EBOOT read back = build) | (1) drawing: triangles walk only their row span, covers / gradients / prism colours / text in integers (PPU float->int conversions stall); PC: PS2 list 7.1 -> 3.9 ms/frame, every screen within 2 levels of v0.9 except ~1 font texel on sleeve initials; `/dev_hdd0/tmp/prism_perf.log` = console frame times. (2) PS2 start deletes `/dev_hdd0/tmp/turnoff` first (owner: "not turned off properly" after every PS2 game from the app; webMAN DELETE_TURNOFF). (3) PS3 games: webMAN starts them from the XMB - `/dev_hdd0/tmp/wm_request` = `/play.ps3<path>`, a poke so webMAN reads it, app quits (owner: Borderlands went back to the XMB on direct boot: trophy path). Needs webMAN setup s (wait additionally) = 3, restored 21:3x |
| 0.11 - 0.37 | 11-22 / 11-23 | yes | not listed here: see the memory note `prism-s36-lr-stage2.md` and `HANDOFF_rip_exfat.md` |
| 0.38 - 0.42 | 11-23 17:40 - 19:05 | yes | PS1 / PS2 disc copy (`drive.c`, `ripcore.c`, `rip_image.h`); the options menu offers it, and "Copy to another device" for PS2 games on the hard disk; the unlock steps (webMAN unmount + disc put in again). Run table: `../re_notes/ps12_disc_rip_2026-11-23.md` |
| 0.43 | 11-23 19:32 | yes (EBOOT read back = build) | title list `USRDIR/titles.txt` (22,878 IDs) names discs and copies; CD gap window 156 sectors |
| 0.44 | 11-23 20:43 | see the run table | never reads through a game image Cobra shows in place of the drive (the "Guitar Hero II copied as Loaded" bug); IRISMAN's many-sectors-a-command CD read behind first-run checks; the list line says where every game is, PS2 games on the card too |
| 0.45 | 11-23 21:00 | yes, 21:09 (EBOOT read back = build) | the list shows a copy's title without the " (SLUS_...)" its file name carries |
| 0.46 | 11-23 21:15 | see the run table | Settings -> Help (10 pages, `src/help.c`); terms of use shown once per console before anything else (`TERMS.md`, `USRDIR/terms_ok.txt`); the fast CD read is OFF (it worked and was no faster: the drive reads CDs at about 4x); `tools/mk_release.py` makes the download zip with the terms first |
| 0.47 - 0.66 | 11-23 / 11-24 | yes | not listed here: see the memory note `prism-s36-lr-stage2.md` and `HANDOFF_2026-11-24_next.md` (covers, PSP from the card, card slots, the bad-adaptor check, games from every slot) |
| 0.67 | 11-24 23:55 | yes; **died after 11-23 s, three runs** | **the RSX draws the screens** (`src/gfx.c`, the `g_` half of `src/ui.c`, `src/shaders/`): 60.2 fps on the console against 18.7 (`logs/2026-11-25_v067_crash/`). Died each time about 1 MB of commands had been queued: the end of the whole command buffer. Continue / recently played (`src/played.c`: Triangle on HOME, "played N times"). PS2 saves as `.psu` files to and from USB drives and cards (`src/vmcw.c` vmc_export_psu / vmc_import_psu, `src/savefiles.c`; Memory Card screen: X -> Send to, Square -> Bring in). Held-direction repeat by the clock |
| 0.68 | 11-25 00:08 | yes; **black screen, closed itself after 3 s** | command buffer rewound after every frame: WRONG (the RSX stood at the buffer's start and was sent to the buffer's start, so it drew nothing; `prism_gfx.log`: STUCK waiting for a flip, PUT = GET = 0x1000). The watchdog that closed it is from this build. Multi-game copy: R2 marks games in a list, Square -> Copy the marked games (`src/rip.c` copy_games) |
| 0.69 | 11-25 00:19 | yes (EBOOT read back = build); **not started yet** | the rewind in two steps (wait for the frame to finish, then jump back to where the buffer stood after rsxInit); the CPU drawing left as v0.66 had it; if a wait runs out Prism closes and the same version draws on the CPU from the next start (`/dev_hdd0/tmp/prism_rsx_failed`). `host/fake_rsx.c` now models GET / PUT / jump and refuses 0.68's sequence |
| 0.70 | 11-25 00:38 | yes (EBOOT read back = build); not started yet | PPSSPP as a third PSP launcher (Square on a PSP game, and SETTINGS): Prism opens it, the game is chosen in its own list (`launch.h` PSPL_PPSSPP). Five new help pages |
| 0.71 | 11-25 00:42 | yes (EBOOT read back = build); **not started yet: the first run decides** | SETTINGS no longer reads the PSP launcher files for every row of every frame (12.5 ms a frame on the console in 0.67, against 1.5 ms for HOME) |
| 0.72 | 11-25 08:40 | yes (EBOOT read back = the package's); Bananafuzzbuddy's first run owed | **Phantom presses fixed** (`pad.c`: a pad with nothing new since the last call keeps what it held; at 60 fps `ioPadGetData` answers len 0 on some frames and 0.67-0.71 read that as a release: one Cross counted up to 4 times). **Menu sounds** (`sfx.c`: one audio port for Prism's whole run, Prism's own three synthesized sounds; the opening plays through it). **Solid 3D save icons** (the RSX depth buffer, `gx_depth`; non-chosen icons darker instead of see-through). **Serials** (`discid.c`): made capital (MGS's disc says `slus_005.94`), and read from raw 2352-byte PS2 CD images (Crazy Taxi). **Covers**: a removed cover is looked up again; games with no readable serial are counted on the page and named in `prism_covers.log`. **Continue** is a list of the last five played (Triangle on HOME). **PPSSPP from a card or USB drive**: an empty file of the game's name in `/dev_hdd0/PSPISO` plus a webMAN `/remap.ps3` to the game (`launch.c pps_link`; cleaned after a restart; `rip.c` refuses to copy onto such a name) |
| 0.73 | 11-25 08:51 | yes (EBOOT read back = the package's); not run yet | **Delete a game from a card's exFAT part** (`exfat_w.c exw_delete`, `manage.c exfat_delete`; `lr_fs.c` gained `lr_fs_cat_where`, LR_APP only). Deleting a FAT32 card game now looks on the game's OWN slot (it looked on `/dev_sd` whatever the slot) |
| 0.74 | 11-25 09:03 | yes (EBOOT read back = the package's) | SETTINGS: "Menu sounds: on / off" (the opening keeps its sound); STORAGE lists a card's exFAT part with its free space (`exw_space`; `exd_test.sh` now 51 checks) |
| 0.75 | 11-25 09:36 | yes (EBOOT read back = the package's); nothing in it run on the console yet | **Save files on an exFAT-only card** (`savefiles.c` exf_*: the card's exFAT part is a place when the PS3 mounts nothing there; `exfat_w.c` gained `exw_mkdir`, `exw_list`, `exw_read`; a save file passes through `/dev_hdd0/tmp/prism_psu`). **Delete the marked games** (Square with games marked; each planned first, the two questions once, then one by one). **Copy, then delete the original** (`rip.c ask_move`, after the "Yes"): the original goes only after its copy was read back and found the same byte for byte (`verify_file`; a card game against the card in `copy_card_ps2`); copies into an exFAT part or in FAT32 parts are not read back yet and keep the original. Memory Card: Square in "Send this save?" also deletes the save from the card once the file is written and read back |
| 0.76 | 11-25 09:52 | yes (EBOOT read back = the package's) | Bananafuzzbuddy's run of 0.75 (10:45): deleting from exFAT, the menu-sounds switch and a save to the Memory Stick and back all WORK on the console. Two faults from the same run fixed: (1) Tiny Tees copied from the hard disk to the SD with "delete the original" = yes was copied and not deleted: a PS2 game onto a card's FAT32 part goes through `copy_to_parts`, the one route 0.75 did not read back. Now `verify_parts` (one file or name.0, .1 ... end to end) and `verify_exfat` (through `lr_fs.c`) read every kind of copy back; an original is kept only when the check is stopped or fails. (2) Memory Card: the Square toggle is gone; "Send this save?" Yes is followed by its own question, "Delete it from this card afterwards?" No / Yes (Circle at either: nothing is sent) |
| 0.79 | 11-25 14:12 | yes (EBOOT `2dd718b8…` and stage2 read back = the package's); not run yet | **New memory cards and card names** (Bananafuzzbuddy, 11-25). MEMORY CARD, START: "New PS2 card", "New PS1 card", "Rename this card", and a small keyboard for the name (`mcard.c`). `vmcnew.c` makes the files: a new PS2 card is byte for byte the PS3's own empty card (one of the owner's cards was an empty one from the XMB; same sha256), a new PS1 card is `host/ps1_card.py`'s blank card. Names: 1 to 28 of letters, digits, space, `-`, `_`, `(`, `)`; none twice. PC: `host/vmcnew_test.sh` (0 failures, mymcplus agrees), `host/vmcnew_mutants.py` (21 of 21), four new pictures in the drawing check (0 of 85 failed). |
| 0.78 | 11-25 13:36 | yes (EBOOT `78c9008e…` and stage2 read back = the package's); not run yet | From Bananafuzzbuddy's run of 0.77 (card-to-card copies worked). **"Already there" with delete asked**: the copy already at the target is compared whole with the card's game and the original deleted when it is the same (`rip.c same_on_card`, `card_sum`, `already_there`); before, two games of one name left the second original in place. **USB drives take PS2 games** to keep them (`/PS2ISO`, under 4 GB): from the hard disk, from a card, listed (`games.c scan_fs_ps2`), copied back, deleted, never started from there. **Marked games in / out of webMAN's list** (`setup.c wm_shortcut_many`). Menu words: "Copy to another device", "N marked: options". PC: the drawing checks pass (0 of 81 failed); the copy, USB and webMAN code is console-only, compiled, never run. |
| 0.77 | 11-25 10:08 | yes (EBOOT read back = the package's); not run yet | **A card game straight to another card** (`rip.c cd_scan` / `cd_choose` / `copy_card_exfat`; `copy_card_ps2` takes its place from `cd_to`): a PS2 game on a card read by Prism can go to the hard disk, to another card's FAT32 part (`/PS2PRISM`, one file, under 4 GB) or to another card's exFAT part (`/PS2ISO`; the folder is made if it is missing). One game or the marked ones; the card it is on is never offered. With "delete the original" the exFAT copy is read back through the card reader and its 64-bit sum compared with the sum of what was read. All console-only code: compiled, never run. (While making it, a patch script emptied `src/version.h`; it was put back from the exported copy.) |

**PC checks added for 0.75:** `exs_test.sh` 28 (folder, list, read; judged by fsck.exfat and exfat-fuse, which also grows the writer-made folder by 300 files) with `exs_mutants.py` 11 of 11; `rsx_host_run.sh` 0 of 81 failed. `rip.c`'s read-back and delete-after-copy, `savefiles.c`'s exFAT path and the multi-delete loop in `main.c` are console-only code: compiled, never run.

**PC checks added for 0.72 / 0.73** (`host/`, WSL): `pad_test.sh` (v0.71's `buttons()` must FAIL it first: it counts
one held Cross 4 times; the new one 22 of 22), `sfx_test.sh` (38) with `sfx_mutants.sh` (8 of 8), `discid_test.sh`
(70; give it real image heads and their serials as arguments), `gx_test.sh` now 3862 with `gx_mutants.py` 29 of 29
(six depth-buffer bugs), `rsx_host_run.sh` 0 of 79 failed, `exw_test.sh` 25 (unchanged by the refactor into
`prep()`), **`exd_test.sh` 46** (delete, judged by fsck.exfat, exfat-fuse, df and a stray-entry reader; its
calibration says what fsck.exfat does NOT see) with **`exd_mutants.py` 12 of 12**. Not on the PC at all: `manage.c`'s
and `launch.c`'s glue, the real audio port, the real RSX depth buffer.

**PC checks for 0.67 - 0.71** (all in `host/`, run from WSL): `rsx_host_run.sh` (every screen drawn by the CPU
path and by the RSX path over `fake_rsx.c`, compared picture by picture: 0 of 76 failed), `gx_test.sh`
(3847 checks) with `gx_mutants.py` (23 of 23 planted bugs caught), `played_test.sh` (37) with
`played_mutants.sh` (11 of 11), `psu_test.sh <mymcplus.exe>` (0 failures, judged by mymcplus) with
`psu_mutants.py` (11 of 11), `vmcw_regress.sh` (card-to-card copies byte-identical to 0.66), `shader_check.py`
(run by `build_app.sh`). **The stand-in is not the RSX:** 0.67's death at the buffer's end was found only on
the console, and 0.68's fault got past the stand-in as it then was.

## Giving Prism to other people

Never hand out the bare `.pkg`. `python tools/mk_release.py <app dir>` makes `out/Prism_<version>.zip`
(the terms first, then the package) and `out/RELEASE_NOTES_<version>.md` (the text for the release
page, which opens with "By downloading ... you agree to the terms"). The app shows the same terms on
a console's first start and does nothing until X is pressed. When `TERMS.md` changes, raise its
version and `TERMS_VER` in `src/help.h` together (the tool refuses if they differ).
Owed before a first public release: the licence check of the two title lists (`db/SOURCES.txt`), a
licence for Prism's own code, and Bananafuzzbuddy's go-ahead. Nothing has been published.

### v0.47 (11-23 21:33) and the licence question

| | built | on the console | what |
|---|---|---|---|
| 0.47 | 11-23 21:33 | see the re_notes | asks the drive for its full CD speed (SET CD SPEED, one first-run question), fast read back on without questions; PS1 title list now from the GPL-3.0 project (the earlier one had no licence) |

Licence facts found 11-23 (not legal advice): PSL1GHT is MIT; webMAN-MOD and IRISMAN are GPL-3.0; the
title lists come from a GPL-3.0 project (`db/SOURCES.txt`); the PS2 helper Prism installs is built
from Cobra's stage2 source, which carries GPL files (`lr/tree/ps2emu_stage2`, e.g. the marcan-derived
debug code); the font is DejaVu Sans (free licence, needs its notice kept). The simplest fit for
Prism's own code is GPL-3.0, with the source published beside every download. Bananafuzzbuddy decides.

## The GitHub repository (2026-11-23)

`averyjn12/prism-ps3`, PRIVATE, GPL-3.0 (`LICENSE`, `NOTICE.md`). It is an export, made by
`python tools/export_repo.py "<lr dir>" "C:/path/to/prism-ps3"`, never these working folders: they hold a
Sony emulator file, key files, game covers and logs that must not be published, and the script refuses if any
get in. After an export: `git add -A`, commit, push in `C:\path\to\prism-ps3`. A draft pre-release
"Prism 0.48" with `Prism_0.48.zip` is attached there, not published. Making the repository public is Bananafuzzbuddy's
decision; the open points are listed in the memory note `prism-ps3-repo.md`.
