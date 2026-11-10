#!/usr/bin/env python3
"""The Jaffedrone manual as a PDF: each page shows a TrimUI Brick, drawn
(a schematic, not to scale), with the app's own screen on it and the
buttons for each command lit and numbered; the Brick Pro's pages add its
two sticks.

usage: mkmanual.py SHOTS_DIR OUT.pdf
         SHOTS_DIR holds the screenshots `build/jd_test` writes (play.ppm, ...)
       mkmanual.py --check OUT.pdf
         fails if the manual is older than what it is made from

The manual carries a fingerprint of its sources (INPUTS: the screens and
controls, the version, this script); --check compares it with the files
now, so CI catches a change that was not followed by `make manual`.
"""
import hashlib
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
# what the manual shows: change any of these and the manual must be rebuilt
INPUTS = ["src/ui.c", "src/engine.h", "src/midi.h", "src/logo_data.h", "tools/mkmanual.py", "dist/Jaffedrone/splash.jdi"]


def fingerprint():
    h = hashlib.sha256()
    for f in INPUTS:
        h.update(f.encode() + b"\0" + open(os.path.join(ROOT, f), "rb").read())
    return h.hexdigest()[:16]


def check(pdf):
    want = "jdmanual-" + fingerprint()
    data = open(pdf, "rb").read()
    if want.encode() in data:
        print("manual is up to date (%s)" % want)
        return 0
    print("the manual is out of date: run `make manual` and commit %s" % os.path.relpath(pdf, ROOT))
    return 1

if __name__ == "__main__" and len(sys.argv) > 2 and sys.argv[1] == "--check":
    sys.exit(check(sys.argv[2]))  # needs nothing but Python

from PIL import Image
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import letter
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.lib.utils import ImageReader
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (Flowable, KeepTogether, PageBreak, Paragraph, SimpleDocTemplate, Spacer, Table,
                                TableStyle)

FONT_DIR = "/usr/share/fonts/truetype/dejavu"
pdfmetrics.registerFont(TTFont("Sans", os.path.join(FONT_DIR, "DejaVuSans.ttf")))
pdfmetrics.registerFont(TTFont("Sans-Bold", os.path.join(FONT_DIR, "DejaVuSans-Bold.ttf")))
pdfmetrics.registerFont(TTFont("Mono", os.path.join(FONT_DIR, "DejaVuSansMono.ttf")))

# the app's palette
CEDAR = colors.HexColor("#b26a3e")
SLATE = colors.HexColor("#4a525e")
SLATE_D = colors.HexColor("#2c323c")
SAND = colors.HexColor("#d9c3a0")
COPPER = colors.HexColor("#b3875c")
SEA = colors.HexColor("#7fd1a8")
NIGHT = colors.HexColor("#14161b")
INK = colors.HexColor("#22262e")
DIM = colors.HexColor("#6b6f7a")
PAPER = colors.HexColor("#faf8f4")
BODY = colors.HexColor("#e6e3dc")
BODY_EDGE = colors.HexColor("#a9a59c")
KEY = colors.HexColor("#5b616c")

H1 = ParagraphStyle("h1", fontName="Sans-Bold", fontSize=20, leading=24, textColor=SLATE_D, spaceAfter=4)
H2 = ParagraphStyle("h2", fontName="Sans-Bold", fontSize=12.5, leading=16, textColor=CEDAR, spaceBefore=8,
                    spaceAfter=3)
P = ParagraphStyle("p", fontName="Sans", fontSize=9.5, leading=13.2, textColor=INK, spaceAfter=5)
SMALL = ParagraphStyle("s", parent=P, fontSize=8.3, leading=11, textColor=DIM)
CELL = ParagraphStyle("c", parent=P, fontSize=8.8, leading=11.4, spaceAfter=0)
CELLB = ParagraphStyle("cb", parent=CELL, fontName="Sans-Bold")
CAP = ParagraphStyle("cap", parent=SMALL, alignment=TA_CENTER)


# ------------------------------------------------------------------ logo
def logo_polys():
    src = open(os.path.join(ROOT, "src", "logo_data.h")).read()
    d = {k: int(v) for k, v in re.findall(r"#define (LOGO_\w+) (\d+)", src)}
    starts = [int(x) for x in re.search(r"logo_start\[[^\]]*\] = \{([^}]*)\}", src).group(1).split(",")]
    pts = [(int(a), int(b)) for a, b in re.findall(r"\{(-?\d+), (-?\d+)\}", src.split("logo_pt")[1])]
    return d, [pts[starts[i]:starts[i + 1]] for i in range(len(starts) - 1)]


LOGO_D, LOGO = logo_polys()


def draw_logo(c, x, y, h, fill=SLATE, edge=SLATE_D):
    """the wordmark, h points high, its top-left at (x, y) (y measured up)"""
    k = h / LOGO_D["LOGO_H"]
    c.saveState()
    c.setFillColor(fill)
    c.setStrokeColor(edge)
    c.setLineWidth(max(0.4, h / 90))
    c.setLineJoin(1)
    for poly in LOGO:
        p = c.beginPath()
        for i, (px, py) in enumerate(poly):
            (p.moveTo if i == 0 else p.lineTo)(x + px * k, y - py * k)
        p.close()
        c.drawPath(p, fill=1, stroke=1)
    c.restoreState()
    return LOGO_D["LOGO_W"] * k


# ---------------------------------------------------------------- device
SHOTS = {}


def shot(name):
    """a screenshot as an ImageReader (converted once from the test's PPM)"""
    if name not in SHOTS:
        path = os.path.join(SHOT_DIR, name + ".ppm")
        im = Image.open(path).convert("RGB")
        png = os.path.join(TMP_DIR, name + ".png")
        im.save(png)
        SHOTS[name] = ImageReader(png)
    return SHOTS[name]


