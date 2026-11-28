"""Tests for src/pspsd_core.c (Prism v0.60, PSP games from a card slot), run in WSL:

    python3 host/pspsd_test.py            the checks, then the planted bugs

The judges are things pspsd_core.c does not own:
  * a reader written here from webMAN-MOD's include/mount/rawseciso.h (how webMAN turns a .ntfs[PSPISO]
    file into the bytes of /dev_bdvd: section 0 read from the header FILE in 2048-byte units, the rest from
    the device in its own sector size; the disc's size = the device sections + 2048 x sections[0])
  * Linux's own ISO 9660 / Joliet driver: the rebuilt disc is loop-mounted (sudo), and the file in it must
    have the image's name and be the image, byte for byte
The "image" is a real disc image (Prism's Tiny Tees ISO) scattered over a pretend card in runs out of order.
Planted bugs: each must make a check FAIL; the unplanted code must pass first."""
import os
import random
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
W = "/tmp/prism_pspsd"
SD = 0x0103000100000010		# the SD slot's storage device (src/storage.h ST_DEV_SD)
passes = fails = 0


def check(ok, what):
    global passes, fails
    print(("PASS  " if ok else "FAIL  ") + what)
    if ok:
        passes += 1
    else:
        fails += 1


def build(core):
    r = subprocess.run(["gcc", "-O2", "-Wall", "-I" + SRC, os.path.join(HERE, "pspsd_test.c"), core, "-o", W + "/t"], capture_output=True, text=True)
    return r.stderr if r.returncode else None


def t(*a):
    return subprocess.run([W + "/t"] + [str(x) for x in a], capture_output=True, text=True)


def both_endian(b, off):
    """an ISO 9660 733 number: (little-endian half, big-endian half). The PS3 is big-endian; Linux reads
    only the first half, so a header can pass Linux with the second one wrong"""
    return struct.unpack_from("<I", b, off)[0], struct.unpack_from(">I", b, off + 4)[0]


def header_numbers(hdr, size, label):
    """both trees, walked from their own volume descriptors: the volume's size and the file's place and
    size, each in both byte orders"""
    want_vol = 0xD000 // 2048 + (size + 2047) // 2048
    ok = True
    for vd in (0x8000, 0x8800):
        vol = both_endian(hdr, vd + 80)
        root = vd + 156
        rext, rsize = both_endian(hdr, root + 2)[0], both_endian(hdr, root + 10)[0]
        p, found = rext * 2048, None
        while p < rext * 2048 + rsize and hdr[p]:
            nl = hdr[p + 32]
            if nl and not (nl == 1 and hdr[p + 33] in (0, 1)):
                found = (both_endian(hdr, p + 2), both_endian(hdr, p + 10))
            p += hdr[p]
        ok &= vol == (want_vol, want_vol) and found == ((26, 26), (size, size))
    check(ok, "%s: both trees, both byte orders: volume %d sectors, the file at sector 26, %d bytes" % (label, want_vol, size))


def rebuild_disc(ntfs, device_img):
    """webMAN's rawseciso.h, fake-ISO mode, in Python: (disc bytes, disc size webMAN reports, emu_mode).
    None when webMAN would not read the file as a fake ISO at all"""
    b = open(ntfs, "rb").read()
    dev, emu, ns, nt = struct.unpack_from(">QIII", b, 0)
    mode_file = (emu & 3072) >> 10
    if mode_file != 2:
        return None
    path = b[20:20 + 0x200].split(b"\0", 1)[0].decode()
    sections = struct.unpack_from(">%dI" % ns, b, 20 + 0x200)
    sizes = struct.unpack_from(">%dI" % ns, b, 20 + 0x200 + 4 * ns)
    disc = bytearray(open(path, "rb").read()[:sizes[0] * 2048])
    with open(device_img, "rb") as d:
        for i in range(1, ns):
            d.seek(sections[i] * 512)
            disc += d.read(sizes[i] * 512)
    reported = sum(sizes[1:]) * 512 + 2048 * sections[0]	# rawseciso.h:969-973, 1072-1073
    return bytes(disc), reported, emu, dev, nt


