#pragma once

namespace youhost
{

// While a plugin that should be processing is down, the channel outputs
// silence. Dry input is used only after the user chooses Bypass plugin,
// and that path fades. A crash never jumps straight to the unprocessed signal.
inline constexpr int kSafetyFadeSamples = 64;

enum class SafetySource
{
    processed,
    silence,
    dry
};

struct SafetyState
{
    float gain = 1.0f;
    int mutedSamples = 0;
    bool pluginWasDown = false;
    bool bypass = false;
};

struct SafetyStep
{
    float gain = 1.0f;
    SafetySource source = SafetySource::processed;
    bool passedUnprocessed = false;
};

inline float approachGain(float current, float target, int samples, int fadeSamples) noexcept
{
    if (fadeSamples <= 0 || samples <= 0)
        return target;
    const float step = static_cast<float>(samples) / static_cast<float>(fadeSamples);
    if (current < target)
    {
        const float next = current + step;
        return next > target ? target : next;
    }
    const float next = current - step;
    return next < target ? target : next;
}

// processedReady: the helper (or the live plugin) produced this block.
// pluginAlive: the plugin process is running.
// userBypass: the user pressed Bypass plugin. That is the only dry path.
inline SafetyStep stepChannelSafety(SafetyState& state,
                                    bool processedReady,
                                    bool pluginAlive,
                                    bool userBypass,
                                    int samples) noexcept
{
    SafetyStep step;
    state.bypass = userBypass;
    if (userBypass)
    {
        state.gain = approachGain(state.gain, 1.0f, samples, kSafetyFadeSamples);
        state.pluginWasDown = false;
        step.gain = state.gain;
        step.source = SafetySource::dry;
        step.passedUnprocessed = false;
        return step;
    }

    if (! pluginAlive || ! processedReady)
    {
        state.pluginWasDown = true;
        state.gain = approachGain(state.gain, 0.0f, samples, kSafetyFadeSamples);
        if (state.gain <= 0.0f)
            state.mutedSamples += samples > 0 ? samples : 0;
        step.gain = state.gain;
        step.source = SafetySource::silence;
        step.passedUnprocessed = false;
        return step;
    }

    if (state.pluginWasDown)
        state.gain = approachGain(0.0f, 1.0f, samples, kSafetyFadeSamples);
    else
        state.gain = approachGain(state.gain, 1.0f, samples, kSafetyFadeSamples);
    if (state.gain >= 1.0f)
        state.pluginWasDown = false;
    step.gain = state.gain;
    step.source = SafetySource::processed;
    step.passedUnprocessed = false;
    return step;
}

inline void applySafety(float* left, float* right, int samples, float gain, SafetySource source,
                        const float* dryLeft, const float* dryRight) noexcept
{
    if (samples <= 0)
        return;
    for (int index = 0; index < samples; ++index)
    {
        float dryL = dryLeft != nullptr ? dryLeft[index] : 0.0f;
        float dryR = dryRight != nullptr ? dryRight[index] : 0.0f;
        if (source == SafetySource::dry)
        {
            if (left != nullptr)
                left[index] = dryL * gain;
            if (right != nullptr)
                right[index] = dryR * gain;
            continue;
        }
        if (source == SafetySource::silence || gain <= 0.0f)
        {
            if (left != nullptr)
                left[index] = 0.0f;
            if (right != nullptr)
                right[index] = 0.0f;
            continue;
        }
        if (left != nullptr)
            left[index] *= gain;
        if (right != nullptr)
            right[index] *= gain;
    }
}

} // namespace youhost
