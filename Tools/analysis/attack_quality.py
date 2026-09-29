# ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Per-attack delay and sharpness of a pitch-shifted render vs its input.

    python Tools/analysis/attack_quality.py <input.wav> <output.wav> [more outputs ...]

Onsets are found on the input (fast/slow energy envelope ratio). For each
onset, the output's attack time is the first sample within 60 ms where its
fast envelope reaches half of its maximum over that window. Reports the
median / 90th percentile delay and the median 10-90 % rise time.
All files must share the input's timeline (align exports first).
"""

import sys

import numpy as np
import soundfile as sf


def envelope(y, sr, ms):
    w = max(1, int(ms * 1e-3 * sr))
    return np.sqrt(np.convolve(y * y, np.ones(w) / w, mode="same"))


def onsets(x, sr):
    fast = envelope(x, sr, 1.0)
    slow = envelope(x, sr, 30.0) + 1e-9
    level = np.percentile(fast, 90)
    idx = np.where((fast > 4.0 * np.roll(slow, int(0.015 * sr))) & (fast > 0.2 * level))[0]
    result = []
    for i in idx:
        if not result or i - result[-1] > int(0.08 * sr):
            result.append(i)
    return np.array(result), fast


def attack_times(y, sr, starts, fast_x):
    fast = envelope(y, sr, 1.0)
    span = int(0.06 * sr)
    delays, rises = [], []
    for s in starts:
        # input reference: half-maximum crossing of the input itself
        seg_x = fast_x[s:s + span]
        tx = s + np.argmax(seg_x >= 0.5 * seg_x.max())
        seg = fast[s - int(0.005 * sr):s + span]
        if seg.max() <= 0:
            continue
        peak = seg.max()
        t50 = np.argmax(seg >= 0.5 * peak)
        t10 = np.argmax(seg >= 0.1 * peak)
        t90 = np.argmax(seg >= 0.9 * peak)
        delays.append((s - int(0.005 * sr) + t50 - tx) / sr * 1e3)
        rises.append((t90 - t10) / sr * 1e3)
    return np.array(delays), np.array(rises)


def main():
    inp, outs = sys.argv[1], sys.argv[2:]
    x, sr = sf.read(inp)
    x = x.mean(1) if x.ndim > 1 else x
    starts, fast_x = onsets(x, sr)
    _, rise_in = attack_times(x, sr, starts, fast_x)
    print("onsets=%d  input rise10-90 median %.1f ms" % (len(starts), np.median(rise_in)))
    for path in outs:
        y, _ = sf.read(path)
        y = y.mean(1) if y.ndim > 1 else y
        y = np.pad(y, (0, max(0, len(x) - len(y))))[:len(x)]
        d, r = attack_times(y, sr, starts, fast_x)
        print("%-45s delay median %5.1f ms  p90 %5.1f ms  late(>15ms) %4.1f%%  rise median %5.1f ms"
              % (path[-45:], np.median(d), np.percentile(d, 90), 100 * (d > 15).mean(), np.median(r)))


if __name__ == "__main__":
    main()
