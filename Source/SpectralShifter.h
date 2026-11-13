// ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

// Low-latency phase-vocoder pitch shifter (one voice, up to two channels).
//
// 1. Resampling: the input is read at speed `ratio` with a band-limited sinc
//    interpolator, producing a "resampled stream" whose pitch is already the
//    target pitch (and whose duration is 1 / ratio times longer or shorter).
//    Every resampled sample remembers the input time it was read at.
// 2. Phase vocoder: every `hop` input samples, the newest N resampled
//    samples are analysed with an asymmetric window (long rise for low
//    frequency resolution, short M-sample fall for low latency). Phases are
//    propagated as in a time-scale phase vocoder: the analysis advance is the
//    resampled-stream advance, the synthesis advance is the output-time
//    advance of the window peak.
// 3. Synthesis: only the 2M samples around the window peak are synthesised
//    (Hann window, weighted overlap-add), centred on the input time of the
//    peak. The output is read M (1 + ratio) samples (plus the interpolator's
//    look-ahead) behind the input: the earliest time every frame covering it
//    has been computed.
// 4. Attacks: a causal energy detector marks pick attacks; the next frame
//    whose peak has passed the attack resets its synthesis phases to the
//    analysis phases, so the attack is not smeared across frames.
class SpectralShifter
{
public:
    void prepare(double newSampleRate)
    {
        sampleRate = newSampleRate;
        const double scale = sampleRate / 44100.0;
        fftOrder = scale > 1.5 ? 13 : 12;                        // ~93 ms window
        fftSize = 1 << fftOrder;
        fall = juce::jmax(32, (int) std::lround(192.0 * scale)); // ~4.4 ms
        hop = juce::jmax(8, (int) std::lround(fall * hopFraction));
        bins = fftSize / 2 + 1;
        fft = std::make_unique<juce::dsp::FFT>(fftOrder);

        analysisWindow.assign((size_t) fftSize, 0.0f);
        const int rise = fftSize - fall;
        for (int i = 0; i < rise; ++i)
            analysisWindow[(size_t) i] = 0.5f - 0.5f * (float) std::cos(juce::MathConstants<double>::pi * i / rise);
        for (int i = 0; i < fall; ++i)
            analysisWindow[(size_t) (rise + i)] = 0.5f + 0.5f * (float) std::cos(juce::MathConstants<double>::pi * i / fall);
        synthesisWindow.assign((size_t) (2 * fall), 0.0f);
        overlapWeight.assign((size_t) (2 * fall), 0.0f);
        for (int i = 0; i < 2 * fall; ++i)
        {
            synthesisWindow[(size_t) i] = 0.5f - 0.5f * (float) std::cos(juce::MathConstants<double>::twoPi * i / (2 * fall));
            overlapWeight[(size_t) i] = synthesisWindow[(size_t) i] * analysisWindow[(size_t) (fftSize - 2 * fall + i)];
        }
        binFrequency.assign((size_t) bins, 0.0f);
        for (int k = 0; k < bins; ++k)
            binFrequency[(size_t) k] = (float) (juce::MathConstants<double>::twoPi * k / fftSize);
        buildSincTable();

        fftBuffer.assign((size_t) (2 * fftSize), 0.0f);
        magnitude.assign((size_t) bins, 0.0f);
        phase.assign((size_t) bins, 0.0f);
        for (auto& c : channelState)
        {
            c.previousPhase.assign((size_t) bins, 0.0f);
            c.synthesisPhase.assign((size_t) bins, 0.0f);
        }
        fastLength = juce::jmax(1, (int) std::lround(0.001 * sampleRate));
        slowLength = juce::jmax(1, (int) std::lround(0.030 * sampleRate));
        longCoefficient = 1.0f - (float) std::exp(-1.0 / (2.0 * sampleRate));
        onsetRefractory = (int) std::lround(0.08 * sampleRate);
        ratioSmoothing = 1.0 - std::exp(-1.0 / (0.020 * sampleRate));
        reset();
    }

    void reset()
    {
        for (auto& c : channelState)
        {
            c.input.fill(0.0f);
            c.resampled.fill(0.0f);
            c.output.fill(0.0f);
            std::fill(c.previousPhase.begin(), c.previousPhase.end(), 0.0f);
            std::fill(c.synthesisPhase.begin(), c.synthesisPhase.end(), 0.0f);
        }
        resampledTime.fill(0.0);
        weight.fill(0.0f);
        energyHistory.fill(0.0f);
        position = 0;
        clearedThrough = -1;
        readPointer = 0.0;
        resampledCount = 0;
        hopCounter = 0;
        firstFrame = true;
        previousFrameEnd = 0;
        previousPeak = 0;
        currentRatio = targetRatio;
        fastSum = slowSum = 0.0;
        longLevel = 0.0f;
        sinceOnset = onsetRefractory;
        pendingOnset = -1.0;
        onsetCount = resetCount = 0;
        channelsDiffer = false;
    }

