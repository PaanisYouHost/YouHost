#pragma once

#include "HostLimits.h"

#include <cstring>
#include <string>
#include <vector>

namespace youhost
{

// Pairs are adjacent and odd/even in channel number: 1-2, 3-4, ...
// The odd channel (index 0, 2, 4, ...) is left. The even channel is right.
inline int stereoLeftIndex(int channel) noexcept
{
    if (channel < 0 || channel >= kMaxChannels)
        return -1;
    return channel & ~1;
}

inline int stereoRightIndex(int channel) noexcept
{
    const int left = stereoLeftIndex(channel);
    if (left < 0 || left + 1 >= kMaxChannels)
        return -1;
    return left + 1;
}

inline bool canStereoLink(int channel, int channelCount) noexcept
{
    const int left = stereoLeftIndex(channel);
    if (left < 0)
        return false;
    const int limit = channelCount < kMaxChannels ? channelCount : kMaxChannels;
    return left + 1 < limit;
}

// The other channel of an adjacent pair, or -1 when channel is out of range.
inline int stereoPartnerIndex(int channel) noexcept
{
    const int left = stereoLeftIndex(channel);
    const int right = stereoRightIndex(channel);
    if (left < 0 || right < 0)
        return -1;
    return channel == left ? right : left;
}

// Right-click on REC or HOST. The same words are used from either strip.
inline std::string stereoLinkMenuLabel(int channel)
{
    if (stereoLeftIndex(channel) < 0)
        return {};
    return "Stereo link";
}

// While a pair is linked, an edit to listen, colour, group, or name on one
// channel is copied to the other. Unlinked channels stay independent.
inline int linkedEditPartner(int channel, bool pairLinked) noexcept
{
    if (! pairLinked)
        return -1;
    return stereoPartnerIndex(channel);
}

// A group or selection edit that names one channel of a linked pair includes
// the partner, so the pair stays in one group.
template <typename IsLinked>
inline void includeStereoPartners(std::vector<int>& channels, IsLinked isLinked)
{
    const auto original = channels.size();
    for (std::size_t index = 0; index < original; ++index)
    {
        const int channel = channels[index];
        if (! isLinked(channel))
            continue;
        const int partner = stereoPartnerIndex(channel);
        if (partner < 0)
            continue;
        bool present = false;
        for (int existing : channels)
        {
            if (existing == partner)
            {
                present = true;
                break;
            }
        }
        if (! present)
            channels.push_back(partner);
    }
}

// The even channel of a linked pair reports the odd channel's plugin latency,
// so compensation delays the pair together.
inline int pairChainSamples(bool linkedRight, int ownChain, int leftChain) noexcept
{
    return linkedRight ? leftChain : ownChain;
}

// Linked pairs show one plugin row, on the odd channel. The even channel's
// slots are hidden. Recording is not part of this decision.
inline bool hostPluginSlotsVisible(int channel, bool pairLinked) noexcept
{
    if (channel < 0)
        return false;
    if (! pairLinked)
        return true;
    return (channel & 1) == 0;
}

// One stereo processBlock for a linked pair. Two mono channels are two calls.
inline int pluginProcessCallsForPair(bool linked, bool stereoBuses) noexcept
{
    if (linked && stereoBuses)
        return 1;
    return 2;
}

// One stereo processBlock pays call overhead once and covers both channels.
// Two mono processBlock calls pay it twice. The stereo cost is lower.
inline int pairProcessCost(bool linked, bool stereoBuses, int samples) noexcept
{
    const int calls = pluginProcessCallsForPair(linked, stereoBuses);
    constexpr int overhead = 64;
    const int covered = (linked && stereoBuses) ? samples * 2 : samples;
    return calls * (overhead + (samples > 0 ? covered : 0));
}

template <typename Plugin>
inline int runLinkedPair(float* left, float* right, int numSamples, bool linked, bool stereoBuses, Plugin plugin)
{
    if (left == nullptr || numSamples <= 0)
        return 0;
    int calls = 0;
    if (linked && stereoBuses && right != nullptr)
    {
        plugin(left, right, numSamples);
        ++calls;
    }
    else
    {
        plugin(left, nullptr, numSamples);
        ++calls;
        if (right != nullptr)
        {
            plugin(right, nullptr, numSamples);
            ++calls;
        }
    }
    return calls;
}

// An empty chain copies the dry input. It does not read the delay line.
inline void emptyChainPassthrough(float* output, const float* dry, int numSamples) noexcept
{
    if (output == nullptr || dry == nullptr || numSamples <= 0)
        return;
    if (output != dry)
        std::memcpy(output, dry, sizeof(float) * static_cast<std::size_t>(numSamples));
}

inline void resetDelayLine(float* line, int length, int& write) noexcept
{
    if (line != nullptr && length > 0)
        std::memset(line, 0, sizeof(float) * static_cast<std::size_t>(length));
    write = 0;
}

} // namespace youhost
