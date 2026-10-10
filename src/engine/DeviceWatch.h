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

// The interface always opens this many channels: every name the device reports, capped at 128.
inline int channelsToOpen(int reported) noexcept
{
    return usableChannelCount(reported);
}

// True when the first channelsToOpen(reported) bits are on. A shorter saved mask is not complete.
inline bool deviceMaskIsComplete(const bool* open, int reported) noexcept
{
    const int count = channelsToOpen(reported);
    if (count <= 0)
        return true;
    if (open == nullptr)
        return false;
    for (int index = 0; index < count; ++index)
        if (! open[index])
            return false;
    return true;
}

// Turns on every channel the device reported, up to 128. The caller supplies at least that many flags.
inline void openAllReportedChannels(bool* open, int reported) noexcept
{
    const int count = channelsToOpen(reported);
    if (open == nullptr || count <= 0)
        return;
    for (int index = 0; index < count; ++index)
        open[index] = true;
}

} // namespace youhost
