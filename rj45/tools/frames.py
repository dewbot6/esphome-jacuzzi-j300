#!/usr/bin/env python3
"""Split a digital.csv into 100 ms frames (pin4 = CH2 clock) and report per-frame patterns."""
import csv, sys, numpy as np
PIN = {0:1,1:2,2:4,3:5,4:6,5:7,6:8}
rows = list(csv.reader(open(sys.argv[1])))
d = [(float(r[0]), [int(x) for x in r[1:]]) for r in rows[1:]]
E = {ch: [] for ch in range(7)}
for (t, v), (_, pv) in zip(d[1:], d):
    for ch in range(7):
        if v[ch] != pv[ch]: E[ch].append((t, v[ch]))
for ch in range(7):
    print(f"pin{PIN[ch]}: {len(E[ch])} edges")
rise = [t for t, v in E[2] if v == 1]
B, cur = [], [rise[0]]
for a, b in zip(rise, rise[1:]):
    if b - a > 1e-3: B.append(cur); cur = [b]
    else: cur.append(b)
B.append(cur)
base = None; prev = None
for i, b in enumerate(B):
    iv = tuple(np.round(np.diff(b) * 1e6).astype(int))
    if base is None: base = iv
    if iv != prev:
        print(f"frame {i:3d} t={b[0]:.3f}s n={len(b)} {'BASE' if iv==base else 'DIFF'}: {' '.join(map(str, iv))}")
    prev = iv
for ch in (0, 1, 3, 4, 5, 6):
    if E[ch] and ch != 3:
        print(f"pin{PIN[ch]} edges:", [(round(t, 4), v) for t, v in E[ch][:40]])
