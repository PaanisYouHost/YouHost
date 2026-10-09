#pragma once

#include "Theme.h"
#include "engine/MeterScale.h"

namespace youhost
{

inline juce::Colour meterLevelColour(float linearGain, int referenceDb)
{
    switch (meterColourForDb(gainToDb(linearGain), referenceDb))
    {
        case MeterColour::red:
            return theme::red;
        case MeterColour::yellow:
            return theme::amber;
        case MeterColour::green:
            break;
    }
    return theme::green;
}

} // namespace youhost
