// ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>

// Stereo RBJ biquad with coefficient updates that keep the filter state, so
// gains and cutoffs can follow parameters without resetting.
class StereoBiquad
{
public:
    void reset()
    {
        for (auto& state : states)
            state = {};
    }

    void setLowShelf(double sampleRate, double frequency, double gainDb)
    {
        const double a = std::pow(10.0, gainDb / 40.0);
        const double w0 = juce::MathConstants<double>::twoPi * frequency / sampleRate;
        const double cosine = std::cos(w0);
        const double alpha = std::sin(w0) / 2.0 * std::sqrt(2.0); // shelf slope S = 1
        const double root = 2.0 * std::sqrt(a) * alpha;
        setCoefficients(a * ((a + 1.0) - (a - 1.0) * cosine + root),
                        2.0 * a * ((a - 1.0) - (a + 1.0) * cosine),
                        a * ((a + 1.0) - (a - 1.0) * cosine - root),
                        (a + 1.0) + (a - 1.0) * cosine + root,
                        -2.0 * ((a - 1.0) + (a + 1.0) * cosine),
                        (a + 1.0) + (a - 1.0) * cosine - root);
    }

    void setHighShelf(double sampleRate, double frequency, double gainDb)
    {
        const double a = std::pow(10.0, gainDb / 40.0);
        const double w0 = juce::MathConstants<double>::twoPi * frequency / sampleRate;
        const double cosine = std::cos(w0);
        const double alpha = std::sin(w0) / 2.0 * std::sqrt(2.0);
        const double root = 2.0 * std::sqrt(a) * alpha;
        setCoefficients(a * ((a + 1.0) + (a - 1.0) * cosine + root),
                        -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine),
                        a * ((a + 1.0) + (a - 1.0) * cosine - root),
                        (a + 1.0) - (a - 1.0) * cosine + root,
                        2.0 * ((a - 1.0) - (a + 1.0) * cosine),
                        (a + 1.0) - (a - 1.0) * cosine - root);
    }

    // 12 dB/oct Butterworth high-pass.
    void setHighPass(double sampleRate, double frequency)
    {
        const double w0 = juce::MathConstants<double>::twoPi * frequency / sampleRate;
        const double cosine = std::cos(w0);
        const double alpha = std::sin(w0) / juce::MathConstants<double>::sqrt2; // Q = 1/sqrt(2)
        setCoefficients((1.0 + cosine) * 0.5, -(1.0 + cosine), (1.0 + cosine) * 0.5,
                        1.0 + alpha, -2.0 * cosine, 1.0 - alpha);
    }

    float process(int channel, float x)
    {
        auto& s = states[(size_t) channel];
        const float y = b0 * x + s.z1;
        s.z1 = b1 * x - a1 * y + s.z2;
        s.z2 = b2 * x - a2 * y;
        return y;
    }

private:
    struct State { float z1 = 0.0f, z2 = 0.0f; };

    void setCoefficients(double nb0, double nb1, double nb2, double na0, double na1, double na2)
    {
        b0 = (float) (nb0 / na0);
        b1 = (float) (nb1 / na0);
        b2 = (float) (nb2 / na0);
        a1 = (float) (na1 / na0);
        a2 = (float) (na2 / na0);
    }

    std::array<State, 2> states {};
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};
