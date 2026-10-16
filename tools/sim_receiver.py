#!/usr/bin/env python3
"""Replay a Logic 8 digital.csv as the Waveshare receiver sees it (pulse after each
15 us gap dropped, frame pulse 1 dropped) through jacuzzi_monitor.cpp's decoder and
set-point logic. Compares against the full (unloaded) decode."""
import csv, sys
rows = csv.reader(open(sys.argv[1])); next(rows)
pulses, groups, prev, t_up = [], [], None, None
for r in rows:
    t, v = float(r[0]), int(r[3])
    if prev == 0 and v == 1: t_up = t
    elif prev == 1 and v == 0 and t_up is not None:
        if t - t_up >= 1e-6:
            if pulses and round(t_up * 1e7) - (pulses[-1][0] + pulses[-1][1]) > 10000:
                groups.append(pulses); pulses = []
            pulses.append((round(t_up * 1e7), round((t - t_up) * 1e7)))
        t_up = None
    prev = v
groups.append(pulses)

def full_decode(g):
    if len(g) < 31: return None
    f = g[-31:]; b = 0
    for k in range(1, 24):
        d = f[k][0] - f[k-1][0]
        if 135 <= d < 160: b |= 1 << (23 - k)
        elif d < 135 or d > 185: return None
    return b

def receiver_view(g):
    if len(g) < 31: return g
    head, f = g[:-31], g[-31:]
    keep = [p for k, p in enumerate(f) if not (k == 0 or (k < 24 and f[k][0] - f[k-1][0] < 160))]
    return head + keep

def decode(g):  # mirror of JacuzziMonitor::decode_
    if len(g) < 9: return None
    w0 = len(g) - 7
    if any(not (55 <= w <= 90) for _, w in g[w0:]): return None
    fs = w0
    while fs > 0 and 12 <= g[fs-1][1] <= 35: fs -= 1
    if w0 - fs < 1: return None
    bits, m, i = 0, 25, w0
    while i > fs and m > 1:
        gap = g[i][0] - g[i-1][0]
        n = (gap - 170 + 75) // 150; err = gap - (170 + 150 * n); j = m - n - 1
        if n < 0 or abs(err) > 25 or j < 1: break
        for k in range(j, m - 1): bits |= 1 << (23 - k)
        m = j; i -= 1
    return bits if m <= 2 else None

st = dict(last=None, pub=None, disp=-1, since=0, prev=-1, prev_held=0, settled=False, water=-1, setv=-1)
def track(disp, now):
    if disp != st['disp']:
        st['prev'], st['prev_held'] = st['disp'], now - st['since']
        st['disp'], st['since'], st['settled'] = disp, now, False
        return
    if st['settled'] or now - st['since'] < 4000: return
    st['settled'] = True
    if st['water'] != disp: st['water'] = disp; print(f"{now/1000:7.2f}s  WATER {disp}")
    sp = None
    if st['prev'] >= 0 and st['prev'] != disp:
        if 2500 <= st['prev_held'] <= 3700: sp = st['prev']        # display reverted to water
        elif st['prev_held'] < 2500: sp = disp                     # a press landed on the water temp
    if sp is not None and sp != st['setv']: st['setv'] = sp; print(f"{now/1000:7.2f}s  SET POINT {sp}")

ok = bad = mism = 0
for g in groups:
    ref = full_decode(g); b = decode(receiver_view(g))
    if b is None: bad += 1; continue
    ok += 1
    if ref is not None and b != ref: mism += 1
    now = g[-1][0] // 10000
    if b != st['last']: st['last'] = b; continue
    h, tn, u = b >> 8 & 15, b >> 4 & 15, b & 15
    if max(h, tn, u) <= 9: track(h * 100 + tn * 10 + u, now)
print(f"groups={len(groups)} ok={ok} bad={bad} mismatches_vs_full={mism}")
