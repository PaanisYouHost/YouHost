#pragma once

#include "MeterLook.h"

#include <atomic>
#include <cmath>

namespace youmix
{

inline constexpr int kPseSlots = 16;

// Levels shared by every PSE instance in this process. Each instance keeps
// its own expander settings. Slots are claimed on the message thread.
class PseBus
{
public:
    int claim()
    {
        for (int slot = 0; slot < kPseSlots; ++slot)
        {
            int expected = 0;
            if (active_[slot].compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
            {
                levelDb_[slot].store(-160.0f, std::memory_order_relaxed);
                return slot;
            }
        }
        return -1;
    }

    void release(int slot)
    {
        if (slot < 0 || slot >= kPseSlots)
            return;
        levelDb_[slot].store(-160.0f, std::memory_order_relaxed);
        active_[slot].store(0, std::memory_order_release);
    }

    void publishLevel(int slot, float db) noexcept
    {
        if (slot >= 0 && slot < kPseSlots)
            levelDb_[slot].store(db, std::memory_order_relaxed);
    }

    float loudestOtherDb(int slot) const noexcept
    {
        float best = -160.0f;
        for (int other = 0; other < kPseSlots; ++other)
        {
            if (other == slot || active_[other].load(std::memory_order_acquire) == 0)
                continue;
            const float level = levelDb_[other].load(std::memory_order_relaxed);
            if (level > best)
                best = level;
        }
        return best;
    }

private:
    std::atomic<int> active_[kPseSlots] {};
    std::atomic<float> levelDb_[kPseSlots] {};
};

inline PseBus& pseSharedBus()
{
    static PseBus bus;
    return bus;
}

struct PseSettings
{
    bool bypass = false;
    float thresholdDb = -40.0f;
    float rangeDb = 18.0f;
    float attackMs = 12.0f;
    float hangMs = 80.0f;
    float releaseMs = 140.0f;
    float kneeDb = 12.0f;
};

// Smooth primary-source expander. Gain follows a soft-knee downward curve and
// is capped at Range, so it never snaps open or shut. A slow noise-floor
// tracker can raise the threshold. Other PSE instances in this process pull a
// quieter mic down when a louder one is the source.
class PseCore
{
public:
    explicit PseCore(PseBus* bus = nullptr)
        : bus_(bus)
    {
        if (bus_ != nullptr)
            slot_ = bus_->claim();
        publish();
    }

    ~PseCore()
    {
        if (bus_ != nullptr)
            bus_->release(slot_);
    }

    PseCore(const PseCore&) = delete;
    PseCore& operator=(const PseCore&) = delete;

    void setBypass(bool bypass)
    {
        staging_.bypass = bypass;
        publish();
    }

    void setThresholdDb(float db) { setField(&PseSettings::thresholdDb, clamp(db, -80.0f, 0.0f)); }
    void setRangeDb(float db) { setField(&PseSettings::rangeDb, clamp(db, 0.0f, 60.0f)); }
    void setAttackMs(float ms) { setField(&PseSettings::attackMs, clamp(ms, 1.0f, 200.0f)); }
    void setHangMs(float ms) { setField(&PseSettings::hangMs, clamp(ms, 0.0f, 500.0f)); }
    void setReleaseMs(float ms) { setField(&PseSettings::releaseMs, clamp(ms, 5.0f, 1000.0f)); }
    void setKneeDb(float db) { setField(&PseSettings::kneeDb, clamp(db, 0.0f, 24.0f)); }

    PseSettings settings() const
    {
        return params_[published_.load(std::memory_order_acquire)];
    }

    float gainFor() const noexcept { return gain_; }
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
        const float envCoeff = 1.0f - std::exp(-1.0f / static_cast<float>(rate * 0.020));
        const float otherDb = bus_ != nullptr ? bus_->loudestOtherDb(slot_) : -160.0f;

        double sumSq = 0.0;
        int counted = 0;
        float gain = gain_;
        float lastLevel = -160.0f;

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
                if (std::isnan(sample))
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
            lastLevel = levelDb;
            followFloor(levelDb, rate);

            float reduction = 0.0f;
            if (! settings.bypass)
            {
                float threshold = settings.thresholdDb;
                const float lifted = floorDb_ + 9.0f;
                if (lifted > threshold)
                    threshold = lifted;
                reduction = downwardDb(levelDb, threshold, settings.kneeDb, kRatio, settings.rangeDb);
                if (otherDb > -120.0f)
                {
                    const float gap = otherDb - levelDb;
                    if (gap > 6.0f)
                        reduction += (gap - 6.0f) * 0.5f;
                }
                if (reduction > settings.rangeDb)
                    reduction = settings.rangeDb;
                if (reduction < 0.0f)
                    reduction = 0.0f;
            }

