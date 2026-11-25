"""PPM (the PC renderer's output, host/render.sh) -> PNG, pure Python. python3 ppm2png.py <dir>: every .ppm in it."""
import glob, os, struct, sys, zlib

for p in sorted(glob.glob(os.path.join(sys.argv[1], "*.ppm"))):
    d = open(p, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    ch = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    open(p[:-4] + ".png", "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                                      + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))
    os.remove(p)
