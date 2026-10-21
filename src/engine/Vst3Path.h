#pragma once

#include "CpuMeter.h"
#include "SessionChannels.h"

#include <cstdint>

namespace youhost
{

// One instance stays on one core. The stress count below does not add a
// pool wait on top of the plugin time.
inline bool vst3ProcessOnAudioThread() noexcept
{
    return true;
}

// A shorter callback is legal. Only a callback larger than the prepared
// block asks for a new prepare, and the caller still waits for a streak.
inline bool blockAsksReprepare(int callbackSamples, int preparedSamples) noexcept
{
    return preparedSamples > 0 && callbackSamples > preparedSamples;
}

// Four heavy inserts at 32 samples. Channels past the open input are not called.
// The accounted plugin time is what the meter must show, not a latched 52.
struct Vst3StressResult
{
    int liveCalls = 0;
    int hiddenCalls = 0;
    int reprepares = 0;
    int waits = 0;
    int callbackPercent = 0;
    int meter = 0;
};

inline Vst3StressResult runHeavyVst3At32(int blocks) noexcept
{
    constexpr int kChannels = 8;
    int inputPacked[kChannels];
    for (int channel = 0; channel < kChannels; ++channel)
        inputPacked[channel] = -1;
    inputPacked[0] = 0;
    // Channel 1 is live. Channels 6 and 8 are loaded but hidden.
    const int pluginChannel[3] = { 0, 5, 7 };
    Vst3StressResult result;
    std::uint64_t liveNs = 0;
    if (blocks < 1)
        blocks = 1;
    for (int block = 0; block < blocks; ++block)
    {
        if (blockAsksReprepare(32, 32))
            ++result.reprepares;
        if (! vst3ProcessOnAudioThread())
            ++result.waits;
        for (int plugin = 0; plugin < 3; ++plugin)
        {
            const int channel = pluginChannel[plugin];
            if (! channelProcessesPlugins(channel, 1, inputPacked[channel]))
            {
                ++result.hiddenCalls;
                continue;
            }
            ++result.liveCalls;
            liveNs += 20000;
        }
    }
    const auto perBlock = liveNs / static_cast<std::uint64_t>(blocks);
    result.callbackPercent = callbackLoadPercent(perBlock, 32, 48000.0);
    result.meter = dspReading(result.callbackPercent, 52);
    return result;
}

} // namespace youhost
