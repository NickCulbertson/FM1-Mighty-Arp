#!/usr/bin/env python3
"""Mighty Arp's boot screen: "MIGHTY" in bold pink over a big cyan "ARP", drawn with Barlow (base/assets/fonts, OFL) and
written as app/splash.h, run-length RGB565 (app.c app_splash). Run it again after changing the look:

  python3 tools/make_splash.py [preview.png]
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / "base/assets/fonts"
S = 3                                     # drawn at 3x, then brought down: smooth edges
W = H = 240 * S
CYAN, PINK, PURPLE = (110, 225, 255), (255, 46, 151), (178, 96, 255)

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


def paste_mask(img, colour, mask, xy):
    img.paste(Image.new("RGB", mask.size, colour), xy, mask)


def draw():
    """plain and bold: "MIGHTY" in pink over a big "ARP" in cyan, the line of what it is at the bottom"""
    img = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(img)
    arp = text_mask("ARP", font("BarlowSemiCondensed-Bold.ttf", 130), pad=0)
    mighty = text_mask("MIGHTY", font("BarlowSemiCondensed-Bold.ttf", 58), pad=0)
    gap = 6 * S
    top = (int(H * 0.86) - (mighty.size[1] + gap + arp.size[1])) // 2   # the two together, above the line
    paste_mask(img, PINK, mighty, ((W - mighty.size[0]) // 2, top))
    paste_mask(img, CYAN, arp, ((W - arp.size[0]) // 2, top + mighty.size[1] + gap))
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
