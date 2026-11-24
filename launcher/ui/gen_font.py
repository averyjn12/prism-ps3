"""Make font_data.h for the Prism launcher's PS2 program: four cuts of DejaVu Sans drawn into
one 512 x 256 coverage atlas, 4 bits per pixel.

    python gen_font.py <DejaVuSans.ttf> <DejaVuSans-Bold.ttf>

DejaVu Fonts are free to use, embed and redistribute (Bitstream Vera / Arev licence); the
font files come from the WSL distro (/usr/share/fonts/truetype/dejavu). Nothing here is Sony's.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 512, 256
FIRST, LAST = 32, 127      # 127 stands for the middle dot
EXTRA = {127: chr(0xB7)}
PAD = 2
FONTS = ((0, 14), (0, 16), (1, 19), (1, 26))        # (which ttf, pixel size): small, body, bold, heading


def main(argv):
    ttfs = argv[1:3]
    atlas = Image.new("L", (W, H), 0)
    draw = ImageDraw.Draw(atlas)
    x = y = PAD
    row_h = 0
    tables = []
    metrics = []
    for which, px in FONTS:
        font = ImageFont.truetype(ttfs[which], px)
        ascent, descent = font.getmetrics()
        metrics.append((ascent, ascent + descent))
        glyphs = []
        for code in range(FIRST, LAST + 1):
            ch = EXTRA.get(code, chr(code))
            box = font.getbbox(ch)                    # (x0, y0, x1, y1), y from the ascender line
            adv = int(round(font.getlength(ch)))
            gw, gh = max(0, box[2] - box[0]), max(0, box[3] - box[1])
            if gw == 0 or gh == 0:
                glyphs.append((0, 0, 0, 0, 0, 0, adv))
                continue
            if x + gw + PAD > W:
                x = PAD
                y += row_h + PAD
                row_h = 0
            if y + gh + PAD > H:
                raise SystemExit("atlas full")
            draw.text((x - box[0], y - box[1]), ch, font=font, fill=255)
            glyphs.append((x, y, gw, gh, box[0], box[1], adv))
            x += gw + PAD
            row_h = max(row_h, gh)
        tables.append(glyphs)
    px = atlas.tobytes()
    packed = bytearray()
    for i in range(0, W * H, 2):
        packed.append(((px[i] + 8) // 17) | (((px[i + 1] + 8) // 17) << 4))

    out = []
    out.append("/* Made by gen_font.py from DejaVu Sans (free licence). Do not edit. */")
    out.append("#define FONT_TEX_W %d" % W)
    out.append("#define FONT_TEX_H %d" % H)
    out.append("#define FONT_FIRST %d" % FIRST)
    out.append("#define FONT_LAST %d" % LAST)
    out.append("typedef struct { unsigned short u, v; unsigned char w, h; signed char ox, oy; unsigned char adv; } glyph_t;")
    out.append("static const unsigned char font_ascent[%d] = { %s };" % (len(FONTS), ", ".join(str(m[0]) for m in metrics)))
    out.append("static const unsigned char font_height[%d] = { %s };" % (len(FONTS), ", ".join(str(m[1]) for m in metrics)))
    out.append("static const glyph_t font_glyphs[%d][%d] = {" % (len(FONTS), LAST - FIRST + 1))
    for glyphs in tables:
        out.append("  {")
        for g in glyphs:
            out.append("    { %d, %d, %d, %d, %d, %d, %d }," % g)
        out.append("  },")
    out.append("};")
    out.append("/* 4-bit coverage, low nibble = the left pixel */")
    out.append("static const unsigned char font_cov[%d] = {" % len(packed))
    for i in range(0, len(packed), 32):
        out.append("  " + ",".join(str(b) for b in packed[i:i + 32]) + ",")
    out.append("};")
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "font_data.h"), "w", newline="\n") as fh:
        fh.write("\n".join(out) + "\n")
    atlas.save(os.path.join(here, "font_atlas.png"))
    print("font_data.h written; atlas used down to row %d of %d" % (y + row_h, H))


if __name__ == "__main__":
    main(sys.argv)
