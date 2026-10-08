#pragma once

#include "HostLimits.h"

#include <algorithm>
#include <array>

namespace youhost
{

// Two seconds at 96 kHz. Longer look-ahead is clamped so the delay lines stay bounded.
inline constexpr int kMaxCompensationSamples = 192000;

struct ChannelLatencyInput
{
    int pluginSamples = 0;
    bool include = false;
};

struct CompensationPlan
{
    // Extra delay added on top of each channel's own plugins. Excluded channels stay 0.
    std::array<int, kMaxChannels> delaySamples {};
    // Slowest included chain. This is what the round-trip figure adds: every included
    // output is aligned to it, so the desk hears that many extra samples.
    int alignmentSamples = 0;
};

inline int clampLatencySamples(int samples)
{
    if (samples < 0)
        return 0;
    if (samples > kMaxCompensationSamples)
        return kMaxCompensationSamples;
    return samples;
}

inline int sumSlotLatency(const int* latencies, const bool* occupied, const bool* bypassed, int count)
{
    int sum = 0;
    if (latencies == nullptr || occupied == nullptr || bypassed == nullptr)
        return 0;

    for (int slot = 0; slot < count; ++slot)
    {
        if (occupied[slot] && ! bypassed[slot])
            sum += clampLatencySamples(latencies[slot]);
    }
    return sum;
}

// Included channels line up on the slowest included chain. Excluded channels are
// not delayed and do not pull the alignment later.
inline CompensationPlan planCompensation(const ChannelLatencyInput* channels, int count)
{
    CompensationPlan plan;
    if (channels == nullptr || count <= 0)
        return plan;

    const int n = std::min(count, kMaxChannels);
    int slowest = 0;
    for (int channel = 0; channel < n; ++channel)
    {
        if (! channels[channel].include)
            continue;
        slowest = std::max(slowest, clampLatencySamples(channels[channel].pluginSamples));
    }

    plan.alignmentSamples = slowest;
    for (int channel = 0; channel < n; ++channel)
    {
        if (! channels[channel].include)
            continue;
        const int chain = clampLatencySamples(channels[channel].pluginSamples);
        plan.delaySamples[static_cast<std::size_t>(channel)] = slowest - chain;
    }
    return plan;
}

} // namespace youhost
