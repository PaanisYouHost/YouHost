#pragma once

#include "HostPath.h"

#include <cmath>
#include <string>

namespace youhost
{

// A device rate and a session rate disagree when both are known and at least 1 Hz apart.
inline bool sampleRatesDiffer(double deviceRate, double sessionRate) noexcept
{
    if (! (deviceRate > 0.0) || ! (sessionRate > 0.0))
        return false;
    return std::fabs(deviceRate - sessionRate) >= 1.0;
}

inline std::string sampleRateWarningText(double deviceRate, double sessionRate)
{
    if (! sampleRatesDiffer(deviceRate, sessionRate))
        return {};

    const auto device = std::to_string(static_cast<int>(std::llround(deviceRate)));
    const auto session = std::to_string(static_cast<int>(std::llround(sessionRate)));
    return "Device is " + device + " Hz. This session is " + session
           + " Hz. Playback needs the same rate.";
}

// How many device channels YouHost will use. The fixed rack stops at 128.
inline int usableChannelCount(int deviceChannels) noexcept
{
    if (deviceChannels < 0)
        return 0;
    if (deviceChannels > 128)
        return 128;
    return deviceChannels;
}

} // namespace youhost
