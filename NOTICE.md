# Prism: licence and credits

Prism is free software: you can redistribute it and/or modify it under the terms of the GNU General
Public License as published by the Free Software Foundation, version 3 (`LICENSE`).

Prism is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
License for more details, and `TERMS.md` for the terms of use that go with every download.

Prism is not made, approved or supported by Sony. "PlayStation" is a trademark of Sony Interactive
Entertainment Inc. Prism contains no Sony artwork, logos or sounds.

## What Prism is built from, and whose work that is

| Part | Where | From | Licence |
|---|---|---|---|
| The app's own code | `app/src`, `app/host`, `app/tools` | this project | GPL-3.0 |
| PS2 helper ("stage2") | `tree/`, shipped as `out/lr_g5.bin` | the Cobra CFW source (Cobra 8.5 for 4.93 CEX, as published by Evilnat), ported to a current compiler by `port_tree.sh`, with this project's card-reader code in `tree/ps2emu_stage2/prism` | Cobra's files carry GPL notices (for example the debug code by Hector Martin "marcan", GPL-2.0); this project's additions are GPL-3.0 |
| PS2-side launcher image | `launcher/ui`, shipped as `app/setup/carrier.iso` | this project, built with PS2SDK and gsKit | GPL-3.0 (PS2SDK: Academic Free License 2.0) |
| Tiny Tees | `app/setup/tinytees.iso` | a PS2 homebrew game by this project's author | included as a ready-made image |
| Title lists | `app/db` | PSBBN Definitive Project by CosmicScale, whose credits name GDX-X's title database | GPL-3.0 |
| Font | built into `app/src/font_ps3.h`, `launcher/ui/*.ttf` | DejaVu Sans | the DejaVu / Bitstream Vera font licence |
| PS3 SDK | not included | PSL1GHT and the ps3dev toolchain | MIT (PSL1GHT) |
| webMAN MOD header excerpts | `app/ref` | webMAN MOD by aldostools and contributors | GPL-3.0 |

How the disc drive is read was learned from, and each command is cited in `app/src/drive.c` to:
the Cobra library ("cobralib") as shipped in ManaGunZ (Zarh) and webMAN MOD, multiMAN (deanK),
IRISMAN (Estwald, aldostools), and the Linux kernel's `ps3rom` and `sr` drivers. PS2 games are
started through webMAN MOD and Cobra.

Thank you to everyone whose published work made this possible.
