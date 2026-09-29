// ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PluginEditor.h"

ModernPitchAudioProcessorEditor::ModernPitchAudioProcessorEditor(ModernPitchAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setSize(420, 752);

    title.setText("MODERN PITCH", juce::dontSendNotification);
    title.setJustificationType(juce::Justification::centred);
    title.setFont(juce::Font(22.0f, juce::Font::bold));
    addAndMakeVisible(title);

    studioLabel.setText("Soft Studio", juce::dontSendNotification);
    studioLabel.setJustificationType(juce::Justification::centredLeft);
    versionLabel.setText(juce::String::fromUTF8("v" MODERNPITCH_VERSION "  Â·  AGPLv3"),
                         juce::dontSendNotification);
    versionLabel.setJustificationType(juce::Justification::centredRight);
    versionLabel.setTooltip("About, licence and source code");
    versionLabel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    versionLabel.addMouseListener(this, false);
    for (auto* footerLabel : { &studioLabel, &versionLabel })
    {
        footerLabel->setFont(juce::Font(11.0f));
        footerLabel->setColour(juce::Label::textColourId, juce::Colour(0xff7d8791));
        addAndMakeVisible(*footerLabel);
    }

    tuneEnabledButton.setButtonText("ACTIVE");
    tuneEnabledButton.setTooltip("Enable or bypass the Tune section");
    addAndMakeVisible(tuneEnabledButton);
    octaveEnabledButton.setButtonText("ACTIVE");
    octaveEnabledButton.setTooltip("Enable or bypass the Octaver section");
    addAndMakeVisible(octaveEnabledButton);
    whammyEnabledButton.setButtonText("ACTIVE");
    whammyEnabledButton.setTooltip("Enable or bypass the Whammy section");
    addAndMakeVisible(whammyEnabledButton);
    eqCompButton.setButtonText("EQ COMP");
    eqCompButton.setTooltip("Counters the tonal shift of pitching: cuts lows / adds highs when going "
                            "down, the opposite when going up");
    addAndMakeVisible(eqCompButton);
    octaveModeBox.addItem("OCTAVE", 1);
    octaveModeBox.addItem("RING", 2);
    addAndMakeVisible(octaveModeBox);
    for (auto* lagLabel : { &tuneLagLabel, &octaveLagLabel })
    {
        lagLabel->setJustificationType(juce::Justification::centred);
        lagLabel->setFont(juce::Font(12.0f));
        lagLabel->setColour(juce::Label::textColourId, juce::Colour(0xff9aa4ae));
        lagLabel->setTooltip("Current pitch-shift delay; varies with the played note");
        addAndMakeVisible(*lagLabel);
    }

    whammyPitchSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    whammyPitchSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 100, 24);
    whammyPitchSlider.setRange(0.0, 24.0, 0.001);
    whammyPitchSlider.setNumDecimalPlacesToDisplay(2);
    whammyPitchSlider.setTextValueSuffix(" st");
    addAndMakeVisible(whammyPitchSlider);

    pitchSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    pitchSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 90, 24);
    pitchSlider.setRange(-12.0, 12.0, 1.0);
    pitchSlider.setNumDecimalPlacesToDisplay(0);
    addAndMakeVisible(pitchSlider);

    octaveMixSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    octaveMixSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 90, 24);
    octaveMixSlider.setRange(0.0, 100.0, 1.0);
    octaveMixSlider.setNumDecimalPlacesToDisplay(0);
    octaveMixSlider.setTextValueSuffix(" %");
    addAndMakeVisible(octaveMixSlider);

    gritSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    gritSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 90, 24);
    gritSlider.setRange(0.0, 100.0, 1.0);
    gritSlider.setNumDecimalPlacesToDisplay(0);
    gritSlider.setTextValueSuffix(" %");
    addAndMakeVisible(gritSlider);

    lowCutSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    lowCutSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 90, 24);
    lowCutSlider.textFromValueFunction = [](double value)
    {
        return value <= ModernPitchAudioProcessor::minimumLowCutHz + 0.5
            ? juce::String("OFF") : juce::String(juce::roundToInt(value)) + " Hz";
    };
    lowCutSlider.valueFromTextFunction = [](const juce::String& text)
    {
        return text.trim().equalsIgnoreCase("off") ? (double) ModernPitchAudioProcessor::minimumLowCutHz
                                                   : text.getDoubleValue();
    };
    lowCutSlider.setTooltip("12 dB/oct high-pass on the octave signal, after Ring and Grit");
    addAndMakeVisible(lowCutSlider);
    lowCutLabel.setText("LOW CUT", juce::dontSendNotification);
    lowCutLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(lowCutLabel);

    pitchLabel.setText("PITCH", juce::dontSendNotification);
    pitchLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(pitchLabel);

    tuneTitle.setText("TUNE", juce::dontSendNotification);
    tuneTitle.setJustificationType(juce::Justification::centredLeft);
    tuneTitle.setFont(juce::Font(18.0f, juce::Font::bold));
    addAndMakeVisible(tuneTitle);

    octaveTitle.setText("OCTAVER  -12 st", juce::dontSendNotification);
    octaveTitle.setJustificationType(juce::Justification::centredLeft);
    octaveTitle.setFont(juce::Font(18.0f, juce::Font::bold));
    addAndMakeVisible(octaveTitle);

    octaveMixLabel.setText("MIX", juce::dontSendNotification);
    octaveMixLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(octaveMixLabel);

    gritLabel.setText("GRIT", juce::dontSendNotification);
    gritLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(gritLabel);

    whammyTitle.setText("WHAMMY", juce::dontSendNotification);
    whammyTitle.setJustificationType(juce::Justification::centredLeft);
    whammyTitle.setFont(juce::Font(18.0f, juce::Font::bold));
    addAndMakeVisible(whammyTitle);
    whammyPitchLabel.setText("PITCH", juce::dontSendNotification);
    whammyPitchLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(whammyPitchLabel);

    pitchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "pitch", pitchSlider);

    octaveMixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "octaveMix", octaveMixSlider);
    gritAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "grit", gritSlider);
    lowCutAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "octaveLowCut", lowCutSlider);
    whammyPitchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "whammyPitch", whammyPitchSlider);
    tuneEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "tuneEnabled", tuneEnabledButton);
    octaveEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "octaveEnabled", octaveEnabledButton);
    whammyEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "whammyEnabled", whammyEnabledButton);
    eqCompAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "eqCompEnabled", eqCompButton);
    octaveModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        processor.parameters, "octaveMode", octaveModeBox);
    addChildComponent(aboutPanel);
    startTimerHz(8);
}

