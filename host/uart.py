#!/usr/bin/env python3
# Talk to the bare-metal application over the board UART (115200 8N1): send keys, print and log
# what comes back until the line is quiet.
#   python3 host/uart.py [--port /dev/ttyUSB1] [--log build/uart.log] [--quiet S] [--max S] [keys ...]
# Each key argument is sent as it is (e.g. x f 2); with no keys it only listens.
import argparse
import sys
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="/dev/ttyUSB1")
ap.add_argument("--log", default=None)
ap.add_argument("--quiet", type=float, default=3.0, help="stop after this many seconds without output")
ap.add_argument("--max", type=float, default=120.0, help="stop after this many seconds in total per key")
ap.add_argument("keys", nargs="*")
a = ap.parse_args()

log = open(a.log, "a") if a.log else None
s = serial.Serial(a.port, 115200, timeout=0.2)


def listen():
    t0 = last = time.time()
    while time.time() - last < a.quiet and time.time() - t0 < a.max:
        d = s.read(4096)
        if d:
            last = time.time()
            txt = d.decode(errors="replace")
            sys.stdout.write(txt)
            sys.stdout.flush()
            if log:
                log.write(txt)
                log.flush()


listen()
for k in a.keys:
    s.write(k.encode())
    listen()
