#!/usr/bin/env python3
"""Cut a power analyser recording of the power test (env:powertest) into its steps and print the current of each.

Usage: tools/analyze_powertest.py capture.csv        CSV with the columns Timestamp(ms),Current(uA), as exported by the Nordic PPK2 software.
Needs numpy. The test marks its start and end with the backlight: 1 s on, 1 s off, three times; the steps follow on a fixed timetable.
"""
import sys
import numpy as np

# KEEP IN STEP WITH PT_PLAN in src/app/powertest.h: (name, seconds, is a transmit step)
PLAN = [("sleep, radio receiving (normal)", 30, False), ("sleep, radio asleep", 30, False), ("sleep, radio receiving (repeat)", 20, False),
        ("awake 80 MHz, GPS asleep", 20, False), ("awake 40 MHz, GPS asleep", 20, False), ("awake 20 MHz, GPS asleep", 20, False),
        ("GPS warm-up (not compared)", 20, False), ("GPS on, 80 MHz", 30, False), ("GPS on, 40 MHz", 30, False), ("GPS on, 20 MHz", 30, False),
        ("GPS on, 80 MHz (repeat)", 30, False), ("transmit at 80 MHz", 20, True), ("transmit at 40 MHz", 20, True), ("transmit at 20 MHz", 20, True),
        ("sleep, radio receiving (end)", 30, False)]
BIN_MS = 10
MARK_S = 6

def load_bins(path):
    """10 ms means in mA, read in blocks so that a capture of several gigabytes fits."""
    out, carry, tail = [], np.empty(0), b''
    per = None
    with open(path, 'rb') as f:
        f.readline()
        while True:
            block = f.read(1 << 25)
            if not block and not tail:
                break
            data = tail + block
            if block:
                cut = data.rfind(b'\n') + 1
                data, tail = data[:cut], data[cut:]
            else:
                tail = b''
            a = np.fromstring(data.replace(b'\r', b'').replace(b'\n', b',').decode(), sep=',')
            if per is None:
                per = int(round(BIN_MS / (a[2] - a[0])))            # samples in one bin, from the file's own sample spacing
            cur = np.concatenate([carry, a[1::2]])
            k = cur.size // per * per
            out.append(cur[:k].reshape(-1, per).mean(1) / 1000.0)
            carry = cur[k:]
    return np.concatenate(out)

def find_markers(m):
    """Start positions (in bins) of the backlight square waves, best matches first removed from their neighbourhood."""
    one = 1000 // BIN_MS
    tpl = np.tile(np.concatenate([np.ones(one), -np.ones(one)]), 3)
    n = tpl.size
    if m.size < n:
        return []
    size = 1 << int(np.ceil(np.log2(m.size + n)))
    dot = np.fft.irfft(np.fft.rfft(m, size) * np.conj(np.fft.rfft(tpl, size)), size)[:m.size - n + 1]
    c1 = np.concatenate([[0], np.cumsum(m)]); c2 = np.concatenate([[0], np.cumsum(m * m)])
    var = np.maximum((c2[n:] - c2[:-n]) - (c1[n:] - c1[:-n]) ** 2 / n, 1e-9)
    score = dot / np.sqrt(var * n)
    # also require a real step in current, not just a similar shape in the noise
    found = []
    work = score.copy()
    while True:
        i = int(np.argmax(work))
        if work[i] < 0.85:
            break
        seg = m[i:i + n]
        if seg[tpl > 0].mean() - seg[tpl < 0].mean() > 5:
            found.append(i)
        work[max(0, i - n):i + n] = -1
    return sorted(found)

def analyse(m):
    total = sum(s for _, s, _ in PLAN)
    marks = find_markers(m)
    per_s = 1000 // BIN_MS
    want = (MARK_S + total) * per_s
    start = None
    for a in marks:
        if any(abs((b - a) - want) <= 2 * per_s for b in marks):
            start = a
            break
    if start is None:
        sys.exit(f"could not find the start and end markers {want / per_s:.0f} s apart (markers seen at: {[round(x / per_s, 1) for x in marks]} s). "
                 "Record from before the test starts until after the screen says it has finished.")
    t = start + MARK_S * per_s
    print(f"test starts at {start / per_s:.1f} s in the recording\n")
    print(f"{'step':36s} {'average':>9s} {'median':>9s}   (mA; the first 3 s and last 0.5 s of each step left out)")
    rows = {}
    for name, secs, tx in PLAN:
        whole = m[t:t + secs * per_s]
        seg = m[t + 3 * per_s: t + secs * per_s - per_s // 2]
        t += secs * per_s
        if tx:
            high = whole > 100
            idle = np.median(whole)
            extra = float((whole - idle).sum()) * BIN_MS / 1000.0
            print(f"{name:36s} {whole.mean():9.2f} {idle:9.2f}   above 100 mA for {high.sum() * BIN_MS / 1000:.2f} s"
                  + (f" at {whole[high].mean():.0f} mA" if high.any() else "") + f"; charge above the step's idle level {extra:.0f} mA·s")
        else:
            print(f"{name:36s} {seg.mean():9.2f} {np.median(seg):9.2f}")
        rows[name] = float(seg.mean())
    return rows

if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    analyse(load_bins(sys.argv[1]))
