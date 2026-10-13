#pragma once

#include "MeterLook.h"

#include <atomic>
#include <cmath>
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

    void applyPreset(int preset)
    {
        const bool singing = preset == 2;
        staging_.preset = singing ? 2 : 1;
        staging_.attackMs = singing ? 15.0f : 5.0f;
        staging_.releaseMs = singing ? 280.0f : 70.0f;
        const float pink = singing ? -44.0f : -32.0f;
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
        const float reported = (settings.bypass || muted || ! grouped) ? 0.0f : meanSq;
        power_[slot].store(reported, std::memory_order_relaxed);

        float mine = 0.0f;
        float sum = 0.0f;
        if (! settings.bypass && grouped && ! muted)
        {
            sum = pinkPower(settings.pinkDb[group - 1]);
            for (int other = 0; other < kYouMixSlots; ++other)
            {
                if (! settings.active[other] || settings.mode[other] == 1 || settings.group[other] != group)
                    continue;
                const float power = power_[other].load(std::memory_order_relaxed);
                sum += power;
                if (other == slot)
                    mine = power;
            }
        }

        float target = 1.0f;
        if (settings.bypass || ! grouped)
            target = 1.0f;
        else if (muted)
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
        if (db < -60.0f)
            return -60.0f;
        if (db > 0.0f)
            return 0.0f;
        return db;
    }

    static float pinkPower(float db) noexcept
    {
        const float linear = decibelsToGain(db);
        return linear * linear;
    }

    void activate(int slot)
    {
        staging_.active[slot] = true;
        staging_.group[slot] = 1;
        staging_.mode[slot] = 0;
        staging_.name[slot][0] = 0;
        gain_[slot] = 1.0f;
        power_[slot].store(0.0f, std::memory_order_relaxed);
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
    std::atomic<float> power_[kYouMixSlots] {};
    std::atomic<float> inputRms_[kYouMixSlots] {};
    std::atomic<float> reductionDb_[kYouMixSlots] {};
};

inline YouMixCore& youMixShared()
{
    static YouMixCore core;
    return core;
}

} // namespace youmix
