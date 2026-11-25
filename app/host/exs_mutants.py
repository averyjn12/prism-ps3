#!/usr/bin/env python3
"""v0.74: planted bugs in a copy of src/exfat_w.c's mkdir, list and read, against host/exs_test.sh.
   python3 host/exs_mutants.py
A mutant is caught only by a FAIL line of the test itself; a build error or a crash is BROKEN."""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src", "exfat_w.c")
OUT = os.path.expanduser("~/prism_app_host/exs_mut")
os.makedirs(OUT, exist_ok=True)
src = open(SRC).read()

MUT = [
    ("the new folder's cluster is not written as zeros", "\t\tif (wr(lba, n, zero))\n", "\t\tif (0 && wr(lba, n, zero))\n", 1),
    ("the new folder's cluster is not marked used", "\tbmp[(c - 2) >> 3] |= (uint8_t)(1u << ((c - 2) & 7));\n", "\t;\n", 1),
    ("the new folder is named as a file", "\tr = add_entry(0x0010, c, csz, why, wn);", "\tr = add_entry(0x0020, c, csz, why, wn);", 1),
    ("the new folder has no size", "\tr = add_entry(0x0010, c, csz, why, wn);", "\tr = add_entry(0x0010, c, 0, why, wn);", 1),
    ("the cluster after the free one is taken", "\t\t\tc = (uint32_t)i + 2;\n", "\t\t\tc = (uint32_t)i + 3;\n", 1),
    ("a name already there is made again", "\tif (fd.got)\n\t{\n\t\tsay(why, wn, \"that name is already there\");", "\tif (0)\n\t{\n\t\tsay(why, wn, \"that name is already there\");", 1),
    ("three-byte letters come out wrong in a list", "\t\t\t\t\tu8[n++] = (char)(0xE0 | (c >> 12));", "\t\t\t\t\tu8[n++] = (char)(0xC0 | (c >> 12));", 1),
    ("a list stops after the first name", "\t\t\tif (f->cb(f->arg, u8, f->len, f->dir))\n\t\t\t\treturn 1;", "\t\t\tf->cb(f->arg, u8, f->len, f->dir);\n\t\t\treturn 1;", 1),
    ("a file in pieces is read as one run", "\t\tuint32_t lba = chain_lba(fd.clus, fd.nofat, off), n = fd.len - off < SEC", "\t\tuint32_t lba = chain_lba(fd.clus, 1, off), n = fd.len - off < SEC", 1),
    ("a folder is read as a file", "\tif (walk(dir_clus, dir_nofat, dir_len, name_cb, &fd) || !fd.got || fd.dir)\n\t{\n\t\tsay(why, wn, \"no file with that name is there\");", "\tif (walk(dir_clus, dir_nofat, dir_len, name_cb, &fd) || !fd.got)\n\t{\n\t\tsay(why, wn, \"no file with that name is there\");", 1),
    ("the last part-sector of a file is read whole", "n = fd.len - off < SEC ? (uint32_t)(fd.len - off) : SEC;", "n = SEC;", 1),
]


def run(path):
    env = dict(os.environ, EXW=path)
    try:
        p = subprocess.run(["bash", os.path.join(HERE, "exs_test.sh")], env=env, capture_output=True, text=True, errors="replace", timeout=900)
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
print("exs_mutants: %d of %d caught" % (caught, len(MUT)))
sys.exit(0 if caught == len(MUT) else 1)
