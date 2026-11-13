# ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Pitch-shift fidelity on any material, including chords.

    python Tools/analysis/poly_quality.py <input.wav> <semitones> <out.wav> [more.wav ...]

An ideal shift moves every spectral line f of the input to f * ratio. For
each frame, the ideal output spectrum is the input spectrum resampled on a
frequency axis scaled by the ratio. Over frames with signal, reports:
  - on-line : median share of the output energy (40 Hz .. 3 kHz) that falls
              on the ideal spectrum's lines (peaks within 30 dB of the
              frame's strongest one, +-2 bins)
  - spectral: median correlation between the output and ideal log spectra
  - delay   : the output delay used (searched automatically, in ms)
Higher is better for both scores.
"""

import os
import sys

import numpy as np
import soundfile as sf
from scipy.signal import correlate, correlation_lags, find_peaks


def load(path):
    y, sr = sf.read(path)
    return (y.mean(1) if y.ndim > 1 else y), sr


def envelope(s):
    return np.convolve(np.abs(np.diff(s)), np.ones(64) / 64, "same")


def find_delay(x, y, sr):
    ex, ey = envelope(x), envelope(y)
    n = min(len(ex), len(ey))
    a, b = ex[:n] - ex[:n].mean(), ey[:n] - ey[:n].mean()
    c = correlate(b, a, "full", method="fft")
    lags = correlation_lags(n, n)
    keep = (lags > -int(0.01 * sr)) & (lags < int(0.08 * sr))
    return int(lags[keep][np.argmax(c[keep])])


def main():
    x, sr = load(sys.argv[1])
    ratio = 2.0 ** (float(sys.argv[2]) / 12.0)
    frame, hop = 8192, 2048
    window = np.hanning(frame)
    freqs = np.fft.rfftfreq(frame, 1.0 / sr)
    band = (freqs >= 40) & (freqs <= 3000)
    for path in sys.argv[3:]:
        y, _ = load(path)
        delay = find_delay(x, y, sr)
        online, spectral = [], []
        for start in range(0, min(len(x), len(y) - delay) - frame, hop):
            seg_x = x[start:start + frame]
            if np.sqrt(np.mean(seg_x ** 2)) < 1e-3:
                continue
            seg_y = y[start + delay:start + delay + frame]
            mag_x = np.abs(np.fft.rfft(seg_x * window))
            mag_y = np.abs(np.fft.rfft(seg_y * window))
            # Ideal: value at f is the input's value at f / ratio.
            ideal = np.interp(freqs / ratio, freqs, mag_x)
            ideal_db = 20 * np.log10(ideal + 1e-9)
            peaks, _ = find_peaks(ideal_db, height=ideal_db[band].max() - 30)
            lines = np.zeros_like(band)
            for p in peaks:
                lines[max(0, p - 2):p + 3] = True
            power_y = mag_y ** 2
            total = power_y[band].sum()
            if total <= 1e-12:
                continue
            online.append(power_y[band & lines].sum() / total)
            out_db = 20 * np.log10(mag_y + 1e-9)
            spectral.append(np.corrcoef(out_db[band], ideal_db[band])[0, 1])
        print("%-48s delay %5.1f ms  on-line %.3f  spectral %.3f  frames %d"
              % (os.path.basename(path)[-48:], 1e3 * delay / sr, np.median(online),
                 np.median(spectral), len(online)))


if __name__ == "__main__":
    main()
