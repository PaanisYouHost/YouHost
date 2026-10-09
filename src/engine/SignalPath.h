#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace youhost
{

// The dry path is a copy. It does not filter, flip polarity, dither, or mix a
// delayed copy back in. A mix of a signal with a slightly delayed or inverted
// copy is what thins the low end and lifts the highs. These helpers exist so
// the audio thread and the null test share one implementation.

inline bool pluginSlotRuns(bool occupied, bool bypassed, bool bypassAll) noexcept
{
    return occupied && ! bypassed && ! bypassAll;
}

// How many plugin inputs receive this mono channel.
// Mono-in/stereo-out has one input. The extra channel is an output and must
// stay silent: copying the mono signal into it makes the plugin see stereo,
// and a one-buffer-old or offset second input is a comb if the plugin sums.
inline int pluginInputsFed(int inputChannels) noexcept
{
    if (inputChannels < 1)
        return 1;
    if (inputChannels > 2)
        return 2;
    return inputChannels;
}

inline void stagePluginChannels(float* const* plugin,
                                int inputChannels,
                                int outputChannels,
                                const float* mono,
                                int numSamples) noexcept
{
    if (plugin == nullptr || mono == nullptr || numSamples <= 0)
        return;

    const int width = std::max(inputChannels, outputChannels);
    const int fed = pluginInputsFed(inputChannels);
    const auto bytes = sizeof(float) * static_cast<std::size_t>(numSamples);
    for (int index = 0; index < width && index < 2; ++index)
    {
        float* dest = plugin[index];
        if (dest == nullptr || dest == mono)
            continue;
        if (index < fed)
            std::memcpy(dest, mono, bytes);
        else
            std::memset(dest, 0, bytes);
    }
}

// Keep the first plugin output. Never add or subtract the other channel.
// Adding a delayed right side combs. Subtracting it is a high-pass.
inline void takePluginChannel(float* mono,
                              const float* const* plugin,
                              int outputChannels,
                              int numSamples) noexcept
{
    if (mono == nullptr || plugin == nullptr || outputChannels <= 1 || numSamples <= 0)
        return;
    const float* first = plugin[0];
    if (first == nullptr || first == mono)
        return;
    std::memcpy(mono, first, sizeof(float) * static_cast<std::size_t>(numSamples));
}

inline bool sameFloatBits(float left, float right) noexcept
{
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::memcpy(&a, &left, sizeof(a));
    std::memcpy(&b, &right, sizeof(b));
    return a == b;
}

// Unity gain is the bit pattern of 1 and is not applied. Any other value is a straight multiply.
inline void applyOutputTrim(float* data, int numSamples, float linear) noexcept
{
    if (data == nullptr || numSamples <= 0 || sameFloatBits(linear, 1.0f))
        return;
    for (int index = 0; index < numSamples; ++index)
        data[index] *= linear;
}

} // namespace youhost
