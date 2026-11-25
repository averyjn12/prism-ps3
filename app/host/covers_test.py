"""Tests for the Game covers core (src/covers_core.c), Prism v0.57. Run on Windows (PIL lives there):

    python host/covers_test.py            the checks, then the planted bugs
    python host/covers_test.py --quick    the checks only

The C is built and run in WSL (distro PSBBN) by host/covers_test.c. The judges are things covers_core.c
does not own:
  * PIL: Image.reduce (an exact box average) for the scaling, NEAREST for an enlargement
  * bench/lr_art.py build_pack: the PC tool the art file format comes from makes the OLD file
  * src/art.c, the app's own reader, compiled for the PC: it must find every cover in the NEW file
  * a reader written here from the format's description in lr_art.py
Planted bugs: each must make a check FAIL (a crash or a build failure is a broken run, not a catch),
and the unplanted code must pass first."""
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
SRC = os.path.join(APP, "src")
LR = os.path.dirname(APP)
sys.path.insert(0, os.path.join(LR, "bench"))
W = os.path.join(tempfile.gettempdir(), "prism_covers_test")
BIN = "/home/psbbn/prism_covers_test"
COVER = 128
PIX = COVER * COVER * 4
SECTOR = 2048

passes = fails = 0


def wslpath(p):
    p = os.path.abspath(p)
    return "/mnt/" + p[0].lower() + p[2:].replace("\\", "/")


def wsl(*args, **kw):
    return subprocess.run(["wsl", "-d", "PSBBN", "--"] + list(args), capture_output=True, text=True, **kw)


def check(ok, what):
    global passes, fails
    print(("PASS  " if ok else "FAIL  ") + what)
    if ok:
        passes += 1
    else:
        fails += 1


def build(core_c):
    wsl("mkdir", "-p", BIN)
    r = wsl("gcc", "-O2", "-Wall", "-I" + wslpath(SRC), wslpath(os.path.join(HERE, "covers_test.c")), wslpath(core_c), "-o", BIN + "/t")
    if r.returncode:
        return r.stderr
    shim = os.path.join(W, "shim")
    os.makedirs(shim, exist_ok=True)
    with open(os.path.join(shim, "ppu-types.h"), "w") as f:
        f.write("#include <stdint.h>\ntypedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;\n"
                "typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;\n")
    with open(os.path.join(W, "artread.c"), "w") as f:
        f.write('#include <stdio.h>\n#include "games.h"\n'
                "int main(int c, char **v) { int i, n = art_load_from(v[1]); printf(\"load %d\\n\", n);\n"
                " for (i = 2; i < c; i++) { const char *t; const tex_t *x = art_find(v[i], &t); unsigned long s = 0; int k;\n"
                "  if (x) for (k = 0; k < x->w * x->h * 4; k++) s = s * 31 + x->px[k];\n"
                "  printf(\"%s %s %lu %s\\n\", v[i], x ? \"cover\" : \"none\", s, t ? t : \"\"); } return 0; }\n")
    r = wsl("gcc", "-O2", "-w", "-I" + wslpath(shim), "-I" + wslpath(SRC), wslpath(os.path.join(W, "artread.c")), wslpath(os.path.join(SRC, "art.c")),
            "-o", BIN + "/artread")
    return r.stderr if r.returncode else None


def t(*args):
    return wsl(BIN + "/t", *[wslpath(a) if os.path.isabs(str(a)) else str(a) for a in args])


def read_art(path):
    """[(id, slot, plat, title, cover bytes or None)] from the format's description (lr_art.py docstring)."""
    with open(path, "rb") as f:
        b = f.read()
    head = b[:SECTOR].split(b"\0", 1)[0].decode("ascii")
    kv = dict(l.split("=", 1) for l in head.split("\n")[1:] if "=" in l)
    assert head.startswith("PRISMART1\n") and kv["w"] == "128" and kv["h"] == "128", head
    n, data = int(kv["count"]), int(kv["data"])
    out = []
    for i in range(n):
        r = b[SECTOR + 128 * i: SECTOR + 128 * (i + 1)]
        slot = struct.unpack_from("<H", r, 16)[0]
        cov = None if slot == 0xFFFF else b[data * SECTOR + slot * PIX: data * SECTOR + (slot + 1) * PIX]
        out.append((r[:16].split(b"\0", 1)[0].decode(), slot, r[18], r[20:].split(b"\0", 1)[0].decode("ascii", "replace"), cov))
    return out, len(b), data


