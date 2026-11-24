# Installing Prism

Read `TERMS.md` (in the download: `0 - READ FIRST - TERMS.txt`) before you install.

## What you need

- A PlayStation 3 with custom firmware that includes Cobra. Prism was made and tested on **one**
  console, on Evilnat 4.93 with Cobra 8.5. It does not check the firmware number, so it installs and
  runs on older custom firmware too, **untested**: see "Older firmware" below.
- webMAN MOD, the full edition (not Lite), installed and working. webMAN's Auto-Play must be off;
  if it is on, Prism's Settings screen offers "Turn webMAN's Auto-Play off" and does it for you.
- A USB flash drive formatted FAT32.
- To play PS2 games from the SD card slot: a CECHC or CECHE console (the early models with a card
  reader that play PS2 discs through the PS2 emulator). On every other model Prism still lists and
  starts PS1, PS3 and PSP games and copies discs.

## Steps

1. On a computer, unzip the download and copy `Prism_<version>.pkg` to the top level of the flash
   drive (not into a folder).
2. Plug the flash drive into the console.
3. On the XMB go to Game, then Package Manager, then Install Package Files, then Standard.
4. Choose the Prism package and press X. Wait for it to finish, then press Circle to go back.
5. Prism is now in the Game column. Start it. (On its first start Prism copies Tiny Tees, the small
   PS2 mini golf game that comes with it, into `PS2ISO` on the internal drive: under 2 MB, once.)
6. The first start shows the terms of use. Read them. After three seconds X means "I agree"; Circle
   quits.
7. Open Settings (the last tile). Under THIS CONSOLE, check that webMAN MOD says "answering". If it
   says Auto-Play is on, choose "Turn webMAN's Auto-Play off" on the same screen.
8. For PS2 games from the SD card: in Settings choose "Set up PS2 play", then "Set up". Quit Prism
   (Circle on its home screen) and start it again. Settings must now say "Ready".
   This adds a few lines to `/dev_hdd0/onxmb.bat` and copies two small files to the internal drive.
   Nothing is written to the console's system memory, and Settings can take it out again.
   Starting a PS2 game from the card afterwards copies nothing to the internal drive.
9. Settings, Help answers the common questions.

## Older firmware

Prism has only ever been run on firmware 4.93 with Cobra 8.5. On anything else you are the first to
try it, at your own risk (see the terms).

- Listing and starting PS1, PS3 and PSP games, and copying discs, do not depend on the firmware
  number. They need Cobra and webMAN MOD.
- PS2 games from the SD card need Cobra 8.3, 8.4 or 8.5 (firmware 4.84 to 4.93), because the helper
  Prism installs is built on the part of Cobra that is the same in those versions. On a firmware it
  was not tested on, Settings says "Ready, but not tested here". If a PS2 game then gives a black
  screen, hold the power button until the console turns off; nothing was written to the console's
  system memory, and "Remove the PS2 setup" in Settings takes the helper out again.
- On older Cobra versions Prism says "Not on this console" for PS2 games from the card and leaves
  the PS2 emulator alone.

## Where games go

| Games | Folder | On |
|---|---|---|
| PS1 | `PSXISO` (.bin with its .cue) | internal drive, USB storage, a card in a card slot |
| PS2 | `PS2ISO` | internal drive |
| PS2 | `PS2ISO` (exFAT part) or `PS2PRISM` (FAT32 part, parts under 4 GB) | the SD card |
| PS3 | `GAMES` (folders) or `PS3ISO` | internal drive, USB storage |
| PSP | `PSPISO` | internal drive, USB storage, a card in a card slot |

Prism can make these copies itself from your own discs: put a disc in, choose it in its list, press
Square.

## Removing Prism

In Settings choose "Remove the PS2 setup" if it is set up. Then delete Prism in the Game column
(Triangle, Delete). Your games, copies and memory cards stay where they are.
