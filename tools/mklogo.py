#!/usr/bin/env python3
"""The JAFFEDRONE wordmark.

Block letters after a hand drawing, made clean: every letter is a single
outline with no counters (a solid D, R and O, an A with no crossbar, cut
by a tall notch), drawn on one stroke width. The tops follow the Jaffe
house's roofline: JAFFE and DRONE are two roof planes meeting in a valley
between E and D, the left one rising higher, over a level baseline. Inside,
the letters can carry the house's vertical board siding.

Writes src/logo_data.h (outlines in logo units) and, with a second
argument, a preview PNG of the treatments.

usage: mklogo.py OUT.h [PREVIEW.png]
"""
import math
import sys

H = 100.0  # letter height before the roof
S = 22.0   # stroke
GAP = 9.0
ROOF_L, ROOF_V, ROOF_R = 0.0, 26.0, 10.0  # roof height at the left end, the valley, the right end


def arc(cx, cy, rx, ry, a0, a1, n=12):
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * k / n)),
             cy + ry * math.sin(math.radians(a0 + (a1 - a0) * k / n))) for k in range(n + 1)]


def sarc(cx, cy, rx, ry, a0, a1, n=16, p=2.5):
    """part of a superellipse: a round shape squared toward its box, like the house's boards"""
    out = []
    for k in range(n + 1):
        a = math.radians(a0 + (a1 - a0) * k / n)
        c, s = math.cos(a), math.sin(a)
        out.append((cx + rx * math.copysign(abs(c) ** (2 / p), c), cy + ry * math.copysign(abs(s) ** (2 / p), s)))
    return out


def J(w):
    """a flat top bar, the stem, and a round hook whose short spur ends level"""
    r = w / 2
    cy = H - r
    return ([(0, 0), (w, 0)] + arc(r, cy, r, r, 0, 180, 18) + arc(r, cy, r - S, r - S, 180, 0, 14)
            + [(w - S, S), (0, S)])


def J_square(w):
    """a flat top bar, the stem, a flat foot with a rounded corner and a short spur"""
    R = S
    return ([(0, 0), (w, 0)] + arc(w - R, H - R, R, R, 0, 90, 8) + [(0, H), (0, H - S * 1.6), (S, H - S * 1.6), (S, H - S),
                                                                   (w - S, H - S), (w - S, S), (0, S)])


def J_tail(w):
    """a flat top bar, the stem, and a hook that turns only part way"""
    r = w / 2
    cy = H - r
    return [(0, 0), (w, 0)] + arc(r, cy, r, r, 0, 140, 16) + arc(r, cy, r - S, r - S, 140, 0, 12) + [(w - S, S), (0, S)]


def A(w):
    top = 0.36 * w
    return [(0, H), ((w - top) / 2, 0), ((w + top) / 2, 0), (w, H), (w / 2 + 0.17 * w, H), (w / 2, H * 0.52),
            (w / 2 - 0.17 * w, H)]


def F(w):
    m = H * 0.43
    return [(0, 0), (w, 0), (w, S), (S, S), (S, m), (w * 0.8, m), (w * 0.8, m + S), (S, m + S), (S, H), (0, H)]


def E(w):
    m = (H - S) / 2
    return [(0, 0), (w, 0), (w, S), (S, S), (S, m), (w * 0.82, m), (w * 0.82, m + S), (S, m + S), (S, H - S),
            (w, H - S), (w, H), (0, H)]


def D(w):
    rx = w * 0.72  # the bow squared toward its box
    return [(0, 0), (w - rx, 0)] + sarc(w - rx, H / 2, rx, H / 2, -90, 90, 24)[1:-1] + [(w - rx, H), (0, H)]


def R(w):
    b = H * 0.56  # the bowl's depth
    rx = b * 0.5
    e = w - S * 0.25  # the bowl and the leg end on one line
    return ([(0, 0), (e - rx, 0)] + sarc(e - rx, b / 2, rx, b / 2, -90, 90, 16)[1:-1]
            + [(e, H), (e - S * 1.15, H), (S, b + S * 0.35), (S, H), (0, H)])


def O(w):
    return sarc(w / 2, H / 2, w / 2, H / 2, 0, 360, 48)[:-1]


def N(w):
    return [(0, 0), (S, 0), (w - S, H * 0.62), (w - S, 0), (w, 0), (w, H), (w - S, H), (S, H * 0.38), (S, H), (0, H)]


LETTERS = [(J, 62), (A, 74), (F, 52), (F, 52), (E, 48), (D, 50), (R, 64), (O, 58), (N, 66), (E, 48)]
# optical spacing after each letter: a round side leaves air of its own, so
# the gaps beside the O close up
KERN = [0, 0, 0, 0, 0, 0, -6.5, -2, 0, 0]
VALLEY_AFTER = 4  # the roofs meet after the first E


def layout():
    x, out = 0.0, []
    for i, (fn, w) in enumerate(LETTERS):
        out.append((x, w, fn(w)))
        x += w + GAP + KERN[i] + (GAP * 0.6 if i == VALLEY_AFTER else 0)
    return out, x - GAP


