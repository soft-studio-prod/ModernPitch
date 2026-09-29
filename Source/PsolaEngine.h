// ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include "PsolaPitchDetector.h"

// Streaming pitch-synchronous overlap-add (PSOLA) pitch shifter with
// resampled grains.
//
// Analysis (shared by both voices): a causal F0 detector plus pitch marks
// placed one period apart by waveform correlation.
//
// Synthesis (per voice): Hann grains two output periods long are placed every
// output period. Each grain replays input periods resampled by the pitch
// ratio, so the whole harmonic series scales like a retuned string. Grains read
// their source lazily, one sample at a time, as late as the input allows, so
// the delay follows the played note instead of a fixed window:
//   lag = halfWidth * |1 - rate|   (reading must stay behind the input)
//       + slack                    (distance back to a phase-coherent source)
// Consecutive grains only need a constant phase relative to the pitch marks,
// not a particular phase, so each voice chooses that phase when a note starts
// to make the slack (almost) zero.
//
// Unpitched input (chords, noise) falls back to a 10 ms granular shifter.
// Host PDC is not reported; the delay varies with the input.
class PsolaEngine
{
public:
    static constexpr int numVoices = 2;

    void prepare(double newSampleRate)
    {
        sampleRate = newSampleRate;
        ratioSmoothingCoefficient = 1.0 - std::exp(-1.0 / (sampleRate * 0.020));
        granularHalfWidth = 0.010 * sampleRate;
        minimumRise = 0.002 * sampleRate;
        restartCrossfade = 0.002 * sampleRate;
        dcBlockerCoefficient = (float) std::exp(-juce::MathConstants<double>::twoPi * 20.0 / sampleRate);
        lowPassStages[0].design(sampleRate, 1500.0, 0.5412);
        lowPassStages[1].design(sampleRate, 1500.0, 1.3066);
        for (int i = 0; i < windowTableSize; ++i)
            windowTable[(size_t) i] = 0.5f + 0.5f * (float) std::cos(
                juce::MathConstants<double>::pi * (double) i / (double) (windowTableSize - 1));
        detector.prepare(sampleRate);
        reset();
    }

    void reset()
    {
        for (auto& channel : inputRing)
            channel.fill(0.0f);
        analysisRing.fill(0.0f);
        for (auto& stage : lowPassStages)
            stage.reset();
        dcBlockerState = dcBlockerInput = 0.0f;
        detector.reset();
        position = 0;
        clearMarks();
        markGeneration = 0;
        voiced = false;
        voicedRun = unvoicedRun = harmonicJumpRun = 0;
        detectedPeriod = 0.0;
        for (auto& voice : voices)
        {
            voice.grainCount = 0;
            voice.nextCentre = 0.0;
            voice.lagEstimate = readMargin;
            voice.latticeGeneration = -1;
            voice.latticeFraction = 0.0;
            voice.currentRatio = voice.targetRatio;
            voice.grainKindSamples.fill(0);
        }
    }

    void setPitchSemitones(int voice, float semitones)
    {
        auto& v = voices[(size_t) voice];
        v.currentRatio = v.targetRatio = ratioFromSemitones(semitones);
    }

    void setPitchSemitonesSmoothed(int voice, float semitones)
    {
        voices[(size_t) voice].targetRatio = ratioFromSemitones(semitones);
    }

    // Note tracking. true (default): adaptive-window MPM detector, lock on the
    // first voiced estimate, octave-error rejection and dropout hold.
    // false: YIN detector, two estimates to lock, no rejection or hold.
    void setImprovedTracking(bool enabled)
    {
        robustTracking = enabled;
        lockEstimates = enabled ? 1 : 2;
        detector.setMethod(enabled ? PsolaPitchDetector::Method::AdaptiveMpm
                                   : PsolaPitchDetector::Method::Yin);
    }

    // Start a newly locked note's first grain at once, fading the previous
    // grains out (default), instead of waiting for the old pulse train.
    void setRestartOnNewNote(bool enabled) { restartOnNewNote = enabled; }

    // Pitched grains 1.4 instead of 2 output periods long (same spacing):
    // 1-3 ms less delay, but rougher attacks. Off by default.
    void setTightGrains(bool enabled) { grainLengthFactor = enabled ? tightGrainLengthFactor : 1.0; }