def mount_and_read(disc_path, opts="ro,loop"):
    """the files Linux sees on the disc, or (None, why): a mount or read error is a result, not a crash"""
    mnt = W + "/mnt"
    os.makedirs(mnt, exist_ok=True)
    subprocess.run(["sudo", "umount", mnt], capture_output=True)
    r = subprocess.run(["sudo", "mount", "-o", opts, disc_path, mnt], capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr.strip()
    try:
        out = {}
        for n in os.listdir(mnt):
            with open(os.path.join(mnt, n), "rb") as f:
                out[n] = f.read()
        return out, ""
    except OSError as e:
        return None, str(e)
    finally:
        subprocess.run(["sudo", "umount", mnt], capture_output=True)


def scenario(name, image, runs_order, label):
    """image scattered over a pretend card in 512-byte runs; returns nothing, records checks"""
    hdr = W + "/hdr.bin"
    r = t("header", name, len(image), hdr)
    if r.returncode:
        check(False, "%s: the header was refused (%d)" % (label, r.returncode))
        return
    # the pretend card: 64 MiB of noise, the image in runs at scattered places
    card = bytearray(random.randbytes(64 << 20))
    secs = (len(image) + 511) // 512
    cuts = sorted(random.sample(range(1, secs), runs_order - 1)) if runs_order > 1 else []
    bounds = [0] + cuts + [secs]
    runs, at = [], 2048
    for k in range(len(bounds) - 1):
        n = bounds[k + 1] - bounds[k]
        at += random.randrange(64, 4096)
        runs.append((at, n, bounds[k]))
        at += n
    random.shuffle(runs)		# placed out of order on the card ...
    for lba, n, first in runs:
        chunk = image[first * 512:(first + n) * 512]
        card[lba * 512:lba * 512 + len(chunk)] = chunk
    runs.sort(key=lambda x: x[2])	# ... but listed in the file's order, as the card reader gives them
    dev = W + "/card.img"
    open(dev, "wb").write(card)
    ntfs = W + "/game.ntfs[PSPISO]"
    r = t("args", "%x" % SD, hdr, ntfs, *["%d:%d" % (l, n) for l, n, _ in runs])
    if r.returncode:
        check(False, "%s: the .ntfs file was refused (%d)" % (label, r.returncode))
        return
    header_numbers(open(hdr, "rb").read(), len(image), label)
    rb = rebuild_disc(ntfs, dev)
    if rb is None:
        check(False, "%s: webMAN would not read this .ntfs file as a fake ISO (mode_file is not 2)" % label)
        return
    disc, reported, emu, devid, nt = rb
    check(devid == SD and emu == 0x805 and nt == 0, "%s: device 0x%x (the SD slot), mode 0x%x (BD, fake ISO), %d tracks" % (label, devid, emu, nt))
    check(reported == len(disc) and len(disc) == 0xD000 + secs * 512,
          "%s: webMAN's disc size %d = header + image (%d)" % (label, reported, 0xD000 + secs * 512))
    dp = W + "/disc.iso"
    open(dp, "wb").write(disc)
    files, err = mount_and_read(dp)
    if files is None:
        check(False, "%s: Linux would not mount the rebuilt disc: %s" % (label, err))
        return
    want = name if len(name.encode()) <= 110 else None
    names = list(files)
    if want:
        check(names == [want], "%s: Linux (Joliet) shows one file, named %r: %r" % (label, want, names))
    else:
        check(len(names) == 1 and names[0].endswith(".iso") and len(names[0].encode()) <= 110,
              "%s: a long name is cut to 110 bytes, .iso kept: %r" % (label, names[0] if names else None))
    got = files[names[0]] if names else b""
    check(got == image, "%s: the file on the disc is the image, byte for byte (%d bytes, %d runs)" % (label, len(image), len(runs)))
    files2, err2 = mount_and_read(dp, "ro,loop,norock,nojoliet")
    check(files2 is not None and len(files2) == 1 and list(files2.values())[0] == image,
          "%s: without Joliet, the plain ISO 9660 record finds the same bytes%s" % (label, "" if files2 is not None else " (%s)" % err2))


def run_checks(quiet=False):
    global passes, fails
    passes = fails = 0
    random.seed(424)
    image = open(os.path.join(HERE, "..", "setup", "tinytees.iso"), "rb").read()
    scenario("Call of Duty - Roads to Victory (USA).iso", image, 7, "a PSP-style name, 7 runs")
    scenario("Tiny Tees.iso", image, 1, "one run")
    scenario("パズル (Japan) été.iso", image, 3, "a Japanese / accented name")
    scenario("A" * 140 + ".iso", image, 2, "a 144-byte name")
    # refusals
    check(t("header", "x.iso", 0, W + "/h0").returncode != 0, "refused: an empty image")
    check(t("header", "x.iso", 0x100000000, W + "/h1").returncode != 0, "refused: an image of 4 GiB or more")
    check(t("header", "a/b.iso", 2048, W + "/h2").returncode != 0, "refused: a name with a slash")
    many = ["%d:1" % (100 + 2 * k) for k in range(8200)]
    check(t("args", "%x" % SD, W + "/hdr.bin", W + "/big", *many).returncode != 0, "refused: more runs than webMAN's 64 KB holds")
    check(t("args", "%x" % SD, W + "/hdr.bin", W + "/z", "100:0").returncode != 0, "refused: a run of no sectors")
    check(t("args", "%x" % SD, "/" + "p" * 0x200, W + "/z2", "100:4").returncode != 0, "refused: a header path longer than 0x1FF")
    if not quiet:
        print("---- %d passed, %d failed" % (passes, fails))
    return fails


MUTANTS = [
    ("the image's first sector one off", "set733(p + 2, lba);", "set733(p + 2, lba + 1);"),
    ("prepISO's primary record place (over '..')", "uint8_t *idr = out + 0xB844", "uint8_t *idr = out + 0xB840"),
    ("the size's big-endian half dropped", "p[4] = (uint8_t)(n >> 24); p[5] = (uint8_t)(n >> 16); p[6] = (uint8_t)(n >> 8); p[7] = (uint8_t)n;", "p[4] = p[5] = p[6] = p[7] = 0;"),
    ("Joliet written little-endian", "out[k++] = (uint8_t)(c >> 8);\n\t\t\tout[k++] = (uint8_t)c;", "out[k++] = (uint8_t)c;\n\t\t\tout[k++] = (uint8_t)(c >> 8);"),
    ("the volume size not grown", "last_lba += (uint32_t)((size + 0x7FF) / 0x800);", "last_lba += 0;"),
    ("Joliet's volume size left alone", "set733(out + 0x8800 + 80, last_lba);", ";"),
    ("section 0 started at 0, as prepISO does", "be32(sec, FI_HDR_SECTORS);", "be32(sec, 0);"),
    ("not fake-ISO mode", "be32(out + 8, FI_EMU_BD | FI_FAKE_ISO);", "be32(out + 8, FI_EMU_BD);"),
    ("the runs' sizes in 2048s", "be32(siz + 4 * (i + 1), cnt[i]);", "be32(siz + 4 * (i + 1), cnt[i] / 4);"),
    ("runs one sector late", "be32(sec + 4 * (i + 1), lba[i]);", "be32(sec + 4 * (i + 1), lba[i] + 1);"),
    ("the device's halves swapped", "be32(out, (uint32_t)(device >> 32));\n\tbe32(out + 4, (uint32_t)device);", "be32(out + 4, (uint32_t)(device >> 32));\n\tbe32(out, (uint32_t)device);"),
    ("a long name not cut", "if (len_name > MAX_NAME_LEN)\n\t{", "if (0)\n\t{"),
    ("4 GiB accepted", "size > 0xFFFFF800ull", "size > 0xFFFFFFFFFFull"),
    ("the 64 KB limit not kept", "total > cap || total > FI_ARGS_MAX", "total > cap"),
]


def main():
    shutil.rmtree(W, ignore_errors=True)
    os.makedirs(W)
    core = os.path.join(SRC, "pspsd_core.c")
    err = build(core)
    if err:
        print("build failed:\n" + err)
        return 1
    if run_checks():
        return 1
    code = open(core).read()
    # v0.82: pspsd_core.c also holds ps3_args (tested by host/ps3sd_test.py), which repeats two of these lines: the
    # PSP plants go into the PSP half only (everything before ps3_args)
    cut = code.find("\nint ps3_args(")
    head, tail = (code[:cut], code[cut:]) if cut >= 0 else (code, "")
    caught = 0
    for name, a, b in MUTANTS:
        if head.count(a) != 1:
            print("FAIL  planted bug %s: the line to change is not there exactly once" % name)
            continue
        mp = W + "/pspsd_core_mut.c"
        open(mp, "w").write(head.replace(a, b) + tail)
        if build(mp):
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
