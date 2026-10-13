#pragma once

#include <algorithm>
#include <cmath>
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

// Zero scratch that is not the live mono buffer, then copy the current input.
// A right channel left from the previous block is the plugin's own output.
// Feeding that back in is the loop that builds up and clips.
inline void preparePluginBlock(float* const* plugin,
                               int inputChannels,
                               int outputChannels,
                               const float* mono,
                               int numSamples) noexcept
{
    if (plugin == nullptr || numSamples <= 0)
        return;
    const int width = std::max(inputChannels, outputChannels);
    const auto bytes = sizeof(float) * static_cast<std::size_t>(numSamples);
    for (int index = 0; index < width && index < 2; ++index)
    {
        float* dest = plugin[index];
        if (dest == nullptr || dest == mono)
            continue;
        std::memset(dest, 0, bytes);
    }
    stagePluginChannels(plugin, inputChannels, outputChannels, mono, numSamples);
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

// Stereo plugin on a mono channel. Use L matches a mono destination in REAPER.
// Sum is (L+R)/2, so identical sides stay at full level and inverted sides cancel.
enum class StereoFold
{
    left = 0,
    right = 1,
    sum = 2
};

inline StereoFold stereoFoldFromInt(int value) noexcept
{
    if (value == static_cast<int>(StereoFold::right))
        return StereoFold::right;
    if (value == static_cast<int>(StereoFold::sum))
        return StereoFold::sum;
    return StereoFold::left;
}

inline const char* stereoFoldToken(StereoFold fold) noexcept
{
    if (fold == StereoFold::right)
        return "R";
    if (fold == StereoFold::sum)
        return "sum";
    return "L";
}

inline StereoFold stereoFoldFromToken(const char* token) noexcept
{
    if (token == nullptr)
        return StereoFold::left;
    if (token[0] == 'R' && token[1] == '\0')
        return StereoFold::right;
    if (token[0] == 's')
        return StereoFold::sum;
    return StereoFold::left;
}

inline void takeFoldedChannel(float* mono,
                              const float* const* plugin,
                              int outputChannels,
                              int numSamples,
                              StereoFold fold) noexcept
{
    if (mono == nullptr || plugin == nullptr || outputChannels <= 1 || numSamples <= 0)
        return;
    if (fold == StereoFold::left || plugin[1] == nullptr)
    {
        takePluginChannel(mono, plugin, outputChannels, numSamples);
        return;
    }
    const float* right = plugin[1];
    if (fold == StereoFold::right)
    {
        if (right != mono)
            std::memcpy(mono, right, sizeof(float) * static_cast<std::size_t>(numSamples));
        return;
    }
    const float* left = plugin[0];
    for (int index = 0; index < numSamples; ++index)
    {
        const float leftSample = left != nullptr ? left[index] : 0.0f;
        mono[index] = (leftSample + right[index]) * 0.5f;
    }
}

// One mono channel through a stereo (or mono) plugin. Scratch belongs to this
// slot only. The plugin never sees the previous block's output.
template <typename Process>
inline void renderFoldedMono(float* mono,
                             int numSamples,
                             int inputChannels,
                             int outputChannels,
                             StereoFold fold,
                             float* scratchLeft,
                             float* scratchRight,
                             Process&& process)
{
    if (mono == nullptr || numSamples <= 0 || scratchLeft == nullptr)
        return;
    const int width = std::max(1, std::max(inputChannels, outputChannels));
    if (width > 1 && scratchRight == nullptr)
        return;
    float* plugin[2] = { scratchLeft, scratchRight };
    preparePluginBlock(plugin, inputChannels, outputChannels, mono, numSamples);
    const int viewWidth = std::min(width, 2);
    process(plugin, viewWidth, numSamples);
    if (viewWidth <= 1)
    {
        if (scratchLeft != mono)
            std::memcpy(mono, scratchLeft, sizeof(float) * static_cast<std::size_t>(numSamples));
        return;
    }
    takeFoldedChannel(mono, plugin, outputChannels, numSamples, fold);
}

inline bool sameFloatBits(float left, float right) noexcept
{
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::memcpy(&a, &left, sizeof(a));
    std::memcpy(&b, &right, sizeof(b));
    return a == b;
}

// Above full scale, so a real clip still latches, and finite, so a meter can fall.
inline constexpr float kPluginOutputCeiling = 4.0f;

struct SanitizeReport
{
    bool nonFinite = false;
    bool clamped = false;
};

// NaN and Inf become silence. Finite values outside the ceiling are clamped.
// No allocation. The caller logs on the message thread when nonFinite is set.
inline SanitizeReport sanitizeAudioBlock(float* data, int numSamples) noexcept
{
    SanitizeReport report;
    if (data == nullptr || numSamples <= 0)
        return report;
    for (int index = 0; index < numSamples; ++index)
    {
        const float sample = data[index];
        if (! std::isfinite(sample))
        {
            data[index] = 0.0f;
            report.nonFinite = true;
        }
        else if (sample > kPluginOutputCeiling)
        {
            data[index] = kPluginOutputCeiling;
            report.clamped = true;
        }
        else if (sample < -kPluginOutputCeiling)
        {
            data[index] = -kPluginOutputCeiling;
            report.clamped = true;
        }
    }
    return report;
}

// Recordings stay bit-exact for finite samples. Only NaN and Inf are silenced.
inline void silenceNonFinite(float* data, int numSamples) noexcept
{
    if (data == nullptr || numSamples <= 0)
        return;
    for (int index = 0; index < numSamples; ++index)
        if (! std::isfinite(data[index]))
            data[index] = 0.0f;
}

inline float finiteTrimGain(float linear) noexcept
{
    if (! std::isfinite(linear))
        return 1.0f;
    return linear;
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
