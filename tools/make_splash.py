#!/usr/bin/env python3
"""Mighty Arp's boot screen: "Mighty" brushed in pink across a chrome "ARP", speed stripes behind, drawn with Barlow (base/assets/fonts, OFL) and
written as app/splash.h, run-length RGB565 (app.c app_splash). Run it again after changing the look:

  python3 tools/make_splash.py [preview.png]
"""
import random
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / "base/assets/fonts"
S = 3                                     # drawn at 3x, then brought down: smooth edges
W = H = 240 * S
CYAN, PINK, PURPLE = (110, 225, 255), (255, 46, 151), (178, 96, 255)

random.seed(7)                            # the same brush every time


def font(name, px):
    return ImageFont.truetype(str(FONTS / name), px * S)


def text_mask(txt, f, shear=0.0, pad=40):
    """the text's coverage, white on black, slanted by shear (x moves right as y goes up)"""
    l, t, r, b = f.getbbox(txt)
    w, h = r - l + pad * 2 * S, b - t + pad * 2 * S
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).text((pad * S - l, pad * S - t), txt, font=f, fill=255)
    if shear:
        m = m.transform((w + int(abs(shear) * h), h), Image.AFFINE, (1, shear, -shear * h if shear > 0 else 0, 0, 1, 0),
                        resample=Image.BICUBIC)
    return m.crop(m.getbbox())


def dry_brush(mask):
    """a brush's streaks along the strokes: thin gaps running across, ragged ends, a little bleed"""
    w, h = mask.size
    streaks = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(streaks)
    for _ in range(int(h / (2.2 * S))):   # gaps: thin, a little slanted, of uneven length
        y = random.uniform(0, h)
        x0 = random.uniform(-w * 0.2, w * 0.9)
        x1 = x0 + random.uniform(w * 0.15, w * 0.7)
        d.line((x0, y, x1, y - (x1 - x0) * 0.04), fill=random.randint(0, 120), width=random.choice((1, 1, 2)) * S)
    streaks = streaks.filter(ImageFilter.GaussianBlur(0.6 * S))
    rough = mask.filter(ImageFilter.GaussianBlur(1.2 * S)).point(lambda v: 255 if v > random.randint(90, 150) else 0)
    out = ImageChops.multiply(ImageChops.lighter(mask, rough), streaks)
    return out.filter(ImageFilter.GaussianBlur(0.4 * S))


def chrome_fill(size, horizon):
    """80s chrome: a pale sky to white at the top, light streaks across it; a dark horizon that curves up at both
    ends (a lens's, not a ruler's), its edge bright and a thin reflection under it; purple ground brightening to pink"""
    w, h = size
    g = Image.new("RGB", (w, h))
    px = g.load()
    hz0, bend = h * horizon, h * 0.26
    streaks = [(0.18, 0.012), (0.30, 0.008), (0.40, 0.006)]   # (where, how thick), as parts of the height
    for x in range(w):
        u = (x - w / 2) / (w / 2)
        hz = hz0 - bend * u ** 4         # flat in the middle, up at the ends
        for y in range(h):
            if y < hz:
                t = y / max(hz, 1)
                c = [a + (b - a) * t for a, b in zip((120, 190, 255), (255, 255, 255))]
                for at, th in streaks:   # bright bands in the sky, curving with the horizon
                    if abs(y - (at * h - bend * u ** 4 * at / horizon)) < th * h:
                        c = [min(255, v + 40) for v in c]
            else:
                t = (y - hz) / max(h - hz, 1)
                c = [a + (b - a) * t ** 0.7 for a, b in zip((35, 5, 60), PINK)]
                if y - hz < 2 * S:       # the horizon's bright edge
                    c = (255, 240, 252)
                elif abs((y - hz) - h * 0.10) < 0.7 * S:   # a thin reflection under it
                    c = [min(255, v + 90) for v in c]
            px[x, y] = tuple(int(v) for v in c)
    return g


def paste_mask(img, colour, mask, xy):
    img.paste(Image.new("RGB", mask.size, colour), xy, mask)


def glint(d, x, y, r, c=(255, 255, 255)):
    """a four-pointed sparkle"""
    d.polygon([(x, y - r), (x + r * 0.18, y - r * 0.18), (x + r, y), (x + r * 0.18, y + r * 0.18), (x, y + r),
               (x - r * 0.18, y + r * 0.18), (x - r, y), (x - r * 0.18, y - r * 0.18)], fill=c)


