"""Derive the CrusherFX "Crusher 1080 tuning" filter set.

Input : data/crusher1080_soundguys.json - SoundGuys B&K 5128 measurement of the
        Crusher 1080 ANC, as [[hz, db], ...]. Crusher wheel position is unknown.
Output: PEQ table to paste into src/dsp.h (kTuning).

Deliberately conservative, because the data has three known limits:
  * Below 200 Hz nothing is corrected. The wheel position during measurement is
    unknown, so the bass there is not a stable property of the headphone.
  * The target is an APPROXIMATE Harman-style shape typed in by hand, not an
    official curve, and Harman was defined on a different rig from the 5128.
    5128 readings run hot above 5 kHz compared with older rigs, so treble
    correction is applied at half strength.
  * Every filter is capped at 5 dB. This is a starting point to audition, not a
    measured-in-ear correction.
"""
import json
import math
import sys

import numpy as np

FS = 48000.0
MAX_FILTERS = 4
MAX_CUT_DB = 5.0
FLOOR_HZ = 200.0
CEIL_HZ = 14000.0

# Approximate target relative to 1 kHz (hand-entered, not an official dataset).
TARGET = [(200, 1.0), (300, 0.3), (400, 0), (700, 0), (1000, 0), (1500, 1.0),
          (2000, 3.5), (2500, 6.5), (3000, 8.5), (4000, 7.5), (5000, 5.0),
          (6000, 3.5), (8000, 1.5), (10000, 0.5), (12000, -1.0), (16000, -5.0)]


def peaking_db(f, fc, gain, q):
    """Magnitude in dB of an RBJ peaking biquad, the same maths as src/dsp.h."""
    a = 10 ** (gain / 40.0)
    w0 = 2 * math.pi * fc / FS
    alpha = math.sin(w0) / (2 * q)
    b = np.array([1 + alpha * a, -2 * math.cos(w0), 1 - alpha * a])
    d = np.array([1 + alpha / a, -2 * math.cos(w0), 1 - alpha / a])
    z = np.exp(-1j * 2 * np.pi * f / FS)
    h = (b[0] + b[1] * z + b[2] * z ** 2) / (d[0] + d[1] * z + d[2] * z ** 2)
    return 20 * np.log10(np.abs(h))


def smooth(y, n):
    return np.convolve(np.pad(y, n, mode="edge"), np.ones(2 * n + 1) / (2 * n + 1), "valid")


def main(path):
    with open(path) as fh:
        pts = np.array(json.load(fh)[0], dtype=float)
    f = np.geomspace(FLOOR_HZ, CEIL_HZ, 160)
    measured = np.interp(np.log(f), np.log(pts[:, 0]), pts[:, 1])
    measured -= np.interp(np.log(1000.0), np.log(pts[:, 0]), pts[:, 1])
    tx, ty = zip(*TARGET)
    target = np.interp(np.log(f), np.log(tx), ty)

    # Ignore narrow peaks and nulls: they shift with fit and seal, so EQ can't fix them.
    error = smooth(measured - target, 6)
    error = np.where(f > 5000, error * 0.5, error)

    filters = []
    residual = error.copy()
    fcs = np.geomspace(FLOOR_HZ * 1.2, CEIL_HZ * 0.8, 50)
    qs = [0.7, 1.0, 1.4, 2.0]
    gains = np.arange(-MAX_CUT_DB, MAX_CUT_DB + 0.01, 0.5)
    for _ in range(MAX_FILTERS):
        best = None
        base = float(np.mean(residual ** 2))
        for fc in fcs:
            for q in qs:
                for g in gains:
                    if g == 0:
                        continue
                    score = float(np.mean((residual + peaking_db(f, fc, g, q)) ** 2))
                    if best is None or score < best[0]:
                        best = (score, fc, g, q)
        if best is None or base - best[0] < 0.05:
            break
        _, fc, g, q = best
        fc = float(round(fc, -1))
        filters.append((fc, float(g), q))
        residual = residual + peaking_db(f, fc, g, q)

    print("rms error before: %.2f dB, after: %.2f dB" % (
        np.sqrt(np.mean(error ** 2)), np.sqrt(np.mean(residual ** 2))))
    print("deviation from target, relative to 1 kHz (smoothed, before halving treble):")
    raw = smooth(measured - target, 6)
    for hz in (250, 500, 1000, 2000, 3000, 4000, 5000, 7000, 9000, 12000):
        print("  %6d Hz  %+5.1f dB" % (hz, np.interp(hz, f, raw)))
    print("static const Peq kTuning[] = {")
    for fc, g, q in sorted(filters):
        print("    {%.0f, %.1f, %.1f}," % (fc, g, q))
    print("};")


if __name__ == "__main__":
    main(sys.argv[1])
