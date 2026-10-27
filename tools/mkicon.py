#!/usr/bin/env python3
"""Draw the OnionOS app icon: five vibrating strings.

usage: mkicon.py OUT.png
"""
import math
import sys
from PIL import Image, ImageDraw

COLORS = [(255, 120, 80), (250, 210, 70), (90, 220, 170), (90, 160, 255), (190, 130, 255)]


def main():
    s = 4
    size = 80
    im = Image.new("RGBA", (size * s, size * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, size * s - 1, size * s - 1], radius=16 * s, fill=(16, 18, 26, 255))
    for i, col in enumerate(COLORS):
        y = (15 + i * 12.5) * s
        amp = (1.5 + i * 1.6) * s
        pts = []
        for k in range(0, 64 * s + 1):
            x = 8 * s + k
            t = k / (64 * s)
            pts.append((x, y + amp * math.sin(t * math.pi) * math.sin(t * math.pi * (2 + i) + i)))
        d.line(pts, fill=col, width=int(1.6 * s))
    im.resize((size, size), Image.LANCZOS).save(sys.argv[1])


if __name__ == "__main__":
    main()
