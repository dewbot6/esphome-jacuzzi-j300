#!/usr/bin/env python3
"""Decode pin-4 (CH2) clock timing into slot bits per 100 ms frame.

Each frame: 24 short clock pulses whose spacing is 15 us ('1') or 17 us ('0'),
then 7 wide pulses. Once a second a 15-pulse header precedes the frame.
Prints a timeline of changes: time, header bits (if any), 24 frame bits.
"""
import csv, sys, numpy as np

MIN_W = 1.0e-6  # ignore glitches (relay/light switching noise) shorter than 1 us
rows = csv.reader(open(sys.argv[1])); next(rows)
prev_v, t_rise, rise = None, None, []
for r in rows:
    t, v = float(r[0]), int(r[3])  # CH2 = RJ45 pin 4
    if prev_v == 0 and v == 1: t_rise = t
    elif prev_v == 1 and v == 0 and t_rise is not None:
        if t - t_rise >= MIN_W: rise.append(t_rise)
        t_rise = None
    prev_v = v

groups, cur = [], [rise[0]]
for a, b in zip(rise, rise[1:]):
    if b - a > 1e-3: groups.append(cur); cur = [b]
    else: cur.append(b)
groups.append(cur)

def bits(ts):
    iv = np.round(np.diff(ts) * 1e6).astype(int)
    # '1' = 15 us, '0' = 17 us, '?' = stretched by an interrupt, 'w' = wide-pulse spacing
    return "".join("w" if x <= 12 else "1" if x <= 15 else "0" if x <= 17 else "?" for x in iv)

last = None
for g in groups:
    frame, hdr = g[-31:], g[:-31]          # frame = last 31 pulses (24 short + 7 wide)
    body = bits(frame[:24])                # 23 intervals among the 24 short pulses
    h = bits(hdr) if hdr else ""
    if body != last or h:
        print(f"{g[0]:8.3f}s  frame {body}" + (f"   header[{len(hdr)}] {h}" if hdr else ""))
        last = body
