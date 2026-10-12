#pragma once

#include <cstdint>

namespace youhost
{

// Status and the CPU window show this. Above 100 is a real overload, up to 999.
inline constexpr int kCpuPercentCeiling = 999;

inline int clampCpuPercent(int percent) noexcept
{
    if (percent < 0)
        return 0;
    if (percent > kCpuPercentCeiling)
        return kCpuPercentCeiling;
    return percent;
}

// One block's time against the buffer period, as a whole percent.
// Arithmetic only: the audio thread calls this once per plugin per block.
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

} // namespace youhost