def draw():
    img = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(img)
    arp = text_mask("ARP", font("BarlowSemiCondensed-Bold.ttf", 124))
    ax, ay = (W - arp.size[0]) // 2, int(H * 0.42)
    # speed stripes behind: thin and thick, pink to purple, running out of ARP's sides
    for i, (yy, th, c) in enumerate([(0.30, 2, PINK), (0.36, 5, PINK), (0.44, 2, PURPLE), (0.50, 8, PURPLE),
                                     (0.58, 2, (90, 40, 160))]):
        y = ay + arp.size[1] * yy * 1.6
        d.rectangle((0, y, W, y + th * S), fill=c)
    # ARP: chrome, big: a pink glow, a dark outline, a thin pale one, the chrome
    pad = 30 * S                         # room round the letters, so the glow fades out and never shows its edge
    big = Image.new("L", (arp.size[0] + 2 * pad, arp.size[1] + 2 * pad), 0)
    big.paste(arp, (pad, pad))
    glow = big.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(10 * S))
    paste_mask(img, (150, 20, 110), glow, (ax - pad, ay - pad))
    paste_mask(img, (12, 0, 24), arp.filter(ImageFilter.MaxFilter(2 * 4 + 1)), (ax, ay))
    paste_mask(img, (235, 225, 255), arp.filter(ImageFilter.MaxFilter(3)), (ax, ay))
    img.paste(chrome_fill(arp.size, 0.55), (ax, ay), arp)
    d = ImageDraw.Draw(img)
    glint(d, ax + arp.size[0] * 0.62, ay + arp.size[1] * 0.08, 8 * S)
    glint(d, ax + arp.size[0] * 0.92, ay + arp.size[1] * 0.60, 5 * S, (255, 220, 245))
    # Mighty: pink, brushed, smaller, 20 degrees up, across ARP's top left; a cyan double behind it, 80s style
    mighty = dry_brush(text_mask("Mighty", font("BarlowSemiCondensed-Bold.ttf", 50)))
    mighty = mighty.rotate(20, expand=True, resample=Image.BICUBIC)
    mx = ax - int(arp.size[0] * 0.10)
    my = ay - int(mighty.size[1] * 0.62)
    paste_mask(img, (0, 0, 0), mighty.filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.GaussianBlur(1.5 * S)),
               (mx, my))
    paste_mask(img, CYAN, mighty, (mx + 3 * S, my + 3 * S))
    paste_mask(img, PINK, mighty, (mx, my))
    # what it is, small
    tag = "FM SYNTH  ·  CHANCE ARP"
    f = font("BarlowSemiCondensed-SemiBold.ttf", 12)
    tw = d.textlength(tag, font=f)
    d.text(((W - tw) / 2, H * 0.90), tag, font=f, fill=PURPLE)
    return img.resize((240, 240), Image.LANCZOS)


def rgb565(c):
    r, g, b = c[:3]
    return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)


def main():
    img = draw()
    if len(sys.argv) > 1:
        img.save(sys.argv[1])
    px = [rgb565(img.getpixel((x, y))) for y in range(240) for x in range(240)]
    runs, i = [], 0
    while i < len(px):                    # (colour, run) pairs, a run at most 65535
        j = i
        while j < len(px) and px[j] == px[i] and j - i < 65535:
            j += 1
        runs += [px[i], j - i]
        i = j
    out = ["/* SPDX-License-Identifier: MPL-2.0 */",
           "/* Mighty Arp's boot screen, made by tools/make_splash.py (do not edit): 240 x 240 RGB565 as (colour, run)",
           " * pairs, row after row. Drawn with Barlow (OFL, base/assets/fonts/Barlow-OFL.txt) */",
           "#pragma once", f"#define SPLASH_RUNS {len(runs) // 2}u", "static const uint16_t SPLASH_RLE[] = {"]
    for k in range(0, len(runs), 12):
        out.append("    " + ", ".join(f"0x{v:04x}" for v in runs[k:k + 12]) + ",")
    out.append("};")
    (ROOT / "app/splash.h").write_text("\n".join(out) + "\n")
    print(f"app/splash.h: {len(runs) // 2} runs, {len(runs) * 2} bytes")


main()
