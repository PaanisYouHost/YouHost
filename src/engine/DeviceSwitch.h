#pragma once

#include "ClockSync.h"
#include "SyncLost.h"

#include <cstdint>
#include <string>
#include <utility>

namespace youhost
{

// A live panic: the user jumps cards, including Offline, and the last card
// has to run at once. The watchdog shares beginDeviceOpenGrace with the engine.
struct ShowCard
{
    std::string name;
    bool open = false;
    bool offline = true;
    bool recording = false;
    bool resumeRecording = false;
    bool hung = false;
    int opens = 0;
    int watchSteps = 0;
    std::int64_t nowNs = 0;
    std::int64_t lastCallbackNs = 0;
    std::uint64_t callbacks = 0;
    std::uint32_t dropouts = 0;
    int dspPercent = 0;
    bool propertyFault = false;
    ClockSyncMemory clock {};
    SyncLostState overlay {};
};

inline void showOpen(ShowCard& show, std::string name, std::int64_t nowNs) noexcept
{
    const bool recording = show.recording;
    show.hung = true;
    show.overlay.dismiss();
    beginDeviceOpenGrace(show.clock, nowNs);
    show.offline = false;
    show.open = true;
    show.name = std::move(name);
    show.nowNs = nowNs;
    show.lastCallbackNs = 0;
    show.callbacks = 0;
    show.dropouts = 0;
    show.dspPercent = 0;
    show.propertyFault = false;
    show.recording = recording;
    show.hung = false;
    ++show.opens;
}

inline void showOffline(ShowCard& show, std::int64_t nowNs) noexcept
{
    const bool recording = show.recording;
    show.hung = true;
    show.overlay.dismiss();
    show.clock.reset();
    show.offline = true;
    show.open = false;
    show.name = "Offline";
    show.nowNs = nowNs;
    show.propertyFault = false;
    show.recording = recording;
    show.hung = false;
}

inline ClockSyncAction showWatch(ShowCard& show) noexcept
{
    ++show.watchSteps;
    if (show.offline || ! show.open)
        return ClockSyncAction::hold;

    ClockSyncInput input;
    input.nowNs = show.nowNs;
    input.lastCallbackNs = show.lastCallbackNs;
    input.callbackCount = show.callbacks;
    input.dropoutCount = show.dropouts;
    input.dspPercent = show.dspPercent;
    input.offline = show.offline;
    input.deviceOpen = show.open;
    input.hostWasRunning = true;
    input.aliveDown = show.propertyFault;
    const auto action = decideClockSync(show.clock, input);
    if (action == ClockSyncAction::lost && ! show.overlay.isMissing())
        show.overlay.deviceLost(10, 34, 49);
    else if (action == ClockSyncAction::restored && show.overlay.isMissing())
        show.overlay.deviceReturned(1000);
    return action;
}

inline void showCallback(ShowCard& show, std::int64_t nowNs, bool dropout) noexcept
{
    show.nowNs = nowNs;
    show.lastCallbackNs = nowNs;
    ++show.callbacks;
    if (dropout)
    {
        ++show.dropouts;
        show.dspPercent = 0;
    }
    showWatch(show);
}

inline void showSilence(ShowCard& show, std::int64_t nowNs) noexcept
{
    show.nowNs = nowNs;
    show.dspPercent = 0;
    showWatch(show);
}

// A card that disappears on its own. A user open (grace still up) does not
// count: that path must not raise SYNC LOST or stop a take.
inline void showHotUnplug(ShowCard& show) noexcept
{
    if (show.clock.grace || show.offline || ! show.open)
        return;
    show.open = false;
    if (! show.overlay.isMissing())
        show.overlay.deviceLost(10, 35, 0);
    if (show.recording)
    {
        show.recording = false;
        show.resumeRecording = true;
    }
}

inline void showHotReturn(ShowCard& show) noexcept
{
    if (! show.overlay.isMissing())
        return;
    show.open = true;
    show.offline = false;
    const bool resume = show.resumeRecording;
    show.overlay.deviceReturned(1500);
    show.clock.reset();
    if (resume)
    {
        show.recording = true;
        show.resumeRecording = false;
    }
}

// Steady callbacks from `fromNs` through `untilNs`. Returns false if the
// step would not advance, which is the hang the panic test rejects.
inline bool showRunSteady(ShowCard& show, std::int64_t fromNs, std::int64_t untilNs, std::int64_t stepNs) noexcept
{
    if (stepNs <= 0 || untilNs < fromNs)
    {
        show.hung = true;
        return false;
    }
    for (std::int64_t t = fromNs; t <= untilNs; t += stepNs)
        showCallback(show, t, false);
    return ! show.hung;
}

} // namespace youhost
