#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace youhost
{

inline constexpr const char* kSyncLostTitle = "SYNC LOST";
inline constexpr const char* kSyncLostFinnish = "Äänikortti kadonnut";
inline constexpr const char* kSyncRestoredFinnish = "Ääni palautettu";

// Settings may remember a card. Startup does not open it. The user picks one,
// or a session file names one.
inline bool startupOpensSavedDevice() noexcept
{
    return false;
}

// SYNC LOST is a card that was already running in this session and then
// disappeared. A saved card that never opened, startup, and Offline do not.
inline bool syncLostMayShow(bool deviceWasRunning, bool offline) noexcept
{
    return deviceWasRunning && ! offline;
}

inline bool choosingOfflineClearsSyncLost() noexcept
{
    return true;
}

struct StartupCardDecision
{
    bool openSaved = false;
    bool offline = true;
    bool showSyncLost = false;
    std::string notice;
};

// The device list can still be empty on the first look after launch.
// Ask this many times, with a gap between them, before the Offline notice.
inline constexpr int kStartupDeviceListTries = 8;
inline constexpr std::uint32_t kStartupDeviceListGapMs = 250;

struct StartupListWatch
{
    int tries = 0;
    bool pending = false;
    bool listed = false;
    std::string notice;
};

inline void beginStartupListWatch(StartupListWatch& watch) noexcept
{
    watch = {};
    watch.pending = true;
}

// A missing saved card stays Offline with the same notice as a session whose
// card is gone. The overlay stays down.
inline StartupCardDecision decideStartupCard(std::string_view savedName, bool savedListed) noexcept
{
    StartupCardDecision decision;
    decision.openSaved = startupOpensSavedDevice();
    decision.offline = ! decision.openSaved;
    decision.showSyncLost = false;
    if (! savedName.empty() && ! savedListed)
    {
        decision.notice = "Saved card ";
        decision.notice.append(savedName.begin(), savedName.end());
        decision.notice += " not found - Offline";
    }
    return decision;
}

// One look. A hit ends the watch with no notice. The Offline notice is
// published only on the last miss. Earlier misses stay quiet.
inline bool pollStartupList(StartupListWatch& watch, std::string_view savedName, bool listedNow)
{
    if (! watch.pending)
        return false;
    ++watch.tries;
    if (listedNow)
    {
        watch.pending = false;
        watch.listed = true;
        watch.notice.clear();
        return false;
    }
    if (watch.tries < kStartupDeviceListTries)
        return false;
    watch.pending = false;
    watch.listed = false;
    watch.notice = decideStartupCard(savedName, false).notice;
    return ! watch.notice.empty();
}

// The notice and the reopen are separate. While the card is gone a large
// window stays centered. After audio returns it shrinks to a bottom-left
// box until OK. Reattach does not wait for that click.
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

    // OK dismisses the restored dialog only. The centered window stays
    // while sync is still lost.
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

enum class SyncLostChrome
{
    hidden,
    center,
    corner
};

inline SyncLostChrome syncLostChrome(const SyncLostState& state) noexcept
{
    if (state.isMissing())
        return SyncLostChrome::center;
    if (state.showRestored())
        return SyncLostChrome::corner;
    return SyncLostChrome::hidden;
}

struct SyncLostBox
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

inline SyncLostBox syncLostCenterBox(int parentWidth, int parentHeight) noexcept
{
    int width = parentWidth - 80;
    if (width > 720)
        width = 720;
    if (width < 280)
        width = 280;
    int height = parentHeight / 3;
    if (height > 320)
        height = 320;
    if (height < 180)
        height = 180;
    return { (parentWidth - width) / 2, (parentHeight - height) / 2, width, height };
}

inline SyncLostBox syncLostCornerBox(int parentWidth, int parentHeight) noexcept
{
    (void) parentWidth;
    return { 12, parentHeight - 48, 168, 36 };
}

inline SyncLostBox syncLostDialogBox(int parentWidth, int parentHeight) noexcept
{
    int width = parentWidth - 80;
    if (width > 640)
        width = 640;
    if (width < 320)
        width = 320;
    constexpr int height = 200;
    return { (parentWidth - width) / 2, (parentHeight - height) / 2, width, height };
}

// "Sync was lost at hh:mm:ss for N s, now restored"
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

    std::string line = "Sync was lost at ";
    appendTwoDigits(line, hour);
    line.push_back(':');
    appendTwoDigits(line, minute);
    line.push_back(':');
    appendTwoDigits(line, second);
    line += " for ";
    line += std::to_string(syncLossWholeSeconds(durationMs));
    line += " s, now restored";
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
    }
    return text;
}

} // namespace youhost
