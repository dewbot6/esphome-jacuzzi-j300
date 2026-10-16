#!/usr/bin/env python3
"""Replay a Logic 8 digital.csv through a Python port of jacuzzi_monitor.cpp's logic."""
import csv, sys
rows = csv.reader(open(sys.argv[1])); next(rows)
# Build RMT-like groups: pulses (start, width) in 0.1 us ticks, glitches <1 us dropped, >1 ms idle ends a group
pulses, groups, prev, t_up = [], [], None, None
for r in rows:
    t, v = float(r[0]), int(r[3])
    if prev == 0 and v == 1: t_up = t
    elif prev == 1 and v == 0 and t_up is not None:
        if t - t_up >= 1e-6:
            if pulses and t_up - (pulses[-1][0] + pulses[-1][1]) > 1e-3:
                groups.append(pulses); pulses = []
            pulses.append((t_up, t - t_up))
        t_up = None
    prev = v
groups.append(pulses)

def decode(g):
    if len(g) < 31: return None
    f = g[-31:]
    for i, (_, w) in enumerate(f):
        w = round(w * 1e7)
        if not ((12 <= w <= 35) if i < 24 else (55 <= w <= 90)): return None
    bits = 0
    for k in range(1, 24):
        d = round((f[k][0] - f[k - 1][0]) * 1e7)
        if 135 <= d < 160: bits |= 1 << (23 - k)
        elif d < 135 or d > 185: return None
    return bits

st = dict(last=None, pub=None, disp=-1, since=0, prev=-1, prev_held=0, settled=False, water=-1, setv=-1)
def track(disp, now):
    if disp != st['disp']:
        st['prev'], st['prev_held'] = st['disp'], now - st['since']
        st['disp'], st['since'], st['settled'] = disp, now, False
    elif not st['settled'] and now - st['since'] >= 4000:
        st['settled'] = True
        if st['water'] != disp:
            st['water'] = disp; print(f"{now/1000:7.2f}s  WATER {disp}")
        if st['prev'] >= 0 and st['prev'] != disp and 2500 <= st['prev_held'] <= 3700 and st['setv'] != st['prev']:
            st['setv'] = st['prev']; print(f"{now/1000:7.2f}s  SET POINT {st['setv']}")

ok = bad = 0
for g in groups:
    b = decode(g)
    if b is None: bad += 1; continue
    ok += 1
    now = int(g[-1][0] * 1000)
    if b != st['last']: st['last'] = b; continue
    if b != st['pub']:
        st['pub'] = b
        print(f"{now/1000:7.2f}s  frame {b:023b} heater={b>>19&1} jets1={b>>16&1} jets2={b>>12&1}")
    h, tn, u = b >> 8 & 15, b >> 4 & 15, b & 15
    if max(h, tn, u) <= 9: track(h * 100 + tn * 10 + u, now)
print(f"groups={len(groups)} ok={ok} bad={bad}")
