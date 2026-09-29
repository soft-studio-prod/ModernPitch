// ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>

// Pitch-tracked ring modulator carrier.
//
// The carrier is a pure sine at half the fundamental of the signal it
// modulates. Multiplying a note by it moves every harmonic k*F to
// (k +- 1/2)*F: a hollow, square-like tone one octave down with odd
// harmonics only.
//
// The pitch comes from the PSOLA engine's tracking, which runs at analysis
// time. The shifted signal leaves the engine `lag` samples later, so the
// tracked period is delayed by the same amount before use. When no pitch is
// locked (chords, noise, silence) the last frequency is held.
class RingModulator
{
public:
    void prepare(double newSampleRate)
    {
        sampleRate = newSampleRate;
        // ~1 ms glide keeps note changes free of sudden frequency steps.
        frequencySmoothing = 1.0 - std::exp(-1.0 / (0.001 * sampleRate));
        reset();
    }

    void reset()
    {
        periodHistory.fill(0.0f);
        writePosition = 0;
        phase = 0.0;
        frequency = targetFrequency = 0.0;
    }

    // trackedPeriod: input period (samples) at analysis time, 0 = unknown.
    // lagSamples: current delay of the modulated signal behind the analysis.
    // ratio: pitch ratio of the modulated signal relative to the input.
    // Returns the carrier sample, or 0 until a first pitch has been seen.
    float next(float trackedPeriod, double lagSamples, double ratio)
    {
        periodHistory[(size_t) writePosition] = trackedPeriod;
        const int delay = juce::jlimit(0, historySize - 1, (int) std::lround(lagSamples));
        const float period = periodHistory[(size_t) ((writePosition - delay) & historyMask)];
        writePosition = (writePosition + 1) & historyMask;

        if (period > 0.0f)
            targetFrequency = 0.5 * sampleRate / (double) period * ratio;
        if (targetFrequency <= 0.0)
            return 0.0f;
        frequency = frequency > 0.0 ? frequency + frequencySmoothing * (targetFrequency - frequency)
                                    : targetFrequency;

        phase += juce::MathConstants<double>::twoPi * frequency / sampleRate;
        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;
        // sqrt(2): a sine carrier halves the power; keep the ring voice at the
        // level of the signal it modulates.
        return (float) (juce::MathConstants<double>::sqrt2 * std::sin(phase));
    }

private:
    static constexpr int historySize = 8192;
    static constexpr int historyMask = historySize - 1;

    std::array<float, historySize> periodHistory {};
    double sampleRate = 48000.0;
    double frequencySmoothing = 0.02;
    double phase = 0.0;
    double frequency = 0.0;
    double targetFrequency = 0.0;
    int writePosition = 0;
};
