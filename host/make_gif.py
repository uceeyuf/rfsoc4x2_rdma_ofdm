#!/usr/bin/env python3
# README GIF from rf_ofdm --save output: the video frame sent (left) and the frame as received
# after DAC -> cable -> ADC and the host demodulator (right), with the counters at that moment.
#   python3 host/make_gif.py build/rx_720p.yuv 1280x720 docs/img/ofdm_720p.gif [16-QAM "5.29 Gb/s"]
# Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
import sys

from PIL import Image, ImageDraw, ImageFont

src, size, dst = sys.argv[1], sys.argv[2], sys.argv[3]
QAM = sys.argv[4] if len(sys.argv) > 4 else "16-QAM"
RATE = sys.argv[5] if len(sys.argv) > 5 else "5.29 Gb/s"
W, H = map(int, size.split("x"))
VF = W * H * 3 // 2
TW, TH = 400, 225
FONT = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 12)
BG, FG, DIM, OK, BAD = (30, 30, 30), (225, 225, 225), (150, 150, 150), (120, 220, 120), (240, 110, 110)


def rgb(b):
    y = Image.frombytes("L", (W, H), b[: W * H])
    u = Image.frombytes("L", (W // 2, H // 2), b[W * H: W * H * 5 // 4]).resize((W, H))
    v = Image.frombytes("L", (W // 2, H // 2), b[W * H * 5 // 4: VF]).resize((W, H))
    return Image.merge("YCbCr", (y, u, v)).convert("RGB").resize((TW, TH), Image.BILINEAR)


rx = open(src, "rb").read()
sv = open(src + ".src", "rb").read()
meta = [ln.split() for ln in open(src + ".txt")]
M, TOP, BOT = 10, 20, 44
frames = []
for k, m in enumerate(meta):
    v, intact, t = int(m[0]), int(m[1]), float(m[2])
    nv, nvi, nf, be, nb = map(int, m[3:8])
    im = Image.new("RGB", (2 * TW + 3 * M, TOP + TH + BOT), BG)
    d = ImageDraw.Draw(im)
    d.text((M, 4), f"Sent  video frame {v}  ({W}x{H} raw YUV 4:2:0)", font=FONT, fill=FG)
    d.text((2 * M + TW, 4), f"Received  frame {v}  " + ("byte-exact" if intact else "with bit errors"),
           font=FONT, fill=OK if intact else BAD)
    im.paste(rgb(sv[v * VF:(v + 1) * VF]), (M, TOP))
    im.paste(rgb(rx[k * VF:(k + 1) * VF]), (2 * M + TW, TOP))
    d.text((M, TOP + TH + 6), f"2 GSPS I/Q OFDM {QAM}, {RATE}: host CPU mod -> 100G RDMA -> DACs -> cable -> "
           "ADCs -> RDMA -> CPU demod", font=FONT, fill=DIM)
    d.text((M, TOP + TH + 24), f"t {t:5.1f} s   {nv} video frames ({nvi} byte-exact)   {nf} OFDM frames   "
           f"BER {be / max(nb, 1):.1e}", font=FONT, fill=FG)
    frames.append(im)
frames[0].save(dst, save_all=True, append_images=frames[1:], duration=120, loop=0, optimize=True)
print(f"{dst}: {len(frames)} frames")