    // Frame spacing as a fraction of the window fall M (next prepare()).
    void setHopFraction(double fraction) { hopFraction = fraction; }

    void setRatio(double ratio) { targetRatio = juce::jlimit(0.25, 4.0, ratio); }
    void setRatioImmediately(double ratio) { currentRatio = targetRatio = juce::jlimit(0.25, 4.0, ratio); }

    // Current input-to-output delay, in samples.
    double getLatencySamples() const
    {
        return fall * (1.0 + currentRatio) + lookAhead(currentRatio) + currentRatio + 2.0;
    }

    // Diagnostics: attacks detected / phase resets applied since prepare().
    int getOnsetCount() const { return onsetCount; }
    int getResetCount() const { return resetCount; }

    void process(const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output)
    {
        const int channels = juce::jmin(2, input.getNumChannels(), output.getNumChannels());
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            const auto slot = (size_t) (position & inputMask);
            float mono = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
            {
                const float x = input.getSample(ch, i);
                channelState[(size_t) ch].input[slot] = x;
                mono += x;
            }
            if (channels == 2 && input.getSample(0, i) != input.getSample(1, i))
                channelsDiffer = true;
            detectOnset(mono / (float) channels);
            currentRatio += (targetRatio - currentRatio) * ratioSmoothing;
            produceResampled(channels);

            if (++hopCounter >= hop && resampledCount >= fftSize)
            {
                hopCounter = 0;
                computeFrame(channels);
            }

            const double readTime = (double) position - getLatencySamples();
            for (int ch = 0; ch < channels; ++ch)
                output.setSample(ch, i, readOutput(ch, readTime));
            clearBehind(channels, readTime);
            ++position;
        }
    }