void ModernPitchAudioProcessorEditor::mouseUp(const juce::MouseEvent& event)
{
    if (event.eventComponent == &versionLabel)
    {
        aboutPanel.setVisible(!aboutPanel.isVisible());
        aboutPanel.toFront(false);
    }
}

ModernPitchAudioProcessorEditor::AboutPanel::AboutPanel()
{
    licenceLink.setButtonText("GNU Affero General Public License v3 (gnu.org)");
    licenceLink.setURL(juce::URL("https://www.gnu.org/licenses/agpl-3.0.html"));
    addAndMakeVisible(licenceLink);
    const juce::String sourceUrl(MODERNPITCH_SOURCE_URL);
    sourceLink.setButtonText("Source code");
    sourceLink.setURL(juce::URL(sourceUrl));
    addChildComponent(sourceLink);
    sourceLink.setVisible(sourceUrl.isNotEmpty());
    for (auto* link : { &licenceLink, &sourceLink })
        link->setFont(juce::Font(13.0f, juce::Font::underlined), false);
}

void ModernPitchAudioProcessorEditor::AboutPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xf2080a0c));
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(20.0f, juce::Font::bold));
    auto area = textArea;
    g.drawText("MODERN PITCH  v" MODERNPITCH_VERSION, area.removeFromTop(28),
               juce::Justification::centredLeft);
    g.setFont(juce::Font(13.0f));
    g.setColour(juce::Colour(0xffc8ced4));
    g.drawText(juce::String::fromUTF8("Copyright (C) 2026 Micka\xc3\xabl Doux (Soft Studio)"),
               area.removeFromTop(22), juce::Justification::centredLeft);
    area.removeFromTop(10);
    g.drawFittedText("This program is free software: you can redistribute it and/or modify it under "
                     "the terms of the GNU Affero General Public License as published by the Free "
                     "Software Foundation, either version 3 of the License, or (at your option) any "
                     "later version.\n\n"
                     "This program is distributed in the hope that it will be useful, but WITHOUT "
                     "ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS "
                     "FOR A PARTICULAR PURPOSE. See the licence for details.",
                     area.removeFromTop(150), juce::Justification::topLeft, 12);
    area.removeFromTop(64);
    g.setFont(juce::Font(12.0f));
    g.setColour(juce::Colour(0xff8f98a1));
    g.drawFittedText(juce::String(MODERNPITCH_SOURCE_URL).isEmpty()
                         ? "The source code is published with the plugin, under the same licence.\n\n"
                         : "",
                     area.removeFromTop(40), juce::Justification::topLeft, 3);
    g.drawFittedText("Built with JUCE (AGPLv3).\n"
                     "VST is a registered trademark of Steinberg Media Technologies GmbH.\n\n"
                     "Click anywhere to close.",
                     area.removeFromTop(80), juce::Justification::topLeft, 5);
}

