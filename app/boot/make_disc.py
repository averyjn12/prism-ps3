#!/usr/bin/env python3
"""Lay out Prism's boot disc folder (v0.29): PS3_DISC.SFB, PS3_GAME/LICDIR/LIC.DAT and the marker
file PRISM_BOOT.TXT. PARAM.SFO, ICON0.PNG and EBOOT.BIN are added by build_app.sh.

    python3 make_disc.py <out dir> <title id, e.g. PRSM00002>

The two binary files follow a real disc backup on the bench console (BLUS30386, read 2026-11-23):
  PS3_DISC.SFB  1536 bytes: ".SFB", version 1; entries at 0x20 (HYBRID_FLAG -> 0x200, 0x20 bytes)
                and 0x40 (TITLE_ID -> 0x220, 0x10 bytes); the values at 0x200 ("g" = game) and
                0x220 ("PRSM-00002").
  LIC.DAT       64 KiB: "PS3LICDA" header, 0x01 + title ID at 0x800, and at 0x20 the CRC32 of the
                first 0x900 bytes with that field zeroed (checked: it reproduces BLUS30386's value).
"""
import os
import struct
import sys
import zlib


def sfb(title_id):
    d = bytearray(1536)
    d[0:4] = b".SFB"
    d[4:8] = struct.pack(">I", 0x00010000)
    d[0x20:0x30] = b"HYBRID_FLAG".ljust(16, b"\0")
    d[0x30:0x38] = struct.pack(">II", 0x200, 0x20)
    d[0x40:0x50] = b"TITLE_ID".ljust(16, b"\0")
    d[0x50:0x58] = struct.pack(">II", 0x220, 0x10)
    d[0x200:0x201] = b"g"
    tid = (title_id[:4] + "-" + title_id[4:]).encode()
    d[0x220:0x220 + len(tid)] = tid
    return bytes(d)


def lic(title_id):
    d = bytearray(0x10000)
    d[0:8] = b"PS3LICDA"
    d[8:0x20] = struct.pack(">IIIIII", 1, 0x80000000, 0x900, 0x800, 0x100, 1)
    t = b"\x01" + title_id.encode()
    d[0x800:0x800 + len(t)] = t
    crc = zlib.crc32(bytes(d[:0x900])) & 0xFFFFFFFF
    d[0x20:0x24] = struct.pack(">I", crc)
    return bytes(d)


NOTE = """Prism's boot disc (the Prism app, PRSM00001).
webMAN mounts this folder after the console starts when Prism's SETTINGS switch
"Start Prism with the console" is on; its program starts Prism, and Prism unmounts it.
"""


def main():
    out, tid = sys.argv[1], sys.argv[2]
    assert len(tid) == 9 and tid[:4].isalpha() and tid[4:].isdigit(), tid
    os.makedirs(os.path.join(out, "PS3_GAME", "LICDIR"), exist_ok=True)
    os.makedirs(os.path.join(out, "PS3_GAME", "USRDIR"), exist_ok=True)
    with open(os.path.join(out, "PS3_DISC.SFB"), "wb") as f:
        f.write(sfb(tid))
    with open(os.path.join(out, "PS3_GAME", "LICDIR", "LIC.DAT"), "wb") as f:
        f.write(lic(tid))
    with open(os.path.join(out, "PRISM_BOOT.TXT"), "w") as f:
        f.write(NOTE)


if __name__ == "__main__":
    main()
