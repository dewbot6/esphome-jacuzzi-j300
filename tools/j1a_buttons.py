#!/usr/bin/env python3
"""Decode a J1-A Logic 8 capture (CHn = J1-A pin n+1): per frame, the 24 data bits on
pin 5 (sampled at each clock pulse on pin 6) and the button-reply bits on pin 4 during
the 7 wide scan pulses. Prints only changes."""
import csv, sys, numpy as np
rows = csv.reader(open(sys.argv[1])); next(rows)
T, V = [], []
for r in rows: T.append(float(r[0])); V.append([int(x) for x in r[1:]])
T, V = np.array(T), np.array(V)
def lvl(ch, t): return V[max(np.searchsorted(T, t, side='right') - 1, 0), ch]
up, P = None, []
for i in range(1, len(T)):
    if V[i, 5] != V[i-1, 5]:
        if V[i, 5] == 1: up = T[i]
        elif up is not None:
            if T[i] - up >= 1e-6: P.append((up, T[i] - up))
            up = None
G, cur = [], []
for p in P:
    if cur and p[0] - cur[-1][0] > 1e-3: G.append(cur); cur = []
    cur.append(p)
G.append(cur)
BTN = {2: "Jets 2", 3: "-", 4: "+", 7: "Jets 1"}
last = None
for g in G:
    if len(g) < 31: continue
    f = g[-31:]
    data = "".join(str(lvl(4, p[0] + 1e-6)) for p in f[:24])
    scan = "".join(str(lvl(3, p[0] + 3.5e-6)) for p in f[24:])
    b = data[1:]
    disp = f"{int(b[11:15],2)}{int(b[15:19],2)}{int(b[19:23],2)}"
    if (data, scan) != last:
        pressed = [BTN.get(k + 1, f"pos{k+1}") for k, c in enumerate(scan) if c == "1"]
        print(f"{f[0][0]:7.2f}s  heat={b[3]} slot5={b[4]} j1={b[6]} j2={b[10]} disp={disp}  scan {scan} {pressed or ''}")
        last = (data, scan)
