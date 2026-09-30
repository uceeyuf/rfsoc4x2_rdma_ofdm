#!/usr/bin/env python3
# Spectrum and constellation figure from host/ofdm_plots output (PIL only, no numpy):
#   python3 host/plot_ofdm.py build/plot_16qam docs/img/ofdm_16qam.png ["title"]
# left: received PSD (blue) over the modulator's own output (grey), right: the equalised data
# sub-carrier symbols as a density plot (log scale) with the ideal points.
# Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
import math
import re
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

pre, dst = sys.argv[1], sys.argv[2]
title = sys.argv[3] if len(sys.argv) > 3 else ""
FONT = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 13)
SMALL = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 11)
BG, FG, GRID, AX = (255, 255, 255), (30, 30, 30), (225, 225, 225), (90, 90, 90)
RX, TX = (31, 119, 180), (170, 170, 170)

# ---------------------------------------------------------------- data
lines = open(pre + "_spectrum.txt").read().splitlines()
head = lines[0]
qam = re.search(r"(\d+)-QAM", head).group(1)
evm = float(re.search(r"EVM (-?[\d.]+) dB", head).group(1))
spec = [tuple(map(float, ln.split())) for ln in lines[1:]]
raw = open(pre + "_const.bin", "rb").read()
n = len(raw) // 8
L = int(round(math.sqrt(int(qam))))

W, H = 1360, 560
im = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(im)
if title:
    d.text((20, 10), title, font=FONT, fill=FG)

# ---------------------------------------------------------------- spectrum
x0, y0, pw, ph = 80, 50, 760, 440
top = math.ceil(max(max(s[1], s[2]) for s in spec) / 10) * 10 + 10
bot = top - 80
fx = lambda f: x0 + (f + 1000) / 2000 * pw
fy = lambda v: y0 + (top - max(bot, min(top, v))) / (top - bot) * ph
for f in range(-1000, 1001, 250):
    d.line([(fx(f), y0), (fx(f), y0 + ph)], fill=GRID)
    d.text((fx(f) - 14, y0 + ph + 6), f"{f}", font=SMALL, fill=AX)
for v in range(int(bot), int(top) + 1, 10):
    d.line([(x0, fy(v)), (x0 + pw, fy(v))], fill=GRID)
    d.text((x0 - 40, fy(v) - 7), f"{v}", font=SMALL, fill=AX)
d.rectangle([x0, y0, x0 + pw, y0 + ph], outline=AX)
for col, idx in ((TX, 2), (RX, 1)):
    pts = [(fx(s[0]), fy(s[idx])) for s in spec]
    d.line(pts, fill=col, width=1)
d.text((x0 + pw / 2 - 90, y0 + ph + 24), "frequency (MHz), 2.0 GSPS complex", font=FONT, fill=FG)
d.text((12, y0 + ph / 2 - 60), "dBFS\nper\n0.98 MHz", font=SMALL, fill=FG)
d.line([(x0 + 14, y0 + 16), (x0 + 40, y0 + 16)], fill=RX, width=2)
d.text((x0 + 46, y0 + 9), "received (ADC_B + j ADC_D)", font=SMALL, fill=FG)
d.line([(x0 + 14, y0 + 34), (x0 + 40, y0 + 34)], fill=TX, width=2)
d.text((x0 + 46, y0 + 27), "transmitted (modulator output)", font=SMALL, fill=FG)

# ---------------------------------------------------------------- constellation (density)
cx0, cy0, cs = 900, 50, 440
lim = L + 0.2 * L / 4 + 0.6
B = 220                                         # bins per axis
hist = [0] * (B * B)
step = max(1, n // 400000)
for i in range(0, n, step):
    re_, im_ = struct.unpack_from("<ff", raw, 8 * i)
    bx = int((re_ + lim) / (2 * lim) * B)
    by = int((lim - im_) / (2 * lim) * B)
    if 0 <= bx < B and 0 <= by < B:
        hist[by * B + bx] += 1
hmax = max(hist) or 1
# viridis-like ramp
ramp = [(68, 1, 84), (59, 82, 139), (33, 145, 140), (94, 201, 98), (253, 231, 37)]
def color(t):
    t = max(0.0, min(1.0, t)) * (len(ramp) - 1)
    i = min(int(t), len(ramp) - 2)
    a, b, u = ramp[i], ramp[i + 1], t - i
    return tuple(int(a[k] + (b[k] - a[k]) * u) for k in range(3))
cimg = Image.new("RGB", (B, B), (255, 255, 255))
px = cimg.load()
for by in range(B):
    for bx in range(B):
        v = hist[by * B + bx]
        if v:
            px[bx, by] = color(math.log1p(v) / math.log1p(hmax))
im.paste(cimg.resize((cs, cs), Image.NEAREST), (cx0, cy0))
cxy = lambda u, v: (cx0 + (u + lim) / (2 * lim) * cs, cy0 + (lim - v) / (2 * lim) * cs)
for gi in range(-(L - 1), L, 2):
    for gq in range(-(L - 1), L, 2):
        x, y = cxy(gi, gq)
        d.line([(x - 3, y), (x + 3, y)], fill=(230, 60, 60))
        d.line([(x, y - 3), (x, y + 3)], fill=(230, 60, 60))
d.rectangle([cx0, cy0, cx0 + cs, cy0 + cs], outline=AX)
d.text((cx0, cy0 + cs + 6), f"{qam}-QAM, {n:,} data sub-carrier symbols, EVM {evm:.1f} dB", font=FONT, fill=FG)
d.text((cx0, cy0 + cs + 24), "after the widely linear equaliser and pilot phase; + ideal points",
       font=SMALL, fill=AX)
im.save(dst)
print(f"{dst}: {qam}-QAM, EVM {evm:.1f} dB, {n} symbols")
