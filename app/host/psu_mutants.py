#!/usr/bin/env python3
"""v0.67: planted bugs in a COPY of src/vmcw.c, run through host/psu_test.sh (which lets VMCW name the
writer to build). Caught = psu_test reports failures on its own lines. A mutant whose text is not found
exactly once, or that does not compile (psu_test exits 2), is BROKEN and stops the run. The unmutated
writer must pass first.

    python3 host/psu_mutants.py <mymcplus.exe>
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src", "vmcw.c")
B = os.path.expanduser("~/prism_app_host/psu_mut")
W = "/path/to/prism_psu_mut"

MUTANTS = [
    ("export: a file's last cluster left out", "ok = fwrite(pk.data[j - 2], PKG_CL, pk.ncl[j - 2], f) == pk.ncl[j - 2];",
     "ok = fwrite(pk.data[j - 2], PKG_CL, pk.ncl[j - 2] - 1, f) == pk.ncl[j - 2] - 1;"),
    ("export: the entry count one short", "put32(e + 4, 2 + pk.nf);\n\tput32(e + 0x10, 0);", "put32(e + 4, 1 + pk.nf);\n\tput32(e + 0x10, 0);"),
    ("import: the filling after a file not skipped", "at += have;", "at += flen;"),
    ("import: a file's bytes one off", "memcpy(pk.data[pk.nf], d + at, have);", "memcpy(pk.data[pk.nf], d + at + 1, have - 1);"),
    ("import: the same name twice accepted", "if (!strncmp((const char *)pk.ents[2 + k] + 0x40, (const char *)e + 0x40, 32))", "if (0)"),
    ("import: a folder inside accepted", "if ((le16(e) & (DF_EXISTS | DF_DIR | DF_FILE)) != (DF_EXISTS | DF_FILE) || !name_ok(e))\n\t\t{\n\t\t\tsnprintf(why, why_n, \"entry %u",
     "if (0)\n\t\t{\n\t\t\tsnprintf(why, why_n, \"entry %u"),
    ("import: a file cut short accepted", "if (flen > (u32)(len - at))", "if (0)"),
    ("import: the cluster count not kept", "pk.ncl[pk.nf] = cl;", ";"),
    ("import: the first file skipped", "\t\tpk.nf++;\n\t}\n\tr = pkg_to_card(&pk, to, why, why_n);", "\t\tif (j > 2)\n\t\t\tpk.nf++;\n\t}\n\tr = pkg_to_card(&pk, to, why, why_n);"),
    ("card: a save's files written a cluster late", "cl_write(&w, to->alloc_offset + newcl[used + k], 0, p->data[j] + (size_t)k * PKG_CL, to->cl);",
     "cl_write(&w, to->alloc_offset + newcl[used + k], 0, p->data[j] + (size_t)(k ? k - 1 : 0) * PKG_CL, to->cl);"),
    ("names: a slash in the folder name accepted", "n[k] == '/' || ", ""),
]


def run(vmcw):
    env = dict(os.environ, VMCW=vmcw)
    r = subprocess.run(["bash", os.path.join(HERE, "psu_test.sh"), sys.argv[1], W], capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr


def main():
    os.makedirs(B, exist_ok=True)
    rc, out = run(SRC)
    if rc != 0:
        print("BROKEN: the unmutated writer fails psu_test\n" + out[-600:])
        return 2
    print("baseline:", out.strip().splitlines()[-1])
    src = open(SRC).read()
    caught = 0
    for name, old, new in MUTANTS:
        if src.count(old) != 1:
            print("BROKEN: %s: the text to change is found %d times" % (name, src.count(old)))
            return 2
        m = os.path.join(B, "vmcw.c")
        open(m, "w").write(src.replace(old, new))
        rc, out = run(m)
        if rc == 2:
            print("BROKEN: %s does not compile\n%s" % (name, out[:400]))
            return 2
        why = [l.strip() for l in out.splitlines() if "FAIL" in l]
        if rc == 1 and why:
            caught += 1
            print("caught    %-46s %s" % (name, why[0][:70]))
        else:
            print("SURVIVED  %-46s (exit %d)" % (name, rc))
    print("psu_mutants: %d of %d caught" % (caught, len(MUTANTS)))
    return 0 if caught == len(MUTANTS) else 1


if __name__ == "__main__":
    sys.exit(main())
