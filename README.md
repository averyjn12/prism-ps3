# Prism

A game launcher for a PlayStation 3 with custom firmware. One screen for your PS1, PS2, PS3 and PSP
games, and a way to copy your own game discs to storage from the couch.

**Before you download: read [TERMS.md](TERMS.md). By downloading, installing or using Prism you
agree to those terms. Prism is free, comes with no warranty, and you use it at your own risk.**

## What it does

- Lists PS1, PS2, PS3 and PSP games from the internal drive, USB storage and the console's card
  slots, with search and an A to Z strip, and starts them.
- Copies a PS1, PS2 or PS3 game disc you own to storage, named by the game's title. A PS1 disc
  becomes a `.bin` and `.cue`, a PS2 disc an `.iso`, a PS3 disc a folder.
- Plays PS2 games straight from the SD card slot on the PS3 models that have one and run PS2
  discs through the PS2 emulator (CECHC and CECHE), with nothing copied to the internal drive.
  PS2 games over 4 GB can sit on a FAT32 card in parts.
- Copies games between storage devices, deletes them (after two questions), and views, copies and
  deletes memory-card saves.
- Help pages on the console (Settings, Help).

## Status

Early. Prism has been made and tested on **one** console: a CECHE01 on Evilnat 4.93 with Cobra 8.5
and webMAN MOD 1.47. It does not refuse older custom firmware, but nobody has tried it there: on
anything else you are the first, and it may not work. Read the terms, and "Older firmware" in
[INSTALL.md](INSTALL.md).

## Download and install

1. Get `Prism_<version>.zip` from the [Releases](../../releases) page.
2. Follow [INSTALL.md](INSTALL.md) (it is also inside the zip).

You need custom firmware with Cobra, and webMAN MOD (the full edition). If webMAN's Auto-Play is
on, Prism's Settings screen turns it off for you with one choice.

## What Prism changes on a console

- It installs like any other package, into the Game column.
- "Set up PS2 play" (optional, in Settings) adds a few lines to `/dev_hdd0/onxmb.bat` and copies
  two small files to the internal drive. Settings takes them out again.
- On its first start it copies Tiny Tees, a small PS2 mini golf game that comes with Prism (under
  2 MB), into `PS2ISO` on the internal drive, once. Delete it from Prism's PS2 list if you do not
  want it; it is not copied again.
- Starting a PS2 game from the SD card copies nothing: the game is read from the card as it plays.
- Nothing is ever written to the console's system memory (flash).

## Source

| Folder | What |
|---|---|
| `app/` | the PS3 app (PSL1GHT). `app/src` the code, `app/host` PC tests, `app/build_app.sh` the build |
| `tree/` | the PS2 helper Prism installs: Cobra's PS2 stage2 source plus Prism's card-reader code |
| `launcher/ui/` | the small PS2-side launcher image |
| `out/` | the tested build of the helper that goes into the package |
| `build.sh`, `port_tree.sh`, `apply_prism.py` | how the helper is built |

Building needs the ps3dev toolchain and PSL1GHT under Linux or WSL; `app/tools` has the notes. The
build has only ever been run on the author's PC, so expect to adjust paths. The helper is
reproducible: `bash build.sh <copy of tree> 5 name` gives `out/lr_g5.bin` byte for byte (checked
2026-11-23).

Tiny Tees, the PS2 mini golf game Prism carries (`app/setup/tinytees.iso`, version 0.12), is by the
same author. It is included as a ready-made disc image.

## Licence and credits

GPL-3.0: see [LICENSE](LICENSE). Whose work Prism stands on: [NOTICE.md](NOTICE.md).

Prism is not made, approved or supported by Sony. It contains no Sony artwork, logos or sounds.
Copy only discs you own, for your own use, where your law allows it.
