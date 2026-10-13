#pragma once

#include "ChannelStrip.h"
#include "HostLimits.h"

#include "SignalPath.h"

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

inline void healMeterState(MeterState& state) noexcept
{
    if (! std::isfinite(state.meanSquare) || state.meanSquare < 0.0f)
        state.meanSquare = 0.0f;
    if (! std::isfinite(state.heldPeak) || state.heldPeak < 0.0f)
        state.heldPeak = 0.0f;
    if (state.peakHoldSamples < 0)
        state.peakHoldSamples = 0;
}

inline void clearMeterBallistics(MeterState& state, MeterSnapshot& meter) noexcept
{
    state = {};
    meter.rms.store(0.0f, std::memory_order_relaxed);
    meter.peak.store(0.0f, std::memory_order_relaxed);
    meter.clipped.store(false, std::memory_order_relaxed);
    meter.clearRequested.store(false, std::memory_order_relaxed);
    meter.resetRequested.store(false, std::memory_order_relaxed);
}

inline void clearMeterBallistics(ChannelStrip& strip) noexcept
{
    clearMeterBallistics(strip.meterState, strip.meter);
    clearMeterBallistics(strip.outputMeterState, strip.outputMeter);
}

// A non-finite sample cannot enter the ballistics. Huge finite values are
// clamped so one garbage block cannot pin the meter at the top.
inline float meterSample(float sample) noexcept
{
    if (! std::isfinite(sample))
        return 0.0f;
    if (sample > kPluginOutputCeiling)
        return kPluginOutputCeiling;
    if (sample < -kPluginOutputCeiling)
        return -kPluginOutputCeiling;
    return sample;
}

// Audio thread only. `samples` is non-null and numSamples > 0.
inline void updateMeter(MeterState& state, MeterSnapshot& meter, const float* samples, int numSamples, const BlockMeterGains& gains)
{
    // A plugin-chain change drops the level and the clip immediately.
    if (meter.resetRequested.exchange(false, std::memory_order_relaxed))
    {
        state = {};
        meter.clearRequested.store(false, std::memory_order_relaxed);
    }
    else if (meter.clearRequested.exchange(false, std::memory_order_relaxed))
    {
        // The clip mark stays on until the UI asks for it to be cleared.
        state.clipped = false;
    }

    healMeterState(state);

    float peak = 0.0f;
    double sumSquares = 0.0;
    bool clipped = false;

    for (int i = 0; i < numSamples; ++i)
    {
        const float sample = meterSample(samples[i]);
        const float absolute = std::fabs(sample);
        if (absolute > peak)
            peak = absolute;
        if (absolute >= 1.0f)
            clipped = true;
        sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
    }

    const float meanSquare = static_cast<float>(sumSquares / static_cast<double>(numSamples));
    state.meanSquare = state.meanSquare * gains.rmsKeep + meanSquare * (1.0f - gains.rmsKeep);

    if (peak >= state.heldPeak)
    {
        state.heldPeak = peak;
        state.peakHoldSamples = gains.peakHoldSamples;
    }
    else if (state.peakHoldSamples > numSamples)
    {
        state.peakHoldSamples -= numSamples;
    }
    else
    {
        state.peakHoldSamples = 0;
        state.heldPeak *= gains.peakKeep;
    }

    healMeterState(state);

    if (clipped)
        state.clipped = true;

    meter.rms.store(std::sqrt(state.meanSquare), std::memory_order_relaxed);
    meter.peak.store(state.heldPeak, std::memory_order_relaxed);
    meter.clipped.store(state.clipped, std::memory_order_relaxed);
}

inline void updateMeter(ChannelStrip& strip, const float* samples, int numSamples, const BlockMeterGains& gains)
{
    updateMeter(strip.meterState, strip.meter, samples, numSamples, gains);
}

// Audio thread only. A channel that is off holds a dark meter instead of coasting.
inline void parkMeter(MeterState& state, MeterSnapshot& meter) noexcept
{
    if (meter.resetRequested.exchange(false, std::memory_order_relaxed))
    {
        state.clipped = false;
        meter.clearRequested.store(false, std::memory_order_relaxed);
    }
    else if (meter.clearRequested.exchange(false, std::memory_order_relaxed))
        state.clipped = false;

    state.meanSquare = 0.0f;
    state.heldPeak = 0.0f;
    state.peakHoldSamples = 0;
    meter.rms.store(0.0f, std::memory_order_relaxed);
    meter.peak.store(0.0f, std::memory_order_relaxed);
    meter.clipped.store(state.clipped, std::memory_order_relaxed);
}

inline void parkMeter(ChannelStrip& strip) noexcept
{
    parkMeter(strip.meterState, strip.meter);
}

} // namespace youhost
