#pragma once

#include "HostLimits.h"

#include <algorithm>
#include <array>

namespace youhost
{

// Two seconds at 96 kHz. Longer look-ahead is clamped so the delay lines stay bounded.
inline constexpr int kMaxCompensationSamples = 192000;

enum class AlignMode
{
    all = 0,
    group = 1
};

struct ChannelLatencyInput
{
    int pluginSamples = 0;
    bool include = false;
    int group = -1;
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

// All aligned: every included channel lines up on the slowest included chain.
// Per group: each group lines up on that group's slowest chain. An ungrouped
// channel is not delayed. Excluded channels stay at 0 and do not move the max.
inline CompensationPlan planCompensation(const ChannelLatencyInput* channels, int count, AlignMode mode = AlignMode::all)
{
    CompensationPlan plan;
    if (channels == nullptr || count <= 0)
        return plan;

    const int n = std::min(count, kMaxChannels);
    if (mode == AlignMode::group)
    {
        int groupMax[128] = {};
        bool groupUsed[128] = {};
        int slowest = 0;
        for (int channel = 0; channel < n; ++channel)
        {
            if (! channels[channel].include)
                continue;
            const int chain = clampLatencySamples(channels[channel].pluginSamples);
            const int group = channels[channel].group;
            if (group < 0 || group >= 128)
                continue;
            groupUsed[group] = true;
            groupMax[group] = std::max(groupMax[group], chain);
            slowest = std::max(slowest, groupMax[group]);
        }
        plan.alignmentSamples = slowest;
        for (int channel = 0; channel < n; ++channel)
        {
            if (! channels[channel].include)
                continue;
            const int group = channels[channel].group;
            if (group < 0 || group >= 128 || ! groupUsed[group])
                continue;
            const int chain = clampLatencySamples(channels[channel].pluginSamples);
            plan.delaySamples[static_cast<std::size_t>(channel)] = groupMax[group] - chain;
        }
        return plan;
    }

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

// Realtime delay of `length` samples. A dry channel uses this so it lines up
// with a channel whose plugins already delayed the signal.
inline void delayInPlace(float* line, int length, int& write, float* data, int numSamples) noexcept
{
    if (line == nullptr || data == nullptr || length <= 0 || numSamples <= 0)
        return;
    if (write < 0 || write >= length)
        write = 0;

    for (int index = 0; index < numSamples; ++index)
    {
        const float oldest = line[write];
        line[write] = data[index];
        data[index] = oldest;
        if (++write >= length)
            write = 0;
    }
}

} // namespace youhost
