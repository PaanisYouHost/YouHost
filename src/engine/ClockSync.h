#pragma once

#include <cstdint>

namespace youhost
{

// A network driver (SoundGrid, Dante, AVB) can stay listed while its clock stops.
// Silence longer than this, or dropouts that keep climbing this long at ~0% DSP, is SYNC LOST.
inline constexpr std::int64_t kClockSilenceNs = 200000000;
// Regular callbacks have to hold this long before the notice shrinks to the corner.
inline constexpr std::int64_t kClockRestoredNs = 1000000000;
// A card the user just opened does not raise SYNC LOST during this window.
// The watchdog stays quiet until the window has passed and callbacks are steady.
inline constexpr std::int64_t kDeviceOpenGraceNs = 2000000000;
// Dropouts count as a clock fault only while the realtime path is this idle.
inline constexpr int kClockDspNearZeroPercent = 1;

enum class ClockSyncAction
{
    hold,
    lost,
    restored
};

struct ClockSyncInput
{
    std::int64_t nowNs = 0;
    std::int64_t lastCallbackNs = 0;
    std::uint64_t callbackCount = 0;
    std::uint32_t dropoutCount = 0;
    int dspPercent = 0;
    bool offline = false;
    bool deviceOpen = false;
    bool hostWasRunning = false;
    bool openingDevice = false;
    bool aliveDown = false;
    bool runningDown = false;
    bool clockSourceChanged = false;
    bool nominalRateChanged = false;
};

struct ClockSyncMemory
{
    bool missing = false;
    std::int64_t lostAtNs = 0;
    std::uint64_t callbackAtLoss = 0;
    std::int64_t regularSinceNs = 0;
    bool dropoutStamped = false;
    std::uint32_t dropoutStamp = 0;
    bool dropoutRising = false;
    std::int64_t dropoutRoseAtNs = 0;
    std::uint32_t dropoutRoseCount = 0;
    // Set when the user opens a card. Loss stays suppressed until the grace
    // floor has passed and callbacks have been steady. The first dropouts
    // never arm the burst detector.
    bool grace = false;
    std::int64_t graceUntilNs = 0;
    std::int64_t graceSteadySinceNs = 0;

