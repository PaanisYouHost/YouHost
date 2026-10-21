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

// Empty when either rate is unknown or they already agree. Does not change audio.
inline std::string sampleRateMismatchText(double sessionRate, double deviceRate)
{
    if (! sampleRatesDiffer(deviceRate, sessionRate))
        return {};
    return "Sample rate mismatch: session " + formatRateKhz(sessionRate)
           + " kHz, audio device " + formatRateKhz(deviceRate) + " kHz";
}

inline std::string sampleRateWarningText(double deviceRate, double sessionRate)
{
    return sampleRateMismatchText(sessionRate, deviceRate);
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
// A chosen device stays put. Polls do not scan or probe it again until the
// device is actually lost, the inventory is still empty, or the user picks
// another card.
inline bool deviceInventoryQueryAllowed(bool setupLocked, bool deviceLost, bool force, bool inventoryEmpty) noexcept
{
    if (force || deviceLost || inventoryEmpty)
        return true;
    return ! setupLocked;
}

// The "active" count in the device log is the enabled-channel mask, not a
// plugin. A live card that reports fewer channels than the set already
// running must not shrink that set: the callback would copy the dry input
// and skip the plugins. Offline and a different card still follow the new count.
inline bool keepPluginsOnShrink(bool sameOpenCard,
                                int runningInputs,
                                int runningOutputs,
                                int nextInputs,
                                int nextOutputs) noexcept
{
    if (! sameOpenCard)
        return false;
    if (runningInputs <= 0 && runningOutputs <= 0)
        return false;
    return nextInputs < runningInputs || nextOutputs < runningOutputs;
}

inline void openAllReportedChannels(bool* open, int reported) noexcept
{
    const int count = channelsToOpen(reported);
    if (open == nullptr || count <= 0)
        return;
    for (int index = 0; index < count; ++index)
        open[index] = true;
}

} // namespace youhost
