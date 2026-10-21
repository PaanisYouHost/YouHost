#pragma once

#include <cstdint>

namespace youhost
{

// Ignore scheduler jitter tighter than this, even on very short buffers.
inline constexpr int64_t kDropoutMinGapToleranceNs = 250000;

inline int64_t expectedPeriodNs(double sampleRate, int bufferSamples)
{
    if (sampleRate <= 0.0 || bufferSamples <= 0)
        return 0;

    const double nanos = 1000000000.0 * static_cast<double>(bufferSamples) / sampleRate;
    if (nanos <= 0.0 || nanos >= 9223372036854775807.0)
        return 0;
    return static_cast<int64_t>(nanos + 0.5);
}

// A callback is late when it starts more than half a buffer after it was due.
inline int64_t dropoutGapToleranceNs(int64_t periodNs)
{
    if (periodNs <= 0)
        return kDropoutMinGapToleranceNs;
    const int64_t half = periodNs / 2;
    return half > kDropoutMinGapToleranceNs ? half : kDropoutMinGapToleranceNs;
}

// 1 when this callback started late enough to count as a dropout.
// The first callback, and the first one after a device restart, pass skipGap.
// Built-in audio at 32 samples has a short period. A late wakeup counts here
// while the work inside the callback still fits the buffer.
inline int dropoutGapCount(int64_t previousCallbackNs, int64_t nowNs, int64_t periodNs, bool skipGap)
{
    if (skipGap || previousCallbackNs <= 0 || periodNs <= 0 || nowNs <= previousCallbackNs)
        return 0;

    const int64_t interval = nowNs - previousCallbackNs;
    const int64_t limit = periodNs + dropoutGapToleranceNs(periodNs);
    return interval > limit ? 1 : 0;
}

// 1 when the work inside the callback outlasted the buffer itself.
inline int dropoutOverrunCount(int64_t processingNs, int64_t periodNs)
{
    if (periodNs <= 0 || processingNs <= periodNs)
        return 0;
    return 1;
}

// Deadline for the group-edit stress test. Pass the realtime thread's own CPU
// time, not the wall clock: a deschedule makes wall time late without the
// callback doing more work. Continuous dropouts still fail, because the median
// wall time of the whole window moves past the buffer period.
inline int callbackCpuOverrun(int64_t cpuNs, int64_t periodNs) noexcept
{
    return dropoutOverrunCount(cpuNs, periodNs);
}

// 1.5x the quiet median, plus 20 us. A meter stuck near 270% of that quiet
// reading is outside this limit. The slack covers a callback that was already
// only a few microseconds.
inline bool callbackCpuNearBaseline(int64_t baselineNs, int64_t afterNs) noexcept
{
    if (baselineNs < 0 || afterNs < 0)
        return false;
    const int64_t limit = baselineNs + baselineNs / 2 + 20000;
    return afterNs <= limit;
}

// One late sample does not move the median. A callback that is late on every
// buffer does.
inline bool callbackMedianInsidePeriod(int64_t medianNs, int64_t periodNs) noexcept
{
    return periodNs > 0 && medianNs >= 0 && medianNs <= periodNs;
}

// How many callbacks in a row had CPU time past the buffer period.
// One late callback is a hitch. The Remove-group failure is a run of them.
inline int callbackOverrunStreak(const int64_t* cpuNs, int count, int64_t periodNs) noexcept
{
    int streak = 0;
    int longest = 0;
    if (cpuNs == nullptr || count <= 0)
        return 0;
    for (int index = 0; index < count; ++index)
    {
        if (callbackCpuOverrun(cpuNs[index], periodNs) != 0)
        {
            ++streak;
            if (streak > longest)
                longest = streak;
        }
        else
        {
            streak = 0;
        }
    }
    return longest;
}

// A quiet macOS CI baseline already produces a run of a few late callbacks
// (measured longest: 4, about 2.7 ms at 48 kHz / 32) at the same rate as the
// edit window. The Remove-group failure keeps every callback late for the
// rest of the session. 48 buffers is about 32 ms: above that hitch. A stuck
// meter still fails the median and the return-to-baseline checks.
inline constexpr int kDeadlineStreakLimit = 48;

inline bool callbackDeadlinesHeld(int longestStreak) noexcept
{
    return longestStreak < kDeadlineStreakLimit;
}

// Group edits may add at most two percentage points of late callbacks on top
// of the quiet baseline. A rebuilt graph puts essentially every callback over
// the period, in the edit window and after it.
inline bool editOverrunsNearBaseline(int baselineOver, int baselineCount,
                                     int editOver, int editCount) noexcept
{
    if (baselineCount <= 0 || editCount <= 0 || baselineOver < 0 || editOver < 0)
        return false;
    const int64_t baseCount = baselineCount;
    const int64_t editCount64 = editCount;
    const int64_t left = static_cast<int64_t>(editOver) * baseCount;
    const int64_t right = static_cast<int64_t>(baselineOver) * editCount64
                        + (baseCount * editCount64) / 50;
    return left <= right;
}

} // namespace youhost
