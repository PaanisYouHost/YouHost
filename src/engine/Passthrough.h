#pragma once

#include "HostLimits.h"
#include "MeterBallistics.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace youhost
{

// Maps a device channel index to the packed pointer index JUCE passes in the
// callback (-1 when that device channel is not open).
struct Routing
{
    std::array<std::int16_t, kMaxChannels> inputPacked {};
    std::array<std::int16_t, kMaxChannels> outputPacked {};
    int inputCount = 0;
    int outputCount = 0;
    int visibleChannels = 0;

    Routing()
    {
        inputPacked.fill(-1);
        outputPacked.fill(-1);
    }
};

struct AudioThreadConfig
{
    Routing routing {};
    MeterTiming meterTiming {};
    int64_t expectedPeriodNs = 0;
};

inline Routing makeRouting(const std::array<bool, kMaxChannels>& inputs,
                           const std::array<bool, kMaxChannels>& outputs)
{
    Routing routing;
    std::int16_t nextInput = 0;
    std::int16_t nextOutput = 0;
    int highest = -1;

    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        if (inputs[static_cast<std::size_t>(channel)])
        {
            routing.inputPacked[static_cast<std::size_t>(channel)] = nextInput++;
            highest = channel;
        }
        if (outputs[static_cast<std::size_t>(channel)])
        {
            routing.outputPacked[static_cast<std::size_t>(channel)] = nextOutput++;
            highest = std::max(highest, channel);
        }
    }

    routing.inputCount = nextInput;
    routing.outputCount = nextOutput;
    routing.visibleChannels = highest + 1;
    return routing;
}

// Realtime: no allocation, no locks, no logging.
// Input device-channel n is copied to output device-channel n. Meters read the raw input.
inline void processPassthrough(const float* const* inputs,
                               int numInputs,
                               float* const* outputs,
                               int numOutputs,
                               int numSamples,
                               const AudioThreadConfig& config,
                               ChannelStrip* strips,
                               int stripCount)
{
    if (numSamples <= 0)
        return;

    const BlockMeterGains gains = blockMeterGains(config.meterTiming, numSamples);
    std::array<bool, kMaxChannels> outputWritten {};

    const int channels = std::min(stripCount, kMaxChannels);
    for (int channel = 0; channel < channels; ++channel)
    {
        const int inputIndex = config.routing.inputPacked[static_cast<std::size_t>(channel)];
        const int outputIndex = config.routing.outputPacked[static_cast<std::size_t>(channel)];

        const float* input = nullptr;
        if (inputs != nullptr && inputIndex >= 0 && inputIndex < numInputs)
            input = inputs[inputIndex];

        float* output = nullptr;
        if (outputs != nullptr && outputIndex >= 0 && outputIndex < numOutputs)
            output = outputs[outputIndex];

        if (output != nullptr)
        {
            if (input != nullptr)
                std::memcpy(output, input, sizeof(float) * static_cast<std::size_t>(numSamples));
            else
                std::memset(output, 0, sizeof(float) * static_cast<std::size_t>(numSamples));

            if (outputIndex >= 0 && outputIndex < kMaxChannels)
                outputWritten[static_cast<std::size_t>(outputIndex)] = true;
        }

        if (strips == nullptr)
            continue;

        if (input != nullptr)
            updateMeter(strips[channel], input, numSamples, gains);
        else if (strips[channel].meter.clearRequested.exchange(false, std::memory_order_relaxed))
        {
            strips[channel].meterState.clipped = false;
            strips[channel].meter.clipped.store(false, std::memory_order_relaxed);
        }
    }

    if (outputs == nullptr)
        return;

    for (int index = 0; index < numOutputs; ++index)
    {
        const bool alreadyWritten = index < kMaxChannels && outputWritten[static_cast<std::size_t>(index)];
        if (! alreadyWritten && outputs[index] != nullptr)
            std::memset(outputs[index], 0, sizeof(float) * static_cast<std::size_t>(numSamples));
    }
}

} // namespace youhost
