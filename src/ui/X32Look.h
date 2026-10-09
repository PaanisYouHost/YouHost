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

// Hue used when a strip or row needs a visible colour, including the inverted
// scribble colours whose solid fill is otherwise a dark panel.
inline juce::Colour x32Hue(int index)
{
    const int id = normaliseX32Colour(index);
    if (id == 0)
        return juce::Colours::transparentBlack;
    if (kX32Colours[id].inverted && id != 8)
        return x32Rgb(kX32Colours[id]);
    return x32Fill(id);
}

// Soft wash over the whole strip. The solid bar carries the colour; this only tints.
inline juce::Colour x32Wash(int index)
{
    const auto hue = x32Hue(index);
    if (hue.isTransparent())
        return hue;
    return hue.withAlpha(0.18f);
}

} // namespace youhost
