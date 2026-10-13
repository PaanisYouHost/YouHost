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
inline void paintSplitMeter(juce::Graphics& graphics, juce::Rectangle<float> area, float inputLinear, float reductionDb)
{
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