    // Per-note phase alignment (default). Offline comparison only.
    void setPhaseAlignment(bool enabled) { phaseAlignment = enabled; }

    // Detector choice for offline experiments.
    void setDetectorMethod(PsolaPitchDetector::Method method) { detector.setMethod(method); }

    // Smoothed input-to-output offset at grain centres, in samples.
    double getLagSamples(int voice) const { return voices[(size_t) voice].lagEstimate; }

    // Output samples covered by pitched / granular grains (diagnostics).
    const std::array<int64_t, 2>& getGrainKindSamples(int voice) const
    {
        return voices[(size_t) voice].grainKindSamples;
    }

    // Offline event log (diagnostics only: it allocates, never set it in the
    // audio thread).
    enum class EventType { Estimate, VoicedOn, VoicedOff, ResetJump, ResetCorrelation, NewNote, Mark, Grain };
    struct Event
    {
        int64_t position = 0;
        EventType type = EventType::Estimate;
        double value = 0.0;  // period, period ratio (ResetJump) or grain lag (Grain)
        double extra = 0.0;  // confidence, correlation or grain kind
    };
    void setEventLog(std::vector<Event>* log) { eventLog = log; }

    // Pitch ratio currently applied to a voice (after smoothing).
    double getCurrentRatio(int voice) const { return voices[(size_t) voice].currentRatio; }

    // Both outputs must hold at least as many channels and samples as input.
    // voice 0 renders into mainOutput, voice 1 into secondOutput. When given,
    // trackedPeriod receives, per sample, the input period being tracked (in
    // samples, analysis time) or 0 when no pitch is locked.
    void process(const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& mainOutput,
                 juce::AudioBuffer<float>& secondOutput, float* trackedPeriod = nullptr)
    {
        channels = juce::jmin(2, input.getNumChannels(), mainOutput.getNumChannels(),
                              secondOutput.getNumChannels());
        const int samples = input.getNumSamples();
        for (int i = 0; i < samples; ++i)
        {
            float mono = 0.0f;
            const auto ringIndex = (size_t) (position & ringMask);
            for (int ch = 0; ch < channels; ++ch)
            {
                const float value = input.getSample(ch, i);
                inputRing[(size_t) ch][ringIndex] = value;
                mono += value;
            }
            mono /= (float) juce::jmax(1, channels);

            analyse(mono);
            if (trackedPeriod != nullptr)
                trackedPeriod[i] = voiced && markCount > 0 ? (float) mark(0).period : 0.0f;
            renderVoice(voices[0], mainOutput, i);
            renderVoice(voices[1], secondOutput, i);
            ++position;
        }
    }

private:
    static constexpr int ringSize = 65536;
    static constexpr int64_t ringMask = ringSize - 1;
    static constexpr int markCapacity = 64;
    static constexpr int maxGrains = 32;
    static constexpr int windowTableSize = 1025;
    static constexpr int maxPredictedPeriods = 4;
    // Hermite interpolation reads two samples past the read position.
    static constexpr double readMargin = 3.0;
    // Protects joins where grains overlap less than 50 %.
    static constexpr float weightFloor = 0.35f;
    // Phase alignment leaves this fraction of a period as slack so small
    // period fluctuations do not flip the choice back by a whole period.
    static constexpr double alignmentSlack = 0.15;
    // Below ~0.7 the overlap between neighbouring grains drops under the
    // weight floor and the level would dip between grains.
    static constexpr double tightGrainLengthFactor = 0.7;
    // Robust tracking. Estimates arrive every ~2.5 ms.
    static constexpr int harmonicJumpConfirmEstimates = 6;  // ~15 ms
    static constexpr int dropoutHoldEstimates = 8;          // ~20 ms
    static constexpr double healthyMarkScore = 0.7;
    static constexpr double holdMarkScore = 0.6;

    enum class GrainKind { Pitched = 0, Granular = 1 };

    struct Grain
    {
        double centre = 0.0;
        double halfWidth = 1.0;
        double inverseHalfWidth = 1.0;
        // Rising half, shorter than halfWidth when the grain started late.
        double inverseRiseHalfWidth = 1.0;
        // Early fade-out when a new note restarts the grain train.
        double fadeEnd = std::numeric_limits<double>::max();
        double source = 0.0;
        double rate = 1.0;
    };

    struct GrainPlan
    {
        GrainKind kind = GrainKind::Granular;
        double halfWidth = 1.0;
        double spacing = 1.0;
        double rate = 1.0;
    };

