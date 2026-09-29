// ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class ModernPitchAudioProcessorEditor : public juce::AudioProcessorEditor,
                                        private juce::Timer
{
public:
    explicit ModernPitchAudioProcessorEditor(ModernPitchAudioProcessor&);
    ~ModernPitchAudioProcessorEditor() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void mouseUp(const juce::MouseEvent&) override;

    // Licence notices required by the AGPLv3 for interactive interfaces
    // (copyright, no warranty, how to get the licence and the source).
    class AboutPanel : public juce::Component
    {
    public:
        AboutPanel();
        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseUp(const juce::MouseEvent&) override { setVisible(false); }

    private:
        juce::HyperlinkButton licenceLink;
        juce::HyperlinkButton sourceLink;
        juce::Rectangle<int> textArea;
    };

    ModernPitchAudioProcessor& processor;

    juce::Slider pitchSlider;
    juce::Slider octaveMixSlider;
    juce::Slider gritSlider;
    juce::Slider lowCutSlider;
    juce::Slider whammyPitchSlider;
    juce::ToggleButton tuneEnabledButton;
    juce::ToggleButton octaveEnabledButton;
    juce::ToggleButton whammyEnabledButton;
    juce::ToggleButton eqCompButton;
    juce::ComboBox octaveModeBox;

    juce::Label title;
    juce::Label tuneTitle;
    juce::Label pitchLabel;
    juce::Label octaveTitle;
    juce::Label whammyTitle;
    juce::Label octaveMixLabel;
    juce::Label gritLabel;
    juce::Label lowCutLabel;
    juce::Label whammyPitchLabel;
    juce::Label tuneLagLabel;
    juce::Label octaveLagLabel;
    juce::Label studioLabel;
    juce::Label versionLabel;
    AboutPanel aboutPanel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> octaveMixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gritAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lowCutAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> whammyPitchAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tuneEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> octaveEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> whammyEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> eqCompAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> octaveModeAttachment;

    juce::Rectangle<int> tuneSectionBounds;
    juce::Rectangle<int> octaveSectionBounds;
    juce::Rectangle<int> whammySectionBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModernPitchAudioProcessorEditor)
};
