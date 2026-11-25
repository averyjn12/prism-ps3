"""The two shortcut templates the A01 kit copies under each card game's name (kit 1.2).
They are exactly what bench/lr_title.py pinkit puts on the mule for the launcher's pins (run S1,
Amendment F): marker "PRISMLR1 / mode=iso / path=*" at 0x7800, typed DVD or CD by the root
directory size word (PVD+0xAA), the real launcher PRISMUI.ELF as the fallback program (it says the
card or the game is missing). NEVER PRISMUI_DEMO2.ELF.
usage: mk_templates.py <out_dir>"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LR = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(LR, "bench"))
import lr_title as T  # noqa: E402

ELF = os.path.join(LR, "launcher", "ui", "PRISMUI.ELF")
assert os.path.basename(ELF) == "PRISMUI.ELF"
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
for is_cd, name in ((False, "tpl_dvd.iso"), (True, "tpl_cd.iso")):
    img = T.build_template(ELF, is_cd)
    m = img[T.MARK_OFF:T.MARK_OFF + T.MARK_SZ]
    assert m.startswith(b"PRISMLR1\nmode=iso\npath=*\n"), "marker"
    pvd = img[16 * 2048:17 * 2048]
    assert pvd[1:6] == b"CD001", "no PVD"
    assert (struct.unpack_from(">I", pvd, 0xAA)[0] == 0x800) == is_cd, "CD/DVD word"
    open(os.path.join(out, name), "wb").write(img)
    print("%s: %d bytes, %s, marker OK" % (name, len(img), "CD" if is_cd else "DVD"))