class Brick(Flowable):
    """a TrimUI Brick from the front (a schematic), its shoulder buttons
    above, the app on its 4:3 screen; `lit` maps buttons (UP DOWN LEFT RIGHT
    DPAD A B X Y L1 L2 R1 R2 SELECT START MENU, and on the Brick Pro LS RS
    for the sticks) to a badge number or "". pro: draw the two sticks."""

    W, H = 78.0, 112.0  # the body, millimetres

    def __init__(self, screen, lit=None, width=62 * mm, caption=None, pro=False):
        super().__init__()
        self.screen, self.lit, self.caption, self.pro = screen, lit or {}, caption, pro
        self.k = width / self.W
        self.width = width
        self.height = (self.H + 9) * self.k + (14 if caption else 0)

    def wrap(self, aw, ah):
        return self.width, self.height

    # coordinates in mm on the body, origin bottom left
    def X(self, v):
        return v * self.k

    def Y(self, v):
        return (v + (14 / self.k if self.caption else 0)) * self.k

    def key(self, name, shape, *a):
        c, k = self.canv, self.k
        on = name in self.lit or (name in ("UP", "DOWN", "LEFT", "RIGHT") and "DPAD" in self.lit)
        c.setFillColor(COPPER if on else KEY)
        c.setStrokeColor(colors.HexColor("#7a5530") if on else colors.HexColor("#3b4049"))
        c.setLineWidth(0.6)
        if shape == "circle":
            cx, cy, r = a
            c.circle(self.X(cx), self.Y(cy), r * k, fill=1, stroke=1)
        elif shape == "rect":
            x, y, w, h, r = a
            c.roundRect(self.X(x), self.Y(y), w * k, h * k, r * k, fill=1, stroke=1)
        return on

    def label(self, x, y, text, size=4.2, color=colors.white, font="Sans-Bold"):
        c = self.canv
        c.setFillColor(color)
        c.setFont(font, size * self.k * 0.9)
        c.drawCentredString(self.X(x), self.Y(y) - size * self.k * 0.32, text)

    def badge(self, x, y, n):
        if n in (None, ""):
            return
        c, k = self.canv, self.k
        c.setFillColor(CEDAR)
        c.setStrokeColor(colors.white)
        c.setLineWidth(0.8)
        c.circle(self.X(x), self.Y(y), 3.3 * k, fill=1, stroke=1)
        self.label(x, y, str(n), 4.0)

    def draw(self):
        c, k = self.canv, self.k
        W, H = self.W, self.H
        # shoulders: L2 / R2 behind, L1 / R1 in front, on the top edge
        for name, x in (("L2", 7), ("R2", W - 7 - 17)):
            self.key(name, "rect", x, H + 3.0, 17, 5.5, 1.6)
            self.label(x + 8.5, H + 6.1, name, 3.6)
        for name, x in (("L1", 4), ("R1", W - 4 - 21)):
            self.key(name, "rect", x, H - 1.5, 21, 4.6, 1.6)
            self.label(x + 10.5, H + 1.2, name, 3.6)
        # the body
        c.setFillColor(BODY)
        c.setStrokeColor(BODY_EDGE)
        c.setLineWidth(0.9)
        c.roundRect(self.X(0), self.Y(0), W * k, H * k, 7 * k, fill=1, stroke=1)
        # the screen: a 1024 x 768 picture (the app's 640 x 480, scaled) in its bezel
        c.setFillColor(colors.HexColor("#15171c"))
        c.roundRect(self.X(4), self.Y(55), 70 * k, 53 * k, 3.5 * k, fill=1, stroke=0)
        if self.screen:
            c.drawImage(shot(self.screen), self.X(7), self.Y(57.5), 64 * k, 48 * k)
        # the d-pad
        cx, cy = 18.0, 35.0
        c.setFillColor(colors.HexColor("#cfcbc2"))
        c.circle(self.X(cx), self.Y(cy), 12.5 * k, fill=1, stroke=0)
        for name, dx, dy in (("UP", 0, 1), ("DOWN", 0, -1), ("LEFT", -1, 0), ("RIGHT", 1, 0)):
            self.key(name, "rect", cx + dx * 6.2 - 3.4, cy + dy * 6.2 - 3.4, 6.8, 6.8, 1.2)
        c.setFillColor(KEY if "DPAD" not in self.lit else COPPER)
        c.rect(self.X(cx - 3.4), self.Y(cy - 3.4), 6.8 * k, 6.8 * k, fill=1, stroke=0)
        # the face buttons
        fx, fy, off = 60.0, 35.0, 7.8
        c.setFillColor(colors.HexColor("#cfcbc2"))
        c.circle(self.X(fx), self.Y(fy), 13.5 * k, fill=1, stroke=0)
        for name, dx, dy in (("X", 0, 1), ("A", 1, 0), ("B", 0, -1), ("Y", -1, 0)):
            self.key(name, "circle", fx + dx * off, fy + dy * off, 4.2)
            self.label(fx + dx * off, fy + dy * off, name, 4.4)
        # menu, select, start
        self.key("MENU", "circle", 39, 47.0, 2.4)
        self.label(39, 42.8, "MENU", 2.6, DIM, "Sans")
        sy_ = 9.0 if self.pro else 14.0
        self.key("SELECT", "rect", 27.5, sy_, 9, 3.4, 1.7)
        self.key("START", "rect", 41.5, sy_, 9, 3.4, 1.7)
        self.label(32, sy_ - 3.4, "SELECT", 2.6, DIM, "Sans")
        self.label(46, sy_ - 3.4, "START", 2.6, DIM, "Sans")
        if self.pro:  # the Brick Pro's two sticks, pressed for L3 / R3
            for name, x in (("LS", 22.0), ("RS", 56.0)):
                c.setFillColor(colors.HexColor("#cfcbc2"))
                c.circle(self.X(x), self.Y(14.5), 6.6 * k, fill=1, stroke=0)
                self.key(name, "circle", x, 14.5, 4.6)
                self.label(x, 14.5, "L" if name == "LS" else "R", 3.6)
        else:  # a speaker grille
            c.setFillColor(colors.HexColor("#b9b5ac"))
            for i in range(4):
                for j in range(3):
                    c.circle(self.X(66 + i * 2.2), self.Y(6 + j * 2.2), 0.55 * k, fill=1, stroke=0)
        # badges on the lit buttons
        pos = {"UP": (cx, cy + 6.2), "DOWN": (cx, cy - 6.2), "LEFT": (cx - 6.2, cy), "RIGHT": (cx + 6.2, cy),
               "DPAD": (cx - 8.8, cy + 8.8), "X": (fx + 4.6, fy + off + 4.0), "A": (fx + off + 4.6, fy + 4.0),
               "B": (fx + 4.6, fy - off + 4.0), "Y": (fx - off - 3.8, fy + 4.0), "MENU": (43.5, 49.0),
               "SELECT": (27.0, sy_ + 5.5), "START": (51.0, sy_ + 5.5), "L1": (29.0, H + 0.8),
               "R1": (W - 29.0, H + 0.8), "L2": (2.6, H + 6.3), "R2": (W - 2.6, H + 6.3),
               "LS": (22.0 - 7.5, 21.0), "RS": (56.0 + 7.5, 21.0)}
        for name, n in self.lit.items():
            if name in pos:
                self.badge(*pos[name], n)
        if self.caption:
            c.setFillColor(DIM)
            c.setFont("Sans", 7.5)
            c.drawCentredString(self.width / 2, 2, self.caption)