    void reset() noexcept
    {
        *this = {};
    }
};

inline bool clockWatchSuppressed(const ClockSyncInput& in) noexcept
{
    return in.offline || ! in.deviceOpen || ! in.hostWasRunning || in.openingDevice;
}

inline bool clockPropertyFault(const ClockSyncInput& in) noexcept
{
    return in.aliveDown || in.runningDown || in.clockSourceChanged || in.nominalRateChanged;
}

// Clears any loss memory and holds the watchdog. Call this for a device
// change the user makes. Offline uses reset() and does not start a grace.
inline void beginDeviceOpenGrace(ClockSyncMemory& memory, std::int64_t nowNs) noexcept
{
    memory.reset();
    memory.grace = true;
    memory.graceUntilNs = nowNs + kDeviceOpenGraceNs;
}

// True while this sample must not raise or clear a clock loss. The sample
// that ends the grace still holds, so the open itself cannot alarm.
inline bool deviceOpenGraceHolding(ClockSyncMemory& memory, const ClockSyncInput& in) noexcept
{
    if (! memory.grace)
        return false;

    const bool callbackSeen = in.lastCallbackNs > 0;
    const std::int64_t gap = callbackSeen ? in.nowNs - in.lastCallbackNs : 0;
    const bool regular = callbackSeen && gap >= 0 && gap <= kClockSilenceNs && ! clockPropertyFault(in);
    if (! regular)
    {
        memory.graceSteadySinceNs = 0;
        return true;
    }

    if (memory.graceSteadySinceNs == 0)
        memory.graceSteadySinceNs = in.nowNs;
    const bool steady = in.nowNs - memory.graceSteadySinceNs >= kClockSilenceNs;
    const bool floorPassed = in.nowNs >= memory.graceUntilNs;
    if (steady && floorPassed)
    {
        memory.grace = false;
        memory.graceSteadySinceNs = 0;
        memory.dropoutStamped = false;
        memory.dropoutRising = false;
        memory.dropoutRoseAtNs = 0;
        memory.dropoutRoseCount = 0;
    }
    return true;
}

// One dropout does not fire. The first sample only stamps the count, so a
// historical total cannot alarm. A later increase has to still be climbing
// after the silence window.
inline bool clockDropoutBurst(ClockSyncMemory& memory, const ClockSyncInput& in) noexcept
{
    if (in.dspPercent > kClockDspNearZeroPercent)
    {
        memory.dropoutStamped = false;
        memory.dropoutRising = false;
        memory.dropoutRoseAtNs = 0;
        memory.dropoutRoseCount = 0;
        return false;
    }

    if (! memory.dropoutStamped)
    {
        memory.dropoutStamped = true;
        memory.dropoutStamp = in.dropoutCount;
        memory.dropoutRising = false;
        memory.dropoutRoseAtNs = 0;
        memory.dropoutRoseCount = 0;
        return false;
    }

    if (in.dropoutCount <= memory.dropoutStamp)
    {
        memory.dropoutRising = false;
        memory.dropoutRoseAtNs = 0;
        memory.dropoutRoseCount = 0;
        return false;
    }

    if (! memory.dropoutRising)
    {
        memory.dropoutRising = true;
        memory.dropoutRoseAtNs = in.nowNs;
        memory.dropoutRoseCount = in.dropoutCount;
        return false;
    }

    if (in.dropoutCount <= memory.dropoutRoseCount)
        return false;
    return in.nowNs - memory.dropoutRoseAtNs > kClockSilenceNs;
}

// Offline, a closed card, and a card that has not run never raise SYNC LOST.
// A loss is not re-stamped while it is already missing. Restore waits until
// callbacks have moved on and stayed inside the buffer gap for the restore window.
inline ClockSyncAction decideClockSync(ClockSyncMemory& memory, const ClockSyncInput& in) noexcept
{
    if (clockWatchSuppressed(in))
        return ClockSyncAction::hold;
    if (deviceOpenGraceHolding(memory, in))
        return ClockSyncAction::hold;

    const bool fault = clockPropertyFault(in);
    const bool callbackSeen = in.lastCallbackNs > 0;
    const std::int64_t gap = callbackSeen ? in.nowNs - in.lastCallbackNs : 0;
    const bool silent = callbackSeen && gap > kClockSilenceNs;
    const bool dropoutLost = clockDropoutBurst(memory, in);

    if (! memory.missing)
    {
        if (fault || silent || dropoutLost)
        {
            memory.missing = true;
            memory.lostAtNs = in.nowNs;
            memory.callbackAtLoss = in.callbackCount;
            memory.regularSinceNs = 0;
            return ClockSyncAction::lost;
        }
        return ClockSyncAction::hold;
    }

    const bool advanced = in.callbackCount > memory.callbackAtLoss;
    const bool gapSmall = callbackSeen && gap >= 0 && gap <= kClockSilenceNs;
    const bool regular = ! fault && advanced && gapSmall && ! dropoutLost;
    if (! regular)
    {
        memory.regularSinceNs = 0;
        return ClockSyncAction::hold;
    }
    if (memory.regularSinceNs == 0)
        memory.regularSinceNs = in.nowNs;
    if (in.nowNs - memory.regularSinceNs < kClockRestoredNs)
        return ClockSyncAction::hold;

    const auto lostAt = memory.lostAtNs;
    memory.missing = false;
    memory.regularSinceNs = 0;
    memory.dropoutStamped = false;
    memory.dropoutRising = false;
    memory.lostAtNs = lostAt;
    return ClockSyncAction::restored;
}

} // namespace youhost