private:
    static constexpr int inputSize = 1 << 15;
    static constexpr int64_t inputMask = inputSize - 1;
    static constexpr int resampledSize = 1 << 15;
    static constexpr int64_t resampledMask = resampledSize - 1;
    static constexpr int outputSize = 1 << 13;
    static constexpr int64_t outputMask = outputSize - 1;
    static constexpr int energySize = 1 << 13;
    static constexpr int64_t energyMask = energySize - 1;
    static constexpr int sincHalfTaps = 8;
    static constexpr int sincPhases = 512;

    struct Channel
    {
        std::array<float, inputSize> input {};
        std::array<float, resampledSize> resampled {};
        std::array<float, outputSize> output {};
        std::vector<float> previousPhase;
        std::vector<float> synthesisPhase;
    };

    static double cutoffFor(double ratio) { return juce::jmin(1.0, 0.95 / ratio); }
    static double lookAhead(double ratio) { return std::ceil(sincHalfTaps / cutoffFor(ratio)) + 1.0; }

    // Windowed sinc (Blackman), sincHalfTaps zero crossings each side.
    void buildSincTable()
    {
        const int length = sincHalfTaps * sincPhases + 2;
        sincTable.assign((size_t) length, 0.0f);
        for (int i = 0; i < length; ++i)
        {
            const double x = (double) i / sincPhases;
            if (x >= sincHalfTaps)
                continue;
            const double sinc = i == 0 ? 1.0 : std::sin(juce::MathConstants<double>::pi * x)
                                                   / (juce::MathConstants<double>::pi * x);
            const double t = x / sincHalfTaps;
            const double blackman = 0.42 + 0.5 * std::cos(juce::MathConstants<double>::pi * t)
                                  + 0.08 * std::cos(juce::MathConstants<double>::twoPi * t);
            sincTable[(size_t) i] = (float) (sinc * blackman);
        }
    }

    float kernel(double x) const // x in zero-crossing units
    {
        const double a = std::abs(x) * sincPhases;
        const auto index = (size_t) a;
        if (index + 1 >= sincTable.size())
            return 0.0f;
        const float frac = (float) (a - (double) index);
        return sincTable[index] + frac * (sincTable[index + 1] - sincTable[index]);
    }

    // Reads the input at speed `ratio` up to what the interpolator can see.
    void produceResampled(int channels)
    {
        const double ratio = currentRatio;
        const double cutoff = cutoffFor(ratio);
        const double ahead = lookAhead(ratio);
        const int span = (int) std::ceil(sincHalfTaps / cutoff);
        while (readPointer + ahead <= (double) position)
        {
            const auto centre = (int64_t) std::floor(readPointer);
            const double frac = readPointer - (double) centre;
            const auto out = (size_t) (resampledCount & resampledMask);
            std::array<float, 2> sum {};
            for (int k = -span + 1; k <= span; ++k)
            {
                const int64_t index = centre + k;
                if (index < 0)
                    continue;
                const float w = kernel(((double) k - frac) * cutoff);
                const auto in = (size_t) (index & inputMask);
                for (int ch = 0; ch < channels; ++ch)
                    sum[(size_t) ch] += channelState[(size_t) ch].input[in] * w;
            }
            for (int ch = 0; ch < channels; ++ch)
                channelState[(size_t) ch].resampled[out] = sum[(size_t) ch] * (float) cutoff;
            resampledTime[out] = readPointer;
            ++resampledCount;
            readPointer += ratio;
        }
    }

    // Attack = RMS of the last 1 ms above 4x the RMS of the 30 ms before it,
    // and above a third of the long-term (2 s) level; at most one per 80 ms.
    void detectOnset(float x)
    {
        const float energy = x * x;
        energyHistory[(size_t) (position & energyMask)] = energy;
        fastSum += energy - energyAt(position - fastLength);
        slowSum += energyAt(position - fastLength) - energyAt(position - fastLength - slowLength);
        const float fastRms = std::sqrt((float) juce::jmax(0.0, fastSum) / (float) fastLength);
        const float slowRms = std::sqrt((float) juce::jmax(0.0, slowSum) / (float) slowLength);
        longLevel += longCoefficient * (fastRms - longLevel);
        if (sinceOnset < onsetRefractory)
            ++sinceOnset;
        else if (position > fastLength + slowLength && fastRms > 4.0f * slowRms
                 && fastRms > 0.35f * longLevel && fastRms > 1.0e-4f)
        {
            sinceOnset = 0;
            pendingOnset = (double) position;
            ++onsetCount;
        }
    }

    float energyAt(int64_t index) const
    {
        return index < 0 ? 0.0f : energyHistory[(size_t) (index & energyMask)];
    }

    static float wrapPhase(float p)
    {
        constexpr float twoPi = juce::MathConstants<float>::twoPi;
        return p - twoPi * std::floor(p / twoPi + 0.5f);
    }

    // |error| < 1.1e-5 rad
    static float fastAtan2(float y, float x)
    {
        const float ax = std::abs(x), ay = std::abs(y);
        const float largest = juce::jmax(ax, ay);
        if (largest <= 0.0f)
            return 0.0f;
        const float a = juce::jmin(ax, ay) / largest;
        const float s = a * a;
        float r = ((((-0.0134804f * s + 0.0574773f) * s - 0.1212390f) * s + 0.1956359f) * s
                   - 0.3329946f) * s * a + a;
        if (ay > ax)
            r = 1.5707963f - r;
        if (x < 0.0f)
            r = 3.1415927f - r;
        return y < 0.0f ? -r : r;
    }

    // sin and cos of p in [-pi, pi], |error| < 4e-6
    static void fastSinCos(float p, float& s, float& c)
    {
        constexpr float halfPi = juce::MathConstants<float>::halfPi;
        constexpr float pi = juce::MathConstants<float>::pi;
        auto sine = [](float x) // x in [-pi/2, pi/2]
        {
            const float x2 = x * x;
            return x * (1.0f + x2 * (-1.6666667e-1f + x2 * (8.3333331e-3f + x2 * (-1.9840874e-4f
                                                                                 + x2 * 2.7525562e-6f))));
        };
        s = sine(p > halfPi ? pi - p : (p < -halfPi ? -pi - p : p));
        float q = p + halfPi; // cos p = sin(p + pi/2)
        if (q > pi)
            q -= 2.0f * pi;
        c = sine(q > halfPi ? pi - q : (q < -halfPi ? -pi - q : q));
    }

    void computeFrame(int channels)
    {
        const int64_t frameEnd = resampledCount - 1;                // newest resampled sample
        const int64_t peakIndex = frameEnd - fall;
        const auto peak = (int64_t) std::llround(resampledTime[(size_t) (peakIndex & resampledMask)]);
        bool reset = firstFrame;
        if (pendingOnset >= 0.0
            && resampledTime[(size_t) ((frameEnd - fall / 2) & resampledMask)] >= pendingOnset)
        {
            reset = true;
            pendingOnset = -1.0;
            ++resetCount;
        }
        const auto analysisStep = (float) juce::jmax<int64_t>(1, frameEnd - previousFrameEnd);
        const auto synthesisStep = (float) (peak - previousPeak);
        previousFrameEnd = frameEnd;
        previousPeak = peak;

        // Identical channels (mono source on a stereo bus): analyse once.
        const int analysed = channelsDiffer ? channels : 1;
        channelsDiffer = false;
        for (int ch = 0; ch < analysed; ++ch)
        {
            auto& c = channelState[(size_t) ch];
            float* data = fftBuffer.data();
            for (int j = 0; j < fftSize; ++j)
                data[j] = c.resampled[(size_t) ((frameEnd - fftSize + 1 + j) & resampledMask)]
                        * analysisWindow[(size_t) j];
            std::fill(fftBuffer.begin() + fftSize, fftBuffer.end(), 0.0f);
            fft->performRealOnlyForwardTransform(data, true);

            for (int k = 0; k < bins; ++k)
            {
                const float re = data[2 * k], im = data[2 * k + 1];
                const float mag = std::sqrt(re * re + im * im);
                const float ph = fastAtan2(im, re);
                float& synth = c.synthesisPhase[(size_t) k];
                if (reset)
                    synth = ph;
                else
                {
                    const float omega = binFrequency[(size_t) k];
                    const float delta = wrapPhase(ph - c.previousPhase[(size_t) k] - omega * analysisStep);
                    synth = wrapPhase(synth + (omega + delta / analysisStep) * synthesisStep);
                }
                c.previousPhase[(size_t) k] = ph;
                float s, co;
                fastSinCos(synth, s, co);
                data[2 * k] = mag * co;
                data[2 * k + 1] = mag * s;
            }
            for (int k = bins; k < fftSize; ++k)
            {
                data[2 * k] = data[2 * (fftSize - k)];
                data[2 * k + 1] = -data[2 * (fftSize - k) + 1];
            }
            fft->performRealOnlyInverseTransform(data);

            for (int i = 0; i < 2 * fall; ++i)
            {
                const auto slot = (size_t) ((peak - fall + i) & outputMask);
                const float value = data[fftSize - 2 * fall + i] * synthesisWindow[(size_t) i];
                c.output[slot] += value;
                if (analysed == 1 && channels == 2)
                    channelState[1].output[slot] += value;
                if (ch == 0)
                    weight[slot] += overlapWeight[(size_t) i];
            }
        }
        if (analysed == 1 && channels == 2)
        {
            channelState[1].previousPhase = channelState[0].previousPhase;
            channelState[1].synthesisPhase = channelState[0].synthesisPhase;
        }
        firstFrame = false;
    }

    float readOutput(int ch, double time) const
    {
        const auto index = (int64_t) std::floor(time);
        if (index < 0)
            return 0.0f;
        const float frac = (float) (time - (double) index);
        const auto& out = channelState[(size_t) ch].output;
        const auto a = (size_t) (index & outputMask), b = (size_t) ((index + 1) & outputMask);
        const float wa = juce::jmax(weight[a], 1.0e-3f), wb = juce::jmax(weight[b], 1.0e-3f);
        return (out[a] / wa) * (1.0f - frac) + (out[b] / wb) * frac;
    }

    // Frees the output slots that will not be read again.
    void clearBehind(int channels, double readTime)
    {
        const auto oldest = (int64_t) std::floor(readTime) - 2;
        while (clearedThrough < oldest)
        {
            ++clearedThrough;
            const auto slot = (size_t) (clearedThrough & outputMask);
            for (int ch = 0; ch < channels; ++ch)
                channelState[(size_t) ch].output[slot] = 0.0f;
            weight[slot] = 0.0f;
        }
    }

    std::array<Channel, 2> channelState {};
    std::array<double, resampledSize> resampledTime {};
    std::array<float, outputSize> weight {};
    std::array<float, energySize> energyHistory {};
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> analysisWindow, synthesisWindow, overlapWeight, binFrequency, sincTable;
    std::vector<float> fftBuffer, magnitude, phase;
    double sampleRate = 44100.0;
    double hopFraction = 0.5;
    double currentRatio = 1.0, targetRatio = 1.0, ratioSmoothing = 0.001;
    double readPointer = 0.0, pendingOnset = -1.0, fastSum = 0.0, slowSum = 0.0;
    int64_t position = 0, clearedThrough = -1, resampledCount = 0, previousFrameEnd = 0, previousPeak = 0;
    int fftOrder = 12, fftSize = 4096, bins = 2049, fall = 192, hop = 96, hopCounter = 0;
    int fastLength = 44, slowLength = 1323, onsetRefractory = 3528, sinceOnset = 0;
    int onsetCount = 0, resetCount = 0;
    float longCoefficient = 1.0e-5f, longLevel = 0.0f;
    bool firstFrame = true, channelsDiffer = false;
};