def to_argb(im):
    rgb = im.convert("RGB").tobytes()
    out = bytearray(len(rgb) // 3 * 4)
    out[0::4] = b"\xff" * (len(rgb) // 3)
    out[1::4] = rgb[0::3]
    out[2::4] = rgb[1::3]
    out[3::4] = rgb[2::3]
    return bytes(out)


def scale(im, name, how="scale"):
    src = os.path.join(W, name + ".argb")
    dst = os.path.join(W, name + ".rgba")
    with open(src, "wb") as f:
        f.write(to_argb(im))
    r = t(how, src, str(im.width), str(im.height), dst)
    if r.returncode:
        return None
    with open(dst, "rb") as f:
        return f.read()


def rgb_of(rgba):
    return bytes(b for i, b in enumerate(rgba) if i % 4 != 3)


def run_checks(quiet=False):
    """All the checks; returns the number that failed."""
    global passes, fails
    passes = fails = 0
    r = t("unit", os.path.join(APP, "setup", "cover_sources.txt"))
    for line in r.stdout.splitlines():
        if line.startswith(("PASS", "FAIL")):
            check(line.startswith("PASS"), "unit: " + line[6:])
    check(r.returncode in (0, 1) and "----" in r.stdout, "unit: ran to the end (exit %d)" % r.returncode)

    random.seed(7)
    # 1. a 512 x 512 picture of noise: a factor of exactly 4, where PIL's reduce is the box average
    im = Image.frombytes("RGB", (512, 512), bytes(random.randrange(256) for _ in range(512 * 512 * 3)))
    got = scale(im, "noise")
    want = im.reduce(4).tobytes()
    if got is None:
        check(False, "scale 512x512: the tool failed")
    else:
        d = max(abs(a - b) for a, b in zip(rgb_of(got), want))
        check(d == 0, "scale 512x512 -> 128x128 = PIL's box average exactly, largest difference %d" % d)
        check(all(got[i] == 0x80 for i in range(3, PIX, 4)), "scale: every alpha byte is 0x80 (opaque on the PS2's GS)")
    # 2. a cover-shaped picture (600 x 848, not a whole factor): PIL BOX, which weighs edge pixels by part
    im = Image.new("RGB", (600, 848))
    px = im.load()
    for y in range(848):
        for x in range(600):
            px[x, y] = (x * 255 // 599, y * 255 // 847, ((x // 75) ^ (y // 106)) & 1 and 200 or 40)
    got = scale(im, "cover")
    want = im.resize((COVER, COVER), Image.BOX).tobytes()
    if got is None:
        check(False, "scale 600x848: the tool failed")
    else:
        diffs = [abs(a - b) for a, b in zip(rgb_of(got), want)]
        mean = sum(diffs) / len(diffs)
        check(mean < 2.0, "scale 600x848 (a real cover's shape) ~ PIL BOX: mean difference %.2f (< 2)" % mean)
    # 3. colours land in the right bytes: pure red, green, blue strips
    im = Image.new("RGB", (300, 300))
    for x in range(300):
        for y in range(300):
            im.putpixel((x, y), (255, 0, 0) if y < 100 else (0, 255, 0) if y < 200 else (0, 0, 255))
    got = scale(im, "rgb")
    ok = got is not None and got[0:4] == b"\xff\x00\x00\x80" and got[(64 * 128 + 5) * 4:(64 * 128 + 5) * 4 + 4] == b"\x00\xff\x00\x80" \
        and got[(127 * 128 + 127) * 4:] == b"\x00\x00\xff\x80"
    check(ok, "scale: red top, green middle, blue bottom come out as R G B A")
    # 4. smaller than a cover: every pixel doubled, as PIL's NEAREST does it
    im = Image.frombytes("RGB", (64, 64), bytes(random.randrange(256) for _ in range(64 * 64 * 3)))
    got = scale(im, "small")
    check(got is not None and rgb_of(got) == im.resize((COVER, COVER), Image.NEAREST).tobytes(), "scale 64x64 -> 128x128 = PIL NEAREST, exactly")

    # 4a. v0.58: an icon of noise, 512 x 256: its box is 128 x 64, a factor of exactly 4: PIL's reduce, exactly
    im = Image.frombytes("RGB", (512, 256), bytes(random.randrange(256) for _ in range(512 * 256 * 3)))
    got = scale(im, "iconnoise", "fit")
    if got is None:
        check(False, "fit 512x256: the tool failed")
    else:
        inside = bytearray()
        for y in range(32, 96):
            for x in range(COVER):
                inside += got[(y * COVER + x) * 4:(y * COVER + x) * 4 + 3]
        d = max(abs(a - b) for a, b in zip(inside, im.reduce(4).tobytes()))
        check(d == 0, "fit 512x256: rows 32..95 = PIL's box average exactly, largest difference %d" % d)
    # 4b. v0.58: a game's own icon keeps its shape: PS3's 320 x 176 and a tall one, letterboxed in the square
    for (w, h) in ((320, 176), (80, 144)):
        im = Image.new("RGB", (w, h))
        px = im.load()
        for y in range(h):
            for x in range(w):
                px[x, y] = (x * 255 // (w - 1), y * 255 // (h - 1), 90)
        got = scale(im, "icon%dx%d" % (w, h), "fit")
        if got is None:
            check(False, "fit %dx%d: the tool failed" % (w, h))
            continue
        if w >= h:
            bw, bh = COVER, (COVER * h + w // 2) // w
        else:
            bw, bh = (COVER * w + h // 2) // h, COVER
        x0, y0 = (COVER - bw) // 2, (COVER - bh) // 2
        stat = [sum(c) // (w * h) for c in zip(*im.getdata())]
        fill = bytes(v * 3 // 8 for v in stat) + b"\x80"
        inside = bytearray()
        outside_ok = True
        for y in range(COVER):
            for x in range(COVER):
                p = got[(y * COVER + x) * 4:(y * COVER + x) * 4 + 4]
                if x0 <= x < x0 + bw and y0 <= y < y0 + bh:
                    inside += p[:3]
                elif p != fill:
                    outside_ok = False
        want = im.resize((bw, bh), Image.BOX).tobytes()
        mean = sum(abs(a - b) for a, b in zip(inside, want)) / len(want)
        check(mean < 2.0, "fit %dx%d: the picture is a %dx%d box at (%d, %d), ~ PIL BOX (mean difference %.2f)" % (w, h, bw, bh, x0, y0, mean))
        check(outside_ok, "fit %dx%d: everything outside the box is the picture's average colour at 3/8 (%s)" % (w, h, fill[:3].hex()))

    # 4c. v0.59: the shape decides: the PSP icon webMAN left beside Bananafuzzbuddy's game is 144 x 80 (read 11-24)
    for (w, h, want_how) in ((144, 80, "fit"), (512, 724, "scale"), (256, 256, "scale"), (115, 100, "scale"), (116, 100, "fit")):
        im = Image.frombytes("RGB", (w, h), bytes(random.randrange(256) for _ in range(w * h * 3)))
        a = scale(im, "shape%dx%d" % (w, h), "cover")
        b = scale(im, "shape%dx%d_%s" % (w, h, want_how), want_how)
        check(a is not None and a == b, "cover %dx%d is done as %s (%s)" % (w, h, "an icon, shape kept" if want_how == "fit" else "a box, square filled",
                                                                       "the same bytes" if a == b else "DIFFERENT"))

    # 5. the art file: an old one made by bench/lr_art.py, covers added and replaced, read back three ways
    import lr_art
    pngs = []
    for k, c in enumerate([(200, 30, 30), (30, 200, 30)]):
        p = os.path.join(W, "old%d.png" % k)
        Image.new("RGB", (256, 256), c).save(p)
        pngs.append(p)
    old = os.path.join(W, "old.dat")
    with open(old, "wb") as f:
        f.write(lr_art.build_pack([("SLUS_212.43", "Metal Gear Solid 3: Snake Eater", pngs[0]), ("SLUS_209.46", "No Cover Yet", None),
                                   ("SLUS_205.52", "Second Game", pngs[1])]))
    olds, _, _ = read_art(old)
    covs = []
    for k in range(2):
        im = Image.new("RGB", (160, 230), (k * 90 + 10, 70, 160 - k * 50))
        covs.append(scale(im, "new%d" % k))
    new = os.path.join(W, "new.dat")
    r = t("merge", old, new, "SLUS_000.67", "1", "A PS1 Game", os.path.join(W, "new0.rgba"),
          "SLUS_209.46", "2", "ignored", os.path.join(W, "new1.rgba"), "SLUS_212.43", "2", "ignored", "-")
    check(r.returncode == 0, "merge: written (%s)" % r.stdout.strip())
    try:
        recs, size, data = read_art(new)
    except Exception as e:  # noqa: BLE001
        check(False, "merge: the new file does not read as an art file (%s)" % e)
        return fails
    ids = [x[0] for x in recs]
    check(ids == ["SLUS_212.43", "SLUS_209.46", "SLUS_205.52", "SLUS_000.67"], "merge: the old records in their order, the PS1 game after the PS2 ones: %s" % ids)
    by = {x[0]: x for x in recs}
    oby = {x[0]: x for x in olds}
    check(len(recs) == 4 and by["SLUS_212.43"][4] == oby["SLUS_212.43"][4], "merge: MGS3's cover from lr_art.py kept byte for byte")
    check(len(recs) == 4 and by["SLUS_205.52"][4] == oby["SLUS_205.52"][4], "merge: the other old cover kept byte for byte")
    check(len(recs) == 4 and by["SLUS_209.46"][4] == covs[1], "merge: the game lr_art.py had no cover for now has the new one")
    check(len(recs) == 4 and by["SLUS_000.67"][4] == covs[0] and by["SLUS_000.67"][2] == 1, "merge: the PS1 game's cover, marked PS1")
    check(len(recs) == 4 and by["SLUS_212.43"][3] == "Metal Gear Solid 3: Snake Eater" and by["SLUS_000.67"][3] == "A PS1 Game", "merge: titles kept / given")
    check(data == 32 and size == 32 * SECTOR + 4 * PIX, "merge: data at sector 32, then exactly 4 covers (%d bytes)" % size)
    r = wsl(BIN + "/artread", wslpath(new), "SLUS_212.43", "SLUS_209.46", "SLUS_205.52", "SLUS_000.67", "SLUS_999.99")
    lines = r.stdout.splitlines()

    def sum31(b):
        if b is None:
            return -1	# a record that lost its cover: no reader line can match this
        s = 0
        for x in b:
            s = (s * 31 + x) & 0xFFFFFFFFFFFFFFFF
        return s
    want = ["load 4"] + ["%s cover %d %s" % (i, sum31(by[i][4]), by[i][3]) for i in ids] + ["SLUS_999.99 none 0 "]
    check([l.rstrip() for l in lines] == [w.rstrip() for w in want], "the app's own reader (src/art.c) finds all 4 covers, the right bytes, and no fifth")
    r = t("parse", new)
    check(r.returncode == 0 and len(r.stdout.splitlines()) == 4, "covers_core reads its own file back: 4 records")

    # 5b. v0.58: PS3 and PSP records, and a cover the player removed (MGS3 here), then the file rewritten untouched
    new2 = os.path.join(W, "new2.dat")
    new3 = os.path.join(W, "new3.dat")
    r = t("merge", new, new2, "BLUS30386", "3", "", os.path.join(W, "new0.rgba"), "ULUS10041", "4", "", os.path.join(W, "new1.rgba"),
          "SLUS_212.43", "r", "", "-")
    r3 = t("merge", new2, new3)
    try:
        for label, path in (("after the removal", new2), ("rewritten again with nothing changed", new3)):
            recs, size, data = read_art(path)
            ids = [x[0] for x in recs]
            by = {x[0]: x for x in recs}
            check(ids == ["SLUS_212.43", "SLUS_209.46", "SLUS_205.52", "SLUS_000.67", "BLUS30386", "ULUS10041"],
                  "%s: PS2 records first, then PS1, PS3, PSP: %s" % (label, ids))
            check(len(recs) == 6 and by["SLUS_212.43"][1] == 0xFFFF and by["SLUS_212.43"][2] == 0x82 and by["SLUS_212.43"][3] == "",
                  "%s: the removed cover: no slot, marked removed (0x82), no title" % label)
            check(len(recs) == 6 and by["BLUS30386"][2] == 3 and by["BLUS30386"][4] == covs[0] and by["ULUS10041"][2] == 4 and by["ULUS10041"][4] == covs[1],
                  "%s: the PS3 and PSP covers, marked 3 and 4" % label)
            check(size == 32 * SECTOR + 5 * PIX, "%s: exactly 5 covers left in the file" % label)
        r = wsl(BIN + "/artread", wslpath(new3), "SLUS_212.43", "BLUS30386", "ULUS10041")
        lines = [l.split(" ")[:2] for l in r.stdout.splitlines()]
        check(lines == [["load", "6"], ["SLUS_212.43", "none"], ["BLUS30386", "cover"], ["ULUS10041", "cover"]],
              "the app's reader (art.c): MGS3 has no cover any more; the 9-letter PS3 and PSP IDs are found: %s" % lines)
    except Exception as e:  # noqa: BLE001
        check(False, "PS3 / PSP / removed: the files do not read (%s)" % e)

    # 6. limits: 300 PS2 + 300 PS1 records, every one with a cover
    cov_path = wslpath(os.path.join(W, "new0.rgba"))
    big = os.path.join(W, "big.dat")
    lst = os.path.join(W, "list.txt")

    def merge_list(rows):
        with open(lst, "w", newline="\n") as fh:
            fh.write("".join("%s\t%s\t\t%s\n" % r for r in rows))
        return wsl(BIN + "/t", "merge", "-", wslpath(big), "@" + wslpath(lst))
    rows = []
    for k in range(300):
        rows.append(("SLUS_%03d.%02d" % (200 + k // 100, k % 100), "2", cov_path))
        rows.append(("SLUS_%03d.%02d" % (k // 100, k % 100), "1", cov_path))
    r = merge_list(rows)
    try:
        recs, size, data = read_art(big)
        check(len(recs) == 496, "limits: 600 games -> 496 records (sectors 1..31): %d" % len(recs))
        check(all(x[2] == 2 for x in recs[:256]), "limits: the PS2 launcher's first 256 records are all PS2 games")
        # (the 511-cover cap cannot bind: 496 records come first. Every kept record keeps its cover)
        check(sum(1 for x in recs if x[4] is not None) == 496, "limits: all 496 kept records keep their covers")
        check(size <= 32 * 1024 * 1024, "limits: the file fits art.c's and the stage2's 32 MiB (%d bytes)" % size)
    except Exception as e:  # noqa: BLE001
        check(False, "limits: the big file does not read (%s)" % e)
    # 525 PS2 games with covers: more than 511 records would hold, so only the record cap and cover cap together decide
    r = merge_list([("SCES_%03d.%02d" % (500 + k // 100, k % 100), "2", cov_path) for k in range(525)])
    try:
        recs, size, data = read_art(big)
        check(len(recs) == 496 and all(x[1] == i for i, x in enumerate(recs)) and size == 32 * SECTOR + 496 * PIX,
              "limits: 525 PS2 covers -> 496 records, slots 0..495 in order, nothing past the covers")
    except Exception as e:  # noqa: BLE001
        check(False, "limits 2: %s" % e)
    if not quiet:
        print("---- %d passed, %d failed" % (passes, fails))
    return fails


MUTANTS = [
    ("the dash form keeps the dot", "memcpy(out + 8, id + 9, 2);", "memcpy(out + 8, id + 8, 2);"),
    ("{DASH} expands to the plain ID", "put = dash, len = 6;", "put = id, len = 6;"),
    ("a result one byte too long accepted", "if (k + m >= n)", "if (k + m > n)"),
    ("plain http:// sources accepted", "else if (b[1] == '/' && strlen(b + 1) < sizeof s.path)", "else if (strlen(b + 1) < sizeof s.path)"),
    ("platform digits read one off", "if (strchr(a + 1, '0' + p))", "if (strchr(a + 1, '1' + p))"),
    ("PS3 / PSP serials refused", "if (n != 11 && n != 9)", "if (n != 11)"),
    ("a PS3 serial's dash form one digit short", "memcpy(out + 5, id + 4, 5);", "memcpy(out + 5, id + 4, 4);"),
    ("game: accepted with no file name", 'if (!strncmp(b + 1, "game:", 5) && b[6] && strlen(b + 6) < sizeof s.path)',
     'if (!strncmp(b + 1, "game:", 5) && strlen(b + 6) < sizeof s.path)'),
    ("game: sources made to wait for a folder (0.58's bug, the PSP icon never offered)", "if (s->local != 1)\n\t\treturn 0;",
     'if (s->local == 0)\n\t\treturn 0;\n\tif (s->local == 2)\n\t{\n\t\tsnprintf(out, n, "/dev_hdd0/none");\n\t\treturn 1;\n\t}'),
    ("a cover pack offered without its folder", "if (s->local != 1)\n\t\treturn 0;", "return 0;"),
    ("every picture letterboxed", "if ((long long)w * 100 > (long long)h * 115)", "if (1)"),
    ("no picture letterboxed", "if ((long long)w * 100 > (long long)h * 115)", "if (0)"),
    ("the shape threshold at 1.25", "if ((long long)w * 100 > (long long)h * 115)", "if ((long long)w * 100 > (long long)h * 125)"),
    ("beside: card paths taken for the console's", 'strncmp(game_path, "/dev_", 5) || ', ""),
    ("beside: a dot in a folder name taken for the extension", "|| dot < slash ", ""),
    ("beside: one name fewer", "if (k < 0 || k > 3 ", "if (k < 0 || k > 2 "),
    ("beside: a name one byte too long accepted", "if (base + 5 > n)", "if (base + 4 > n)"),
    ("an icon squeezed instead of letterboxed", "bh = (int)(((long long)CV_COVER * h + w / 2) / w);", "bh = CV_COVER;"),
    ("an icon put at the top, not centred", "y0 = (CV_COVER - bh) / 2;", "y0 = 0;"),
    ("the fill not darkened", "uint8_t fr = (uint8_t)(sr / n * 3 / 8)", "uint8_t fr = (uint8_t)(sr / n)"),
    ("a removed cover's mark not read back", "e->refused = r[18] & CV_REFUSED;", "e->refused = 0;"),
    ("a removed record's platform taken with the mark in it",
     "e->plat = (r[18] & 0x7F) >= CV_PS1 && (r[18] & 0x7F) <= CV_PSP ? r[18] & 0x7F : 0;", "e->plat = r[18];"),
    ("the removed mark not written", "r[18] = (uint8_t)(x->plat | (x->refused ? CV_REFUSED : 0));", "r[18] = (uint8_t)x->plat;"),
    ("red and blue swapped (box covers)", "r += p[1];", "r += p[3];", 0),
    ("red and blue swapped (icons)", "r += p[1];", "r += p[3];", 1),
    ("truncated instead of rounded (icons)", "o[0] = (uint8_t)((r + cnt / 2) / cnt);", "o[0] = (uint8_t)(r / cnt);", 1),
    ("alpha left at 0xFF", "o[3] = 0x80;", "o[3] = 0xFF;"),
    ("the last source row of each box dropped", "for (y = y0; y < y1; y++)", "for (y = y0; y < y1 - (y1 - y0 > 1); y++)"),
    ("truncated instead of rounded (box covers)", "o[0] = (uint8_t)((r + cnt / 2) / cnt);", "o[0] = (uint8_t)(r / cnt);", 0),
    ("PS1 games written first", "if ((pass == 0) == (e[i].plat == CV_PS2 || e[i].plat == 0) && cv_is_id(e[i].id))",
     "if ((pass == 1) == (e[i].plat == CV_PS2 || e[i].plat == 0) && cv_is_id(e[i].id))"),
    ("no record cap", "for (i = 0; i < n && n_ord < CV_REC_MAX; i++)", "for (i = 0; i < n; i++)"),
    # not planted: "no cover cap" (if (x->px && slot < CV_COV_MAX) -> if (x->px)) is an equivalent change,
    # since CV_REC_MAX (496) < CV_COV_MAX (511) and the record cap applies first
    ("slots counted from 1", "s = slot++;", "s = ++slot;"),
    ("the platform byte not written", "r[18] = (uint8_t)(x->plat | (x->refused ? CV_REFUSED : 0));", "r[18] = (uint8_t)(x->refused ? CV_REFUSED : 0);"),
    ("an old cover's slot read one off", "long off = (long)data * CV_SECTOR + (long)slot * CV_PIX;", "long off = (long)data * CV_SECTOR + (long)(slot + 1) * CV_PIX;"),
    ("data written one sector early", "long at = CV_SECTOR + 128L * n_ord, to = (long)CV_DATA_SEC * CV_SECTOR;",
     "long at = CV_SECTOR + 128L * n_ord, to = (long)(CV_DATA_SEC - 1) * CV_SECTOR;"),
]


def main():
    shutil.rmtree(W, ignore_errors=True)
    os.makedirs(W)
    core = os.path.join(SRC, "covers_core.c")
    err = build(core)
    if err:
        print("build failed:\n" + err)
        return 1
    f = run_checks()
    if f or "--quick" in sys.argv:
        return 1 if f else 0
    with open(core) as fh:
        code = fh.read()
    caught = 0
    for m in MUTANTS:
        name, a, b = m[:3]
        which = m[3] if len(m) > 3 else None	# a line that is there more than once: which copy to change
        if (which is None and code.count(a) != 1) or (which is not None and code.count(a) <= which):
            print("FAIL  planted bug %s: the line to change is not there %s" % (name, "exactly once" if which is None else "%d times" % (which + 1)))
            continue
        at = -1
        for _ in range((which or 0) + 1):
            at = code.index(a, at + 1)
        mp = os.path.join(W, "covers_core_mut.c")
        with open(mp, "w") as fh:
            fh.write(code[:at] + b + code[at + len(a):])
        err = build(mp)
        if err:
            print("FAIL  planted bug %s: does not build (a broken run, not a catch)" % name)
            continue
        try:
            nf = run_checks(quiet=True)
        except Exception as e:  # noqa: BLE001
            print("FAIL  planted bug %s: the checks crashed (%s): a broken run, not a catch" % (name, e))
            continue
        if nf:
            caught += 1
            print("PASS  planted bug caught: %s (%d check(s) failed)" % (name, nf))
        else:
            print("FAIL  planted bug NOT caught: %s" % name)
    build(core)
    print("---- planted bugs: %d of %d caught" % (caught, len(MUTANTS)))
    return 0 if caught == len(MUTANTS) else 1


if __name__ == "__main__":
    sys.exit(main())
