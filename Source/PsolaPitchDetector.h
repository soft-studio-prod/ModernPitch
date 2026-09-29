// ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>

// Causal monophonic F0 estimator for the PSOLA engine. It analyses a
// low-passed copy of the input decimated to roughly 12 kHz and only uses past
// samples, so it adds no audio lookahead. The reported period is expressed in
// full-rate samples. A new estimate is produced every ~2.5 ms.
class PsolaPitchDetector
{
public:
    enum class Method
    {
        Yin,          // de Cheveigne & Kawahara, fixed window (fallback)
        AdaptiveMpm   // McLeod NSDF with a lag-dependent window (default)
    };

    struct Estimate
    {
        double period = 0.0;
        float confidence = 0.0f;
        bool voiced = false;
    };

    // 40 Hz covers an 8-string in standard tuning (F#1 = 46 Hz) with margin.
    static constexpr float minimumFrequencyHz = 40.0f;
    static constexpr float maximumFrequencyHz = 1100.0f;

    void prepare(double sampleRate)
    {
        decimation = juce::jmax(1, (int) std::lround(sampleRate / 12000.0));
        const double decimatedRate = sampleRate / (double) decimation;
        minLag = juce::jmax(2, (int) std::floor(decimatedRate / maximumFrequencyHz));
        maxLag = juce::jmin(lagCapacity - 2, (int) std::ceil(decimatedRate / minimumFrequencyHz));
        windowLength = maxLag;
        frameLength = windowLength + maxLag + 2;
        warmUpLength = juce::jmin(frameCapacity, 3 * maxLag);
        hop = juce::jmax(4, (int) std::lround(decimatedRate * 0.0025));
        minimumWindow = juce::jlimit(8, maxLag, (int) std::lround(decimatedRate * 0.010));
        reset();
    }

    void reset()
    {
        ring.fill(0.0f);
        writePosition = 0;
        decimationPhase = 0;
        samplesSinceAnalysis = 0;
        decimatedCount = 0;
        estimate = {};
    }

    void setMethod(Method newMethod) { method = newMethod; }

    // Push one full-rate, already low-passed sample. Returns true when a new
    // estimate is available.
    bool push(float sample)
    {
        if (++decimationPhase < decimation)
            return false;
        decimationPhase = 0;

        ring[(size_t) writePosition] = sample;
        writePosition = (writePosition + 1) & ringMask;
        ++decimatedCount;

        // Wait for three of the lowest periods before the first estimate.
        if (++samplesSinceAnalysis < hop || decimatedCount < warmUpLength)
            return false;
        samplesSinceAnalysis = 0;
        analyse();
        return true;
    }

    const Estimate& getEstimate() const { return estimate; }

private:
    static constexpr int lagCapacity = 512;
    static constexpr int frameCapacity = 2 * lagCapacity + 4;
    static constexpr int ringSize = 2048;
    static constexpr int ringMask = ringSize - 1;
    // -60 dBFS RMS: below this the frame is treated as silence.
    static constexpr float silenceRms = 0.001f;

    static double parabolicOffset(double left, double centre, double right)
    {
        const double denominator = left - 2.0 * centre + right;
        if (std::abs(denominator) < 1.0e-12)
            return 0.0;
        return juce::jlimit(-0.5, 0.5, 0.5 * (left - right) / denominator);
    }

    void analyse()
    {
        for (int i = 0; i < frameLength; ++i)
            frame[(size_t) i] = ring[(size_t) ((writePosition - frameLength + i) & ringMask)];
        referenceStart = frameLength - windowLength;

        double energy = 0.0;
        for (int j = referenceStart; j < frameLength; ++j)
            energy += (double) frame[(size_t) j] * frame[(size_t) j];
        if (std::sqrt(energy / windowLength) < silenceRms)
        {
            estimate = {};
            return;
        }

        double lag = 0.0;
        float confidence = 0.0f;
        const bool voiced = method == Method::Yin ? analyseYin(lag, confidence)
                                                  : analyseAdaptiveMpm(lag, confidence);
        estimate.voiced = voiced && lag >= (double) minLag && lag <= (double) maxLag;
        estimate.confidence = confidence;
        if (estimate.voiced)
            estimate.period = lag * (double) decimation;
    }

