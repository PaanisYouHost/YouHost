#pragma once

#include "MeterLook.h"

#include <atomic>
#include <cmath>

namespace youmix
{

struct PseSettings
{
    bool bypass = false;
    float thresholdDb = -40.0f;
    float rangeDb = 18.0f;
    float attackMs = 8.0f;
    float holdMs = 40.0f;
    float releaseMs = 100.0f;
    float hysteresisDb = 4.0f;
};

// Per-instance primary source expander. No shared state and no allocation in process().
class PseCore
{
public:
    void setBypass(bool bypass)
    {
        staging_.bypass = bypass;
        publish();
    }

    void setThresholdDb(float db) { setField(&PseSettings::thresholdDb, clamp(db, -80.0f, 0.0f)); }
    void setRangeDb(float db) { setField(&PseSettings::rangeDb, clamp(db, 0.0f, 60.0f)); }
    void setAttackMs(float ms) { setField(&PseSettings::attackMs, clamp(ms, 1.0f, 200.0f)); }
    void setHoldMs(float ms) { setField(&PseSettings::holdMs, clamp(ms, 0.0f, 500.0f)); }
    void setReleaseMs(float ms) { setField(&PseSettings::releaseMs, clamp(ms, 5.0f, 1000.0f)); }
    void setHysteresisDb(float db) { setField(&PseSettings::hysteresisDb, clamp(db, 0.0f, 12.0f)); }

    PseSettings settings() const
    {
        return params_[published_.load(std::memory_order_acquire)];
    }

    float gainFor() const noexcept { return gain_; }
    bool isOpen() const noexcept { return open_; }

    float inputRms() const noexcept { return inputRms_.load(std::memory_order_relaxed); }
    float reductionDb() const noexcept { return reductionDb_.load(std::memory_order_relaxed); }
    int latencySamples() const noexcept { return 0; }

    void process(float* const* channels, int numChannels, int numSamples, double sampleRate) noexcept
    {
        if (channels == nullptr || numSamples <= 0)
            return;

        const PseSettings settings = params_[published_.load(std::memory_order_acquire)];
        const int width = numChannels < 1 ? 0 : (numChannels > 8 ? 8 : numChannels);
        const double rate = sampleRate > 1000.0 ? sampleRate : 48000.0;
        const float envCoeff = 1.0f - std::exp(-1.0f / static_cast<float>(rate * 0.010));
        const int holdSamples = static_cast<int>(settings.holdMs * 0.001f * static_cast<float>(rate));
        const float closed = decibelsToGain(-settings.rangeDb);
        const float openAt = settings.thresholdDb + settings.hysteresisDb * 0.5f;
        const float closeAt = settings.thresholdDb - settings.hysteresisDb * 0.5f;

        double sumSq = 0.0;
        int counted = 0;
        float gain = gain_;
        bool open = open_;
        int hold = holdLeft_;

        for (int index = 0; index < numSamples; ++index)
        {
            float energy = 0.0f;
            int used = 0;
            for (int channel = 0; channel < width; ++channel)
            {
                const float* data = channels[channel];
                if (data == nullptr)
                    continue;
                const float sample = data[index];
                if (sample != sample)
                    continue;
                energy += sample * sample;
                ++used;
                ++counted;
                sumSq += static_cast<double>(sample) * static_cast<double>(sample);
            }
            if (used > 0)
                env_ += envCoeff * ((energy / static_cast<float>(used)) - env_);
            if (env_ < 0.0f)
                env_ = 0.0f;

            const float levelDb = gainToDecibels(std::sqrt(env_));
            if (open)
            {
                if (levelDb < closeAt)
                {
                    if (hold > 0)
                        --hold;
                    else
                        open = false;
                }
                else
                {
                    hold = holdSamples;
                }
            }
            else if (levelDb >= openAt)
            {
                open = true;
                hold = holdSamples;
            }

            const float target = settings.bypass ? 1.0f : (open ? 1.0f : closed);
            const float ms = target >= gain ? settings.attackMs : settings.releaseMs;
            const float seconds = (ms < 1.0f ? 1.0f : ms) * 0.001f;
            const float coeff = 1.0f - std::exp(-1.0f / static_cast<float>(rate * static_cast<double>(seconds)));
            const float applied = gain;
            // Exact unity leaves the buffer alone. A float one-pole stalls just
            // short of 1, so the last inaudible step snaps to the target.
            if (applied == 1.0f && target == 1.0f)
            {
                gain = 1.0f;
            }
            else
            {
                if (applied != 1.0f)
                {
                    for (int channel = 0; channel < width; ++channel)
                    {
                        float* data = channels[channel];
                        if (data != nullptr)
                            data[index] *= applied;
                    }
                }
                const float delta = coeff * (target - gain);
                gain = (gain + delta == gain) ? target : gain + delta;
                if (gain > 1.0f)
                    gain = 1.0f;
                if (gain < 1.0e-8f && target < 1.0e-8f)
                    gain = 0.0f;
            }
        }

        gain_ = gain;
        open_ = open;
        holdLeft_ = hold;
        const float meanSq = counted > 0 ? static_cast<float>(sumSq / static_cast<double>(counted)) : 0.0f;
        inputRms_.store(std::sqrt(meanSq), std::memory_order_relaxed);
        const float reduction = gain >= 1.0f ? 0.0f : -gainToDecibels(gain);
        reductionDb_.store(reduction < 0.0f ? 0.0f : reduction, std::memory_order_relaxed);
    }

private:
    template <typename Field>
    void setField(Field PseSettings::* member, float value)
    {
        staging_.*member = value;
        publish();
    }

    static float clamp(float value, float low, float high) noexcept
    {
        if (value < low)
            return low;
        if (value > high)
            return high;
        return value;
    }

    void publish()
    {
        const int current = published_.load(std::memory_order_relaxed);
        params_[1 - current] = staging_;
        published_.store(1 - current, std::memory_order_release);
    }

    PseSettings staging_ {};
    PseSettings params_[2] {};
    std::atomic<int> published_ { 0 };
    float env_ = 0.0f;
    float gain_ = 1.0f;
    bool open_ = false;
    int holdLeft_ = 0;
    std::atomic<float> inputRms_ { 0.0f };
    std::atomic<float> reductionDb_ { 0.0f };
};

} // namespace youmix
