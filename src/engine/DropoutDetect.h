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

} // namespace youhost
