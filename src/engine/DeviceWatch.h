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

// 48000 -> "48", 44100 -> "44.1", 96000 -> "96".
inline std::string formatRateKhz(double rate)
{
    const int milli = static_cast<int>(std::llround(rate));
    if (milli <= 0)
        return "0";
    if (milli % 1000 == 0)
        return std::to_string(milli / 1000);
    const int tenth = (milli + 50) / 100;
    if (tenth % 10 == 0)
        return std::to_string(tenth / 10);
    return std::to_string(tenth / 10) + "." + std::to_string(std::abs(tenth % 10));
}

struct SessionRateAdoption
{
    double rate = 0.0;
    bool changed = false;
    std::string notice;
};

// The session follows the card. A higher rate is announced. A lower rate is quiet.
// The returned rate is what the session stores. WAV files are not rewritten.
inline SessionRateAdoption adoptCardSampleRate(double sessionRate, double cardRate)
{
    SessionRateAdoption result;
    result.rate = sessionRate;
    if (! (cardRate > 0.0))
        return result;
    result.rate = cardRate;
    if (! (sessionRate > 0.0) || ! sampleRatesDiffer(sessionRate, cardRate))
        return result;
    result.changed = true;
    if (cardRate > sessionRate)
        result.notice = "Session moves to " + formatRateKhz(cardRate) + " kHz";
    return result;
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
