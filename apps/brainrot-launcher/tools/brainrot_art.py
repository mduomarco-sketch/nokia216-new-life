#!/usr/bin/env python3
"""Original 36x36 pixel-art fan icons of Italian-brainrot characters for the launcher.

Draws each sprite with simple shapes, writes a preview sheet (PNG) and
src/sprites.h (4-bit palette sprites, RGB565 palettes).
usage: brainrot_art.py src/sprites.h preview.png
"""
import sys
from PIL import Image, ImageDraw

N = 36
K = (20, 20, 24)          # outline
W = (250, 250, 250)


def canvas(bg):
    im = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, N - 1, N - 1], radius=6, fill=bg)
    return im, d


def eye(d, x, y, r=2, look=1):
    d.ellipse([x - r, y - r, x + r, y + r], fill=W, outline=K)
    d.point((x + look, y), fill=K)
    d.point((x + look, y + 1), fill=K)


def tung():
    im, d = canvas((255, 214, 120))
    wood, dark = (176, 112, 60), (120, 72, 36)
    d.line([25, 30, 33, 6], fill=dark, width=4)              # bat
    d.line([25, 30, 33, 6], fill=(150, 95, 50), width=2)
    d.rounded_rectangle([10, 3, 24, 28], radius=5, fill=wood, outline=K)  # log body
    for y in (8, 15, 22):
        d.line([13, y, 16, y], fill=dark)
    d.line([19, 11, 21, 11], fill=dark)
    d.line([18, 25, 21, 25], fill=dark)
    eye(d, 14, 10)
    eye(d, 20, 10)
    d.rectangle([14, 15, 20, 18], fill=K)                    # big mouth
    d.line([15, 16, 19, 16], fill=(220, 60, 60))
    d.line([10, 19, 6, 23], fill=K, width=1)                 # arms
    d.line([24, 19, 27, 23], fill=K, width=1)
    d.line([14, 29, 13, 33], fill=K)                         # legs
    d.line([20, 29, 21, 33], fill=K)
    d.line([11, 33, 14, 33], fill=K)
    d.line([20, 33, 23, 33], fill=K)
    return im


def tralalero():
    im, d = canvas((150, 215, 250))
    shark, belly, shoe = (70, 120, 190), (235, 240, 245), (40, 90, 220)
    d.polygon([(4, 14), (1, 8), (7, 13)], fill=shark, outline=K)          # tail
    d.polygon([(15, 9), (19, 3), (21, 10)], fill=shark, outline=K)         # fin
    d.ellipse([4, 8, 33, 22], fill=shark, outline=K)                       # body
    d.chord([8, 14, 32, 22], 0, 180, fill=belly)
    d.line([24, 18, 31, 18], fill=K)                                       # mouth
    for x in (25, 27, 29):
        d.point((x, 19), fill=W)
    eye(d, 27, 13, 2, 1)
    for x in (12, 18, 24):                                                 # three legs
        d.line([x, 22, x, 28], fill=K)
        d.rounded_rectangle([x - 2, 28, x + 3, 31], radius=1, fill=shoe, outline=K)
        d.line([x - 1, 31, x + 2, 31], fill=W)
    return im


def bombardiro():
    im, d = canvas((175, 205, 235))
    green, dkgreen, metal = (80, 150, 70), (45, 95, 40), (130, 140, 150)
    d.polygon([(10, 16), (26, 16), (18, 4)], fill=metal, outline=K)        # top wing
    d.polygon([(10, 20), (26, 20), (18, 31)], fill=metal, outline=K)       # bottom wing
    d.polygon([(2, 15), (6, 18), (2, 21)], fill=metal, outline=K)          # tail
    d.rounded_rectangle([3, 14, 24, 22], radius=4, fill=green, outline=K)  # croc body
    d.polygon([(22, 14), (34, 16), (34, 18), (22, 18)], fill=green, outline=K)  # upper jaw
    d.polygon([(22, 19), (33, 20), (33, 22), (22, 22)], fill=dkgreen, outline=K)  # lower jaw
    for x in (25, 28, 31):
        d.point((x, 18), fill=W)
        d.point((x + 1, 19), fill=W)
    eye(d, 21, 13, 2, 1)
    for x in (8, 13):                                                     # bombs
        d.ellipse([x, 26, x + 3, 31], fill=(60, 60, 70), outline=K)
    return im


