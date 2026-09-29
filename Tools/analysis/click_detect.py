# ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Count click-like discontinuities in a render.

    python Tools/analysis/click_detect.py <file.wav> [more.wav ...] [--times]

A click is a sample whose second difference exceeds 6x the RMS second
difference of the surrounding 20 ms (and is not tiny in absolute terms).
Clicks closer than 5 ms are merged. Run it on the input too: its count is
the detector's false-positive floor for that material.
"""

import sys

import numpy as np
import soundfile as sf


def detect(path):
    y, sr = sf.read(path)
    y = y.mean(1) if y.ndim > 1 else y
    e = np.abs(np.diff(y, 2))
    w = int(0.02 * sr)
    kernel = np.ones(w) / w
    local = np.sqrt(np.convolve(e * e, kernel, mode="same")) + 1e-9
    peak = np.max(np.abs(y)) + 1e-9
    candidates = np.where((e > 6.0 * local) & (e > 0.01 * peak))[0]
    clicks = []
    for c in candidates:
        if not clicks or c - clicks[-1] > int(0.005 * sr):
            clicks.append(c)
    return np.array(clicks) / sr, len(y) / sr


def main():
    args = [a for a in sys.argv[1:] if a != "--times"]
    show = "--times" in sys.argv
    for path in args:
        times, duration = detect(path)
        print("%-60s clicks=%4d (%.1f/s)" % (path[-60:], len(times), len(times) / duration))
        if show:
            print("   ", " ".join("%.3f" % t for t in times[:40]))


if __name__ == "__main__":
    main()
