#!/usr/bin/env python3
"""Mighty Arp's boot screen: "Mighty" brushed, "ARP" in 80s chrome, drawn with Barlow (base/assets/fonts, OFL) and
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
    """80s chrome: a pale sky to white at the top, a hard dark horizon, purple ground brightening to pink"""
    w, h = size
    g = Image.new("RGB", (w, h))
    px = g.load()
    hz = int(h * horizon)
    for y in range(h):
        if y < hz:
            t = y / max(hz, 1)
            c = tuple(int(a + (b - a) * t) for a, b in zip((150, 205, 255), (255, 255, 255)))
        else:
            t = (y - hz) / max(h - hz, 1)
            c = tuple(int(a + (b - a) * t ** 0.8) for a, b in zip((40, 10, 70), PINK))
            if y - hz < 2 * S:            # the horizon's bright edge
                c = (255, 235, 250)
        for x in range(w):
            px[x, y] = c
    return g


def paste_mask(img, colour, mask, xy):
    img.paste(Image.new("RGB", mask.size, colour), xy, mask)


def glint(d, x, y, r, c=(255, 255, 255)):
    """a four-pointed sparkle"""
    d.polygon([(x, y - r), (x + r * 0.18, y - r * 0.18), (x + r, y), (x + r * 0.18, y + r * 0.18), (x, y + r),
               (x - r * 0.18, y + r * 0.18), (x - r, y), (x - r * 0.18, y - r * 0.18)], fill=c)


def draw():
    img = Image.new("RGB", (W, H), (0, 0, 0))
    # ARP: chrome, big, a dark outline then a thin pale one, a glow of pink behind
    arp = text_mask("ARP", font("BarlowSemiCondensed-Bold.ttf", 118))
    ax, ay = (W - arp.size[0]) // 2, int(H * 0.43)
    glow = arp.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(9 * S))
    paste_mask(img, (120, 20, 90), glow, (ax, ay))
    paste_mask(img, (25, 5, 40), arp.filter(ImageFilter.MaxFilter(2 * 3 + 1)), (ax, ay))
    paste_mask(img, (230, 220, 255), arp.filter(ImageFilter.MaxFilter(3)), (ax, ay))
    img.paste(chrome_fill(arp.size, 0.52), (ax, ay), arp)
    # Mighty: brushed, slanted, cyan, across the top of ARP; a brush stroke under it
    mighty = dry_brush(text_mask("Mighty", font("BarlowSemiCondensed-Bold.ttf", 66), shear=0.28))
    mx, my = int(W * 0.08), int(H * 0.14)
    shadow = mighty.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(2 * S))
    paste_mask(img, (0, 0, 0), shadow, (mx + 2 * S, my + 3 * S))
    paste_mask(img, CYAN, mighty, (mx, my))
    stroke = Image.new("L", (int(W * 0.62), 14 * S), 0)
    ImageDraw.Draw(stroke).polygon([(0, 9 * S), (stroke.size[0], 2 * S), (stroke.size[0] - 6 * S, 7 * S), (8 * S, 13 * S)],
                                   fill=255)
    paste_mask(img, CYAN, dry_brush(stroke), (int(W * 0.20), my + mighty.size[1] - 4 * S))
    d = ImageDraw.Draw(img)
    glint(d, ax + arp.size[0] * 0.16, ay + arp.size[1] * 0.10, 7 * S)
    glint(d, ax + arp.size[0] * 0.86, ay + arp.size[1] * 0.58, 5 * S, (255, 220, 245))
    # what it is, small
    tag = "FM SYNTH  ·  CHANCE ARP"
    f = font("BarlowSemiCondensed-SemiBold.ttf", 12)
    tw = d.textlength(tag, font=f)
    d.text(((W - tw) / 2, H * 0.89), tag, font=f, fill=PURPLE)
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
