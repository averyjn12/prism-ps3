"""PS1 card (.VM1) fixtures and an independent check of the app's PS1 copy / delete (2026-11-22).
Written apart from src/vmcw.c, from psx-spx "Memory Card Data Format".

    python ps1_card.py make  <A.VM1> <B.VM1>    A: a 2-block save BASLUS-00594MGS0 (blocks 1-2)
                                                 and a 1-block save BASLUS-00594MGS1 (block 3);
                                                 B: a 1-block save in block 1, a DELETED (A1)
                                                 frame in block 2, the rest free.
                                                 Writes A.orig / B.orig beside them.
    python ps1_card.py check <A.VM1> <B.VM1>    after: copy MGS0 A -> B, delete MGS0 on A
"""
import os
import sys

FR = 128
BLK = 8192


def frame(body):
    b = bytearray(FR)
    b[:len(body)] = body
    c = 0
    for x in b[:127]:
        c ^= x
    b[127] = c
    return bytes(b)


def blank():
    img = bytearray(128 * 1024)
    img[0:FR] = frame(b"MC")
    for f in range(1, 16):
        img[f * FR:(f + 1) * FR] = frame(b"\xA0\x00\x00\x00" + bytes(4) + b"\xFF\xFF")
    for f in range(16, 36):
        img[f * FR:(f + 1) * FR] = frame(b"\xFF\xFF\xFF\xFF" + bytes(4) + b"\xFF\xFF")
    img[63 * FR:64 * FR] = img[0:FR]
    return img


def dirframe(state, size, nxt, name=b""):
    return frame(bytes([state, 0, 0, 0]) + size.to_bytes(4, "little") + nxt.to_bytes(2, "little") + name)


def title_block(title, seed):
    """block's first 8 KB: SC header, 1 icon frame, title in full-width Shift-JIS, a palette"""
    b = bytearray(BLK)
    b[0:2] = b"SC"
    b[2] = 0x11                      # 1 frame
    b[3] = 1
    sj = bytearray()
    for ch in title:
        if ch == " ":
            sj += b"\x81\x40"
        elif "A" <= ch <= "Z":
            sj += bytes([0x82, 0x60 + ord(ch) - 65])
        elif "0" <= ch <= "9":
            sj += bytes([0x82, 0x4F + ord(ch) - 48])
    b[4:4 + len(sj)] = sj
    for i in range(16):
        c = (i * 2 + seed) & 31
        b[0x60 + 2 * i:0x62 + 2 * i] = (c | (31 - c) << 5 | (i * 2) << 10 | 0x8000).to_bytes(2, "little")
    for i in range(128):
        b[128 + i] = ((i + seed) * 37) & 0xFF
    for i in range(384, BLK):
        b[i] = (i * 7 + seed * 13) & 0xFF   # "save data"
    return b


def make(pa, pb):
    a = blank()
    a[1 * FR:2 * FR] = dirframe(0x51, 2 * BLK, 1, b"BASLUS-00594MGS0")
    a[2 * FR:3 * FR] = dirframe(0x53, 0, 0xFFFF)
    a[3 * FR:4 * FR] = dirframe(0x51, BLK, 0xFFFF, b"BASLUS-00594MGS1")
    a[1 * BLK:2 * BLK] = title_block("METAL GEAR SOLID 1", 3)
    a[2 * BLK:3 * BLK] = bytes(((i * 11 + 5) & 0xFF) for i in range(BLK))
    a[3 * BLK:4 * BLK] = title_block("METAL GEAR SOLID 2", 9)
    b = blank()
    b[1 * FR:2 * FR] = dirframe(0x51, BLK, 0xFFFF, b"BASLUS-00001OTHER")
    b[2 * FR:3 * FR] = dirframe(0xA1, BLK, 0xFFFF, b"BASLUS-00002GONE")
    b[1 * BLK:2 * BLK] = title_block("OTHER GAME", 5)
    b[2 * BLK:3 * BLK] = title_block("GONE GAME", 7)
    for p, img in ((pa, a), (pb, b)):
        open(p, "wb").write(img)
        open(p + ".orig", "wb").write(img)
    print("made %s (2 saves), %s (1 save, 1 deleted frame)" % (os.path.basename(pa), os.path.basename(pb)))


def sumok(fr):
    c = 0
    for x in fr[:127]:
        c ^= x
    return c == fr[127]


def check(pa, pb):
    a, b = open(pa, "rb").read(), open(pb, "rb").read()
    a0, b0 = open(pa + ".orig", "rb").read(), open(pb + ".orig", "rb").read()
    errs = []
    for name, img in (("A", a), ("B", b)):
        for f in list(range(0, 36)) + [63]:
            if not sumok(img[f * FR:(f + 1) * FR]):
                errs.append("%s frame %d checksum" % (name, f))
        if img[0:FR] != img[63 * FR:64 * FR]:
            errs.append("%s frame 63 != frame 0" % name)
    # A: MGS0's frames deleted (51 -> A1, 53 -> A3), everything else as it was
    if a[1 * FR] != 0xA1 or a[2 * FR] != 0xA3:
        errs.append("A: MGS0 frames are %02x %02x, want a1 a3" % (a[FR], a[2 * FR]))
    for f in range(1, 3):
        if a[f * FR + 1:(f + 1) * FR - 1] != a0[f * FR + 1:(f + 1) * FR - 1]:
            errs.append("A frame %d changed beyond its state" % f)
    if a[3 * FR:] != a0[3 * FR:] or a[:FR] != a0[:FR]:
        errs.append("A: something besides MGS0's two frames changed")
    # B: block 1 untouched; MGS0 in blocks 2 and 3 (first the deleted frame, then a free one)
    if b[:2 * FR] != b0[:2 * FR] or b[BLK:2 * BLK] != b0[BLK:2 * BLK]:
        errs.append("B: the other save changed")
    f2, f3 = b[2 * FR:3 * FR], b[3 * FR:4 * FR]
    if f2[0] != 0x51 or f2[4:8] != (2 * BLK).to_bytes(4, "little") or f2[8:10] != (2).to_bytes(2, "little") or f2[10:26] != b"BASLUS-00594MGS0":
        errs.append("B frame 2: %s" % f2[:32].hex())
    if f3[0] != 0x53 or f3[8:10] != b"\xff\xff":
        errs.append("B frame 3: %s" % f3[:16].hex())
    if b[2 * BLK:4 * BLK] != a0[1 * BLK:3 * BLK]:
        errs.append("B: MGS0's data differs from A's")
    if b[4 * FR:36 * FR] != b0[4 * FR:36 * FR] or b[4 * BLK:] != b0[4 * BLK:]:
        errs.append("B: frames/blocks past 3 changed")
    print("PS1 OK" if not errs else "PS1 BAD:\n  " + "\n  ".join(errs))


if __name__ == "__main__":
    {"make": make, "check": check}[sys.argv[1]](sys.argv[2], sys.argv[3])