    struct Voice
    {
        std::array<Grain, maxGrains> grains {};
        std::array<int64_t, 2> grainKindSamples {};
        int grainCount = 0;
        double nextCentre = 0.0;
        double currentRatio = 1.0;
        double targetRatio = 1.0;
        double lagEstimate = readMargin;
        // Source phase used for this note, as a fraction of a period after
        // each pitch mark. Chosen once per mark sequence (latticeGeneration).
        int64_t latticeGeneration = -1;
        double latticeFraction = 0.0;
    };

    struct Mark
    {
        double position = 0.0;
        double period = 0.0;
    };

    struct Biquad
    {
        void design(double fs, double frequency, double q)
        {
            const double w0 = juce::MathConstants<double>::twoPi * frequency / fs;
            const double alpha = std::sin(w0) / (2.0 * q);
            const double cosine = std::cos(w0);
            const double a0 = 1.0 + alpha;
            b0 = (float) ((1.0 - cosine) * 0.5 / a0);
            b1 = (float) ((1.0 - cosine) / a0);
            b2 = b0;
            a1 = (float) (-2.0 * cosine / a0);
            a2 = (float) ((1.0 - alpha) / a0);
        }
        void reset() { z1 = z2 = 0.0f; }
        float process(float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
    };

    static double ratioFromSemitones(float semitones)
    {
        return juce::jlimit(0.25, 4.0, std::pow(2.0, (double) semitones / 12.0));
    }

    //==============================================================================
    // Analysis

    void analyse(float mono)
    {
        // DC offset would bias the correlations.
        float filtered = dcBlockerCoefficient * (dcBlockerState + mono - dcBlockerInput);
        dcBlockerInput = mono;
        dcBlockerState = filtered;
        for (auto& stage : lowPassStages)
            filtered = stage.process(filtered);
        analysisRing[(size_t) (position & ringMask)] = filtered;

        if (detector.push(filtered))
            updateVoicing();
        updateMarks();
    }

    // An octave (or 2x/3x/4x) jump in the estimate while the current pitch
    // marks still correlate well is almost always a detector error on low
    // strings: keep tracking the current period unless the jump persists.
    static bool isHarmonicJump(double ratio)
    {
        for (int k = 2; k <= 4; ++k)
            if (std::abs(ratio / k - 1.0) < 0.06 || std::abs(ratio * k - 1.0) < 0.06)
                return true;
        return false;
    }

    bool marksAreHealthy(double minimumScore) const
    {
        return markCount >= 3 && lastMarkScore >= minimumScore;
    }

    void updateVoicing()
    {
        const auto& estimate = detector.getEstimate();
        logEvent(EventType::Estimate, estimate.voiced ? estimate.period : 0.0, estimate.confidence);
        if (estimate.voiced)
        {
            ++voicedRun;
            unvoicedRun = 0;
            double period = estimate.period;
            if (robustTracking && marksAreHealthy(healthyMarkScore)
                && isHarmonicJump(period / mark(0).period))
            {
                if (++harmonicJumpRun < harmonicJumpConfirmEstimates)
                    period = mark(0).period;
            }
            else
                harmonicJumpRun = 0;
            detectedPeriod = period;
            if (voicedRun >= lockEstimates && !voiced)
            {
                voiced = true;
                logEvent(EventType::VoicedOn, detectedPeriod);
            }
        }
        else
        {
            ++unvoicedRun;
            voicedRun = 0;
            harmonicJumpRun = 0;
            // Brief detector dropouts inside a sustained note: keep the marks
            // while they still correlate instead of falling back to granular.
            const bool holding = robustTracking && unvoicedRun < dropoutHoldEstimates
                              && marksAreHealthy(holdMarkScore);
            if (unvoicedRun >= 2 && !holding)
            {
                if (voiced)
                    logEvent(EventType::VoicedOff, detectedPeriod);
                voiced = false;
                clearMarks();
            }
        }
    }

