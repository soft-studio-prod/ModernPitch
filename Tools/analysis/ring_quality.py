# ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Ring-modulator tracking quality: is the carrier at exactly half the note?

    python Tools/analysis/ring_quality.py <input.wav> <ring.wav> [more.wav ...] [--semitones N]

A sine carrier at F/2 moves the harmonics k*F of a note to (k +- 1/2)*F.
Over frames where the input is clearly pitched, reports:
  - purity  : median share of the energy (0.3F..10.5F) that lies within 3 %
              of the half-integer harmonics (k + 1/2)*F
  - low%    : frames with purity < 0.6 (carrier off-tune or lost)
  - detune  : median distance in cents between the strongest lines and the
              nearest (k + 1/2)*F
Files must share the input's timeline; --semitones gives the TUNE shift
the ring files were rendered with (default 0).
"""

import os
import sys

import numpy as np
import soundfile as sf

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pitch_quality import yin_track  # noqa: E402


def load(path):
    y, sr = sf.read(path)
    return (y.mean(1) if y.ndim > 1 else y), sr


def main():
    args = sys.argv[1:]
    ratio = 1.0
    if "--semitones" in args:
        k = args.index("--semitones")
        ratio = 2.0 ** (float(args[k + 1]) / 12.0)
        del args[k:k + 2]
    x, sr = load(args[0])
    hop, frame = 1024, 8192
    fx, ax = yin_track(x, sr, hop=hop)
    window = np.hanning(frame)
    freqs = np.fft.rfftfreq(frame, 1.0 / sr)
    for path in args[1:]:
        y, _ = load(path)
        n = min(len(x), len(y))
        purities, detunes = [], []
        for i in np.where((fx > 0) & (ax < 0.08))[0]:
            centre = i * hop + 2048
            if centre - frame // 2 < 0 or centre + frame // 2 > n:
                continue
            f0 = fx[i] * ratio
            power = np.abs(np.fft.rfft(y[centre - frame // 2:centre + frame // 2] * window)) ** 2
            band = (freqs > 0.3 * f0) & (freqs < 10.5 * f0)
            total = power[band].sum()
            if total <= 1e-12:
                continue
            relative = freqs / f0
            half = np.abs(relative - (np.floor(relative) + 0.5)) < 0.03 * np.maximum(relative, 1.0)
            purities.append(power[band & half].sum() / total)
            idx = np.where(band)[0]
            strongest = idx[np.argsort(power[idx])[-5:]]
            for k in strongest:
                target = (np.floor(relative[k]) + 0.5) * f0
                detunes.append(abs(1200.0 * np.log2(freqs[k] / target)))
        p = np.array(purities)
        print("%-45s frames=%4d purity=%.2f low%%=%5.1f detune=%5.1f cents"
              % (os.path.basename(path)[-45:], len(p), np.median(p), 100 * (p < 0.6).mean(),
                 np.median(detunes)))


if __name__ == "__main__":
    main()
