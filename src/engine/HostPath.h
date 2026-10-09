#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace youhost
{

// What the plugin accepted, and the layout it opened with before YouHost
// changed buses. REAPER keeps that opened layout. Forcing a stereo effect
// down to mono is the thin path.
struct OpenedLayout
{
    int openInputs = 0;
    int openOutputs = 0;
    bool mono = false;
    bool side = false;
    bool stereo = false;
};

enum class ChosenLayout
{
    none,
    mono,
    side,
    stereo
};

inline ChosenLayout chooseOpenedLayout(const OpenedLayout& offer) noexcept
{
    const bool opensStereo = offer.openOutputs >= 2 || offer.openInputs >= 2;
    const bool opensMono = ! opensStereo && (offer.openInputs > 0 || offer.openOutputs > 0);
    if (opensStereo && offer.stereo)
        return ChosenLayout::stereo;
    if (opensMono && offer.mono)
        return ChosenLayout::mono;
    if (offer.stereo)
        return ChosenLayout::stereo;
    if (offer.mono)
        return ChosenLayout::mono;
    if (offer.side)
        return ChosenLayout::side;
    return ChosenLayout::none;
}

inline const char* chosenLayoutName(ChosenLayout layout) noexcept
{
    if (layout == ChosenLayout::mono)
        return "1/1";
    if (layout == ChosenLayout::side)
        return "1/2";
    if (layout == ChosenLayout::stereo)
        return "2/2";
    return "none";
}

// A callback that is an exact multiple of the prepared block can be sliced
// with no extra delay. Anything else has to wait for a full block.
inline bool pluginBlockFeedsDirect(int numSamples, int preparedBlock) noexcept
{
    if (numSamples <= 0 || preparedBlock <= 0)
        return false;
    if (numSamples < preparedBlock)
        return false;
    return numSamples % preparedBlock == 0;
}

// How many processed samples a short callback can emit after `callbacks`
// arrivals. The first blocks stay inside the queue, which is the extra delay.
inline int fedSamplesAfter(int preparedBlock, int callback, int callbacks) noexcept
{
    if (preparedBlock <= 0 || callback <= 0 || callbacks <= 0)
        return 0;
    int held = 0;
    int ready = 0;
    int emitted = 0;
    for (int index = 0; index < callbacks; ++index)
    {
        held += callback;
        while (held >= preparedBlock)
        {
            held -= preparedBlock;
            ready += preparedBlock;
        }
        const int take = std::min(callback, ready);
        ready -= take;
        emitted += take;
    }
    return emitted;
}

inline void ringPush(float* data, int capacity, int& write, int& count, const float* src, int numSamples) noexcept
{
    if (data == nullptr || src == nullptr || capacity <= 0 || numSamples <= 0)
        return;
    const int room = capacity - count;
    if (room <= 0)
        return;
    const int amount = std::min(numSamples, room);
    for (int index = 0; index < amount; ++index)
    {
        data[write] = src[index];
        write = write + 1 >= capacity ? 0 : write + 1;
    }
    count += amount;
}

inline int ringPop(const float* data, int capacity, int& read, int& count, float* dest, int numSamples) noexcept
{
    if (data == nullptr || dest == nullptr || capacity <= 0 || numSamples <= 0 || count <= 0)
        return 0;
    const int amount = std::min(numSamples, count);
    for (int index = 0; index < amount; ++index)
    {
        dest[index] = data[read];
        read = read + 1 >= capacity ? 0 : read + 1;
    }
    count -= amount;
    return amount;
}

inline int ringRead(const float* data, int capacity, int read, int count, float* dest, int numSamples) noexcept
{
    if (data == nullptr || dest == nullptr || capacity <= 0 || numSamples <= 0 || count < numSamples)
        return 0;
    int cursor = read;
    for (int index = 0; index < numSamples; ++index)
    {
        dest[index] = data[cursor];
        cursor = cursor + 1 >= capacity ? 0 : cursor + 1;
    }
    return numSamples;
}

inline void ringDrop(int capacity, int& read, int& count, int numSamples) noexcept
{
    if (capacity <= 0 || numSamples <= 0 || count <= 0)
        return;
    const int amount = std::min(numSamples, count);
    read += amount;
    if (read >= capacity)
        read %= capacity;
    count -= amount;
}

inline float meanSquare(const float* data, int numSamples) noexcept
{
    if (data == nullptr || numSamples <= 0)
        return 0.0f;
    double sum = 0.0;
    for (int index = 0; index < numSamples; ++index)
    {
        const double sample = static_cast<double>(data[index]);
        sum += sample * sample;
    }
    return static_cast<float>(sum / static_cast<double>(numSamples));
}

inline float amplitudeDb(float meanSquareValue) noexcept
{
    if (! (meanSquareValue > 0.0f))
        return -120.0f;
    return static_cast<float>(10.0 * std::log10(static_cast<double>(meanSquareValue)));
}

// Message-thread probe length. Plugin latency can be enormous; never render it all.
inline int probeSkipSamples(int latencySamples, int cap) noexcept
{
    if (latencySamples <= 0 || cap <= 0)
        return 0;
    return std::min(latencySamples, cap / 2);
}

inline void fillProbeTone(float* dest, int numSamples, double sampleRate, float frequency, float amplitude) noexcept
{
    if (dest == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return;
    const double step = 2.0 * 3.14159265358979323846 * static_cast<double>(frequency) / sampleRate;
    for (int index = 0; index < numSamples; ++index)
        dest[index] = amplitude * static_cast<float>(std::sin(step * static_cast<double>(index)));
}

// Reported channel names versus the bits actually open. A solid prefix means
// channels 0..active-1 are on and the rest are off, so a single missing tail
// channel is an off-by-one rather than a hole the user punched in the middle.
struct ChannelMaskView
{
    int reported = 0;
    int active = 0;
    bool solidPrefix = false;
};

inline ChannelMaskView inspectChannelMask(const bool* open, int reported) noexcept
{
    ChannelMaskView view;
    if (open == nullptr || reported <= 0)
        return view;
    view.reported = reported;
    for (int index = 0; index < reported; ++index)
        if (open[index])
            ++view.active;
    view.solidPrefix = true;
    for (int index = 0; index < reported; ++index)
    {
        const bool want = index < view.active;
        if (open[index] != want)
            view.solidPrefix = false;
    }
    return view;
}

inline bool trailingOutputMissing(const ChannelMaskView& view) noexcept
{
    return view.solidPrefix && view.reported == view.active + 1 && view.active > 0;
}

} // namespace youhost