void ModernPitchAudioProcessorEditor::AboutPanel::resized()
{
    textArea = getLocalBounds().reduced(36, 60);
    auto links = textArea.withTrimmedTop(28 + 22 + 10 + 150).removeFromTop(64);
    licenceLink.setBounds(links.removeFromTop(24).withWidth(textArea.getWidth()));
    links.removeFromTop(6);
    sourceLink.setBounds(links.removeFromTop(24).withWidth(textArea.getWidth()));
    for (auto* link : { &licenceLink, &sourceLink })
        link->setJustificationType(juce::Justification::centredLeft);
}

void ModernPitchAudioProcessorEditor::timerCallback()
{
    tuneLagLabel.setText("lag ~" + juce::String(processor.getTuneLagMs(), 1) + " ms",
                         juce::dontSendNotification);
    octaveLagLabel.setText("lag ~" + juce::String(processor.getOctaveLagMs(), 1) + " ms",
                           juce::dontSendNotification);
}

void ModernPitchAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);

    g.setColour(juce::Colour(0xff111418));
    g.fillRoundedRectangle(tuneSectionBounds.toFloat(), 9.0f);
    g.fillRoundedRectangle(octaveSectionBounds.toFloat(), 9.0f);
    g.fillRoundedRectangle(whammySectionBounds.toFloat(), 9.0f);
    g.setColour(juce::Colour(0xff58616b));
    g.drawRoundedRectangle(tuneSectionBounds.toFloat(), 9.0f, 1.2f);
    g.drawRoundedRectangle(octaveSectionBounds.toFloat(), 9.0f, 1.2f);
    g.drawRoundedRectangle(whammySectionBounds.toFloat(), 9.0f, 1.2f);

    g.setColour(juce::Colours::white);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(8.0f), 8.0f, 1.0f);
}

void ModernPitchAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(20);
    auto footer = area.removeFromBottom(14).translated(0, 4).reduced(8, 0);
    studioLabel.setBounds(footer.removeFromLeft(footer.getWidth() / 2));
    versionLabel.setBounds(footer);
    aboutPanel.setBounds(getLocalBounds());
    title.setBounds(area.removeFromTop(42));
    area.removeFromTop(4);

    auto sections = area.reduced(8, 2);
    tuneSectionBounds = sections.removeFromTop(200);
    sections.removeFromTop(8);
    octaveSectionBounds = sections.removeFromTop(226);
    sections.removeFromTop(8);
    whammySectionBounds = sections;

    auto tune = tuneSectionBounds.reduced(12, 8);
    auto tuneHeader = tune.removeFromTop(30);
    tuneTitle.setBounds(tuneHeader.removeFromLeft(tuneHeader.getWidth() - 112));
    tuneEnabledButton.setBounds(tuneHeader.withSizeKeepingCentre(100, 24));
    tune.removeFromTop(3);
    tuneLagLabel.setBounds(tune.removeFromBottom(16));
    eqCompButton.setBounds(tune.removeFromBottom(27).withSizeKeepingCentre(110, 23));
    tune.removeFromBottom(2);
    pitchLabel.setBounds(tune.removeFromTop(22));
    pitchSlider.setBounds(tune);

    auto octave = octaveSectionBounds.reduced(12, 8);
    auto octaveHeader = octave.removeFromTop(30);
    octaveTitle.setBounds(octaveHeader.removeFromLeft(octaveHeader.getWidth() - 112));
    octaveEnabledButton.setBounds(octaveHeader.withSizeKeepingCentre(100, 24));
    octaveModeBox.setBounds(octave.removeFromTop(27).withSizeKeepingCentre(140, 24));
    octave.removeFromTop(2);
    octaveLagLabel.setBounds(octave.removeFromBottom(16));
    const int octaveColumn = octave.getWidth() / 3;
    auto octaveLeft = octave.removeFromLeft(octaveColumn);
    auto octaveMiddle = octave.removeFromLeft(octaveColumn);
    auto octaveRight = octave;
    octaveMixLabel.setBounds(octaveLeft.removeFromTop(22));
    octaveMixSlider.setBounds(octaveLeft);
    gritLabel.setBounds(octaveMiddle.removeFromTop(22));
    gritSlider.setBounds(octaveMiddle);
    lowCutLabel.setBounds(octaveRight.removeFromTop(22));
    lowCutSlider.setBounds(octaveRight);

    auto whammy = whammySectionBounds.reduced(12, 8);
    auto whammyHeader = whammy.removeFromTop(30);
    whammyTitle.setBounds(whammyHeader.removeFromLeft(whammyHeader.getWidth() - 112));
    whammyEnabledButton.setBounds(whammyHeader.withSizeKeepingCentre(100, 24));
    whammy.removeFromTop(2);
    whammyPitchLabel.setBounds(whammy.removeFromTop(22));
    whammyPitchSlider.setBounds(whammy.withSizeKeepingCentre(180, whammy.getHeight()));
}
