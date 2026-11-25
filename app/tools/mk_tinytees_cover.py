"""Tiny Tees' cover (Prism v0.59, Bananafuzzbuddy 2026-11-24: "add a cover art for tiny tees so that people are
less likely to delete it"). Original art drawn here, in the game's own look:

  * the logo as the game draws it (core/logo.inc): TINY TEES in its blocky 5 x 7 letters, each letter in
    the player colours PCOL[(i + (i > 4)) % 4] (core/tt.c), a dark shadow offset 4, 5
  * striped grass, the stone wall, the cup and the red-flagged pin of the game's logo shot
  * a ball on the tee mat, a little confetti

No Sony artwork, logos or marks. Writes:
  setup/tinytees_cover.png    512 x 512: goes next to the game on the hard disk as "Tiny Tees.png" (webMAN
                              shows it on the XMB) and is the source of the next file
  setup/tinytees_cover.rgba   128 x 128 R G B A (A = 0x80), the art file's own cover format, put in
                              /dev_hdd0/tmp/prism_art.dat for SLUS_999.01 (the serial the Tiny Tees disc names)
Run:  python tools/mk_tinytees_cover.py"""
import os
import random

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SETUP = os.path.join(os.path.dirname(HERE), "setup")
S = 512
PCOL = [(236, 66, 54), (56, 124, 238), (250, 200, 36), (170, 84, 226)]	# core/tt.c PCOL

# 5 x 7 letters, drawn like the game's blocky HUD font
GLYPH = {
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "I": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"],
    "N": ["#...#", "##..#", "#.#.#", "#.#.#", "#..##", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "G": [".####", "#....", "#....", "#.###", "#...#", "#...#", ".###."],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    " ": ["....."] * 7,
}


def text_w(s, u):
    return len(s) * 6 * u - u


def draw_text(d, x, y, s, u, colour_of):
    for i, ch in enumerate(s):
        g = GLYPH[ch]
        for gy, row in enumerate(g):
            for gx, c in enumerate(row):
                if c == "#":
                    px, py = x + (i * 6 + gx) * u, y + gy * u
                    d.rectangle([px, py, px + u - 1, py + u - 1], fill=colour_of(i))


def main():
    random.seed(1999)
    im = Image.new("RGB", (S, S))
    d = ImageDraw.Draw(im)
    # sky
    for y in range(0, 230):
        t = y / 230
        d.line([(0, y), (S, y)], fill=(int(120 + 90 * t), int(180 + 50 * t), int(236 + 14 * t)))
    # low hills, faceted like the game's
    d.polygon([(0, 205), (70, 168), (150, 186), (230, 150), (330, 178), (410, 156), (512, 182), (512, 230), (0, 230)], fill=(118, 160, 128))
    d.polygon([(0, 222), (90, 196), (200, 210), (300, 190), (420, 206), (512, 196), (512, 236), (0, 236)], fill=(92, 142, 96))
    # a few round trees behind the wall
    for (x, r, c) in ((44, 22, (52, 110, 58)), (96, 17, (64, 126, 66)), (448, 24, (50, 104, 56)), (492, 16, (66, 128, 70))):
        d.rectangle([x - 3, 222, x + 3, 238], fill=(98, 72, 48))
        d.ellipse([x - r, 222 - 2 * r, x + r, 226], fill=c)
    # the lawn: stripes widening toward the viewer
    y, k, h = 236, 0, 10
    while y < S:
        d.rectangle([0, y, S, y + h], fill=(98, 190, 74) if k % 2 == 0 else (112, 204, 86))
        y += h
        h = int(h * 1.22) + 1
        k += 1
    # the stone wall along the back of the green
    d.polygon([(0, 236), (512, 236), (512, 252), (0, 258)], fill=(214, 208, 196))
    d.polygon([(0, 258), (512, 252), (512, 258), (0, 266)], fill=(168, 162, 150))
    # the cup, its pin and the red flag
    cx, cy = 352, 408
    d.ellipse([cx - 70, cy - 26, cx + 70, cy + 26], fill=(250, 250, 250))
    d.ellipse([cx - 54, cy - 18, cx + 54, cy + 18], fill=(24, 30, 26))
    d.polygon([(cx - 4, cy - 2), (cx + 4, cy - 2), (cx + 4, 236), (cx - 4, 236)], fill=(226, 228, 232))
    d.polygon([(cx + 4, 236), (cx + 104, 258), (cx + 4, 282)], fill=PCOL[0])
    d.polygon([(cx + 4, 236), (cx + 104, 258), (cx + 54, 249)], fill=(250, 98, 84))
    # the ball on the tee mat
    bx, by = 134, 432
    d.polygon([(bx - 52, by + 22), (bx + 52, by + 22), (bx + 40, by - 2), (bx - 40, by - 2)], fill=(52, 120, 52))
    d.ellipse([bx - 20, by - 26, bx + 20, by + 14], fill=(250, 250, 250))
    d.ellipse([bx - 12, by - 20, bx + 2, by - 8], fill=(255, 255, 255))
    d.arc([bx - 20, by - 26, bx + 20, by + 14], 20, 160, fill=(200, 204, 210), width=4)
    # confetti in the player colours
    for i in range(46):
        x, y = random.randrange(10, S - 10), random.randrange(12, 300)
        if 40 < y < 200 and 30 < x < 480:
            continue	# keep the logo clear
        c = PCOL[i % 4]
        w, hh = random.choice([(10, 5), (5, 10), (8, 8)])
        d.rectangle([x, y, x + w, y + hh], fill=c)
    # the logo, as logo.inc draws it: shadow at (+4, +5), letter i in PCOL[(i + (i > 4)) % 4]
    u = 8
    lx, ly = (S - text_w("TINY TEES", u)) // 2, 64
    draw_text(d, lx + 5, ly + 6, "TINY TEES", u, lambda i: (20, 36, 28))
    draw_text(d, lx, ly, "TINY TEES", u, lambda i: PCOL[(i + (i > 4)) % 4])
    u2 = 4
    sx = (S - text_w("MINI GOLF", u2)) // 2
    draw_text(d, sx + 3, 147, "MINI GOLF", u2, lambda i: (20, 36, 28))
    draw_text(d, sx, 144, "MINI GOLF", u2, lambda i: (255, 255, 255))

    png = os.path.join(SETUP, "tinytees_cover.png")
    im.save(png, optimize=True)
    small = im.reduce(4)	# 512 / 4: an exact box average, as Prism's own cv_scale does it
    rgb = small.tobytes()
    out = bytearray(128 * 128 * 4)
    out[0::4], out[1::4], out[2::4] = rgb[0::3], rgb[1::3], rgb[2::3]
    out[3::4] = b"\x80" * (128 * 128)
    with open(os.path.join(SETUP, "tinytees_cover.rgba"), "wb") as f:
        f.write(out)
    small.resize((256, 256), Image.NEAREST).save(os.path.join(HERE, "tinytees_cover_128_preview.png"))
    print("%s: %d bytes; tinytees_cover.rgba: %d bytes" % (png, os.path.getsize(png), len(out)))


if __name__ == "__main__":
    main()