def ballerina():
    im, d = canvas((255, 200, 225))
    cup, coffee, pink = (245, 245, 240), (150, 95, 55), (255, 110, 170)
    d.ellipse([25, 6, 31, 12], outline=K, width=2)                         # handle
    d.rounded_rectangle([10, 3, 27, 16], radius=3, fill=cup, outline=K)    # cup head
    d.rectangle([11, 4, 26, 6], fill=coffee)
    eye(d, 15, 10, 1, 0)
    eye(d, 21, 10, 1, 0)
    d.line([16, 13, 20, 13], fill=(220, 80, 120))
    d.line([10, 9, 4, 3], fill=K)                                         # arms up
    d.line([27, 9, 32, 3], fill=K)
    d.polygon([(6, 25), (31, 25), (24, 17), (13, 17)], fill=pink, outline=K)  # tutu
    d.line([18, 25, 15, 33], fill=K)                                      # legs
    d.line([19, 25, 24, 31], fill=K)
    d.line([24, 31, 27, 30], fill=K)
    d.line([13, 33, 16, 33], fill=pink)
    return im


def patapim():
    im, d = canvas((190, 230, 160))
    leaf, dkleaf, skin, bark = (60, 150, 60), (35, 100, 40), (205, 150, 100), (120, 80, 45)
    d.ellipse([3, 1, 33, 17], fill=leaf, outline=K)                        # leafy head
    for p in [(8, 5), (14, 3), (22, 4), (28, 7), (11, 11), (25, 11)]:
        d.ellipse([p[0] - 2, p[1] - 2, p[0] + 2, p[1] + 2], fill=dkleaf)
    d.ellipse([9, 8, 27, 26], fill=skin, outline=K)                        # face
    eye(d, 14, 14, 2, 0)
    eye(d, 22, 14, 2, 0)
    d.ellipse([15, 15, 21, 22], fill=(185, 120, 80), outline=K)            # big nose
    d.line([15, 24, 21, 24], fill=K)
    d.rectangle([15, 26, 21, 29], fill=bark, outline=K)                    # trunk-body
    d.ellipse([3, 28, 16, 34], fill=skin, outline=K)                       # huge feet
    d.ellipse([20, 28, 33, 34], fill=skin, outline=K)
    return im


def lirili():
    im, d = canvas((250, 235, 170))
    grey, cactus, spike = (150, 150, 165), (70, 160, 80), (240, 240, 200)
    d.rounded_rectangle([11, 15, 25, 31], radius=5, fill=cactus, outline=K)  # cactus body
    d.rounded_rectangle([4, 18, 10, 26], radius=3, fill=cactus, outline=K)   # cactus arm
    d.rounded_rectangle([26, 17, 32, 24], radius=3, fill=cactus, outline=K)
    for p in [(13, 20), (17, 24), (21, 21), (14, 28), (22, 28), (6, 21), (29, 20)]:
        d.point(p, fill=spike)
    d.ellipse([9, 2, 27, 18], fill=grey, outline=K)                        # elephant head
    d.ellipse([4, 4, 12, 13], fill=grey, outline=K)                        # ears
    d.ellipse([24, 4, 32, 13], fill=grey, outline=K)
    d.line([18, 13, 18, 20], fill=K, width=3)                              # trunk
    d.line([18, 13, 18, 20], fill=grey, width=1)
    eye(d, 15, 9, 1, 0)
    eye(d, 21, 9, 1, 0)
    d.rectangle([11, 31, 16, 33], fill=(160, 100, 50), outline=K)          # sandals
    d.rectangle([20, 31, 25, 33], fill=(160, 100, 50), outline=K)
    return im