    void updateMarks()
    {
        if (!voiced || detectedPeriod <= 0.0)
        {
            if (markCount > 0)
                clearMarks();
            return;
        }

        const double period = detectedPeriod;
        if (markCount > 0)
        {
            const double change = period / mark(0).period;
            if (change > 1.25 || change < 0.8)
            {
                logEvent(EventType::ResetJump, change);
                clearMarks();
            }
        }

        if (markCount == 0)
        {
            // Anchor the first mark on the largest positive peak of the last
            // complete period, avoiding the still-rising most recent samples.
            const int64_t end = position - (int64_t) (0.25 * period);
            const int64_t start = position - (int64_t) (1.25 * period);
            if (start < 0)
                return;
            int64_t best = start;
            for (int64_t i = start + 1; i <= end; ++i)
                if (analysisAt(i) > analysisAt(best))
                    best = i;
            ++markGeneration;
            logEvent(EventType::NewNote, period);
            if (restartOnNewNote)
                restartGrainTrains();
            addMark((double) best, period);
            return;
        }

        for (int guard = 0; guard < 4; ++guard)
        {
            const Mark last = mark(0);
            const int length = (int) juce::jmin(period, 1024.0);
            const double lowest = 0.8 * period;
            const double highest = 1.2 * period;
            if ((double) position < last.position + highest + length / 4 + 1)
                break;

            const int64_t base = (int64_t) std::llround(last.position);
            const int stride = length > 256 ? 2 : 1;
            const int step = period > 300.0 ? 2 : 1;
            const int firstLag = (int) std::ceil(lowest);
            const int lastLag = (int) std::floor(highest);
            int bestLag = firstLag;
            double bestScore = -2.0;
            for (int lag = firstLag; lag <= lastLag; lag += step)
            {
                const double score = markCorrelation(base, lag, length, stride);
                if (score > bestScore)
                {
                    bestScore = score;
                    bestLag = lag;
                }
            }
            if (step > 1)
                for (const int lag : { bestLag - 1, bestLag + 1 })
                {
                    const double score = markCorrelation(base, lag, length, stride);
                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestLag = lag;
                    }
                }

            if (bestScore < 0.3)
            {
                logEvent(EventType::ResetCorrelation, period, bestScore);
                clearMarks();
                break;
            }

            const double left = markCorrelation(base, bestLag - 1, length, stride);
            const double right = markCorrelation(base, bestLag + 1, length, stride);
            const double denominator = left - 2.0 * bestScore + right;
            const double offset = std::abs(denominator) > 1.0e-12
                ? juce::jlimit(-0.5, 0.5, 0.5 * (left - right) / denominator) : 0.0;
            const double measured = (double) bestLag + offset;
            const double markPeriod = std::abs(measured / period - 1.0) < 0.2
                ? 0.7 * measured + 0.3 * last.period : period;
            lastMarkScore = bestScore;
            logEvent(EventType::Mark, markPeriod, measured);
            addMark((double) base + measured, markPeriod);
        }
    }

    // A new note is locked: start its first grain now instead of waiting for
    // the previous pulse train (up to one output period, ~26 ms for a low
    // note at -12). Running grains fade out over restartCrossfade.
    void restartGrainTrains()
    {
        const double now = (double) position;
        for (auto& voice : voices)
        {
            for (int k = 0; k < voice.grainCount; ++k)
                voice.grains[(size_t) k].fadeEnd = juce::jmin(voice.grains[(size_t) k].fadeEnd,
                                                              now + restartCrossfade);
            voice.nextCentre = juce::jmin(voice.nextCentre, now + minimumRise);
        }
    }

    // Normalised correlation between the period ending near a mark and the
    // candidate period `lag` samples later.
    double markCorrelation(int64_t base, int lag, int length, int stride) const
    {
        const int64_t start = base - (3 * length) / 4;
        double cross = 0.0, energyA = 0.0, energyB = 0.0;
        for (int k = 0; k < length; k += stride)
        {
            const float a = analysisAt(start + k);
            const float b = analysisAt(start + lag + k);
            cross += (double) a * b;
            energyA += (double) a * a;
            energyB += (double) b * b;
        }
        const double norm = std::sqrt(energyA * energyB);
        return norm > 1.0e-15 ? cross / norm : 0.0;
    }

    void clearMarks()
    {
        lastMarkScore = 0.0;
        markCount = 0;
        markWrite = 0;
    }

    void addMark(double markPosition, double period)
    {
        marks[(size_t) markWrite] = { markPosition, period };
        markWrite = (markWrite + 1) % markCapacity;
        markCount = juce::jmin(markCount + 1, markCapacity);
    }

