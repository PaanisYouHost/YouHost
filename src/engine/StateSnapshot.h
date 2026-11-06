#pragma once

#include <cstdint>

namespace youhost
{

// Plugin state is copied a few seconds after the last parameter edit, on a
// thread that is not the audio thread. The audio thread only raises dirty.
inline constexpr int kStateSnapshotDelayMs = 3000;

struct StateSnapshotClock
{
    bool dirty = false;
    std::int64_t dirtyAtMs = 0;
    bool captured = false;
};

inline void noteStateEdit(StateSnapshotClock& clock, std::int64_t nowMs) noexcept
{
    if (! clock.dirty)
        clock.dirtyAtMs = nowMs;
    clock.dirty = true;
    clock.captured = false;
}

// audioThreadInside: a processBlock is in the plugin right now.
// liveCallbacks: the card is running. getStateInformation on an Audio Unit
// takes the same lock as render. The check above only sees the current
// block, and the next 666 us callback then sleeps inside that plugin.
// The time shows up as off-CPU on the slowest plugin. Capture only while
// the callback is stopped.
inline bool stateSnapshotDue(const StateSnapshotClock& clock,
                             std::int64_t nowMs,
                             bool audioThreadInside,
                             bool liveCallbacks) noexcept
{
    if (liveCallbacks || ! clock.dirty || clock.captured || audioThreadInside)
        return false;
    if (clock.dirtyAtMs <= 0 || nowMs < clock.dirtyAtMs)
        return false;
    return nowMs - clock.dirtyAtMs >= kStateSnapshotDelayMs;
}

inline void noteStateCaptured(StateSnapshotClock& clock) noexcept
{
    clock.dirty = false;
    clock.captured = true;
}

} // namespace youhost
