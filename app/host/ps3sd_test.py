"""Tests for src/pspsd_core.c ps3_args (Prism v0.82, PS3 ISOs played from a card slot, src/ps3sd.c), run in WSL:

    python3 host/ps3sd_test.py            the checks, then the planted bugs

The judge is a reader written here from webMAN-MOD's _Projects_/rawseciso/main.c (lines 1076-1100 and 1198-1201): the
args header (u64 device, u32 emu_mode, u32 sections, u32 tracks) is read as big-endian; emu_mode & 0x3FF is the type
handed to Cobra, (emu_mode & 3072) >> 10 the file mode (0 = raw: sections and sizes follow the header directly, every
section is device sectors); the disc is the sections' device sectors end to end and its size is the sum of the sizes
times the device's sector size (512 on the card). The "image" is random disc-sized bytes scattered over a pretend card
in runs placed out of order; the rebuilt disc must be the image byte for byte.
Planted bugs: each must make a check FAIL; the unplanted code must pass first (a crash is BROKEN, not a kill)."""
import os
import random
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
W = "/tmp/prism_ps3sd"
SD = 0x0103000100000010		# src/storage.h ST_DEV_SD
EMU_PS3 = 1
passes = fails = 0


def check(ok, what, quiet=False):
    global passes, fails
    if not quiet:
        print(("PASS  " if ok else "FAIL  ") + what)
    if ok:
        passes += 1
    else:
        fails += 1


def build(core):
    r = subprocess.run(["gcc", "-O2", "-Wall", "-I" + SRC, os.path.join(HERE, "pspsd_test.c"), core, "-o", W + "/t"],
                       capture_output=True, text=True)
    return r.stderr if r.returncode else None


def t(*a):
    return subprocess.run([W + "/t"] + [str(x) for x in a], capture_output=True, text=True)


def rebuild(ntfs, device_img, sec_size=512):
    """rawseciso's view of the file: (disc bytes, disc size it reports, type, mode_file, device, tracks)"""
    b = open(ntfs, "rb").read()
    dev, emu, ns, nt = struct.unpack_from(">QIII", b, 0)
    mode_file = (emu & 3072) >> 10
    if len(b) < 20 + 8 * ns:
        return None
    sections = struct.unpack_from(">%dI" % ns, b, 20)
    sizes = struct.unpack_from(">%dI" % ns, b, 20 + 4 * ns)
    disc = bytearray()
    # 11-27: a planted bug that swaps sections and sizes made this read ~30 GB and took the WSL VM down (twice). The
    # disc can never be bigger than the card: past that, it is wrong, and it is not read
    card_bytes = os.path.getsize(device_img)
    if sum(sizes) * sec_size > card_bytes or any(s * sec_size >= card_bytes for s in sections):
        return b"", sum(sizes) * sec_size, emu & 0x3FF, mode_file, dev, nt, len(b), ns
    with open(device_img, "rb") as d:
        for s, n in zip(sections, sizes):
            d.seek(s * sec_size)
            disc += d.read(n * sec_size)
    return bytes(disc), sum(sizes) * sec_size, emu & 0x3FF, mode_file, dev, nt, len(b), ns


