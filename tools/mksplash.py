#!/usr/bin/env python3
"""The start-up screen's background: the Jaffe house's roofline (two shed
roofs meeting in a valley against the sky), scaled to 640x480 and stored
with a 256-colour palette.

File: "JDI1", width and height (uint16 little-endian), 256 RGB palette
entries, then one palette index per pixel, row by row.

usage: mksplash.py PHOTO OUT.jdi [PREVIEW.png]
"""
import struct
import sys
from PIL import Image

W, H = 640, 480
# where the house's valley sits in the photo (art/roofline.jpg, 886 wide)
VALLEY_SRC = 479
ZOOM = 1.12  # larger than the screen, so the photo can slide to line up


def valley_target():
    """x of the wordmark's valley on the start-up screen, from the logo's
    outlines and its height there"""
    import os
    import re
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    logo = open(os.path.join(root, "src", "logo_data.h")).read()
    ui = open(os.path.join(root, "src", "ui.c")).read()
    d = {k: float(v) for k, v in re.findall(r"#define (LOGO_\w+) (\d+)", logo)}
    h = float(re.search(r"#define SPLASH_LOGO_H ([\d.]+)", ui).group(1))
    k = h / d["LOGO_H"]
    return (W - d["LOGO_W"] * k) / 2 + d["LOGO_VALLEY_X"] * k


def main():
    im = Image.open(sys.argv[1]).convert("RGB")
    s = W / im.width * ZOOM
    im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
    x0 = round(VALLEY_SRC * s - valley_target())
    x0 = max(0, min(im.width - W, x0))
    im = im.crop((x0, 0, x0 + W, H))
    q = im.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)
    pal = (q.getpalette() + [0] * 768)[:768]
    with open(sys.argv[2], "wb") as f:
        f.write(b"JDI1" + struct.pack("<HH", W, H) + bytes(pal) + q.tobytes())
    if len(sys.argv) > 3:
        q.convert("RGB").save(sys.argv[3])


if __name__ == "__main__":
    main()
