# ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Frame-by-frame pitch quality of a pitch-shifted render.

    python Tools/analysis/pitch_quality.py <input.wav> <output.wav> <semitones> [--offset-ms N]

Tracks F0 with a full-rate YIN on the input and on the output, then over the
frames where the input is clearly pitched reports:
  - wrong%   : output F0 more than 50 cents from input F0 * ratio
  - octave%  : output F0 off by roughly an octave (or more)
  - ap       : median output aperiodicity (YIN CMND at the selected lag, x100)
  - rough%   : frames whose output aperiodicity is > 3x the input's (glitchy)
The engine delay is small and variable, so the output is compared with a
fixed offset (default 8 ms) and each frame takes the best of +-4 ms.
"""

import sys

import numpy as np
import soundfile as sf


def yin_track(x, sr, frame=4096, hop=256, fmin=30.0, fmax=1000.0):
    max_lag = int(sr / fmin)
    min_lag = int(sr / fmax)
    n = (len(x) - frame - max_lag) // hop
    f0 = np.zeros(n)
    ap = np.ones(n)
    size = 1 << int(np.ceil(np.log2(frame + max_lag)))
    for i in range(n):
        seg = x[i * hop:i * hop + frame + max_lag]
        a = seg[:frame]
        spec_a = np.fft.rfft(a, size)
        spec_s = np.fft.rfft(seg, size)
        cross = np.fft.irfft(np.conj(spec_a) * spec_s, size)[:max_lag + 1]
        sq = np.concatenate([[0.0], np.cumsum(seg * seg)])
        energy_shifted = sq[np.arange(max_lag + 1) + frame] - sq[np.arange(max_lag + 1)]
        d = sq[frame] + energy_shifted - 2.0 * cross
        d[0] = 0.0
        cumulative = np.cumsum(d[1:])
        cmnd = np.ones(max_lag + 1)
        cmnd[1:] = d[1:] * np.arange(1, max_lag + 1) / np.maximum(cumulative, 1e-12)
        if sq[frame] < frame * 1e-6:
            continue
        below = np.where(cmnd[min_lag:max_lag] < 0.15)[0]
        if len(below):
            tau = below[0] + min_lag
            while tau + 1 < max_lag and cmnd[tau + 1] < cmnd[tau]:
                tau += 1
        else:
            tau = np.argmin(cmnd[min_lag:max_lag]) + min_lag
        l, c, r = cmnd[tau - 1], cmnd[tau], cmnd[tau + 1]
        den = l - 2 * c + r
        lag = tau + (0.5 * (l - r) / den if abs(den) > 1e-12 else 0.0)
        f0[i] = sr / lag
        ap[i] = c
    return f0, ap


def main():
    args = sys.argv[1:]
    offset_ms = 8.0
    if "--offset-ms" in args:
        k = args.index("--offset-ms")
        offset_ms = float(args[k + 1])
        del args[k:k + 2]
    inp, out, semitones = args[0], args[1], float(args[2])
    x, sr = sf.read(inp)
    y, sr2 = sf.read(out)
    assert sr == sr2
    x = x.mean(1) if x.ndim > 1 else x
    y = y.mean(1) if y.ndim > 1 else y
    n = min(len(x), len(y))
    x, y = x[:n], y[:n]
    hop = 256
    fx, ax = yin_track(x, sr, hop=hop)
    fy, ay = yin_track(y, sr, hop=hop)
    ratio = 2.0 ** (semitones / 12.0)
    base = int(round(offset_ms * 1e-3 * sr / hop))
    spread = max(1, int(round(4e-3 * sr / hop)))
    good = np.where((fx > 0) & (ax < 0.1))[0]
    good = good[(good + base + spread < len(fy)) & (good + base - spread >= 0)]
    cents = np.full(len(good), 9999.0)
    aps = np.ones(len(good))
    for k, i in enumerate(good):
        best = 9999.0
        best_ap = 1.0
        for j in range(i + base - spread, i + base + spread + 1):
            if fy[j] <= 0:
                continue
            c = 1200.0 * np.log2(fy[j] / (fx[i] * ratio))
            if abs(c) < abs(best):
                best, best_ap = c, ay[j]
        cents[k] = best
        aps[k] = best_ap
    wrong = np.abs(cents) > 50
    octave = np.abs(cents) > 900
    rough = aps > np.maximum(3.0 * ax[good], 0.15)
    print("frames=%d wrong%%=%.2f octave%%=%.2f ap=%.1f rough%%=%.2f median|cents|=%.1f"
          % (len(good), 100 * wrong.mean(), 100 * octave.mean(), 100 * np.median(aps),
             100 * rough.mean(), np.median(np.abs(cents[~wrong])) if (~wrong).any() else -1))


if __name__ == "__main__":
    main()