def chimpanzini():
    im, d = canvas((255, 245, 160))
    banana, peel, fur, face = (250, 215, 50), (230, 180, 30), (120, 75, 40), (220, 170, 120)
    d.polygon([(6, 34), (11, 14), (18, 20)], fill=banana, outline=K)        # peel flaps
    d.polygon([(30, 34), (25, 14), (18, 20)], fill=banana, outline=K)
    d.rounded_rectangle([10, 18, 26, 34], radius=4, fill=peel, outline=K)
    d.ellipse([10, 3, 26, 21], fill=fur, outline=K)                        # monkey head
    d.ellipse([6, 9, 11, 14], fill=fur, outline=K)                         # ears
    d.ellipse([25, 9, 30, 14], fill=fur, outline=K)
    d.ellipse([12, 8, 24, 19], fill=face)
    eye(d, 15, 11, 1, 0)
    eye(d, 21, 11, 1, 0)
    d.arc([14, 13, 22, 18], 20, 160, fill=K)                               # grin
    d.line([18, 1, 19, 4], fill=(90, 140, 40), width=2)                    # banana stem
    return im


def cappuccino():
    im, d = canvas((60, 60, 75))
    cup, coffee, mask, blade = (240, 240, 235), (150, 95, 55), (25, 25, 30), (200, 210, 225)
    d.line([3, 3, 31, 31], fill=blade, width=2)                            # crossed katanas
    d.line([33, 3, 5, 31], fill=blade, width=2)
    d.line([3, 3, 8, 8], fill=(150, 40, 40), width=3)
    d.line([33, 3, 28, 8], fill=(150, 40, 40), width=3)
    d.ellipse([25, 15, 32, 23], outline=K, width=2)                         # handle
    d.rounded_rectangle([8, 9, 27, 30], radius=4, fill=cup, outline=K)     # cup
    d.rectangle([9, 10, 26, 13], fill=coffee)
    d.rectangle([8, 16, 27, 21], fill=mask)                                # ninja mask band
    d.line([14, 18, 16, 19], fill=W)
    d.line([21, 19, 23, 18], fill=W)
    d.line([27, 18, 33, 22], fill=mask, width=2)                           # mask tails
    return im


def trippi():
    im, d = canvas((255, 205, 170))
    shrimp, dk, fur = (250, 120, 70), (210, 80, 45), (240, 160, 90)
    d.pieslice([4, 6, 30, 32], 90, 300, fill=shrimp, outline=K)            # curled shrimp body
    for a in (150, 195, 240):
        import math
        x = 17 + 11 * math.cos(math.radians(a))
        y = 19 + 11 * math.sin(math.radians(a))
        d.line([17, 19, x, y], fill=dk)
    d.polygon([(4, 24), (1, 30), (8, 28)], fill=shrimp, outline=K)         # tail fan
    d.ellipse([16, 4, 33, 20], fill=fur, outline=K)                        # cat head
    d.polygon([(17, 8), (18, 1), (22, 5)], fill=fur, outline=K)            # ears
    d.polygon([(27, 5), (31, 1), (32, 9)], fill=fur, outline=K)
    eye(d, 21, 11, 1, 0)
    eye(d, 28, 11, 1, 0)
    d.point((24, 14), fill=(230, 90, 120))
    d.line([18, 15, 22, 14], fill=K)                                       # whiskers
    d.line([27, 14, 31, 15], fill=K)
    return im


def boneca():
    im, d = canvas((200, 240, 200))
    frog, dk, tire = (90, 190, 80), (50, 130, 50), (40, 40, 45)
    d.ellipse([6, 16, 30, 30], fill=tire, outline=K)                       # tire
    d.ellipse([12, 20, 24, 26], fill=(110, 110, 115))
    d.ellipse([7, 3, 29, 20], fill=frog, outline=K)                        # frog head
    d.ellipse([8, 0, 16, 8], fill=frog, outline=K)                         # eye bumps
    d.ellipse([20, 0, 28, 8], fill=frog, outline=K)
    eye(d, 12, 4, 2, 0)
    eye(d, 24, 4, 2, 0)
    d.arc([10, 8, 26, 17], 15, 165, fill=dk, width=1)                      # smile
    d.line([12, 30, 11, 34], fill=K)                                       # legs
    d.line([24, 30, 25, 34], fill=K)
    d.line([9, 34, 12, 34], fill=K)
    d.line([24, 34, 27, 34], fill=K)
    return im


