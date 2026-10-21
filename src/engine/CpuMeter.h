#pragma once

#include <cstddef>
#include <cstdint>

namespace youhost
{

// Every CPU and DSP figure stops at 100. 100% is full: the buffer is used up.
inline constexpr int kCpuPercentCeiling = 100;
inline constexpr int kCpuDisplaySlots = 16;
inline constexpr std::int64_t kCpuAverageMs = 1000;
inline constexpr std::int64_t kCpuPublishMs = 500;

inline int clampCpuPercent(int percent) noexcept
{
    if (percent < 0)
        return 0;
    if (percent > kCpuPercentCeiling)
        return kCpuPercentCeiling;
    return percent;
}

inline int displayCpuPercent(float percent) noexcept
{
    if (! (percent > 0.0f))
        return 0;
    return clampCpuPercent(static_cast<int>(percent + 0.5f));
}

// One block's time against the buffer period, as a whole percent from 0 to 100.
// 100% fills the buffer. Arithmetic only: the audio thread calls this once per plugin per block.
inline int blockCpuPercent(std::uint64_t elapsedNs, int samples, double sampleRate) noexcept
{
    if (elapsedNs == 0 || samples <= 0 || sampleRate <= 0.0)
        return 0;
    const double period = static_cast<double>(samples) / sampleRate;
    if (period <= 0.0)
        return 0;
    const double percent = (static_cast<double>(elapsedNs) / 1.0e9) / period * 100.0;
    if (percent >= static_cast<double>(kCpuPercentCeiling))
        return kCpuPercentCeiling;
    if (percent <= 0.0)
        return 0;
    return static_cast<int>(percent + 0.5);
}

// Same ratio as blockCpuPercent, but the dropout log can say 180% when the
// callback took almost two buffers. The on-screen meter still stops at 100.
inline int callbackLoadPercent(std::uint64_t elapsedNs, int samples, double sampleRate) noexcept
{
    if (elapsedNs == 0 || samples <= 0 || sampleRate <= 0.0)
        return 0;
    const double period = static_cast<double>(samples) / sampleRate;
    if (period <= 0.0)
        return 0;
    const double percent = (static_cast<double>(elapsedNs) / 1.0e9) / period * 100.0;
    if (percent <= 0.0)
        return 0;
    if (percent >= 999.0)
        return 999;
    return static_cast<int>(percent + 0.5);
}

// wallNs against the buffer period. Same ratio as callbackLoadPercent when
// the callback's sample count is the device buffer.
inline int periodLoadPercent(int wallNs, int periodNs) noexcept
{
    if (wallNs <= 0 || periodNs <= 0)
        return 0;
    const double percent = static_cast<double>(wallNs) / static_cast<double>(periodNs) * 100.0;
    if (percent >= 999.0)
        return 999;
    return static_cast<int>(percent + 0.5);
}

// The DSP meter is this callback's own time. A latched plugin sum (four
// plugins sitting near 13% read as a stuck 52) is not the meter.
inline int dspReading(int callbackPercent, int latchedPluginSum) noexcept
{
    (void) latchedPluginSum;
    return clampCpuPercent(callbackPercent);
}

// Half the previous reading, half this block. The step rounds toward the new
// reading, so a steady load reaches that percent instead of sitting one below it.
// No heap, no lock.
inline int smoothCpuPercent(int previous, int instant) noexcept
{
    const int prior = clampCpuPercent(previous);
    const int now = clampCpuPercent(instant);
    if (now >= prior)
        return clampCpuPercent((prior + now + 1) / 2);
    return clampCpuPercent((prior + now) / 2);
}

// Message-thread display. The first sample shows at once. Later samples wait
// for the publish interval, then the shown figure becomes the mean of the
// last second. Peak keeps the highest sample in that second. No heap.
struct CpuDisplayMeter
{
    struct Sample
    {
        std::int64_t timeMs = 0;
        int percent = 0;
    };

    Sample samples[kCpuDisplaySlots] {};
    int count = 0;
    int next = 0;
    std::int64_t publishedMs = 0;
    int shown = 0;
    int peak = 0;
    bool haveShown = false;

    void reset() noexcept
    {
        count = 0;
        next = 0;
        publishedMs = 0;
        shown = 0;
        peak = 0;
        haveShown = false;
    }

    int push(std::int64_t timeMs, int percent) noexcept
    {
        percent = clampCpuPercent(percent);
        samples[static_cast<std::size_t>(next)] = Sample { timeMs, percent };
        next = (next + 1) % kCpuDisplaySlots;
        if (count < kCpuDisplaySlots)
            ++count;

        int windowPeak = 0;
        int sum = 0;
        int samplesInWindow = 0;
        for (int index = 0; index < count; ++index)
        {
            const Sample& sample = samples[static_cast<std::size_t>(index)];
            if (sample.timeMs > timeMs || timeMs - sample.timeMs > kCpuAverageMs)
                continue;
            sum += sample.percent;
            ++samplesInWindow;
            if (sample.percent > windowPeak)
                windowPeak = sample.percent;
        }
        peak = windowPeak;

        if (! haveShown)
        {
            haveShown = true;
            publishedMs = timeMs;
            shown = percent;
            return shown;
        }
        if (timeMs < publishedMs || timeMs - publishedMs < kCpuPublishMs)
            return shown;
        publishedMs = timeMs;
        if (samplesInWindow > 0)
            shown = clampCpuPercent((sum + samplesInWindow / 2) / samplesInWindow);
        return shown;
    }
};

} // namespace youhost
