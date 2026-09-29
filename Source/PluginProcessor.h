// ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <vector>
#include "PsolaEngine.h"
#include "RingModulator.h"
#include "ToneFilters.h"

class ModernPitchAudioProcessor : public juce::AudioProcessor
{
public:
    ModernPitchAudioProcessor();
    ~ModernPitchAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ModernPitch"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState parameters;

    // Current pitch-shift delay at grain centres, in ms (varies with the note).
    float getTuneLagMs() const { return tuneLagMs.load(); }
    float getOctaveLagMs() const { return octaveLagMs.load(); }

    // The low-cut knob's minimum means "off".
    static constexpr float minimumLowCutHz = 20.0f;

    // Engine options kept as compile-time switches (see README, "Tuning
    // history"). The defaults are the settings validated by listening.
    static constexpr bool useImprovedTracking = true;  // false: YIN note tracking
    static constexpr bool useNoteRestart = true;       // false: no grain restart on a new note
    static constexpr bool useTightGrains = false;      // true: -1..3 ms delay, rougher attacks

private:
    void updateToneFilters();
    static constexpr int toneUpdateInterval = 32;

    juce::AudioBuffer<float> sourceBuffer;
    juce::AudioBuffer<float> tuneShiftedBuffer;
    juce::AudioBuffer<float> octaveShiftedBuffer;
    PsolaEngine psolaEngine;
    RingModulator ringModulator;
    std::vector<float> trackedPeriods;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> octaveMixSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gritSmoother;
    std::atomic<float> tuneLagMs { 0.0f };
    std::atomic<float> octaveLagMs { 0.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> compensationPitchSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> compensationAmountSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> lowCutSmoother;
    StereoBiquad compensationLowShelf;
    StereoBiquad compensationHighShelf;
    StereoBiquad octaveLowCut;
    bool lowCutActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModernPitchAudioProcessor)
};
