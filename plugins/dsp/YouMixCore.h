#pragma once

#include "MeterLook.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace youmix
{

inline constexpr int kYouMixSlots = 16;
inline constexpr int kYouMixGroups = 4;

struct YouMixSettings
{
    bool bypass = false;
    int preset = 1;
    float pinkDb[kYouMixGroups] = { -32.0f, -32.0f, -32.0f, -32.0f };
    float attackMs = 5.0f;
    float releaseMs = 70.0f;
    float thresholdDb = -45.0f;
    float weightDb[kYouMixSlots] = {};
    bool pinkBypass[kYouMixGroups] = {};
    int group[kYouMixSlots] = {};
    int mode[kYouMixSlots] = {};
    bool active[kYouMixSlots] = {};
    char name[kYouMixSlots][32] = {};
};

// One shared automixer for every YouMix instance in this process.
// Slots are claimed on the message thread. process() does not allocate.
class YouMixCore
{
public:
    YouMixCore()
    {
        for (float& gain : gain_)
            gain = 1.0f;
        publish();
    }

    int claimSlot(int preferred = -1)
    {
        if (preferred >= 0 && preferred < kYouMixSlots && ! staging_.active[preferred])
        {
            activate(preferred);
            return preferred;
        }
        for (int slot = 0; slot < kYouMixSlots; ++slot)
        {
            if (! staging_.active[slot])
            {
                activate(slot);
                return slot;
            }
        }
        return -1;
    }

    void releaseSlot(int slot)
    {
        if (slot < 0 || slot >= kYouMixSlots)
            return;
        staging_.active[slot] = false;
        staging_.name[slot][0] = 0;
        staging_.weightDb[slot] = 0.0f;
        colour_[slot].store(0, std::memory_order_relaxed);
        peak_[slot] = 0.0f;
        activity_[slot].store(0.0f, std::memory_order_relaxed);
        publish();
    }

    void setBypass(bool bypass)
    {
        staging_.bypass = bypass;
        publish();
    }

    void setGroup(int slot, int group)
    {
        if (slot < 0 || slot >= kYouMixSlots)
            return;
        if (group < 0)
            group = 0;
        if (group > kYouMixGroups)
            group = kYouMixGroups;
        staging_.group[slot] = group;
        publish();
    }

    void setMode(int slot, int mode)
    {
        if (slot < 0 || slot >= kYouMixSlots)
            return;
        staging_.mode[slot] = mode == 1 ? 1 : 0;
        publish();
    }

    void setPinkDb(int group, float db, bool markCustom = true)
    {
        if (group < 0 || group >= kYouMixGroups)
            return;
        staging_.pinkDb[group] = clampPink(db);
        if (markCustom)
            staging_.preset = 0;
        publish();
    }

    void setAttackMs(float ms, bool markCustom = true)
    {
        staging_.attackMs = clampRange(ms, 1.0f, 500.0f);
        if (markCustom)
            staging_.preset = 0;
        publish();
    }

    void setReleaseMs(float ms, bool markCustom = true)
    {
        staging_.releaseMs = clampRange(ms, 5.0f, 2000.0f);
        if (markCustom)
            staging_.preset = 0;
        publish();
    }

    void setThresholdDb(float db, bool markCustom = true)
    {
        staging_.thresholdDb = clampRange(db, -80.0f, 0.0f);
        if (markCustom)
            staging_.preset = 0;
        publish();
    }

    // Dugan-style bias in dB. 0 dB is an even share. Positive favours this mic.
    void setWeightDb(int slot, float db)
    {
        if (slot < 0 || slot >= kYouMixSlots)
            return;
        staging_.weightDb[slot] = clampRange(db, -12.0f, 12.0f);
        publish();
    }

    // When bypassed, that group's pink reference drops out of the share.
    // The gain smoother keeps the change click-free.
    void setPinkBypass(int group, bool bypass)
    {
        if (group < 0 || group >= kYouMixGroups)
            return;
        staging_.pinkBypass[group] = bypass;
        publish();
    }

    void setName(int slot, const char* text)
    {
        if (slot < 0 || slot >= kYouMixSlots)
            return;
        char* dest = staging_.name[slot];
        dest[0] = 0;
        if (text != nullptr)
        {
            std::size_t count = 0;
            while (text[count] != 0 && count < 31)
            {
                dest[count] = text[count];
                ++count;
            }
            dest[count] = 0;
        }
        publish();
    }

    // Host track colour, ARGB. 0 means the host did not send one.
    // Message thread only. process() does not read it.
    void setTrackColour(int slot, std::uint32_t argb) noexcept
    {
        if (slot < 0 || slot >= kYouMixSlots)
            return;
        colour_[slot].store(argb, std::memory_order_relaxed);
    }

    std::uint32_t trackColour(int slot) const noexcept
    {
        if (slot < 0 || slot >= kYouMixSlots)
            return 0;
        return colour_[slot].load(std::memory_order_relaxed);
    }

    void applyPreset(int preset)
    {
        const bool singing = preset == 2;
        staging_.preset = singing ? 2 : 1;
        staging_.attackMs = singing ? 10.0f : 5.0f;
        staging_.releaseMs = singing ? 400.0f : 70.0f;
        staging_.thresholdDb = singing ? -74.0f : -45.0f;
        const float pink = singing ? -58.0f : -32.0f;
        for (float& value : staging_.pinkDb)
            value = pink;
        publish();
    }

    YouMixSettings settings() const
    {
        return params_[published_.load(std::memory_order_acquire)];
    }

    float gainFor(int slot) const noexcept
    {
        if (slot < 0 || slot >= kYouMixSlots)
            return 1.0f;
        return gain_[slot];
    }

    float inputRms(int slot) const noexcept
    {
        if (slot < 0 || slot >= kYouMixSlots)
            return 0.0f;
        return inputRms_[slot].load(std::memory_order_relaxed);
    }

    float reductionDb(int slot) const noexcept
    {
        if (slot < 0 || slot >= kYouMixSlots)
            return 0.0f;
        return reductionDb_[slot].load(std::memory_order_relaxed);
    }

    int latencySamples() const noexcept { return 0; }

    void process(int slot, float* const* channels, int numChannels, int numSamples, double sampleRate) noexcept
    {
        if (slot < 0 || slot >= kYouMixSlots || channels == nullptr || numSamples <= 0)
            return;

        const YouMixSettings settings = params_[published_.load(std::memory_order_acquire)];
        if (! settings.active[slot])
            return;

        const int width = numChannels < 1 ? 0 : (numChannels > 8 ? 8 : numChannels);
        double sumSq = 0.0;
        int counted = 0;
        for (int index = 0; index < numSamples; ++index)
        {
            for (int channel = 0; channel < width; ++channel)
            {
                const float* data = channels[channel];
                if (data == nullptr)
                    continue;
                const float sample = data[index];
                if (std::isnan(sample))
                    continue;
                sumSq += static_cast<double>(sample) * static_cast<double>(sample);
                ++counted;
            }
        }
        const float meanSq = counted > 0 ? static_cast<float>(sumSq / static_cast<double>(counted)) : 0.0f;
        const int group = settings.group[slot];
        const bool muted = settings.mode[slot] == 1;
        const bool grouped = group >= 1 && group <= kYouMixGroups;
        const bool underThreshold = grouped && ! settings.bypass && ! muted
            && gainToDecibels(std::sqrt(meanSq)) < settings.thresholdDb;
        const bool sharing = grouped && ! settings.bypass && ! muted && ! underThreshold;
        const float activity = sharing ? normalisedActivity(slot, meanSq, numSamples, sampleRate) : 0.0f;
        activity_[slot].store(activity, std::memory_order_relaxed);

        float mine = 0.0f;
        float sum = 0.0f;
        if (sharing)
        {
            // Pink is one extra silent mic. Its level is the group sensitivity.
            // A bypassed group leaves it out. Each real mic is auto-ranged to its
            // own recent level, then biased by its weight, so a quiet singer can
            // still take a full share.
            if (! settings.pinkBypass[group - 1])
                sum = decibelsToGain(settings.pinkDb[group - 1]);
            for (int other = 0; other < kYouMixSlots; ++other)
            {
                if (! settings.active[other] || settings.mode[other] == 1 || settings.group[other] != group)
                    continue;
                const float weighted = activity_[other].load(std::memory_order_relaxed) * weightGain(settings.weightDb[other]);
                sum += weighted;
                if (other == slot)
                    mine = weighted;
            }
        }

        float target = 1.0f;
        if (settings.bypass || ! grouped)
            target = 1.0f;
        else if (muted || underThreshold)
            target = 0.0f;
        else if (sum > 1.0e-20f)
            target = mine / sum;
        if (target > 1.0f)
            target = 1.0f;
        if (target < 0.0f)
            target = 0.0f;

        const double rate = sampleRate > 1000.0 ? sampleRate : 48000.0;
        const float ms = target >= gain_[slot] ? settings.attackMs : settings.releaseMs;
        const float seconds = (ms < 1.0f ? 1.0f : ms) * 0.001f;
        const float coeff = 1.0f - std::exp(-1.0f / static_cast<float>(rate * static_cast<double>(seconds)));

        float gain = gain_[slot];
        if (floatBitsEqual(gain, 1.0f) && floatBitsEqual(target, 1.0f))
        {
            gain = 1.0f;
        }
        else
        {
            for (int index = 0; index < numSamples; ++index)
            {
                const float applied = gain;
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
        gain_[slot] = gain;
        inputRms_[slot].store(std::sqrt(meanSq), std::memory_order_relaxed);
        const float reduction = gain >= 1.0f ? 0.0f : -gainToDecibels(gain);
        reductionDb_[slot].store(reduction < 0.0f ? 0.0f : reduction, std::memory_order_relaxed);
    }

private:
    static float clampPink(float db) noexcept
    {
        return clampRange(db, -60.0f, 0.0f);
    }

    static float clampRange(float value, float low, float high) noexcept
    {
        if (value < low)
            return low;
        if (value > high)
            return high;
        return value;
    }

    // +6 dB of weight doubles this mic's share against an equal neighbour.
    static float weightGain(float db) noexcept
    {
        return decibelsToGain(db);
    }

    // 1 while this mic is at its own recent level, falling as it drops below
    // that level. Absolute loudness between mics cancels out.
    float normalisedActivity(int slot, float power, int numSamples, double sampleRate) noexcept
    {
        float& peak = peak_[slot];
        if (power >= peak)
            peak = power;
        else
        {
            const double rate = sampleRate > 1000.0 ? sampleRate : 48000.0;
            const float blockSeconds = static_cast<float>(static_cast<double>(numSamples) / rate);
            const float coeff = 1.0f - static_cast<float>(std::exp(-static_cast<double>(blockSeconds) / 0.35));
            peak += coeff * (power - peak);
        }
        if (peak < 1.0e-12f || power <= 0.0f)
            return 0.0f;
        float ratio = power / peak;
        if (ratio > 1.0f)
            ratio = 1.0f;
        return std::sqrt(ratio);
    }

    void activate(int slot)
    {
        staging_.active[slot] = true;
        staging_.group[slot] = 1;
        staging_.mode[slot] = 0;
        staging_.name[slot][0] = 0;
        staging_.weightDb[slot] = 0.0f;
        colour_[slot].store(0, std::memory_order_relaxed);
        gain_[slot] = 1.0f;
        peak_[slot] = 0.0f;
        activity_[slot].store(0.0f, std::memory_order_relaxed);
        inputRms_[slot].store(0.0f, std::memory_order_relaxed);
        reductionDb_[slot].store(0.0f, std::memory_order_relaxed);
        publish();
    }

    void publish()
    {
        const int current = published_.load(std::memory_order_relaxed);
        params_[1 - current] = staging_;
        published_.store(1 - current, std::memory_order_release);
    }

    YouMixSettings staging_ {};
    YouMixSettings params_[2] {};
    std::atomic<int> published_ { 0 };
    float gain_[kYouMixSlots] {};
    float peak_[kYouMixSlots] {};
    std::atomic<float> activity_[kYouMixSlots] {};
    std::atomic<float> inputRms_[kYouMixSlots] {};
    std::atomic<float> reductionDb_[kYouMixSlots] {};
    std::atomic<std::uint32_t> colour_[kYouMixSlots] {};
};

inline YouMixCore& youMixShared()
{
    static YouMixCore core;
    return core;
}

} // namespace youmix