    // Cumulative mean normalised difference over a fixed window of maxLag.
    bool analyseYin(double& lag, float& confidence)
    {
        double running = 0.0;
        curve[0] = 1.0f;
        for (int tau = 1; tau <= maxLag + 1; ++tau)
        {
            double difference = 0.0;
            for (int j = referenceStart; j < frameLength; ++j)
            {
                const double delta = (double) frame[(size_t) j] - frame[(size_t) (j - tau)];
                difference += delta * delta;
            }
            running += difference;
            curve[(size_t) tau] = running > 0.0 ? (float) (difference * tau / running) : 1.0f;
        }

        int selected = -1;
        for (int tau = minLag; tau <= maxLag; ++tau)
            if (curve[(size_t) tau] < 0.15f)
            {
                while (tau + 1 <= maxLag && curve[(size_t) (tau + 1)] < curve[(size_t) tau])
                    ++tau;
                selected = tau;
                break;
            }
        if (selected < 0)
        {
            selected = minLag;
            for (int tau = minLag + 1; tau <= maxLag; ++tau)
                if (curve[(size_t) tau] < curve[(size_t) selected])
                    selected = tau;
        }

        const float value = curve[(size_t) selected];
        lag = selected + parabolicOffset(curve[(size_t) (selected - 1)], value,
                                         curve[(size_t) (selected + 1)]);
        confidence = juce::jlimit(0.0f, 1.0f, 1.0f - value);
        return value < 0.3f;
    }

    // MPM with a lag-dependent window: candidate lag tau compares the last
    // max(tau, minimumWindow) samples with the ones tau earlier, so the true
    // period only needs two periods of history. After a note change the
    // estimate is not polluted by up to 50 ms of the previous note, as it is
    // with a window sized for the lowest string.
    bool analyseAdaptiveMpm(double& lag, float& confidence)
    {
        for (int tau = 1; tau <= maxLag + 1; ++tau)
        {
            const int window = juce::jmax(tau, minimumWindow);
            double cross = 0.0, energy = 0.0;
            for (int j = frameLength - window; j < frameLength; ++j)
            {
                const double current = frame[(size_t) j];
                const double delayed = frame[(size_t) (j - tau)];
                cross += current * delayed;
                energy += current * current + delayed * delayed;
            }
            curve[(size_t) tau] = energy > 1.0e-12 ? (float) (2.0 * cross / energy) : 0.0f;
        }
        return pickKeyMaximum(lag, confidence);
    }

    // McLeod key-maximum picking: the first positive-lobe peak within 90 % of
    // the highest one, which avoids sub-octave errors.
    bool pickKeyMaximum(double& lag, float& confidence)
    {
        std::array<int, 64> keyMaxima {};
        int keyCount = 0;
        int tau = 1;
        while (tau <= maxLag && curve[(size_t) tau] > 0.0f)
            ++tau;
        while (tau <= maxLag && keyCount < (int) keyMaxima.size())
        {
            while (tau <= maxLag && curve[(size_t) tau] <= 0.0f)
                ++tau;
            int peak = -1;
            while (tau <= maxLag && curve[(size_t) tau] > 0.0f)
            {
                if (peak < 0 || curve[(size_t) tau] > curve[(size_t) peak])
                    peak = tau;
                ++tau;
            }
            if (peak >= minLag)
                keyMaxima[(size_t) keyCount++] = peak;
        }
        if (keyCount == 0)
            return false;

        float best = 0.0f;
        for (int i = 0; i < keyCount; ++i)
            best = juce::jmax(best, curve[(size_t) keyMaxima[(size_t) i]]);
        int selected = keyMaxima[0];
        for (int i = 0; i < keyCount; ++i)
            if (curve[(size_t) keyMaxima[(size_t) i]] >= 0.9f * best)
            {
                selected = keyMaxima[(size_t) i];
                break;
            }
        if (selected >= maxLag + 1)
            return false;

        const float value = curve[(size_t) selected];
        lag = selected + parabolicOffset(curve[(size_t) (selected - 1)], value,
                                         curve[(size_t) (selected + 1)]);
        confidence = juce::jlimit(0.0f, 1.0f, value);
        return value >= 0.6f;
    }

    std::array<float, ringSize> ring {};
    std::array<float, frameCapacity> frame {};
    std::array<float, lagCapacity + 2> curve {};
    Estimate estimate;
    Method method = Method::AdaptiveMpm;
    int decimation = 4;
    int minLag = 10;
    int maxLag = 300;
    int windowLength = 300;
    int frameLength = 602;
    int warmUpLength = 900;
    int referenceStart = 302;
    int hop = 30;
    int minimumWindow = 120;
    int writePosition = 0;
    int decimationPhase = 0;
    int samplesSinceAnalysis = 0;
    int decimatedCount = 0;
};
