#!/usr/bin/env python3
"""v0.79: planted bugs in a copy of src/vmcnew.c, against host/vmcnew_test.sh (run without mymcplus: the
checks that must catch these are the test's own).
   python3 host/vmcnew_mutants.py
A mutant is caught only by a FAIL line of the test itself; a build error or a crash is BROKEN."""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src", "vmcnew.c")
OUT = os.path.expanduser("~/prism_app_host/vmcnew_mut")
os.makedirs(OUT, exist_ok=True)
src = open(SRC, newline="").read()

MUT = [
    ("the root folder's FAT entry is left free", "fat == 0 || fat >= ALLOC_END ? 0xFFFFFFFF : 0x7FFFFFFF", "fat >= ALLOC_END ? 0xFFFFFFFF : 0x7FFFFFFF", 1),
    ("the FAT's end is one cluster late", "ALLOC_END = 8135", "ALLOC_END = 8136", 1),
    ("a written page's last four spare bytes stay FF", "\tmemset(p + PG + 12, 0, 4);\n", "", 1),
    ("the ECC of the fourth quarter is missing", "\tfor (k = 0; k < 4; k++)\n\t\tvmc_ecc128(p + k * 128, p + PG + k * 3);", "\tfor (k = 0; k < 3; k++)\n\t\tvmc_ecc128(p + k * 128, p + PG + k * 3);", 1),
    ("the root folder says one entry", "\tput32(pg + 4, 2);", "\tput32(pg + 4, 1);", 1),
    ("the second indirect page is not written", "\tmemset(pg, 0xFF, PG);\n\tput_page(img, IFC * PPC + 1, pg);\n", "", 1),
    ("the card flags byte is wrong", "pg[0x151] = 0x2B;", "pg[0x151] = 0x52;", 1),
    ("the stamp is the clock's own time, not Japan's", "unix_s + 9 * 3600", "unix_s", 1),
    ("the month is one late", "(u8)m;", "(u8)(m + 1);", 1),
    # (f < 35 instead of f < 36 is no bug: frame 35 is FF FF FF FF .. FF FF, whose check byte is 0 either way)
    ("the PS1 card's first frame has no check byte", "\tfor (f = 0; f < 36; f++)\n\t{\n\t\tu8 *p = img + f * 128, x = 0;", "\tfor (f = 1; f < 36; f++)\n\t{\n\t\tu8 *p = img + f * 128, x = 0;", 1),
    ("the PS1 card's last frame is not the first", "\tmemcpy(img + 63 * 128, img, 128);\n", "", 1),
    ("a name of 29 characters passes", "if (n > VMC_NAME_MAX)", "if (n > VMC_NAME_MAX + 1)", 1),
    ("a dot passes as a name's character", "c == ' ' || c == '-'", "c == ' ' || c == '.' || c == '-'", 1),
    ("a space at the end passes", "if (name[0] == ' ' || name[n - 1] == ' ')", "if (name[0] == ' ')", 1),
    ("the same name in another case is not seen", "if (l != n + 4 || strncasecmp(file, name, n))", "if (l != n + 4 || strncmp(file, name, n))", 1),
    ("a PS1 card's name does not block a PS2 card", "return !strcasecmp(file + n, \".VM2\") || !strcasecmp(file + n, \".VM1\");", "return !strcasecmp(file + n, \".VM2\");", 1),
    # NOT in the list: "a new card is not read back" (drop the memcmp in vmc_new). It survives: nothing on the PC
    # makes a disk give back other bytes than it took. The read-back is there for the console's disk; untested.
    ("the half-made file is left behind", "\tif (r)\n\t{\n\t\tdrop_file(tmp);", "\tif (r)\n\t{", 1),
    ("a rename onto another card goes ahead", "\tt = name_taken(dir, new_name, old_file);", "\tt = 0;", 1),
    ("a rename takes a file that is not a card", "if (l < 5 || (strcasecmp(old_file + l - 4, \".VM2\") && strcasecmp(old_file + l - 4, \".VM1\")) || strchr(old_file, '/'))", "if (l < 5 || strchr(old_file, '/'))", 1),
    ("a rename takes a path", "\".VM1\")) || strchr(old_file, '/'))", "\".VM1\")))", 1),
    ("a renamed PS1 card becomes a PS2 card", "snprintf(new_file, new_file_n, \"%s%s\", new_name, old_file + l - 4);", "snprintf(new_file, new_file_n, \"%s.VM2\", new_name);", 1),
]


def run(path):
    env = dict(os.environ, VMCNEW=path)
    try:
        p = subprocess.run(["bash", os.path.join(HERE, "vmcnew_test.sh")], env=env, capture_output=True, text=True, errors="replace", timeout=900)
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
    path = os.path.join(OUT, "vmcnew.c")
    f = open(path, "w", newline="")
    f.write(src.replace(old, new))
    f.close()
    rc, out = run(path)
    if rc is None or "BUILD-BROKEN" in out:
        print("BROKEN   %-52s (%s)" % (name, "does not build or run" if rc is not None else out)); continue
    fails = [l for l in out.splitlines() if l.startswith("FAIL")]
    if fails:
        caught += 1
        print("caught   %-52s %s" % (name, fails[0][:60]))
    else:
        print("SURVIVED %-52s (exit %s)" % (name, rc))
print("vmcnew_mutants: %d of %d caught" % (caught, len(MUT)))
sys.exit(0 if caught == len(MUT) else 1)