def trulimero():
    im, d = canvas((170, 225, 240))
    fish, fin, fur = (60, 170, 170), (40, 120, 130), (245, 170, 80)
    d.polygon([(2, 12), (2, 26), (9, 19)], fill=fin, outline=K)            # tail
    d.ellipse([6, 10, 26, 28], fill=fish, outline=K)                       # fish body
    for x in (11, 15, 19):
        d.arc([x - 3, 14, x + 3, 24], 300, 60, fill=fin)
    d.ellipse([19, 6, 34, 21], fill=fur, outline=K)                        # cat head
    d.polygon([(20, 9), (21, 2), (25, 7)], fill=fur, outline=K)
    d.polygon([(29, 7), (33, 2), (33, 11)], fill=fur, outline=K)
    eye(d, 24, 13, 1, 0)
    eye(d, 30, 13, 1, 0)
    d.point((27, 16), fill=(230, 90, 120))
    d.line([13, 28, 12, 34], fill=K)                                       # human legs
    d.line([19, 28, 20, 34], fill=K)
    d.line([10, 34, 13, 34], fill=K)
    d.line([19, 34, 22, 34], fill=K)
    return im


SPRITES = [
    ("TUNG TUNG TUNG SAHUR", tung),
    ("TRALALERO TRALALA", tralalero),
    ("BOMBARDIRO CROCODILO", bombardiro),
    ("BALLERINA CAPPUCCINA", ballerina),
    ("BRR BRR PATAPIM", patapim),
    ("LIRILI LARILA", lirili),
    ("CHIMPANZINI BANANINI", chimpanzini),
    ("CAPPUCCINO ASSASSINO", cappuccino),
    ("TRIPPI TROPPI", trippi),
    ("BONECA AMBALABU", boneca),
    ("TRULIMERO TRULICINA", trulimero),
]


def rgb565(c):
    r, g, b = c[:3]
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def main(header, preview):
    out = ["/* Generated by tools/brainrot_art.py - original fan pixel art. Do not edit. */",
           "#pragma once", f"#define SPRITE_N {N}", f"#define SPRITE_COUNT {len(SPRITES)}",
           "typedef struct { const char *name; unsigned short pal[16]; unsigned char px[SPRITE_N * SPRITE_N / 2]; } sprite_t;",
           "static const sprite_t sprites[SPRITE_COUNT] = {"]
    scale = 4
    sheet = Image.new("RGB", (len(SPRITES) * (N * scale + 8) + 8, N * scale + 16), (40, 40, 48))
    for si, (name, fn) in enumerate(SPRITES):
        im = fn().convert("RGBA")
        colors = []
        idx = []
        for y in range(N):
            for x in range(N):
                p = im.getpixel((x, y))
                if p[3] < 128:
                    idx.append(0)
                    continue
                c = p[:3]
                if c not in colors:
                    colors.append(c)
                idx.append(colors.index(c) + 1)
        if len(colors) > 15:
            # merge down to 15 by quantizing the opaque pixels
            q = im.convert("RGB").quantize(colors=15)
            pal = q.getpalette()[:45]
            colors = [tuple(pal[i:i + 3]) for i in range(0, 45, 3)]
            idx = [0 if im.getpixel((i % N, i // N))[3] < 128 else q.getpixel((i % N, i // N)) + 1 for i in range(N * N)]
            print(f"{name}: quantized to 15 colours", file=sys.stderr)
        pal = [0] + [rgb565(c) for c in colors]
        pal += [0] * (16 - len(pal))
        packed = [(idx[i] << 4) | idx[i + 1] for i in range(0, N * N, 2)]
        out.append(f'\t{{ "{name}", {{ {", ".join(f"0x{v:04X}" for v in pal)} }},')
        out.append("\t  { " + ",".join(str(b) for b in packed) + " } },")
        big = im.resize((N * scale, N * scale), Image.NEAREST)
        sheet.paste(big, (8 + si * (N * scale + 8), 8), big)
    out.append("};")
    open(header, "w", newline="\n").write("\n".join(out) + "\n")
    sheet.save(preview)
    print(f"wrote {len(SPRITES)} sprites -> {header}, preview -> {preview}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