def densify(pts, step=3.0):
    out = []
    for i in range(len(pts)):
        (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % len(pts)]
        n = max(1, int(math.hypot(x1 - x0, y1 - y0) / step))
        out += [(x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n) for k in range(n)]
    return out


def simplify(pts, tol=0.25):
    """drop points that lie on a straight line between their neighbours"""
    out = list(pts)
    changed = True
    while changed and len(out) > 3:
        changed = False
        for i in range(len(out)):
            a, b, c = out[i - 1], out[i], out[(i + 1) % len(out)]
            cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            if abs(cross) / max(1e-6, math.hypot(c[0] - a[0], c[1] - a[1])) < tol:
                del out[i]
                changed = True
                break
    return out


def main():
    letters, W = layout()
    vx = letters[VALLEY_AFTER][0] + letters[VALLEY_AFTER][1] + GAP * 0.8

    def roof(x):
        return ROOF_L + (ROOF_V - ROOF_L) * x / vx if x < vx else ROOF_V + (ROOF_R - ROOF_V) * (x - vx) / (W - vx)

    polys = []
    for x0, w, pts in letters:
        q = []
        for x, y in densify(pts):
            X = x0 + x
            q.append((X, roof(X) + y / H * (H - roof(X))))  # the top follows the roof, the base stays level
        polys.append(simplify(q))
    scale = 4  # store at 4 units per logo unit
    with open(sys.argv[1], "w") as f:
        f.write("/* generated by tools/mklogo.py: the wordmark as outlines */\n#pragma once\n#include <stdint.h>\n")
        f.write(f"#define LOGO_W {round(W * scale)}\n#define LOGO_H {round(H * scale)}\n#define LOGO_NLETTER {len(polys)}\n")
        f.write(f"#define LOGO_VALLEY_X {round(vx * scale)}\n#define LOGO_VALLEY_Y {round(ROOF_V * scale)}\n")
        f.write(f"#define LOGO_ROOF_L {round(ROOF_L * scale)}\n#define LOGO_ROOF_R {round(ROOF_R * scale)}\n")
        starts, flat = [], []
        for p in polys:
            starts.append(len(flat))
            flat += [(round(x * scale), round(y * scale)) for x, y in p]
        starts.append(len(flat))
        f.write(f"#define LOGO_MAXPT {max(b - a for a, b in zip(starts, starts[1:]))}\n")
        f.write("static const uint16_t logo_start[LOGO_NLETTER + 1] = {" + ", ".join(map(str, starts)) + "};\n")
        f.write("static const int16_t logo_pt[][2] = {\n")
        for i in range(0, len(flat), 8):
            f.write("    " + ", ".join(f"{{{x}, {y}}}" for x, y in flat[i:i + 8]) + ",\n")
        f.write("};\n")
    if len(sys.argv) > 2:
        preview(polys, W, vx, roof, sys.argv[2])


def preview(polys, W, vx, roof, path):
    from PIL import Image, ImageDraw
    k, pad, ss = 3.2, 50, 3  # drawn 3x and scaled down for smooth edges
    lw, lh = int(W * k + 2 * pad), int(H * k + 2 * pad)
    themes = [
        # name, background, outline, fill, groove
        ("night", (16, 18, 28), (226, 196, 150), (40, 36, 40), (90, 72, 60)),
        ("cedar", (182, 108, 62), (40, 24, 18), (196, 124, 76), (150, 86, 50)),
        ("siding", (226, 224, 216), (34, 34, 38), (236, 234, 228), (180, 178, 170)),
    ]
    im = Image.new("RGB", (lw * ss, lh * len(themes) * ss))
    d = ImageDraw.Draw(im)
    for t, (_, bg, ol, fill, groove) in enumerate(themes):
        oy = t * lh * ss
        d.rectangle([0, oy, lw * ss, oy + lh * ss], fill=bg)
        mask = Image.new("L", im.size, 0)
        md = ImageDraw.Draw(mask)
        for p in polys:
            q = [((pad + x * k) * ss, oy + (pad + y * k) * ss) for x, y in p]
            d.polygon(q, fill=fill)
            md.polygon(q, fill=255)
        # the siding: vertical boards inside the letters
        stripes = Image.new("RGB", im.size, groove)
        smask = Image.new("L", im.size, 0)
        sd = ImageDraw.Draw(smask)
        x = pad * ss
        while x < (pad + W * k) * ss:
            sd.rectangle([x, oy, x + 2 * ss - 1, oy + lh * ss], fill=255)
            x += 7 * k * ss
        from PIL import ImageChops
        im.paste(stripes, (0, 0), ImageChops.multiply(mask, smask))
        for p in polys:
            q = [((pad + x * k) * ss, oy + (pad + y * k) * ss) for x, y in p]
            d.line(q + [q[0]], fill=ol, width=int(2.4 * ss * k / 2), joint="curve")
    im.resize((lw, lh * len(themes)), Image.LANCZOS).save(path)


if __name__ == "__main__":
    main()