            const float openNow = reduction < 3.0f ? 1.0f : 0.0f;
            const float hangMs = settings.hangMs < 1.0f ? 1.0f : settings.hangMs;
            const float rise = 1.0f - std::exp(-1.0f / static_cast<float>(rate * 0.008));
            const float fall = 1.0f - std::exp(-1.0f / static_cast<float>(rate * static_cast<double>(hangMs) * 0.001));
            const float follow = openNow > activity_ ? rise : fall;
            activity_ += follow * (openNow - activity_);

            const float target = settings.bypass ? 1.0f : decibelsToGain(-reduction);
            const float releaseMs = settings.releaseMs + settings.hangMs * activity_;
            const float ms = target >= gain ? settings.attackMs : releaseMs;
            const float seconds = (ms < 1.0f ? 1.0f : ms) * 0.001f;
            const float coeff = 1.0f - std::exp(-1.0f / static_cast<float>(rate * static_cast<double>(seconds)));
            const float applied = gain;
            if (floatBitsEqual(applied, 1.0f) && floatBitsEqual(target, 1.0f))
            {
                gain = 1.0f;
            }
            else
            {
                if (! floatBitsEqual(applied, 1.0f))
                {
                    for (int channel = 0; channel < width; ++channel)
                    {
                        float* data = channels[channel];
                        if (data != nullptr)
                            data[index] *= applied;
                    }
                }
                const float delta = coeff * (target - gain);
                const float stepped = gain + delta;
                gain = floatBitsEqual(stepped, gain) ? target : stepped;
                if (gain > 1.0f)
                    gain = 1.0f;
                if (gain < 1.0e-8f && target < 1.0e-8f)
                    gain = 0.0f;
            }
        }

        gain_ = gain;
        if (bus_ != nullptr)
            bus_->publishLevel(slot_, settings.bypass ? -160.0f : lastLevel);
        const float meanSq = counted > 0 ? static_cast<float>(sumSq / static_cast<double>(counted)) : 0.0f;
        inputRms_.store(std::sqrt(meanSq), std::memory_order_relaxed);
        const float shown = gain >= 1.0f ? 0.0f : -gainToDecibels(gain);
        reductionDb_.store(shown < 0.0f ? 0.0f : shown, std::memory_order_relaxed);
    }

private:
    static constexpr float kRatio = 2.0f;

    // Soft-knee downward expander. 0 dB at the top of the knee, then a
    // quadratic join into a gentle 2:1 slope. Continuous, including slope.
    static float downwardDb(float levelDb, float thresholdDb, float kneeDb, float ratio, float rangeDb) noexcept
    {
        const float slope = ratio - 1.0f;
        const float knee = kneeDb < 0.5f ? 0.5f : kneeDb;
        const float top = thresholdDb + knee * 0.5f;
        float reduction = 0.0f;
        if (levelDb < top)
        {
            const float bottom = thresholdDb - knee * 0.5f;
            if (levelDb <= bottom)
                reduction = slope * (thresholdDb - levelDb);
            else
            {
                const float distance = top - levelDb;
                reduction = slope * distance * distance / (2.0f * knee);
            }
        }
        if (reduction > rangeDb)
            reduction = rangeDb;
        if (reduction < 0.0f)
            reduction = 0.0f;
        return reduction;
    }

    void followFloor(float levelDb, double rate) noexcept
    {
        const bool falling = levelDb < floorDb_;
        const float seconds = falling ? 0.30f : 8.0f;
        const float coeff = 1.0f - std::exp(-1.0f / static_cast<float>(rate * static_cast<double>(seconds)));
        floorDb_ += coeff * (levelDb - floorDb_);
    }

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

    PseBus* bus_ = nullptr;
    int slot_ = -1;
    PseSettings staging_ {};
    PseSettings params_[2] {};
    std::atomic<int> published_ { 0 };
    float env_ = 0.0f;
    float gain_ = 1.0f;
    float floorDb_ = -90.0f;
    float activity_ = 0.0f;
    std::atomic<float> inputRms_ { 0.0f };
    std::atomic<float> reductionDb_ { 0.0f };
};

} // namespace youmix
