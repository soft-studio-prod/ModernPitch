// ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PluginProcessor.h"
#include "PluginEditor.h"

ModernPitchAudioProcessor::ModernPitchAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS",
      {
          std::make_unique<juce::AudioParameterBool>("tuneEnabled", "Tune Enabled", true),
          std::make_unique<juce::AudioParameterBool>("octaveEnabled", "Octaver Enabled", false),
          std::make_unique<juce::AudioParameterChoice>("octaveMode", "Octaver Mode",
                                                        juce::StringArray { "Octave", "Ring" }, 0),
          std::make_unique<juce::AudioParameterBool>("whammyEnabled", "Whammy Enabled", false),

          std::make_unique<juce::AudioParameterFloat>(
              "pitch", "Pitch",
              juce::NormalisableRange<float>(-12.0f, 12.0f, 1.0f),
              0.0f),

          std::make_unique<juce::AudioParameterFloat>(
              "whammyPitch", "Whammy Pitch",
              juce::NormalisableRange<float>(0.0f, 24.0f, 0.001f),
              0.0f),

          std::make_unique<juce::AudioParameterFloat>(
              "octaveMix", "Octave Mix",
              juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f),
              0.0f),

          std::make_unique<juce::AudioParameterFloat>(
              "grit", "Grit",
              juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f),
              0.0f),

          std::make_unique<juce::AudioParameterBool>("eqCompEnabled", "EQ Compensation", false),

          std::make_unique<juce::AudioParameterFloat>(
              "octaveLowCut", "Octave Low Cut",
              juce::NormalisableRange<float>(minimumLowCutHz, 400.0f, 1.0f, 0.4f),
              minimumLowCutHz)

      })
{
}

void ModernPitchAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const float initialTunePitch = parameters.getRawParameterValue("tuneEnabled")->load() > 0.5f
        ? parameters.getRawParameterValue("pitch")->load() : 0.0f;
    const float initialWhammyPitch = parameters.getRawParameterValue("whammyEnabled")->load() > 0.5f
        ? parameters.getRawParameterValue("whammyPitch")->load() : 0.0f;
    const float initialCombinedPitch = initialTunePitch + initialWhammyPitch;
    psolaEngine.setImprovedTracking(useImprovedTracking);
    psolaEngine.setRestartOnNewNote(useNoteRestart);
    psolaEngine.setTightGrains(useTightGrains);
    psolaEngine.prepare(sampleRate);
    psolaEngine.setPitchSemitones(0, initialCombinedPitch);
    psolaEngine.setPitchSemitones(1, initialCombinedPitch - 12.0f);

    const int bufferSize = juce::jmax(1, samplesPerBlock);
    sourceBuffer.setSize(2, bufferSize, false, false, true);
    tuneShiftedBuffer.setSize(2, bufferSize, false, false, true);
    octaveShiftedBuffer.setSize(2, bufferSize, false, false, true);
    ringModulator.prepare(sampleRate);
    trackedPeriods.assign((size_t) juce::jmax(bufferSize, 4096), 0.0f);
    octaveMixSmoother.reset(sampleRate, 0.020);
    octaveMixSmoother.setCurrentAndTargetValue(
        parameters.getRawParameterValue("octaveMix")->load() * 0.01f);
    gritSmoother.reset(sampleRate, 0.020);
    gritSmoother.setCurrentAndTargetValue(
        parameters.getRawParameterValue("grit")->load() * 0.01f);
    compensationPitchSmoother.reset(sampleRate, 0.020);
    compensationPitchSmoother.setCurrentAndTargetValue(initialCombinedPitch);
    compensationAmountSmoother.reset(sampleRate, 0.050);
    compensationAmountSmoother.setCurrentAndTargetValue(
        parameters.getRawParameterValue("eqCompEnabled")->load() > 0.5f ? 1.0f : 0.0f);
    lowCutSmoother.reset(sampleRate, 0.050);
    lowCutSmoother.setCurrentAndTargetValue(parameters.getRawParameterValue("octaveLowCut")->load());
    compensationLowShelf.reset();
    compensationHighShelf.reset();
    octaveLowCut.reset();
    updateToneFilters();

    // The delay follows the played note, so no fixed latency is reported.
    setLatencySamples(0);
}

void ModernPitchAudioProcessor::releaseResources()
{
    psolaEngine.reset();
}

bool ModernPitchAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& input = layouts.getMainInputChannelSet();
    const auto& output = layouts.getMainOutputChannelSet();

    if (input != output)
        return false;

    return input == juce::AudioChannelSet::mono()
        || input == juce::AudioChannelSet::stereo();
}

void ModernPitchAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                             juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midiMessages);

    const float pitch = parameters.getRawParameterValue("pitch")->load();
    const float octaveMixTarget = parameters.getRawParameterValue("octaveMix")->load() * 0.01f;
    const float gritTarget = parameters.getRawParameterValue("grit")->load() * 0.01f;
    const int octaveMode = (int) parameters.getRawParameterValue("octaveMode")->load();
    const bool whammyEnabled = parameters.getRawParameterValue("whammyEnabled")->load() > 0.5f;
    const float whammyPitch = parameters.getRawParameterValue("whammyPitch")->load();
    const bool tuneEnabled = parameters.getRawParameterValue("tuneEnabled")->load() > 0.5f;
    const bool octaveEnabled = parameters.getRawParameterValue("octaveEnabled")->load() > 0.5f;
    const float effectivePitch = tuneEnabled ? pitch : 0.0f;
    octaveMixSmoother.setTargetValue(octaveEnabled ? octaveMixTarget : 0.0f);
    gritSmoother.setTargetValue(gritTarget);
    const int channels = juce::jmin(buffer.getNumChannels(), 2);
    const int numSamples = buffer.getNumSamples();

    sourceBuffer.setSize(channels, numSamples, false, false, true);
    tuneShiftedBuffer.setSize(channels, numSamples, false, false, true);
    octaveShiftedBuffer.setSize(channels, numSamples, false, false, true);
    for (int ch = 0; ch < channels; ++ch)
        sourceBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    const bool whammyActive = whammyEnabled && whammyPitch > 0.0001f;
    const float combinedPitch = effectivePitch + (whammyActive ? whammyPitch : 0.0f);
    // Keep the shifted path engaged when Tune and Whammy cancel each other;
    // bypassing exactly at net unison would jump between the delayed output
    // and the live input during automation.
    const bool pitchActive = std::abs(combinedPitch) >= 0.0001f
                          || (whammyActive && std::abs(effectivePitch) >= 0.0001f);

    // Tune/Whammy and the Octaver (combined pitch minus 12 st) are two PSOLA
    // voices sharing one analysis. Their delay follows the played note rather
    // than a fixed window, so no alignment delay is applied between them.
    psolaEngine.setPitchSemitonesSmoothed(0, combinedPitch);
    psolaEngine.setPitchSemitonesSmoothed(1, combinedPitch - 12.0f);
    if ((int) trackedPeriods.size() < numSamples)
        trackedPeriods.resize((size_t) numSamples); // host exceeded the announced block size
    psolaEngine.process(sourceBuffer, tuneShiftedBuffer, octaveShiftedBuffer, trackedPeriods.data());
    // The ring modulates the main path: shifted (delayed by the engine lag)
    // when a shift is active, the dry input otherwise.
    const double ringLag = pitchActive ? psolaEngine.getLagSamples(0) : 0.0;
    const double ringRatio = pitchActive ? psolaEngine.getCurrentRatio(0) : 1.0;
    compensationPitchSmoother.setTargetValue(pitchActive ? combinedPitch : 0.0f);
    compensationAmountSmoother.setTargetValue(
        parameters.getRawParameterValue("eqCompEnabled")->load() > 0.5f ? 1.0f : 0.0f);
    lowCutSmoother.setTargetValue(parameters.getRawParameterValue("octaveLowCut")->load());
    tuneLagMs.store((float) (1000.0 * psolaEngine.getLagSamples(0) / getSampleRate()));
    octaveLagMs.store((float) (1000.0 * psolaEngine.getLagSamples(1) / getSampleRate()));

    for (int sample = 0; sample < numSamples; ++sample)
    {
        if (sample % toneUpdateInterval == 0)
            updateToneFilters();
        compensationPitchSmoother.skip(1);
        compensationAmountSmoother.skip(1);
        lowCutSmoother.skip(1);
        // The compensation shelves always run on the shifted signal so their
        // state stays continuous; at 0 st or when EQ COMP is off they are flat.
        for (int ch = 0; ch < channels; ++ch)
        {
            const float shifted = compensationHighShelf.process(
                ch, compensationLowShelf.process(ch, tuneShiftedBuffer.getSample(ch, sample)));
            if (pitchActive)
                buffer.setSample(ch, sample, shifted);
        }

        const float sampleOctaveMix = octaveMixSmoother.getNextValue();
        const float sampleGrit = gritSmoother.getNextValue();
        const float gritDrive = 1.0f + 49.0f * sampleGrit;
        const float gritCompensation = std::pow(gritDrive, 0.35f);
        // Advance the carrier every sample so its pitch history stays aligned.
        const float carrier = ringModulator.next(trackedPeriods[(size_t) sample], ringLag, ringRatio);

        if (sampleOctaveMix <= 0.0f)
            continue;
        for (int ch = 0; ch < channels; ++ch)
        {
            // OCTAVE: the voice one octave down. RING: the main signal times a
            // sine at half its pitch (odd harmonics of the octave below).
            float octave = octaveMode == 1 ? buffer.getSample(ch, sample) * carrier
                                           : octaveShiftedBuffer.getSample(ch, sample);
            if (sampleGrit > 0.0001f)
            {
                const float saturated = std::tanh(octave * gritDrive) / gritCompensation;
                octave = octave * (1.0f - sampleGrit) + saturated * sampleGrit;
            }
            if (lowCutActive)
                octave = octaveLowCut.process(ch, octave);
            buffer.setSample(ch, sample, buffer.getSample(ch, sample) * (1.0f - sampleOctaveMix)
                                           + octave * sampleOctaveMix);
        }
    }
}

void ModernPitchAudioProcessor::updateToneFilters()
{
    const double sampleRate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    // Shifting moves the guitar's energy: down gives more lows and an empty
    // top, up the opposite. Counter-tilt with two shelves that follow the
    // pitch, 0.5 dB per semitone each at 100 %.
    const double shelfGain = juce::jlimit(-12.0, 12.0, 0.5 * compensationAmountSmoother.getCurrentValue()
                                                       * compensationPitchSmoother.getCurrentValue());
    compensationLowShelf.setLowShelf(sampleRate, 200.0, shelfGain);
    compensationHighShelf.setHighShelf(sampleRate, 2500.0, -shelfGain);

    const float cutoff = lowCutSmoother.getCurrentValue();
    lowCutActive = cutoff > minimumLowCutHz + 0.5f;
    octaveLowCut.setHighPass(sampleRate, cutoff);
}

void ModernPitchAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void ModernPitchAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* ModernPitchAudioProcessor::createEditor()
{
    return new ModernPitchAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ModernPitchAudioProcessor();
}
