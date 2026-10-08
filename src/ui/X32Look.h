#pragma once

#include "engine/X32Colours.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace youhost
{

inline juce::Colour x32Rgb(const X32Colour& colour)
{
    return juce::Colour::fromRGB(static_cast<juce::uint8>(colour.red),
                                 static_cast<juce::uint8>(colour.green),
                                 static_cast<juce::uint8>(colour.blue));
}

inline juce::Colour x32Fill(int index)
{
    const int id = normaliseX32Colour(index);
    const auto& colour = kX32Colours[id];
    if (! colour.inverted)
        return x32Rgb(colour);
    if (id == 8)
        return juce::Colour(0xffe6e6e6);
    return juce::Colour(0xff12141a);
}

inline juce::Colour x32Ink(int index)
{
    const int id = normaliseX32Colour(index);
    const auto& colour = kX32Colours[id];
    if (colour.inverted)
        return id == 8 ? juce::Colour(0xff161616) : x32Rgb(colour);

    const int luma = colour.red * 3 + colour.green * 6 + colour.blue;
    return luma > 1200 ? juce::Colour(0xff141414) : juce::Colours::white;
}

} // namespace youhost
