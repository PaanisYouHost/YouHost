#pragma once

#include "ChannelStrip.h"
#include "HostLimits.h"

#include <cmath>
#include <cstdint>

namespace youhost
{

struct AudioThreadConfig;

struct BlockMeterGains
{
    float rmsKeep = 0.0f;
    float peakKeep = 0.0f;
    int peakHoldSamples = 0;
};

// base is in (0, 1]. No libm call, so the audio thread does not depend on errno locks.
inline float raiseUnit(float base, int exponent)
{
    if (exponent <= 0)
        return 1.0f;
    if (base <= 0.0f)
        return 0.0f;

    float result = 1.0f;
    while (exponent > 0)
    {
        if ((exponent & 1) != 0)
            result *= base;
        base *= base;
        exponent >>= 1;
    }
    return result;
}

inline float perSampleKeep(double sampleRate, float seconds)
{
    if (sampleRate <= 0.0 || seconds <= 0.0f)
        return 0.0f;
    return static_cast<float>(std::exp(-1.0 / (sampleRate * static_cast<double>(seconds))));
}

struct MeterTiming
{
    float perSampleRmsKeep = 0.0f;
    float perSamplePeakKeep = 0.0f;
    int peakHoldSamples = 0;
};

inline MeterTiming meterTimingFor(double sampleRate)
{
    MeterTiming timing;
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    timing.perSampleRmsKeep = perSampleKeep(rate, kRmsWindowSeconds);
    timing.perSamplePeakKeep = perSampleKeep(rate, kPeakDecaySeconds);
    timing.peakHoldSamples = static_cast<int>(std::lround(static_cast<double>(kPeakHoldSeconds) * rate));
    return timing;
}

inline BlockMeterGains blockMeterGains(const MeterTiming& timing, int numSamples)
{
    BlockMeterGains gains;
    gains.rmsKeep = raiseUnit(timing.perSampleRmsKeep, numSamples);
    gains.peakKeep = raiseUnit(timing.perSamplePeakKeep, numSamples);
    gains.peakHoldSamples = timing.peakHoldSamples;
    return gains;
}

// Audio thread only. `samples` is non-null and numSamples > 0.
inline void updateMeter(ChannelStrip& strip, const float* samples, int numSamples, const BlockMeterGains& gains)
{
    // The clip mark stays on until the UI asks for it to be cleared.
    if (strip.meter.clearRequested.exchange(false, std::memory_order_relaxed))
        strip.meterState.clipped = false;

    float peak = 0.0f;
    double sumSquares = 0.0;
    bool clipped = false;

    for (int i = 0; i < numSamples; ++i)
    {
        const float sample = samples[i];
        const float absolute = std::fabs(sample);
        if (absolute > peak)
            peak = absolute;
        if (absolute >= 1.0f)
            clipped = true;
        sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
    }

    const float meanSquare = static_cast<float>(sumSquares / static_cast<double>(numSamples));
    strip.meterState.meanSquare = strip.meterState.meanSquare * gains.rmsKeep
                                 + meanSquare * (1.0f - gains.rmsKeep);

    if (peak >= strip.meterState.heldPeak)
    {
        strip.meterState.heldPeak = peak;
        strip.meterState.peakHoldSamples = gains.peakHoldSamples;
    }
    else if (strip.meterState.peakHoldSamples > numSamples)
    {
        strip.meterState.peakHoldSamples -= numSamples;
    }
    else
    {
        strip.meterState.peakHoldSamples = 0;
        strip.meterState.heldPeak *= gains.peakKeep;
    }

    if (clipped)
        strip.meterState.clipped = true;

    strip.meter.rms.store(std::sqrt(strip.meterState.meanSquare), std::memory_order_relaxed);
    strip.meter.peak.store(strip.meterState.heldPeak, std::memory_order_relaxed);
    strip.meter.clipped.store(strip.meterState.clipped, std::memory_order_relaxed);
}

// Audio thread only. A channel that is off holds a dark meter instead of coasting.
inline void parkMeter(ChannelStrip& strip) noexcept
{
    if (strip.meter.clearRequested.exchange(false, std::memory_order_relaxed))
        strip.meterState.clipped = false;

    strip.meterState.meanSquare = 0.0f;
    strip.meterState.heldPeak = 0.0f;
    strip.meterState.peakHoldSamples = 0;
    strip.meter.rms.store(0.0f, std::memory_order_relaxed);
    strip.meter.peak.store(0.0f, std::memory_order_relaxed);
    strip.meter.clipped.store(strip.meterState.clipped, std::memory_order_relaxed);
}

} // namespace youhost
