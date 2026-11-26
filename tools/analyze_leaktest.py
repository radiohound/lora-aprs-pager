#!/usr/bin/env python3
"""Label and measure a power analyser recording of the leak test (env:leaktest).

Usage: tools/analyze_leaktest.py capture.csv [graph.png]     CSV columns Timestamp(ms),Current(uA), as exported by the Nordic PPK2 software.
Needs numpy and Pillow. The step names come from src/app/leaktest.h, so they cannot drift apart. The test marks its start and end with the
backlight (1 s on, 1 s off, three times); every step then shows its own number: a long flash counts 5, a short flash counts 1. The script reads
that number back and says so if it does not match the step it expected.
"""
import os, re, sys
import numpy as np
from analyze_powertest import load_bins, find_markers, BIN_MS

HERE = os.path.dirname(os.path.abspath(__file__))
PLAN = re.findall(r'^\s*\{"([^"]+)",', open(os.path.join(HERE, '..', 'src', 'app', 'leaktest.h')).read().split('LT_PLAN[] = {')[1].split('};')[0], re.M)
STEP_S, MARK_S, SKIP_S, PER = 28, 6, 8, 1000 // BIN_MS      # KEEP IN STEP WITH src/app/leaktest.h (LT_STEP_S)

def read_number(win, thr):
    """Count the flashes in the first seconds of a step: runs above thr; 0.4 s or longer counts 5."""
    high = win > thr
    n, i = 0, 0
    while i < high.size:
        if high[i]:
            j = i
            while j < high.size and high[j]:
                j += 1
            ms = (j - i) * BIN_MS
            if ms >= 100:
                n += 5 if ms >= 400 else 1
            i = j
        else:
            i += 1
    return n

def analyse(m, graph=None):
    marks = find_markers(m)
    want = (MARK_S + len(PLAN) * STEP_S) * PER
    start = next((a for a in marks if any(abs((b - a) - want) <= 3 * PER for b in marks)), None)
    if start is None:
        sys.exit(f"could not find the start and end markers {want / PER:.0f} s apart (markers seen at {[round(x / PER, 1) for x in marks]} s). "
                 "Record from before the test starts until after it has finished.")
    mk = m[start:start + MARK_S * PER].reshape(6, PER)
    on, off = float(np.median(mk[0::2, 20:80])), float(np.median(mk[1::2, 20:80]))
    t0 = start + MARK_S * PER
    base_sleep = float(np.median(m[t0 + SKIP_S * PER: t0 + STEP_S * PER]))
    print(f"test starts at {start / PER:.1f} s; backlight flash adds {on - off:.0f} mA; {len(PLAN)} steps\n")
    print(f"{'step':>4}  {'flashes':>7}  {'what':46s} {'median':>7} {'average':>8} {'vs base':>8}   (mA)")
    rows, base, bad = [], None, 0
    for i, name in enumerate(PLAN):
        a = t0 + i * STEP_S * PER
        seg = m[a + SKIP_S * PER: a + STEP_S * PER - PER // 2]
        med, avg = float(np.median(seg)), float(seg.mean())
        shift = med - base_sleep                          # a step that draws more also lifts its own flashes
        got = read_number(m[a - PER // 4: a + 5 * PER - PER // 4], (on + off) / 2 + max(0.0, min(shift, (on - off) / 3)))
        ok = got == i + 1
        bad += not ok
        if name.startswith('BASE'):
            base = avg
        delta = None if base is None else avg - base
        print(f"{i + 1:4d}  {str(got) + ('' if ok else ' ??'):>7}  {name:46s} {med:7.2f} {avg:8.2f} {('' if delta is None else f'{delta:+.2f}'):>8}")
        rows.append((i + 1, name, med, avg, delta, ok))
    bases = [r[3] for r in rows if r[1].startswith('BASE')]
    print(f"\nbaseline repeats: {', '.join(f'{b:.2f}' for b in bases)} mA (spread {max(bases) - min(bases):.2f})")
    print('every step number read back correctly' if not bad else f'{bad} step number(s) did NOT read back as expected: check the graph before trusting those rows')
    end = t0 + len(PLAN) * STEP_S * PER                   # the end marker, then 30 s of deep sleep
    deep = m[end + (MARK_S + 4) * PER: end + (MARK_S + 24) * PER]
    if deep.size >= 15 * PER:
        print(f'reference, deep sleep after the end marker (processor off, its pins released): median {np.median(deep):.2f} mA')
    else:
        print('the deep-sleep reference at the end was not recorded')
    if graph:
        draw(m, t0, rows, graph)
    return rows

def draw(m, t0, rows, path):
    from PIL import Image, ImageDraw, ImageFont
    f = ImageFont.load_default(size=15)
    top = max(30.0, max(r[3] for r in rows) * 1.25)
    row_h, W, L, B = 26, 1500, 60, 40
    H = 420 + len(rows) * row_h + 60
    im = Image.new('RGB', (W, H), (255, 255, 255)); g = ImageDraw.Draw(im)
    Y = lambda v: 30 + 340 * (1 - min(v, top) / top)
    for v in range(0, int(top) + 1, 5):
        g.line([(L, Y(v)), (W - 10, Y(v))], fill=(228, 228, 228)); g.text((16, Y(v) - 8), str(v), fill=(0, 0, 0), font=f)
    n = len(rows) * STEP_S * PER; w = W - 10 - L
    for px in range(w):
        i = t0 + n * px // w; j = max(i + 1, t0 + n * (px + 1) // w)
        g.line([(L + px, Y(0)), (L + px, Y(float(np.median(m[i:j]))))], fill=(60, 110, 210))
    for k, r in enumerate(rows):
        x = L + w * k // len(rows)
        g.line([(x, 30), (x, 370)], fill=(150, 150, 150))
        g.text((x + 4, 34), str(r[0]), fill=(0, 0, 0), font=f)
        g.text((x + 4, Y(r[3]) - 20), f'{r[3]:.1f}', fill=(0, 0, 0), font=f)
        delta = '' if r[4] is None else f'{r[4]:+.2f}'
        g.text((L, 400 + k * row_h), f'{r[0]:2d}   {r[3]:6.2f} mA   {delta:>7}   {r[1]}' + ('' if r[5] else '   (number not read back)'), fill=(0, 0, 0), font=f)
    g.text((L, 6), 'Leak test: median current (flashes that number each step are the tall spikes at the start of each)', fill=(0, 0, 0), font=f)
    im.save(path)
    print('graph:', path)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    analyse(load_bins(sys.argv[1]), sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(sys.argv[1])[0] + '-labelled.png')
