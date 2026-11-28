"""Tests for src/pspsd_core.c psx_args and psx_cue (Prism v0.82/1.00: PS1 images played from an exFAT card or USB drive
through webMAN's rawseciso), run in WSL:

    python3 host/ps1sd_test.py            the checks, then the planted bugs

The judge is a reader written here from webMAN-MOD's _Projects_/rawseciso/main.c 1076-1100 and 1144-1162 (and prepISO's
prep.h build_file for the layout): the header (u64 device, u32 emu_mode, u32 sections, u32 tracks) big-endian;
emu_mode & 0x3FF must be EMU_PSX (4) with file mode 0 (raw); sections then sizes (device sectors) follow the header;
for EMU_PSX the track descriptors follow the sizes, tracks & 0xFF of them; when tracks > 0xFF the CD sector size is
(tracks & 0xFFFF00) >> 4, else 2352. The disc is the sections' device sectors end to end; it must be the image byte
for byte, the sector size must be the image's, and the track table must be the cue's (track n: number n, 0x14 data /
0x10 audio, its INDEX 01 sector).
Cue sheets: a one-track data cue, a data + 3 audio tracks cue (the usual PS1 layout), and refusals (two FILEs, no
TRACK, a missing INDEX 01, out-of-order tracks).
Planted bugs: each must make a check FAIL; the unplanted code must pass first (a crash is BROKEN, not a kill)."""
import os
import random
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
W = "/tmp/prism_ps1sd"
USB3 = 0x010300000000000D		# USB port 3 (rawseciso storage.h USB_MASS_STORAGE_1(3))
EMU_PSX = 4
passes = fails = 0

CUE_ONE = 'FILE "Game (USA).bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
CUE_AUDIO = ('FILE "Game (USA) (Track 1).bin" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n'
             '  TRACK 02 AUDIO\r\n    INDEX 00 10:00:00\r\n    INDEX 01 10:02:00\r\n'
             '  TRACK 03 AUDIO\r\n    INDEX 00 12:30:10\r\n    INDEX 01 12:32:10\r\n'
             '  track 04 audio\r\n    index 01 15:00:74\r\n')
CUE_BAD = {
    # (11-27: its second track first started at 00:00:00 too, so the order rule refused it and the FILE rule was never
    # tested: a planted "two FILEs allowed" survived. Now only the FILE count is wrong in it)
    "two FILEs": 'FILE "a.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\nFILE "b.bin" BINARY\n TRACK 02 AUDIO\n INDEX 01 00:10:00\n',
    "no TRACK": 'FILE "a.bin" BINARY\n',
    "a TRACK with no INDEX 01": 'FILE "a.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 00 00:00:00\n',
    "tracks out of order": 'FILE "a.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:10:00\n TRACK 02 AUDIO\n INDEX 01 00:05:00\n',
    "a bad time": 'FILE "a.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:61:00\n',
}


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


def judge(ntfs, device_img, dev_sec=512):
    """rawseciso's EMU_PSX view of the file, or None when the file is shorter than it says"""
    b = open(ntfs, "rb").read()
    dev, emu, ns, nt = struct.unpack_from(">QIII", b, 0)
    if len(b) < 20 + 8 * ns:
        return None
    sections = struct.unpack_from(">%dI" % ns, b, 20)
    sizes = struct.unpack_from(">%dI" % ns, b, 20 + 4 * ns)
    cd_sec = (nt & 0xFFFF00) >> 4 if nt > 0xFF else 2352
    count = nt & 0xFF
    tracks = []
    off = 20 + 8 * ns
    if len(b) < off + 8 * count:
        return None
    for k in range(count):
        _r, adr, num, _r2, start = struct.unpack_from(">BBBBI", b, off + 8 * k)
        tracks.append((num, adr, start))
    card_bytes = os.path.getsize(device_img)
    disc = b""
    if sum(sizes) * dev_sec <= card_bytes and all(s * dev_sec < card_bytes for s in sections):
        with open(device_img, "rb") as d:
            parts = []
            for s, n in zip(sections, sizes):
                d.seek(s * dev_sec)
                parts.append(d.read(n * dev_sec))
            disc = b"".join(parts)
    return dict(dev=dev, type=emu & 0x3FF, mode=(emu & 3072) >> 10, disc=disc, cd_sec=cd_sec, tracks=tracks, length=len(b), ns=ns)


