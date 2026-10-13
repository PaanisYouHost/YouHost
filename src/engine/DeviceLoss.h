#pragma once

#include "Markers.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace youhost
{

// The banner stays up until the same card is back. OK only hides the blink.
inline constexpr const char* kDeviceLostBanner = "Device lost - waiting";

// Reattach runs from a device-list notification. The timer must not scan for the card.
inline bool deviceReattachUsesTimer() noexcept
{
    return false;
}

// The plugin graph is left loaded across the gap.
inline bool deviceLossKeepsPlugins() noexcept
{
    return true;
}

struct DeviceLossDecision
{
    bool showWaiting = false;
    bool reattachSame = false;
    bool refusedOther = false;
    bool closeTake = false;
    bool resumeRecording = false;
    bool keepLock = false;
};

// wanted is the session card. appeared is some other open name, empty when
// nothing is open. wantedListed comes from the device-list notification.
// A different name is never opened. The padlock stays armed and does not
// block coming back to this same card.
inline DeviceLossDecision decideDeviceLoss(std::string_view wanted,
                                           std::string_view appeared,
                                           bool wantedListed,
                                           bool currentlyLost,
                                           bool wasRecording,
                                           bool lockArmed) noexcept
{
    DeviceLossDecision decision;
    decision.keepLock = lockArmed;
    if (wanted.empty())
        return decision;

    const bool other = ! appeared.empty() && appeared != wanted;
    if (other)
    {
        decision.refusedOther = true;
        decision.showWaiting = currentlyLost;
        return decision;
    }

    if (! currentlyLost)
        return decision;

    decision.showWaiting = ! wantedListed;
    decision.closeTake = wasRecording && ! wantedListed;
    if (wantedListed)
    {
        decision.reattachSame = true;
        decision.showWaiting = false;
        decision.resumeRecording = wasRecording;
    }
    return decision;
}

inline MarkerPoint deviceLossMarker(std::int64_t sample)
{
    MarkerPoint marker;
    marker.sample = sample < 0 ? 0 : sample;
    marker.name = "Device lost";
    return marker;
}

inline std::string deviceLossStartLine(std::string_view name, bool closedTake)
{
    std::string line = "device lost \"";
    line.append(name.begin(), name.end());
    line += "\"";
    if (closedTake)
        line += ", take closed";
    return line;
}

inline std::string deviceLossLogLine(std::string_view name, std::int64_t durationMs, bool resumedRecording)
{
    if (durationMs < 0)
        durationMs = 0;
    std::string line = "device lost \"";
    line.append(name.begin(), name.end());
    line += "\" for ";
    line += std::to_string(durationMs);
    line += " ms";
    if (resumedRecording)
        line += ", recording resumed";
    return line;
}

} // namespace youhost
