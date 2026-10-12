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

} // namespace youhost
