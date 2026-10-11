#pragma once

#include "HostLimits.h"

#include <cstring>
#include <string>

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

// Right-click on a HOST strip. N is the odd channel's number, so ch 1 says
// "Stereo link with ch 2". The same words are used from either strip of the pair.
inline std::string stereoLinkMenuLabel(int channel)
{
    const int left = stereoLeftIndex(channel);
    if (left < 0)
        return {};
    return "Stereo link with ch " + std::to_string(left + 2);
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
