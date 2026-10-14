#pragma once

#include "dsp/MeterLook.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace youmix
{

inline juce::Colour meterColour(float dbFs)
{
    switch (meterColourIndex(dbFs))
    {
        case 2: return juce::Colour(0xffef5b6a);
        case 1: return juce::Colour(0xfff0c14a);
        default: return juce::Colour(0xff6ee0aa);
    }
}

// Left half is input level from the bottom. Right half is gain reduction from the top.
// Labels stay on the meter: left IN, right GATE/GR.
inline void paintSplitMeter(juce::Graphics& graphics, juce::Rectangle<float> area, float inputLinear, float reductionDb)
{
    auto labels = area.removeFromBottom(juce::jmax(16.0f, juce::jmin(22.0f, area.getHeight() * 0.16f)));
    graphics.setColour(juce::Colour(0xfff3f5f8));
    graphics.setFont(juce::Font(juce::FontOptions(11.0f).withStyle("Bold")));
    auto leftLabel = labels.withWidth(labels.getWidth() * 0.5f).toNearestInt();
    auto rightLabel = labels.withTrimmedLeft(labels.getWidth() * 0.5f).toNearestInt();
    graphics.drawFittedText("IN", leftLabel, juce::Justification::centred, 1);
    graphics.drawFittedText("GATE/GR", rightLabel, juce::Justification::centred, 1);

    graphics.setColour(juce::Colour(0xff0c0e13));
    graphics.fillRoundedRectangle(area, 2.0f);

    const auto left = area.withWidth(area.getWidth() * 0.5f).reduced(1.5f, 2.0f);
    const auto right = area.withTrimmedLeft(area.getWidth() * 0.5f).reduced(1.5f, 2.0f);
    const float inputFill = inputMeterFill(inputLinear);
    if (inputFill > 0.0f && left.getHeight() > 0.0f)
    {
        graphics.setColour(meterColour(gainToDecibels(inputLinear)));
        graphics.fillRect(left.withTrimmedTop(left.getHeight() * (1.0f - inputFill)));
    }

    const float reductionFill = reductionMeterFill(reductionDb);
    if (reductionFill > 0.0f && right.getHeight() > 0.0f)
    {
        graphics.setColour(juce::Colour(0xfff0c14a));
        graphics.fillRect(right.withHeight(right.getHeight() * reductionFill));
    }
}

} // namespace youmix