def scenario(label, sector_size, nsec, nruns, cue_text, want_tracks, quiet=False):
    image = random.randbytes(sector_size * nsec)
    secs = (len(image) + 511) // 512	# a 2352-byte-sector image ends inside a device sector, as on a real card
    padded = image + bytes(secs * 512 - len(image))
    cuts = sorted(random.sample(range(1, secs), nruns - 1)) if nruns > 1 else []
    bounds = [0] + cuts + [secs]
    runs, at = [], 4096
    for k in range(len(bounds) - 1):
        n = bounds[k + 1] - bounds[k]
        at += random.randrange(8, 512)
        runs.append((at, n, bounds[k]))
        at += n
    card = bytearray(random.randbytes((at + 64) * 512))
    random.shuffle(runs)
    for lba, n, first in runs:
        card[lba * 512:(lba + n) * 512] = padded[first * 512:(first + n) * 512]
    runs.sort(key=lambda x: x[2])
    dev = W + "/usb.img"
    open(dev, "wb").write(card)
    cue = "-"
    if cue_text is not None:
        cue = W + "/game.cue"
        open(cue, "w", newline="").write(cue_text)
    ntfs = W + "/game.ntfs[PSXISO]"
    if os.path.exists(ntfs):
        os.remove(ntfs)
    r = t("psxargs", "%x" % USB3, ntfs, sector_size, cue, *["%d:%d" % (l, n) for l, n, _ in runs])
    if r.returncode or not os.path.exists(ntfs):
        check(False, "%s: the .ntfs[PSXISO] file was refused (%d)" % (label, r.returncode), quiet)
        return
    j = judge(ntfs, dev)
    if j is None:
        check(False, "%s: the file is shorter than its own counts say" % label, quiet)
        return
    check(j["dev"] == USB3 and j["type"] == EMU_PSX and j["mode"] == 0,
          "%s: device 0x%x (USB 3), type %d (EMU_PSX), file mode %d (raw)" % (label, j["dev"], j["type"], j["mode"]), quiet)
    check(j["cd_sec"] == sector_size, "%s: rawseciso reads %d-byte CD sectors (the image has %d)" % (label, j["cd_sec"], sector_size), quiet)
    want = [(k + 1, 0x10 if audio else 0x14, lba) for k, (lba, audio) in enumerate(want_tracks)]
    check(j["tracks"] == want, "%s: tracks %s" % (label, j["tracks"]), quiet)
    check(j["length"] == 20 + 8 * len(runs) + 8 * len(want_tracks),
          "%s: %d bytes (20 + 8 x %d runs + 8 x %d tracks)" % (label, j["length"], len(runs), len(want_tracks)), quiet)
    check(len(j["disc"]) == secs * 512 and j["disc"][:len(image)] == image,
          "%s: the disc webMAN serves is the image, byte for byte (%d runs, the last device sector partly used)" % (label, len(runs)), quiet)


def cue_checks(quiet=False):
    for name, text in CUE_BAD.items():
        p = W + "/bad.cue"
        open(p, "w", newline="").write(text)
        r = t("cue", p)
        check(r.returncode == 6, "cue refused: %s" % name, quiet)


def run_checks(quiet=False):
    random.seed(1127)
    scenario("2352, no cue, one run", 2352, 400, 1, None, [(0, 0)], quiet)
    scenario("2352, one-track cue, 23 runs", 2352, 3000, 23, CUE_ONE, [(0, 0)], quiet)
    scenario("2352, data + 3 audio tracks (CRLF, lower case), 9 runs", 2352, 70000, 9, CUE_AUDIO,
             [(0, 0), ((10 * 60 + 2) * 75, 1), ((12 * 60 + 32) * 75 + 10, 1), (15 * 60 * 75 + 74, 1)], quiet)
    scenario("2048 (.iso), no cue, 5 runs", 2048, 2000, 5, None, [(0, 0)], quiet)
    cue_checks(quiet)
    r = t("psxargs", "%x" % USB3, W + "/z.ntfs", 2352, "-", "100:0")
    check(r.returncode != 0, "a run of 0 sectors is refused", quiet)


PLANTS = [
    ("type BD instead of PSX", "be32(out + 8, FI_EMU_PSX);", "be32(out + 8, 5);"),
    ("no sector size parameter", "be32(out + 16, (uint32_t)ntracks | param);", "be32(out + 16, (uint32_t)ntracks);"),
    ("the parameter shifted by 8 for 2048", "(sector_size % 16 ? (uint32_t)sector_size << 8 : (uint32_t)sector_size << 4)",
     "((uint32_t)sector_size << 8)"),
    ("audio marked as data", "trk[8 * i + 1] = tracks[i].audio ? 0x10 : 0x14;", "trk[8 * i + 1] = 0x14;"),
    ("tracks numbered from 0", "trk[8 * i + 2] = (uint8_t)(i + 1);", "trk[8 * i + 2] = (uint8_t)i;"),
    ("tracks placed before the sizes", "trk = siz + 4 * n;", "trk = siz;"),
    ("INDEX 00 taken for 01", 'if (!strcmp(w, "01") || !strcmp(w, "1"))', 'if (!strcmp(w, "00") || !strcmp(w, "01"))'),
    ("the frames ignored", "tracks[n - 1].lba = (mm * 60 + ss) * 75 + ff;", "tracks[n - 1].lba = (mm * 60 + ss) * 75;"),
    ("two FILEs allowed", "if (files != 1 || n == 0 || want_index)", "if (files < 1 || n == 0 || want_index)"),
    ("TRACK case-sensitive", 'tracks[n].audio = !strcmp(w, "AUDIO");', 'tracks[n].audio = !strcmp(w, "AUDIO") && 0;'),
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
    start = src.index("int psx_args(")
    killed = broken = 0
    for what, old, new in PLANTS:
        k = src.find(old, start)
        if k < 0 or src.count(old, start) != 1:
            print("BROKEN  plant %r: its text is not in the PS1 part exactly once" % what)
            broken += 1
            continue
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
