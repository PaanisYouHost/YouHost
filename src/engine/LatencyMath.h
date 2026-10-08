#pragma once

#include <cstdint>
#include <string_view>

namespace youhost
{

// How a backend's reported input and output latency relate to the buffer.
// JUCE 9 CoreAudio adds the current buffer size to both sides (device latency
// + safety offset + buffer + stream latency). ALSA reports period * (periods - 1),
// which already leaves one period out.
enum class RoundTripFormula
{
    coreAudioSubtractOneBuffer,
    alsaAddOneBuffer,
    driverSum
};

struct LatencyNumbers
{
    double sampleRate = 0.0;
    int bufferSamples = 0;
    int inputSamples = 0;
    int outputSamples = 0;
    int compensationSamples = 0;
    int roundTripSamples = 0;
    int xruns = -1;
    RoundTripFormula formula = RoundTripFormula::driverSum;
    bool deviceOpen = false;
};

inline RoundTripFormula formulaForDeviceType(std::string_view type)
{
    if (type == "CoreAudio")
        return RoundTripFormula::coreAudioSubtractOneBuffer;
    if (type == "ALSA")
        return RoundTripFormula::alsaAddOneBuffer;
    return RoundTripFormula::driverSum;
}

inline int roundTripSamples(int inputSamples,
                            int outputSamples,
                            int bufferSamples,
                            int compensationSamples,
                            RoundTripFormula formula)
{
    long long total = static_cast<long long>(inputSamples)
                    + static_cast<long long>(outputSamples)
                    + static_cast<long long>(compensationSamples);

    if (formula == RoundTripFormula::coreAudioSubtractOneBuffer)
        total -= bufferSamples;
    else if (formula == RoundTripFormula::alsaAddOneBuffer)
        total += bufferSamples;

    if (total < 0)
        return 0;
    if (total > 2147483647LL)
        return 2147483647;
    return static_cast<int>(total);
}

inline double samplesToMilliseconds(int samples, double sampleRate)
{
    if (sampleRate <= 0.0)
        return 0.0;
    return 1000.0 * static_cast<double>(samples) / sampleRate;
}

} // namespace youhost
