#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

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

// YouHostMonoNullTest measured 0.000 dB between 1/1 and Fold L 2/2 on
// AUDelay, AUDistortion, AUGraphicEQ, and AUBandpass. Unlinked mono channels
// open 1/1. A linked pair never uses this: the pair stays one stereo process.
inline constexpr bool kUnlinkedMonoUsesMonoBus = true;

inline ChosenLayout layoutForChannel(ChosenLayout opened, bool unlinkedMono, bool monoSupported) noexcept
{
    if (unlinkedMono && kUnlinkedMonoUsesMonoBus && monoSupported && opened != ChosenLayout::none)
        return ChosenLayout::mono;
    return opened;
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

// How many samples a probe has to render before a delayed output can be heard.
// `latencySamples` is the plugin's reported delay. The length is capped so a
// huge latency cannot turn plugin load into a long offline render.
inline int probeRenderSamples(int block, int latencySamples) noexcept
{
    if (block <= 0)
        return 0;
    const int tone = std::min(block * 4, 2048);
    int need = tone;
    if (latencySamples > 0)
        need = std::max(tone, latencySamples + block);
    need = std::min(need, 16384);
    const int padded = ((need + block - 1) / block) * block;
    return std::min(padded, 16384);
}

// One processBlock length. A callback that fits in the prepared maximum is
// one call, even when it is shorter than that maximum. A longer callback is
// cut into prepared-sized pieces. It is never one block bigger than the
// plugin was prepared for, and a short callback does not wait and then run
// the full prepared block inside a single device period.
inline int pluginCallbackSamples(int numSamples, int preparedBlock) noexcept
{
    if (numSamples <= 0 || preparedBlock <= 0)
        return 0;
    return numSamples <= preparedBlock ? numSamples : preparedBlock;
}

// Output at or below this is silence. The probe tone is about -15 dB.
inline constexpr float kProbeSilenceDb = -90.0f;

// A silent reading is not proof the plugin is broken. One short render is
// silence for a lookahead limiter, and a gate is silence until it opens.
// Refusing on that reading turns off working plugins. The audio thread is
// the first real process, on the same layout and block the plugin was prepared for.
inline constexpr bool kRefuseSilentProbe = false;

struct PluginProbeLevels
{
    float lowDb = -120.0f;
    float highDb = -120.0f;
    float leftDb = -120.0f;
    float rightDb = -120.0f;
    float sumDb = -120.0f;
    bool ran = false;
};

inline bool probeLevelsAreSilent(const PluginProbeLevels& levels) noexcept
{
    if (! levels.ran)
        return true;
    const float best = std::max(std::max(levels.lowDb, levels.highDb),
                                std::max(std::max(levels.leftDb, levels.rightDb), levels.sumDb));
    return ! (best > kProbeSilenceDb);
}

// Separate channel storage, main copied into 0..mainIn-1, every other channel
// zero. `process` sees width = max(totalIn, totalOut). A 4/2 sidechain plugin
// therefore gets four distinct pointers, not the stereo host buffer.
template <typename Process>
inline PluginProbeLevels measurePluginProbe(int mainIn,
                                           int mainOut,
                                           int totalIn,
                                           int totalOut,
                                           int block,
                                           double rate,
                                           Process&& process,
                                           int latencySamples = 0)
{
    PluginProbeLevels levels;
    if (mainIn < 1 || mainOut < 1 || totalIn < 1 || totalOut < 1 || block <= 0 || rate <= 0.0)
        return levels;
    const int width = std::max(totalIn, totalOut);
    constexpr int kCap = 8;
    if (width > kCap)
        return levels;

    const int padded = probeRenderSamples(block, latencySamples);
    const int tone = latencySamples > 0 ? std::min(block, std::max(padded - latencySamples, 0))
                                        : std::min(padded, std::min(block * 4, 2048));
    const int measureAt = latencySamples > 0 ? std::min(latencySamples, std::max(padded - std::max(tone, 1), 0)) : 0;
    std::vector<float> storage(static_cast<std::size_t>(width) * static_cast<std::size_t>(padded), 0.0f);
    float* base[kCap] {};
    for (int channel = 0; channel < width; ++channel)
        base[channel] = storage.data() + static_cast<std::size_t>(channel) * static_cast<std::size_t>(padded);

    auto runBlocks = [&](const float* mono, bool copyRight)
    {
        const auto bytes = sizeof(float) * static_cast<std::size_t>(block);
        const int fed = std::min(mainIn, width);
        for (int offset = 0; offset < padded; offset += block)
        {
            float* view[kCap] {};
            for (int channel = 0; channel < width; ++channel)
            {
                view[channel] = base[channel] + offset;
                if (channel == 0 && fed >= 1)
                    std::memcpy(view[channel], mono + offset, bytes);
                else if (channel == 1 && fed >= 2 && copyRight)
                    std::memcpy(view[channel], mono + offset, bytes);
                else
                    std::memset(view[channel], 0, bytes);
            }
            process(view, width, block);
        }
    };

    auto levelOf = [&](int channel)
    {
        if (channel < 0 || channel >= width || base[channel] == nullptr || tone <= 0)
            return -120.0f;
        return amplitudeDb(meanSquare(base[channel] + measureAt, tone));
    };

    std::vector<float> mono(static_cast<std::size_t>(padded), 0.0f);
    fillProbeTone(mono.data(), tone, rate, 80.0f, 0.25f);
    runBlocks(mono.data(), mainIn >= 2);
    levels.lowDb = levelOf(0);

    std::fill(storage.begin(), storage.end(), 0.0f);
    std::fill(mono.begin(), mono.end(), 0.0f);
    fillProbeTone(mono.data(), tone, rate, 6000.0f, 0.25f);
    runBlocks(mono.data(), mainIn >= 2);
    levels.highDb = levelOf(0);

    std::fill(storage.begin(), storage.end(), 0.0f);
    std::fill(mono.begin(), mono.end(), 0.0f);
    fillProbeTone(mono.data(), tone, rate, 1000.0f, 0.25f);
    runBlocks(mono.data(), true);
    levels.leftDb = levelOf(0);
    levels.rightDb = mainOut > 1 && width > 1 ? levelOf(1) : levels.leftDb;
    if (mainOut > 1 && width > 1 && tone > 0)
    {
        const float* left = base[0] + measureAt;
        const float* right = base[1] + measureAt;
        double sum = 0.0;
        for (int index = 0; index < tone; ++index)
        {
            const float mixed = (left[index] + right[index]) * 0.5f;
            sum += static_cast<double>(mixed) * static_cast<double>(mixed);
        }
        levels.sumDb = amplitudeDb(static_cast<float>(sum / static_cast<double>(std::max(tone, 1))));
    }
    else
    {
        levels.sumDb = levels.leftDb;
    }
    levels.ran = true;
    return levels;
}

// 4 inputs (stereo main + stereo sidechain) and 2 outputs. Echoes the main
// pair. A short buffer, a null sidechain pointer, an aliased sidechain, or a
// sidechain that was not zeroed produces silence instead of reading off the
// end of the buffer. That is the crash the live path has to avoid.
struct SidechainPassthrough
{
    int totalIn = 4;
    int totalOut = 2;

    void operator()(float* const* channels, int numChannels, int numSamples) const noexcept
    {
        const int need = std::max(totalIn, totalOut);
        auto silenceOut = [&]()
        {
            if (channels == nullptr || numSamples <= 0)
                return;
            const int outs = std::min(totalOut, numChannels);
            for (int channel = 0; channel < outs; ++channel)
                if (channels[channel] != nullptr)
                    std::memset(channels[channel], 0, sizeof(float) * static_cast<std::size_t>(numSamples));
        };
        if (channels == nullptr || numChannels < need || numSamples <= 0)
        {
            silenceOut();
            return;
        }
        for (int channel = 0; channel < need; ++channel)
        {
            if (channels[channel] == nullptr)
            {
                silenceOut();
                return;
            }
        }
        for (int channel = totalOut; channel < totalIn; ++channel)
        {
            for (int main = 0; main < totalOut; ++main)
            {
                if (channels[channel] == channels[main])
                {
                    silenceOut();
                    return;
                }
            }
        }
        for (int channel = 2; channel < totalIn; ++channel)
        {
            for (int index = 0; index < numSamples; ++index)
            {
                if (channels[channel][index] != 0.0f)
                {
                    silenceOut();
                    return;
                }
            }
        }
    }
};

// Delays the first channel by `delay` samples. A probe that stops before that
// delay hears silence even though the plugin is a straight copy.
struct LookaheadDelay
{
    int delay = 3000;
    mutable std::vector<float> history;

    void operator()(float* const* channels, int numChannels, int numSamples) const
    {
        if (channels == nullptr || numChannels < 1 || channels[0] == nullptr || numSamples <= 0)
            return;
        for (int index = 0; index < numSamples; ++index)
        {
            history.push_back(channels[0][index]);
            const int at = static_cast<int>(history.size()) - 1 - delay;
            channels[0][index] = at >= 0 ? history[static_cast<std::size_t>(at)] : 0.0f;
        }
    }
};

} // namespace youhost