def scenario(nsec2048, nruns, label, quiet=False):
    image = random.randbytes(nsec2048 * 2048)
    secs = len(image) // 512
    cuts = sorted(random.sample(range(1, secs), nruns - 1)) if nruns > 1 else []
    bounds = [0] + cuts + [secs]
    runs, at = [], 2048
    gap_max = max(16, min(4096, (64 << 20) // 512 // nruns))	# gaps add at most ~64 MB to the card
    for k in range(len(bounds) - 1):
        n = bounds[k + 1] - bounds[k]
        at += random.randrange(8, gap_max)
        runs.append((at, n, bounds[k]))
        at += n
    card = bytearray(random.randbytes((at + 64) * 512))	# the card ends after the last run (11-27: a fixed 64 MB card
    random.shuffle(runs)					# was overrun by 900 runs' gaps, and the test failed itself)
    for lba, n, first in runs:
        assert (lba + n) * 512 <= len(card)
        card[lba * 512:(lba + n) * 512] = image[first * 512:(first + n) * 512]
    runs.sort(key=lambda x: x[2])
    dev = W + "/card.img"
    open(dev, "wb").write(card)
    ntfs = W + "/game.ntfs[PS3ISO]"
    if os.path.exists(ntfs):
        os.remove(ntfs)
    r = t("ps3args", "%x" % SD, ntfs, *["%d:%d" % (l, n) for l, n, _ in runs])
    if r.returncode or not os.path.exists(ntfs):
        check(False, "%s: the .ntfs[PS3ISO] file was refused (%d)" % (label, r.returncode), quiet)
        return
    rb = rebuild(ntfs, dev)
    if rb is None:
        check(False, "%s: the file is shorter than its own section count says" % label, quiet)
        return
    disc, reported, typ, mode_file, devid, nt, flen, ns = rb
    check(devid == SD, "%s: device 0x%x is the SD slot" % (label, devid), quiet)
    check(typ == EMU_PS3 and mode_file == 0, "%s: type %d (EMU_PS3), file mode %d (raw)" % (label, typ, mode_file), quiet)
    check(nt == 0 and ns == len(runs) and flen == 20 + 8 * len(runs),
          "%s: 0 tracks, %d sections, %d bytes (20 + 8 x runs)" % (label, ns, flen), quiet)
    check(reported == len(image), "%s: webMAN's disc size %d = the image's %d" % (label, reported, len(image)), quiet)
    check(disc == image, "%s: the disc webMAN serves is the image, byte for byte (%d runs)" % (label, len(runs)), quiet)


def refusals(quiet=False):
    r = t("ps3args", "%x" % SD, W + "/x.ntfs", "100:0")
    check(r.returncode != 0, "a run of 0 sectors is refused", quiet)
    many = ["%d:4" % (1000 + 8 * i) for i in range(8200)]
    r = t("ps3args", "%x" % SD, W + "/y.ntfs", *many[:8189])
    check(r.returncode == 0, "8189 runs (webMAN's 64 KB) are written", quiet)
    r = subprocess.run([W + "/t", "ps3args", "%x" % SD, W + "/z.ntfs"] + many, capture_output=True, text=True)
    check(r.returncode != 0, "8200 runs (past webMAN's 64 KB) are refused", quiet)


def run_checks(quiet=False):
    random.seed(1127)
    scenario(1, 1, "one sector, one run", quiet)
    scenario(4096, 1, "8 MB, one run", quiet)
    scenario(4096, 37, "8 MB, 37 runs out of order", quiet)
    scenario(1500, 900, "3 MB, 900 runs", quiet)
    refusals(quiet)


PLANTS = [
    ("mode 5 (BD) instead of 1 (PS3)", "be32(out + 8, FI_EMU_PS3);", "be32(out + 8, 5);"),
    ("fake-ISO mode bit set", "be32(out + 8, FI_EMU_PS3);", "be32(out + 8, FI_EMU_PS3 | 0x800);"),
    ("sections and sizes swapped", "be32(sec + 4 * i, lba[i]);\n\t\tbe32(siz + 4 * i, cnt[i]);",
     "be32(sec + 4 * i, cnt[i]);\n\t\tbe32(siz + 4 * i, lba[i]);"),
    ("device low word only", "be32(out, (uint32_t)(device >> 32));\n\tbe32(out + 4, (uint32_t)device);\n\tbe32(out + 8, FI_EMU_PS3);",
     "be32(out + 4, (uint32_t)device);\n\tbe32(out + 8, FI_EMU_PS3);"),
    ("one section fewer counted", "be32(out + 12, (uint32_t)n);", "be32(out + 12, (uint32_t)(n - 1));"),
    ("a path block as in the PSP file", "sec = out + 20;\n\tsiz = sec + 4 * n;", "sec = out + 20 + 0x200;\n\tsiz = sec + 4 * n;"),
    ("no limit check", "if (n < 1 || total > cap || total > FI_ARGS_MAX)", "if (n < 1 || total > cap)"),
    ("sizes in 2048-byte units", "be32(siz + 4 * i, cnt[i]);", "be32(siz + 4 * i, cnt[i] / 4);"),
]


def main():
    global passes, fails
    shutil.rmtree(W, ignore_errors=True)
    os.makedirs(W)
    core = os.path.join(SRC, "pspsd_core.c")
    err = build(core)
    if err:
        print("BROKEN: the unplanted build failed\n" + err)
        return 2
    run_checks()
    print("%d passed, %d FAILED" % (passes, fails))
    if fails:
        return 1
    src = open(core, encoding="utf-8").read()
    start = src.index("int ps3_args(")
    killed = broken = 0
    for what, old, new in PLANTS:
        if src.count(old) < 1 or src.find(old, start) < 0:
            print("BROKEN  plant %r: its text is not in ps3_args" % what)
            broken += 1
            continue
        k = src.find(old, start)
        mut = src[:k] + new + src[k + len(old):]
        mp = W + "/mut.c"
        open(mp, "w", encoding="utf-8").write(mut)
        if build(mp):
            print("BROKEN  plant %r: did not compile" % what)
            broken += 1
            continue
        passes = fails = 0
        run_checks(quiet=True)
        if fails:
            killed += 1
            print("killed  %s (%d checks failed)" % (what, fails))
        else:
            print("SURVIVED  %s" % what)
    build(core)
    print("planted bugs: %d of %d killed, %d broken" % (killed, len(PLANTS), broken))
    return 0 if killed == len(PLANTS) else 1


if __name__ == "__main__":
    sys.exit(main())