# --------------------------------------------------------------- helpers
def table(rows, widths=(30 * mm, None), head=None, badge_col=True):
    """a button table: [badge, buttons, what it does]"""
    data = []
    if head:
        data.append([Paragraph(h, CELLB) for h in head])
    for r in rows:
        data.append([Paragraph(str(x), CELLB if i == 0 else CELL) for i, x in enumerate(r)])
    t = Table(data, colWidths=widths, hAlign="LEFT")
    style = [("VALIGN", (0, 0), (-1, -1), "TOP"), ("LINEBELOW", (0, 0), (-1, -1), 0.4, colors.HexColor("#e2ddd3")),
             ("TOPPADDING", (0, 0), (-1, -1), 3), ("BOTTOMPADDING", (0, 0), (-1, -1), 3),
             ("LEFTPADDING", (0, 0), (-1, -1), 3)]
    if head:
        style += [("LINEBELOW", (0, 0), (-1, 0), 0.8, CEDAR)]
    t.setStyle(TableStyle(style))
    return t


def num(n):
    return '<font color="#b26a3e"><b>%s</b></font>' % n


def side(device, *flow, dw=66 * mm):
    """the device on the left, the words on the right"""
    right = [f for f in flow]
    t = Table([[device, right]], colWidths=(dw, None), hAlign="LEFT")
    t.setStyle(TableStyle([("VALIGN", (0, 0), (-1, -1), "TOP"), ("LEFTPADDING", (0, 0), (-1, -1), 0),
                           ("RIGHTPADDING", (0, 0), (0, 0), 10)]))
    return t


def B(name):
    return '<font name="Mono" color="#2c323c"><b>%s</b></font>' % name


# ------------------------------------------------------------------ pages
def cover(c, doc):
    w, h = letter
    c.saveState()
    c.setFillColor(CEDAR)
    c.rect(0, 0, w, h, fill=1, stroke=0)
    # the house's two roofs
    c.setStrokeColor(colors.HexColor("#8f5230"))
    c.setLineWidth(3)
    c.line(0, h - 70, w * 0.49, h - 190)
    c.line(w * 0.49, h - 190, w, h - 110)
    lw = draw_logo(c, 0, 0, 0)  # the width per height
    lh = 46
    lx = (w - LOGO_D["LOGO_W"] * lh / LOGO_D["LOGO_H"]) / 2
    draw_logo(c, lx, h - 230, lh)
    c.setFillColor(SLATE_D)
    c.setFont("Sans-Bold", 13)
    c.drawCentredString(w / 2, h - 300, "A drone instrument and meditation tool for the TrimUI Brick and Brick Pro")
    c.setFont("Sans", 10.5)
    c.drawCentredString(w / 2, h - 318, "User manual  ·  version %s" % VERSION)
    d = Brick("splash", width=78 * mm)
    d.drawOn(c, (w - d.width) / 2, 110)
    c.setFont("Sans", 8)
    c.setFillColor(colors.HexColor("#5a3520"))
    c.drawCentredString(w / 2, 60, "github.com/sydilaxe/Trimui-Jaffedrone-dev")
    c.restoreState()


def later(c, doc):
    w, h = letter
    c.saveState()
    c.setFillColor(PAPER)
    c.rect(0, 0, w, h, fill=1, stroke=0)
    c.setFillColor(CEDAR)
    c.rect(0, h - 26, w, 26, fill=1, stroke=0)
    draw_logo(c, 36, h - 6.5, 13)
    c.setFillColor(colors.white)
    c.setFont("Sans", 8)
    c.drawRightString(w - 36, h - 17, "Jaffedrone %s manual" % VERSION)
    c.setFillColor(DIM)
    c.drawCentredString(w / 2, 22, str(doc.page))
    c.restoreState()