    // index 0 is the newest mark.
    const Mark& mark(int index) const
    {
        return marks[(size_t) ((markWrite - 1 - index + 2 * markCapacity) % markCapacity)];
    }

    void logEvent(EventType type, double value, double extra = 0.0)
    {
        if (eventLog != nullptr)
            eventLog->push_back({ position, type, value, extra });
    }

    float analysisAt(int64_t index) const
    {
        if (index < 0 || index > position || index <= position - ringSize)
            return 0.0f;
        return analysisRing[(size_t) (index & ringMask)];
    }

    float inputAt(int channel, int64_t index) const
    {
        if (index < 0 || index > position || index <= position - ringSize)
            return 0.0f;
        return inputRing[(size_t) channel][(size_t) (index & ringMask)];
    }

    float readInput(int channel, double readPosition) const
    {
        const double floorPosition = std::floor(readPosition);
        const auto index = (int64_t) floorPosition;
        const float fraction = (float) (readPosition - floorPosition);
        const float xm1 = inputAt(channel, index - 1);
        const float x0 = inputAt(channel, index);
        const float x1 = inputAt(channel, index + 1);
        const float x2 = inputAt(channel, index + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * fraction + c2) * fraction + c1) * fraction + x0;
    }

    //==============================================================================
    // Synthesis

    GrainPlan planNextGrain(const Voice& voice) const
    {
        const double ratio = voice.currentRatio;
        if (voiced && markCount > 0)
        {
            const double outputPeriod = mark(0).period / ratio;
            return { GrainKind::Pitched, grainLengthFactor * outputPeriod, outputPeriod, ratio };
        }
        return { GrainKind::Granular, granularHalfWidth, granularHalfWidth, ratio };
    }

    // Latest phase-coherent source position at or before `target`: a point at
    // latticeFraction of a period after a (possibly extrapolated) pitch mark.
    bool selectSource(Voice& voice, double target, double& source) const
    {
        int reference = 0;
        while (reference < markCount && mark(reference).position > target)
            ++reference;
        if (reference == markCount)
            return false;
        const Mark& anchor = mark(reference);

        if (voice.latticeGeneration != markGeneration)
        {
            // New note: choose the phase that puts a source point just
            // before the target, which removes the slack.
            voice.latticeGeneration = markGeneration;
            double periods = (target - anchor.position) / anchor.period;
            if (phaseAlignment)
                periods = juce::jmax(0.0, periods - alignmentSlack);
            else
                periods = std::floor(periods);
            periods = juce::jmin(periods, (double) maxPredictedPeriods);
            voice.latticeFraction = periods - std::floor(periods);
            source = anchor.position + periods * anchor.period;
            return true;
        }

        const double fraction = voice.latticeFraction;
        for (int i = 0; i < markCount; ++i)
        {
            const Mark& candidate = mark(i);
            const double base = candidate.position + fraction * candidate.period;
            if (base > target)
                continue;
            const double periods = i == 0
                ? juce::jmin((double) maxPredictedPeriods, std::floor((target - base) / candidate.period))
                : 0.0;
            source = base + periods * candidate.period;
            return true;
        }
        return false;
    }

    Grain buildGrain(Voice& voice, const GrainPlan& plan)
    {
        Grain grain;
        grain.centre = voice.nextCentre;
        grain.halfWidth = plan.halfWidth;
        grain.inverseHalfWidth = 1.0 / juce::jmax(1.0, plan.halfWidth);
        grain.inverseRiseHalfWidth = grain.inverseHalfWidth;
        grain.rate = plan.rate;

        // Lazy reading stays behind the write position if the grain centre
        // trails its source by at least this many samples.
        const double minimumLag = plan.halfWidth * std::abs(1.0 - plan.rate) + readMargin;
        const double safeLag = juce::jmax(voice.lagEstimate, minimumLag);

        double source = 0.0;
        if (plan.kind == GrainKind::Pitched && selectSource(voice, grain.centre - minimumLag, source))
        {
            grain.source = source;
            voice.lagEstimate += 0.1 * ((grain.centre - source) - voice.lagEstimate);
        }
        else
        {
            grain.source = grain.centre - safeLag;
            // Let the offset settle towards the minimum so noise and chords
            // do not keep the delay of the last pitched note.
            voice.lagEstimate = safeLag + 0.02 * (minimumLag - safeLag);
        }
        return grain;
    }

    void scheduleGrains(Voice& voice)
    {
        const double now = (double) position;
        if (voice.nextCentre < now - 2.0 * granularHalfWidth)
            voice.nextCentre = now;

        for (int guard = 0; guard < 8; ++guard)
        {
            const GrainPlan plan = planNextGrain(voice);
            if (voice.nextCentre - plan.halfWidth > now)
                break;

            // When the grain size jumps up (granular -> low pitched note, or
            // a restart), the next grain's start is already in the past: it
            // would begin mid-window, at non-zero gain, and click. Keep its
            // centre but give it a shorter rising half that starts from zero
            // now (at least minimumRise long).
            const bool late = voice.nextCentre - plan.halfWidth < now - 1.0;
            if (late)
                voice.nextCentre = juce::jmax(voice.nextCentre, now + minimumRise);

            Grain grain = buildGrain(voice, plan);
            if (late)
                grain.inverseRiseHalfWidth = 1.0 / juce::jmax(1.0, juce::jmin(grain.halfWidth,
                                                                             grain.centre - now));
            if (&voice == &voices[0])
                logEvent(EventType::Grain, grain.centre - grain.source, (double) plan.kind);
            if (voice.grainCount < maxGrains)
                voice.grains[(size_t) voice.grainCount++] = grain;
            const double spacing = juce::jmax(2.0, plan.spacing);
            voice.grainKindSamples[(size_t) plan.kind] += (int64_t) spacing;
            voice.nextCentre += spacing;
        }
    }

    void renderVoice(Voice& voice, juce::AudioBuffer<float>& output, int sampleIndex)
    {
        voice.currentRatio += (voice.targetRatio - voice.currentRatio) * ratioSmoothingCoefficient;
        scheduleGrains(voice);

        const double now = (double) position;
        std::array<float, 2> accumulated {};
        float weight = 0.0f;
        for (int k = 0; k < voice.grainCount;)
        {
            Grain& grain = voice.grains[(size_t) k];
            const double offset = now - grain.centre;
            if (offset > grain.halfWidth || now >= grain.fadeEnd)
            {
                grain = voice.grains[(size_t) --voice.grainCount];
                continue;
            }

            const double normalised = juce::jlimit(0.0, 1.0, offset < 0.0
                ? -offset * grain.inverseRiseHalfWidth : offset * grain.inverseHalfWidth);
            float window = windowTable[(size_t) (normalised * (windowTableSize - 1) + 0.5)];
            if (grain.fadeEnd - now < restartCrossfade)
                window *= (float) ((grain.fadeEnd - now) / restartCrossfade);
            const double source = juce::jmin(grain.source + offset * grain.rate, now - readMargin);
            for (int ch = 0; ch < channels; ++ch)
                accumulated[(size_t) ch] += window * readInput(ch, source);
            weight += window;
            ++k;
        }

        const float normaliser = 1.0f / juce::jmax(weight, weightFloor);
        for (int ch = 0; ch < channels; ++ch)
            output.setSample(ch, sampleIndex, accumulated[(size_t) ch] * normaliser);
    }

    std::array<std::array<float, ringSize>, 2> inputRing {};
    std::array<float, ringSize> analysisRing {};
    std::array<float, windowTableSize> windowTable {};
    std::array<Mark, markCapacity> marks {};
    std::array<Voice, numVoices> voices {};
    std::array<Biquad, 2> lowPassStages {};
    PsolaPitchDetector detector;
    std::vector<Event>* eventLog = nullptr;
    double sampleRate = 48000.0;
    double ratioSmoothingCoefficient = 0.001;
    double granularHalfWidth = 480.0;
    double minimumRise = 96.0;
    double restartCrossfade = 96.0;
    double detectedPeriod = 0.0;
    double lastMarkScore = 0.0;
    double grainLengthFactor = 1.0;
    int64_t position = 0;
    int64_t markGeneration = 0;
    int channels = 2;
    int markWrite = 0;
    int markCount = 0;
    int voicedRun = 0;
    int unvoicedRun = 0;
    int harmonicJumpRun = 0;
    int lockEstimates = 1;
    float dcBlockerCoefficient = 0.997f;
    float dcBlockerState = 0.0f;
    float dcBlockerInput = 0.0f;
    bool voiced = false;
    bool robustTracking = true;
    bool restartOnNewNote = true;
    bool phaseAlignment = true;
};
