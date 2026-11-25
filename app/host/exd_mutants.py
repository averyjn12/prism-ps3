#!/usr/bin/env python3
"""v0.73: planted bugs in a copy of src/exfat_w.c's delete, against host/exd_test.sh.
   python3 host/exd_mutants.py
A mutant is caught only by a FAIL line of the test itself; a build error or a crash is BROKEN."""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src", "exfat_w.c")
OUT = os.path.expanduser("~/prism_app_host/exd_mut")
os.makedirs(OUT, exist_ok=True)
src = open(SRC).read()
d0 = src.index("int exw_delete(")

MUT = [
    ("the bitmap is never written", "\t\tif (!dirty[b / SEC])\n\t\t\tcontinue;", "\t\tif (1)\n\t\t\tcontinue;", 1),
    ("the wrong cluster's bit is cleared", "\t\tuint64_t bit = (uint64_t)c - 2;", "\t\tuint64_t bit = (uint64_t)c - 1;", 1),
    ("only the File entry is removed", "\tfor (i = 0; i < fd.got_n; i = j)\n\t{\n\t\tlba = chain_lba(dir_clus, dir_nofat, fd.got_off + 32 * (uint64_t)i);\n\t\tif (!lba || rd(lba, 1, sbuf))\n\t\t{\n\t\t\tfree(dirty);\n\t\t\tsay(why, wn, i ?",
     "\tfor (i = 0; i < 1; i = fd.got_n)\n\t{\n\t\tj = 1;\n\t\tlba = chain_lba(dir_clus, dir_nofat, fd.got_off + 32 * (uint64_t)i);\n\t\tif (!lba || rd(lba, 1, sbuf))\n\t\t{\n\t\t\tfree(dirty);\n\t\t\tsay(why, wn, i ?", 1),
    ("one entry too many is removed", "for (j = i; j < fd.got_n && chain_lba(dir_clus, dir_nofat, fd.got_off + 32 * (uint64_t)j) == lba; j++)\n\t\t\tsbuf[(fd.got_off + 32 * (uint64_t)j) % SEC] &= 0x7F;",
     "for (j = i; j < fd.got_n + 1 && chain_lba(dir_clus, dir_nofat, fd.got_off + 32 * (uint64_t)j) == lba; j++)\n\t\t\tsbuf[(fd.got_off + 32 * (uint64_t)j) % SEC] &= 0x7F;", 1),
    ("a chain is freed as one run", "\t\tdirty[(bit >> 3) / SEC] = 1;\n\t\tif (k + 1 < need)\n\t\t\tc = fd.nofat ? c + 1 : fat_next(c);", "\t\tdirty[(bit >> 3) / SEC] = 1;\n\t\tif (k + 1 < need)\n\t\t\tc = c + 1;", 1),
    ("one cluster too few is freed", "\tneed = (fd.len + csz - 1) / csz;", "\tneed = fd.len / csz;", 1),
    ("a folder is deleted like a file", "\tif (fd.dir)\n\t{\n\t\tsay(why, wn, \"that is a folder, not a file\");", "\tif (0)\n\t{\n\t\tsay(why, wn, \"that is a folder, not a file\");", 1),
    ("the bitmap is not asked before", "if (c < 2 || c >= nclus + 2 || !(bmp[(c - 2) >> 3] & (1u << ((c - 2) & 7))))", "if (c < 2 || c >= nclus + 2)", 1),
    ("the wrong bitmap sector is written", "\t\tdirty[(bit >> 3) / SEC] = 1;", "\t\tdirty[0] = 1;", 1),
    ("the chain's end is not checked", "if (need && !fd.nofat && fat_next(c) != 0xFFFFFFFF)", "if (0)", 1),
    ("the entries are zeroed, not marked unused", "sbuf[(fd.got_off + 32 * (uint64_t)j) % SEC] &= 0x7F;", "sbuf[(fd.got_off + 32 * (uint64_t)j) % SEC] = 0;", 1),
    ("the set after the match is removed", "\t\t\t\tf->got_off = f->set_off;", "\t\t\t\tf->got_off = f->set_off + 32 * (uint64_t)f->set_n;", 1),
]


def run(path):
    env = dict(os.environ, EXW=path)
    try:
        p = subprocess.run(["bash", os.path.join(HERE, "exd_test.sh")], env=env, capture_output=True, text=True, errors="replace", timeout=900)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    return p.returncode, p.stdout + p.stderr


rc, out = run(SRC)
if rc != 0:
    print("BROKEN: the unmutated test does not pass"); print(out[-600:]); sys.exit(2)
print("baseline:", out.strip().splitlines()[-1])
caught = 0
for name, old, new, cnt in MUT:
    if src.count(old) != cnt:
        print("BROKEN: %s: its anchor is in the source %d times, not %d" % (name, src.count(old), cnt)); sys.exit(2)
    path = os.path.join(OUT, "exfat_w.c")
    open(path, "w").write(src.replace(old, new))
    rc, out = run(path)
    if rc is None or "error:" in out or rc == 2:
        print("BROKEN   %-44s (%s)" % (name, "does not build or run" if rc is not None else out)); continue
    fails = [l for l in out.splitlines() if l.startswith("FAIL")]
    if fails:
        caught += 1
        print("caught   %-44s %s" % (name, fails[0][:70]))
    else:
        print("SURVIVED %-44s (exit %s)" % (name, rc))
print("exd_mutants: %d of %d caught" % (caught, len(MUT)))
sys.exit(0 if caught == len(MUT) else 1)