def build(out):
    doc = SimpleDocTemplate(out, pagesize=letter, leftMargin=40, rightMargin=40, topMargin=46, bottomMargin=42,
                            title="Jaffedrone manual", author="Jaffedrone", subject="User manual",
                            keywords="jdmanual-" + fingerprint())
    s = [PageBreak()]

    # -------------------------------------------------- getting started
    s += [Paragraph("Getting started", H1),
          side(Brick("splash", {"START": 1, "L1": 2, "R1": 2, "MENU": 3}),
               Paragraph("Jaffedrone is a drone of five voices: strings that are bowed and plucked, or wind "
                         "voices that are blown and tongued, tuned to a scale and moving through chords. Each "
                         "voice has its own rhythm, or you play it by hand.", P),
               Paragraph("Install", H2),
               Paragraph("Jaffedrone runs on MustardOS. Copy <b>Jaffedrone-mustardos-brick.muxapp</b> (TrimUI "
                         "Brick and Brick Hammer) or <b>Jaffedrone-mustardos-brickpro.muxapp</b> (Brick Pro: it adds "
                         "the sticks) into the <b>ARCHIVE</b> folder of the first SD card, install it from "
                         "<b>Applications → Archive Manager</b> and open <b>Applications → Jaffedrone</b>. The "
                         "start-up screen shows for a moment; any button moves on.", P),
               Paragraph("The first buttons", H2),
               table([(num(1), B("START"), "start or stop the drone (it fades in and out): its rings, chord "
                                         "changes and clock. Only START (or a MIDI or Wi-Fi START) starts it; "
                                         "playing by hand or from MIDI sounds with it stopped"),
                      (num(2), B("L1") + " / " + B("R1"),
                       "previous / next page: PLAY, SOUND, COMPOSE, MIX, SONG, MIDI"),
                      ("", B("SELECT"), "the next chord"),
                      (num(3), B("MENU"), "leave the app")], widths=(9 * mm, 30 * mm, None)),
               Spacer(1, 6),
               Paragraph("Listen on headphones for the deepest low end and for binaural beats; the speaker "
                         "plays little below about 200 Hz.", SMALL))]

    # -------------------------------------------------- the six pages
    s += [Paragraph("The six pages", H2)]
    thumbs = [("play", "PLAY: the strings and their rhythms"), ("wind_sound", "SOUND: each voice's tone"),
              ("compose", "COMPOSE: scale, chords, change"), ("mix", "MIX: levels and effects"),
              ("song", "SONG: presets, chord memory, key"), ("midi", "MIDI: channels, keys, CC map")]
    cells = []
    for name, cap in thumbs:
        cells.append([ImageFlow(name, 82), Paragraph(cap, CAP)])
    t = Table([[c[0] for c in cells], [c[1] for c in cells]], colWidths=[87] * 6)
    t.setStyle(TableStyle([("ALIGN", (0, 0), (-1, -1), "CENTER"), ("LEFTPADDING", (0, 0), (-1, -1), 2),
                           ("RIGHTPADDING", (0, 0), (-1, -1), 2)]))
    s += [t, Paragraph(B("R1") + " moves right along the tabs at the top, " + B("L1") + " left, on every "
                       "page (round from MIDI to PLAY).", SMALL), PageBreak()]

    # -------------------------------------------------- PLAY: rings
    s += [Paragraph("PLAY: the rhythm rings", H1),
          side(Brick("play", {"LEFT": 1, "RIGHT": 1, "UP": 2, "DOWN": 2, "A": 3, "B": 4, "X": 5, "Y": 6}),
               Paragraph("Each string has two Euclidean rings: the <b>pluck ring</b> plucks on its pulses and "
                         "the <b>bow ring</b> swells the bow on its pulses (off, the bow plays steadily). A ring "
                         "spreads its pulses as evenly as it can over its steps, like the rhythms of HI5 and "
                         "RadioScan.", P),
               table([(num(1), B("◀") + " " + B("▶"), "choose a string"),
                      (num(2), B("▲") + " " + B("▼"), "choose its pluck ring or bow ring"),
                      (num(3), B("A"), "switch the ring on or off"),
                      (num(4), B("B"), "fewer pulses, wrapping back to all"),
                      (num(5), B("X"), "rotate the pattern"),
                      (num(6), B("Y"), "a new simple pattern")], widths=(9 * mm, 22 * mm, None)),
               Spacer(1, 6),
               Paragraph("A label like <b>PLUCK 70%</b> means each pulse of that ring plays seven times in "
                         "ten (set with " + B("R2") + " + " + B("X") + " / " + B("Y") + ").", SMALL))]
    s += [Spacer(1, 8), Paragraph("Shaping a ring: hold L2", H2),
          side(Brick("play", {"L2": 1, "UP": 2, "DOWN": 2, "LEFT": 3, "RIGHT": 3, "X": 4, "Y": 5, "A": 6}),
               table([(num(1), "hold " + B("L2"), "then:"),
                      (num(2), B("▲") + " " + B("▼"), "more / fewer steps"),
                      (num(3), B("◀") + " " + B("▶"), "step length (right is faster): 1/16 to 2 bars"),
                      (num(4), B("X"), "more pulses"),
                      (num(5), B("Y"), "rotate back"),
                      (num(6), B("A"), "mute or unmute the string (a red bar marks it)")],
                     widths=(9 * mm, 22 * mm, None))), PageBreak()]

    # -------------------------------------------------- PLAY: by hand
    s += [Paragraph("Playing by hand", H1),
          Paragraph("The lower shoulder buttons play the string selected on PLAY, over whatever its rings are "
                    "doing. A wind voice is tongued and blown instead.", P),
          side(Brick("play_bow", {"L2": 1, "START": 2, "R2": 3}),
               table([(num(1), B("L2"), "pluck the selected string, the moment you press (wind: tongue a note; "
                                        "struck: the mallet). Held, " + B("L2") + " still shapes the ring"),
                      (num(2), B("START") + " + " + B("L2"),
                       "drop a dulcimer mallet on it: a bright hit, then the mallet bounces, each rebound lower "
                       "and sooner, until it rests (wind: a flutter tongue, the breath rolling \"frrr\" and "
                       "settling; struck: a hard mallet, bouncing)"),
                      (num(3), B("R2") + " held",
                       "bow it: the bow is drawn in slowly, swelling over about two seconds and brightening as "
                       "it bites, and sings while you hold (wind: blow). Press another button with " + B("R2") +
                       " held and it is a quick sound change (below): the bow lifts"),
                      ("", B("L2") + " + " + B("R2") + ", either first",
                       "bow with a light touch at a node: the next harmonic up sings (wind: overblow, an octave "
                       "up, a twelfth for the reed). Bowing with " + B("R2") + ", add " + B("L2") + " for "
                       "overblows as you bow")], widths=(9 * mm, 26 * mm, None)),
               Spacer(1, 6),
               Paragraph("The bowed string's column lights while the bow is on it.", SMALL))]
    s += [Spacer(1, 8), Paragraph("Quick sound changes: hold R2", H2),
          side(Brick("play", {"R2": 1, "LEFT": 2, "RIGHT": 2, "UP": 3, "DOWN": 3, "X": 4, "Y": 4, "A": 5, "B": 5}),
               table([(num(1), "hold " + B("R2"), "then:"),
                      (num(2), B("◀") + " " + B("▶"), "previous / next scale"),
                      (num(3), B("▲") + " " + B("▼"), "tone: brighter / darker"),
                      (num(4), B("X") + " / " + B("Y"), "the selected ring's chance, more / less"),
                      (num(5), B("A") + " / " + B("B"), "body resonance, more / less")],
                     widths=(9 * mm, 22 * mm, None))), PageBreak()]

    # -------------------------------------------------- LATCH
    s += [Paragraph("LATCH: hold each string yourself", H1),
          Paragraph("In LATCH the rings rest and every string is silent until you play it. A string you pluck "
                    "or bow keeps droning, with no need to hold the button, until you play it again; then it "
                    "fades over its own RELEASE. The drone needn't be started; stopping it (" + B("START") + ") lets "
                    "every latched string go.", P),
          side(Brick("latch", {"START": 1, "A": 5, "LEFT": 2, "RIGHT": 2, "L2": 3, "R2": 3, "UP": 4,
                               "DOWN": 4, "B": 6, "X": 7, "Y": 7}),
               table([(num(1), B("START") + " + " + B("A"), "switch PLAY MODE between RINGS and LATCH (also on "
                                                             "the SONG page)"),
                      (num(2), B("◀") + " " + B("▶"), "choose a string"),
                      (num(3), B("L2") + " / " + B("R2"),
                       "pluck / bow it on: it drones (" + B("START") + " + " + B("L2") + ": the mallet drops it "
                       "on). Again: it lets go and fades. " + B("R2") + " held for a quick sound change puts the "
                       "string back as it was"),
                      (num(4), B("▲") + " " + B("▼"), "that string's RELEASE, 0.05 s to 20 s"),
                      (num(5), B("A"), "let every string go"),
                      (num(6), B("B") + " held, or " + B("L2") + " + " + B("R2"),
                       "the next harmonic up on the droning string (wind: overblow); with both shoulders, "
                       "either first, it lasts while both are held"),
                      (num(7), B("Y") + " / " + B("X"), "that string's bow ATTACK, shorter / longer, 0.05 s to "
                                                         "10 s")], widths=(9 * mm, 26 * mm, None)),
               Spacer(1, 6),
               Paragraph("Each column shows a board with a shed-roof top. It fills while its string drones and "
                         "drains as it fades, marked <b>STRUCK</b>, <b>PLUCKED</b>, <b>BOWED</b>, "
                         "<b>HARMONIC</b> or <b>FADING</b>, with its ATTACK and RELEASE beneath. A pluck or "
                         "strike starts at once; a bow swells in over its ATTACK. RELEASE is also on the MIX "
                         "page; LATCH, ATTACK and RELEASE are saved with songs and presets.", SMALL))]
    s += [Spacer(1, 6), Paragraph("Try it", H2),
          Paragraph("Press " + B("START") + " + " + B("A") + ". Bow string 1 with " + B("R2") + " for a low "
                    "drone, move to string 3 and pluck it with " + B("L2") + ", then press " + B("L2") + " on "
                    "string 3 again to let it ring away. Raise string 5's RELEASE with " + B("▲") + " to ten "
                    "seconds or more for a long dying shimmer. Press " + B("START") + " + " + B("L2") + " on string "
                    "4 to drop the mallet on it, "
                    "and hold " + B("B") + " to let its harmonic sing. Lower string 1's ATTACK with " + B("Y") +
                    " for a bow that bites at once.", P), PageBreak()]

    # -------------------------------------------------- edit pages
    s += [Paragraph("SOUND, COMPOSE and MIX", H1),
          Paragraph("On these pages the d-pad moves to the nearest control in that direction, and " + B("Y") +
                    " / " + B("X") + " lower / raise it. Held, they speed up.", P),
          side(Brick("wind_sound", {"DPAD": 1, "Y": 2, "X": 2, "A": 3, "B": 4}),
               table([(num(1), "d-pad", "move between controls"),
                      (num(2), B("Y") + " / " + B("X"), "lower / raise the control"),
                      (num(3), B("A"), "on a string's control: mute or unmute that string"),
                      (num(4), B("B"), "pluck the string (to hear a change)")], widths=(9 * mm, 20 * mm, None)),
               Paragraph("SOUND", H2),
               Paragraph("The <b>VOICE</b> row makes each column a <b>STRING</b>, one of seven winds (LIP, "
                         "REED, DOUBLE, FLUTE, FREE, RECORDER, BANSURI) or one of nine struck voices (see "
                         "Struck voices). " + B("R2") + " + " + B("Y") + " / " + B("X") + " on VOICE jumps to "
                         "the previous / next family; " + B("Y") + " / " + B("X") + " alone step one at a time. "
                         "The rows below follow the voice: a string has BOW, DECAY, BRIGHT, FOLD, STIFF, POS and "
                         "BUZZ; a wind BREATH, AIR, EMBOUCH, VIBRATO and GROWL; a struck voice BOW, RING, MALLET, "
                         "STRETCH, SPOT, SIZZLE and SING. The shared row holds COUPLE, BODY, its TYPE, the winds' "
                         "BORE and CUTOFF; the last row the sympathetic strings.", P))]
    s += [Spacer(1, 4),
          Table([[ImageFlow("compose", 230), ImageFlow("mix", 230)],
                 [Paragraph("<b>COMPOSE</b>: the scale and root (a wheel of the scale's notes, the chord "
                            "lit), how often chords change and how: MOTION, HOME, COLOR, CHANCE, RELATE, "
                            "EVOLVE, GLIDE, tempo, Wi-Fi sync and two LFOs.", SMALL),
                  Paragraph("<b>MIX</b>: each voice's LEVEL, PAN, GATE (RELEASE in LATCH) and AURA, the master, "
                            "undertones (SUB), then AURA, delay, reverb and binaural; under AURA, the MIDI "
                            "clock's MIDI and START settings (see USB).", SMALL)]],
                colWidths=(250, 250))]
    s += [PageBreak()]

    s += [Paragraph("AURA: voices that listen", H1),
          Paragraph("AURA lets some voices sound only from the others, as if they stood beside them in one "
                    "room: a gong humming along with a flute, bowls singing when a reed plays. On MIX, set "
                    "each voice's <b>AURA</b> to <b>SEND</b> (it plays, and its sound goes into the air), "
                    "<b>LISTEN</b> (it is never bowed, blown, struck or plucked; it sounds only from what "
                    "reaches it) or <b>OFF</b> (it plays, out of the air). Then raise <b>AMOUNT</b> under "
                    "AURA, from OFF to 100.", P),
          Paragraph("Each voice stands where its PAN puts it: the players' sound reaches a listener later and "
                    "fainter the further it stands. A listener rings where its own notes meet the players': "
                    "struck bodies with a long RING sing most. The sound only goes one way, from players to "
                    "listeners (and from one listener to the next), so it can never feed back. With every "
                    "voice on SEND, AURA does nothing: at least one must listen. Delay, reverb and binaural "
                    "come after, on everything.", P),
          PageBreak()]

    # -------------------------------------------------- struck voices
    s += [Paragraph("Struck voices", H1),
          Paragraph("A struck voice is a physical model of a vibrating body: its resonant modes, struck by a "
                    "mallet (a mass on a spring that meets the surface and bounces off, so a hard mallet makes "
                    "a short bright contact and a soft one a long dark push) or bowed (stick-slip friction, "
                    "so the modes it meets grow until they sing, as on a rubbed bowl or a bowed cymbal). "
                    "Every voice can be struck and bowed.", P),
          side(Brick("wind_play", {"L2": 1, "R2": 2, "B": 3}),
               table([(num(1), B("L2"), "strike (" + B("START") + " + " + B("L2") + ": a hard mallet, dropped, "
                                        "bouncing)"),
                      (num(2), B("R2"), "bow: the body catches and sings"),
                      (num(3), B("L2") + " + " + B("R2") + ", " + B("B"),
                       "a touch: the fundamental is damped and the next mode rings and sings")],
                     widths=(9 * mm, 24 * mm, None)),
               Spacer(1, 4),
               table([("GENDER", "gamelan metallophone bars, in beating pairs (ombak)"),
                      ("BONANG", "gamelan kettle gongs, paired"),
                      ("GONG", "a large gong, low, its pairs beating slowly"),
                      ("BOWL", "a singing bowl: struck, or rubbed until it sings"),
                      ("HANDPAN", "fundamental, octave and fifth above, tuned"),
                      ("STEELPAN", "the same family, brighter"),
                      ("TONGUE", "a steel tongue drum: pure, a bar held at one end"),
                      ("CYMBAL", "a dense shimmer of modes; bowed, it screams and sings"),
                      ("DRUM", "a membrane: a short pitched thud"),
                      ("GLASS", "a glass harmonica: pure, long, made to be bowed"),
                      ("CHORD", "metal tuned to the scale: it rings as a chord"),
                      ("ALIEN", "golden-ratio modes: a metal no body has")], widths=(22 * mm, None)),
               Spacer(1, 4),
               table([("RING", "how long it rings; from 95 up a metal drone (DRONE)"),
                      ("MALLET", "soft felt to hard metal"),
                      ("STRETCH", "pulls the modes apart, far at the top: unearthly clangor"),
                      ("SPOT", "where it is struck and bowed: which modes speak"),
                      ("SIZZLE", "a rattle that follows the ring"), ("SING", "the mode the bow favours"),
                      ("BOW", "bow pressure: from a whisper to a scream")], widths=(22 * mm, None))),
          Paragraph("Each struck body has its own limiter, so strikes on a long ring never pile up. "
                    "The mode frequencies follow measurements and theory: rectangular-bar modes and the "
                    "makers' harmonic partials for the gamelan bars (Sethares), 1 : 2 : 3 for handpan and pan, "
                    "1, 2.9, 5.7, 9.1 for the bowl (Inacio, Henrique and Antunes), a clamped bar for the "
                    "tongue drum, a thin plate's dense series for the cymbal, the Bessel-function modes of a "
                    "membrane for the drum.", SMALL), PageBreak()]

    # -------------------------------------------------- SONG
    s += [Paragraph("SONG: presets, chord memory and key", H1),
          side(Brick("song", {"DPAD": 1, "A": 2, "X": 3, "R2": 4, "Y": 4}),
               table([(num(1), "d-pad", "choose a preset slot or a setting"),
                      (num(2), B("A"), "load the preset (your Wi-Fi and output settings stay)"),
                      (num(3), B("X"), "save the sound and rhythms to the slot"),
                      (num(4), B("R2") + " + " + B("Y"), "clear the slot, or the chord memory")],
                     widths=(9 * mm, 20 * mm, None)),
               Paragraph("Ten preset slots keep everything: voices, rhythms, scale, effects, the chord memory "
                         "and PLAY MODE.", P),
               Paragraph("Chord memory", H2),
               Paragraph("Set <b>PROG</b> to RECORD and every chord change (" + B("SELECT") + " or CHANGES) "
                         "is kept, up to 16. PLAY steps through them round and round; FREE lets the "
                         "algorithm choose again.", P),
               Paragraph("Playing with others", H2),
               Paragraph("<b>KEY LOCK</b> holds the root, scale and MODE (the mode chords come from in EQUAL) "
                         "and tunes chords exactly. <b>ATTACK</b> and <b>RELEASE</b> set how the drone fades "
                         "in and out with " + B("START") + ".", P),
               Paragraph("Output: mono + clock", H2),
               Paragraph("<b>OUTPUT</b> MONO+CLK plays the sound in mono on one side of the headphone jack and "
                         "clock pulses on the other, to lead a Pocket Operator, Volca or other gear with a sync "
                         "input (use a stereo-to-two-mono cable). <b>CLOCK ON</b> picks the side, <b>PPQN</b> the "
                         "pulses per quarter note (2 for Pocket Operators and Volcas, 24 for DIN sync) and "
                         "<b>PULSE</b> the width. The pulses run while the drone plays, on the beat and with "
                         "Wi-Fi sync. Every PAN and stereo spread plays from the centre; the settings are kept "
                         "for STEREO, and binaural becomes OMBAK.", P)), PageBreak()]

    # -------------------------------------------------- quiet view
    s += [Paragraph("The quiet view", H1),
          side(Brick("quiet", {"START": 1, "R2": 1}),
               Paragraph(B("START") + " + " + B("R2") + " on PLAY for the quiet view: the five strings across "
                         "the screen with their rings, and only the chord in words. Again to come back.", P),
               table([(num(1), B("START") + " + " + B("R2"), "the quiet view on / off")],
                     widths=(9 * mm, 24 * mm, None))),
          Spacer(1, 10)]

    # -------------------------------------------------- binaural & tunings
    s += [Paragraph("Binaural, ombak and tunings", H1),
          Paragraph("On MIX, <b>BEAT</b> sets a binaural beat from 0.1 to 40 Hz: the voices below about 1 kHz "
                    "are shifted down in the left ear and up in the right, so every low partial beats between "
                    "the ears. <b>DEPTH</b> sets how much. <b>MODE</b>:", P),
          table([("FREE", "the beat as set"),
                 ("TEMPO", "the beat moves to a power-of-two multiple of the tempo"),
                 ("A>T 10M", "a journey from alpha (10 Hz) down to theta (6 Hz) over ten minutes"),
                 ("T>D 15M", "from theta (6 Hz) down to delta (2.5 Hz) over fifteen minutes"),
                 ("OMBAK", "the beat for both ears, like a Balinese gamelan's paired instruments: the low end "
                           "swells and fades, and it works on the speaker")], widths=(24 * mm, None)),
          Paragraph("Settings to start from", H2),
          table([("Meditation", "BEAT 6 MEDITATE, headphones; or MODE A>T 10M to drift down from alpha"),
                 ("Sleep", "MODE T>D 15M, headphones, a long ATTACK and RELEASE"),
                 ("Gamelan shimmer", "scale TIRUS or BEGBEG, MODE OMBAK, BEAT 7 Hz, on the speaker")],
                widths=(34 * mm, None)),
          Paragraph("The research on binaural beats is limited and mixed; these are the settings most used, "
                    "not a promise of an effect.", SMALL),
          Paragraph("Tunings", H2),
          Paragraph("COMPOSE, or " + B("R2") + " + " + B("◀") + " / " + B("▶") + " on PLAY, chooses the scale: "
                    "EQUAL (with its modes), just major and minor, the harmonic series, just pentatonics, the "
                    "ragas YAMAN, BHAIRAV, KAFI, BHUPALI and MALKAUNS, the gamelan tunings SLENDRO, PELOG and "
                    "SELISIR, and the Balinese gong kebyar tunings BEGBEG, SEDANG and TIRUS (Toth's profiles, "
                    "as reproduced by Vitale and Sethares). Each scale has its own way of moving through "
                    "chords.", P), PageBreak()]

    # -------------------------------------------------- USB and the sticks
    # -------------------------------------------------- the MIDI page
    s += [Paragraph("The MIDI page", H1),
          Paragraph("A MIDI keyboard or controller plays the voices and moves the settings. The MIDI page "
                    "sets how; it belongs to the Brick, not the song (kept in <b>midi.cfg</b>, kept when a preset "
                    "loads).", P),
          side(Brick("midi", {"DPAD": 1, "Y": 2, "X": 2, "A": 3, "R2": 4, "START": 5, "B": 5}),
               table([(num(1), B("◀") + " " + B("▶") + " " + B("▲") + " " + B("▼"), "choose a setting or a CC"),
                      (num(2), B("Y") + " / " + B("X"), "change it (on the map: the CC, by hand)"),
                      (num(3), B("A"), "on the map: learn the entry's CC again (move a knob)"),
                      (num(4), B("R2") + " + " + B("Y"), "on the map: forget the entry"),
                      (num(5), B("START") + " + " + B("B"), "MIDI learn, on any page's selected control; again "
                                                             "to cancel")], widths=(9 * mm, 26 * mm, None))),
          table([("VOICE 1-5", "each voice's own channel (OFF, or 1-16): its keys, and its CCs, come on it"),
                 ("KEYS", "the channel of the keys that play the voice selected on PLAY: ANY (every channel "
                          "that is no voice's) or one"),
                 ("VEL", "how much velocity matters: 100 as struck, 0 every key alike"),
                 ("TOUCH", "what aftertouch moves, a key's own on its note only: BOW (harder), TONE "
                           "(brighter), SWELL (level, from 35% up), VIBRATO, PITCH (up to two semitones), "
                           "HARM (the next harmonic past halfway), FOLD, BUZZ, or OFF"),
                 ("MOD", "what the mod wheel moves, the same choice"),
                 ("BEND", "the pitch bend range, 1 to 24 semitones"),
                 ("PLAY", "what a key does: BOTH plucks and bows while held, PLUCK only plucks (it rings on), "
                          "BOW only bows (it swells in), STRIKE drops the mallet"),
                 ("HOLD", "how long: SONG as PLAY MODE (RINGS while held, LATCH until let go), GATE while held, "
                          "LATCH a key drones on and the same key again lets it go"),
                 ("SHIFT", "every key moved, -24 to +24 semitones"),
                 ("SNAP", "SCALE: each key moves to the nearest note of the song's scale and root"),
                 ("POLY", "how many keys a voice plays at once, 1 to 4 (at 1, each key takes the last's place)"),
                 ("CC MAP", "each CC and what it sets. Select any control on SOUND, COMPOSE, MIX or SONG, "
                            "press " + B("START") + " + " + B("B") + " and move a knob: that CC now sets it. A "
                            "voice's control moves the voice of the channel the CC comes on (the selected one "
                            "on the others). CCs 1, 64, 74, 91, 120 and 123 are fixed")],
                widths=(24 * mm, None)),
          Paragraph("Each key sounds its voice (its instrument and SOUND settings, level and pan) at its own "
                    "pitch on one of eight key voices beside the strings, so the drone and the song are "
                    "untouched and a muted string still plays from the keys. A voice plays four keys at once; a "
                    "fifth takes the place of its key held longest.", SMALL),
          PageBreak()]
    s += [Paragraph("USB: MIDI and audio", H1),
          Paragraph("The Brick's USB-C port takes class-compliant MIDI and audio devices (through an OTG adapter "
                    "or a hub): keyboards, controllers, sequencers, drum machines and USB sound cards. They are "
                    "found as they are plugged in; a message names each one.", P),
          Paragraph("MIDI in", H2),
          table([("Notes", "a key plays the voice of its channel (the MIDI page), at its own pitch, four at once "
                           "a voice; it plucks as hard as it is struck and bows while held (a wind voice is "
                           "tongued and blown). In LATCH a key drones (a soft key swells in with the bow, a firm "
                           "one plucks) until let go, then fades over the voice's RELEASE. Keys sound with the "
                           "drone stopped and never start it"),
                 ("Pitch bend", "BEND semitones (2 at first)"),
                 ("Aftertouch, mod wheel", "what TOUCH and MOD move (BOW at first)"),
                 ("CC64", "sustain"), ("CC74", "TONE: darker / brighter"), ("CC91", "more / less reverb"),
                 ("Clock", "the tempo follows it (fitted over the last four beats of ticks, so USB's jitter "
                           "averages out), its phase held on the sound as it is heard; START, CONTINUE, STOP and "
                           "song position start, stop and place the drone")], widths=(34 * mm, None)),
          Paragraph("MIDI out", H2),
          Paragraph("While the drone plays, and no clock is coming in, Jaffedrone sends MIDI clock (24 a "
                    "quarter note) with START and STOP to every MIDI port, timed to the sound as it is heard. "
                    "It leads a drum machine or sequencer over USB as MONO+CLK does over the headphone jack.", P),
          Paragraph("MIDI and START (on MIX)", H2),
          table([("MIDI", "which way the clock goes: AUTO (the default) sends it, and follows a clock that "
                          "comes in instead; SEND always sends it and ignores one coming in; FOLLOW only "
                          "follows; OFF neither (notes and CCs still come in)"),
                 ("START", "FOLLOW (the default) starts, stops and places the drone with the clock's START, "
                           "STOP, CONTINUE and song position; IGNORE follows the clock's tempo only, so you "
                           "start and stop it yourself")], widths=(34 * mm, None)),
          Paragraph("Both belong to the Brick, like WIFI SYNC: loading a preset keeps them.", SMALL),
          Paragraph("Linking Bricks", H2),
          Paragraph("Several Bricks play in time with one by cable, with no MIDI interface: the leader has a "
                    "powered USB hub in its top port, and each follower plugs its bottom (charging) port into "
                    "the hub with a data cable. The wiring decides who leads. Nothing to set up: Jaffedrone "
                    "offers its bottom port as a \"TrimUI clock link\", and a leader sends its clock, START and "
                    "STOP and where its sound is, about 50 times a second, so the followers sound together "
                    "within a tenth of a millisecond, up to 250 BPM. A follower plugged in mid-song comes in "
                    "at the leader's place; one set to START IGNORE keeps the leader's tempo and bar but "
                    "starts and stops by hand. Columbia Street, HI5 and RadioScan link the same way, so any "
                    "of them can lead. A leader can use USB audio and MIDI gear on the same hub; one that "
                    "follows a drum machine's clock passes it on. <b>JD_LINK=0</b> in launch.sh leaves the "
                    "bottom port alone.", P),
          Paragraph("Audio", H2),
          Paragraph("A USB sound card or DAC, once plugged in, takes the sound from the speaker and "
                    "headphones; unplugged, the sound comes back to them.", P),
          Paragraph("Settings", H2),
          Paragraph("In <b>launch.sh</b> in the app folder: <b>JD_MIDI_NOTES=CHANNELS</b> gives voices 1-5 "
                    "channels 1-5 until the MIDI page sets them; <b>JD_MIDI_CLOCK=0</b> never sends clock; <b>JD_MIDI_FOLLOW=0</b> never "
                    "follows it, whatever MIDI is set to; <b>JD_AUDIO=internal</b> keeps the sound on the speaker and "
                    "headphones.", SMALL),
          PageBreak()]
    s += [Paragraph("Brick Pro: the sticks", H1),
          side(Brick("play_bow", {"LS": 1, "RS": 2}, pro=True),
               Paragraph("On the Brick Pro the sticks play the string selected on PLAY, and spring back when let "
                         "go: nothing they do is saved. Each stick reaches its controls' whole range, straight or "
                         "on a diagonal: it learns how far it really goes the first time it is pushed all the "
                         "way.", P),
               table([(num(1), "left stick ▲", "bow the string by hand: a light touch draws the bow lightly, "
                                               "all the way up as hard as the bow goes, half again as hard as R2 "
                                               "(a wind voice: blow)"),
                      ("", "left stick ◀ ▶", "bend the string, up to two semitones either way"),
                      (num(2), "right stick ▲ ▼", "HARM: up touches higher harmonics, to the 5th; down, lower, to "
                                                 "none (a wind voice: its register; struck: the mode the bow "
                                                 "sings)"),
                      ("", "right stick ◀ ▶", "the SOUND control on the stick (BRIGHT at first): right up to "
                                             "its most, left down to its least"),
                      ("", "L3 (left click)", "pluck the string"),
                      ("", "R3 (right click)", "the next chord; on SOUND, put the row under the cursor on the "
                                              "right stick")], widths=(9 * mm, 30 * mm, None)),
               Spacer(1, 6),
               Paragraph("<b>JD_BEND=7</b> in launch.sh bends up to seven semitones.", SMALL)),
          Paragraph("Choosing the right stick's control", H2),
          side(Brick("sound_stick", {"RS": 1}, pro=True),
               Paragraph("On SOUND, move to a row (BOW, DECAY, BRIGHT, FOLD, STIFF, POS, BUZZ or NOTE) and click "
                         "the right stick " + num(1) + ": its left / right now plays that control. The choice is "
                         "kept. The rows the stick plays are marked on the selected string's column, and light up "
                         "while the stick moves them; the row shows the value playing.", P),
               Paragraph("The lights", H2),
               Paragraph("The lights around the sticks show how far they are pushed: the left glows amber as it "
                         "bows and rose as it bends, the right green for the harmonics and gold for its SOUND "
                         "control, brighter the further each goes. At rest they are dark; on leaving, they show "
                         "what they showed before.", P),
               Paragraph("In launch.sh: <b>JD_LEDS=0</b> leaves the lights alone; <b>JD_STICK_REACH=80</b> takes a "
                         "stick to reach 80% of the way before it shows how far it goes (60 at first).", SMALL)),
          PageBreak()]

    # -------------------------------------------------- log + quick reference
    s += [Paragraph("Quick reference", H1),
          side(Brick("play", {"START": "S", "SELECT": "s", "MENU": "M", "L1": "1", "R1": "2", "L2": "3",
                              "R2": "4"}, width=56 * mm),
               table([(B("START"), "start / stop the drone"),
                      (B("L1") + " / " + B("R1"), "previous / next page"),
                      (B("START") + " + " + B("A"), "PLAY MODE: RINGS / LATCH"),
                      ("LATCH " + B("B") + " / " + B("Y") + B("X"), "harmonic (held) / bow attack"),
                      (B("START") + " + " + B("SELECT"), "the log: OFF / BASIC / DETAIL"),
                      (B("START") + " + " + B("B"), "MIDI learn the selected control"),
                      (B("SELECT"), "next chord"),
                      (B("L2"), "pluck (wind: tongue); LATCH: on / let go"),
                      (B("START") + " + " + B("L2"), "drop the dulcimer mallet (wind: flutter tongue)"),
                      (B("R2"), "bow while held (wind: blow); LATCH: bow on / let go"),
                      (B("L2") + " + " + B("R2"), "harmonic (wind: overblow), either first"),
                      (B("START") + " + " + B("R2"), "quiet view"),
                      (B("L2") + " + ...", "shape the ring, mute (" + B("A") + ")"),
                      (B("R2") + " + ...", "scale, tone, chance, body"),
                      (B("MENU"), "leave")], widths=(36 * mm, None)), dw=60 * mm),
          Paragraph("The log", H2),
          Paragraph(B("START") + " + " + B("SELECT") + " cycles the log through OFF, BASIC and DETAIL. It is "
                    "written to <b>jaffedrone_log.txt</b> in the app folder: how hard the audio is working, any "
                    "dropouts, the levels, each voice's tuning (DETAIL) and what was played, with suggestions "
                    "when something could be better. If you hear crackles, set <b>JD_BUFFER=1024</b> in "
                    "<b>launch.sh</b>.", P)]

    doc.build(s, onFirstPage=cover, onLaterPages=later)


class ImageFlow(Flowable):
    """a screenshot, w points wide, with a fine frame"""

    def __init__(self, name, w):
        super().__init__()
        self.name, self.width, self.height = name, w, w * 0.75

    def wrap(self, aw, ah):
        return self.width, self.height

    def draw(self):
        self.canv.drawImage(shot(self.name), 0, 0, self.width, self.height)
        self.canv.setStrokeColor(colors.HexColor("#c9c3b8"))
        self.canv.setLineWidth(0.5)
        self.canv.rect(0, 0, self.width, self.height, fill=0, stroke=1)


if __name__ == "__main__":
    SHOT_DIR, OUT = sys.argv[1], sys.argv[2]
    TMP_DIR = os.path.join(ROOT, "build", "manual_png")  # the screenshots as PNG, made once
    os.makedirs(TMP_DIR, exist_ok=True)
    # a release build passes its tag's version; otherwise the one in the source
    VERSION = os.environ.get("JD_VERSION") or re.search(r'#define JD_VERSION "([^"]+)"', open(os.path.join(ROOT, "src", "engine.h")).read()).group(1)
    build(OUT)
