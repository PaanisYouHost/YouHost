#pragma once

#include <cstdint>
#include <string>

namespace youhost
{

inline constexpr const char* kSyncLostTitle = "SYNC LOST";
inline constexpr const char* kSyncLostFinnish = "Äänikortti kadonnut";
inline constexpr const char* kSyncRestoredFinnish = "Ääni palautettu";

// The overlay and the reopen are separate. While the card is gone the
// overlay stays up. After audio returns it stays up until a click.
// Reattach does not wait for that click.
struct SyncLostState
{
    bool missing = false;
    bool restoredNotice = false;
    int hour = 0;
    int minute = 0;
    int second = 0;
    bool haveClock = false;
    std::int64_t gapMs = 0;

    void deviceLost(int lossHour = -1, int lossMinute = 0, int lossSecond = 0) noexcept
    {
        if (! missing && lossHour >= 0)
        {
            hour = lossHour;
            minute = lossMinute;
            second = lossSecond;
            haveClock = true;
        }
        missing = true;
        restoredNotice = false;
    }

    void deviceReturned(std::int64_t durationMs = 0) noexcept
    {
        if (! missing)
            return;
        missing = false;
        restoredNotice = true;
        gapMs = durationMs < 0 ? 0 : durationMs;
    }

    void acknowledge() noexcept
    {
        if (missing)
            return;
        restoredNotice = false;
        haveClock = false;
        gapMs = 0;
    }

    void dismiss() noexcept
    {
        missing = false;
        restoredNotice = false;
        haveClock = false;
        gapMs = 0;
    }

    bool isMissing() const noexcept { return missing; }

    bool showAlarm() const noexcept { return missing || restoredNotice; }

    bool keepReattaching() const noexcept { return missing; }

    bool showRestored() const noexcept { return restoredNotice && ! missing; }
};

inline bool syncLostBlink(std::uint32_t millisecondCounter) noexcept
{
    return (millisecondCounter / 400u) % 2u == 0u;
}

inline int syncLossWholeSeconds(std::int64_t durationMs) noexcept
{
    if (durationMs <= 0)
        return 0;
    return static_cast<int>((durationMs + 500) / 1000);
}

inline void appendTwoDigits(std::string& out, int value)
{
    if (value < 0)
        value = 0;
    if (value > 99)
        value = value % 100;
    if (value < 10)
        out.push_back('0');
    out += std::to_string(value);
}

// "SYNC WAS LOST at hh:mm:ss for N s - audio restored"
inline std::string syncRestoredLine(int hour, int minute, int second, std::int64_t durationMs)
{
    if (hour < 0)
        hour = 0;
    if (hour > 23)
        hour = hour % 24;
    if (minute < 0)
        minute = 0;
    if (minute > 59)
        minute = minute % 60;
    if (second < 0)
        second = 0;
    if (second > 59)
        second = second % 60;

    std::string line = "SYNC WAS LOST at ";
    appendTwoDigits(line, hour);
    line.push_back(':');
    appendTwoDigits(line, minute);
    line.push_back(':');
    appendTwoDigits(line, second);
    line += " for ";
    line += std::to_string(syncLossWholeSeconds(durationMs));
    line += " s - audio restored";
    return line;
}

struct SyncOverlayText
{
    bool visible = false;
    bool lost = false;
    std::string primary;
    std::string secondary;
};

inline SyncOverlayText syncOverlayText(const SyncLostState& state)
{
    SyncOverlayText text;
    if (state.isMissing())
    {
        text.visible = true;
        text.lost = true;
        text.primary = kSyncLostTitle;
        text.secondary = kSyncLostFinnish;
        return text;
    }
    if (state.showRestored())
    {
        text.visible = true;
        text.lost = false;
        text.primary = syncRestoredLine(state.hour, state.minute, state.second, state.gapMs);
        text.secondary = kSyncRestoredFinnish;
    }
    return text;
}

} // namespace youhost
