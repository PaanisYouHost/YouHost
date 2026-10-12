#pragma once

#include <algorithm>
#include <cstring>

namespace youhost
{

// One callback, in this order:
// 1. The recorder copies the device input. It never sees the take.
// 2. While a take is playing, that take replaces the live input per channel.
// 3. Plugins, stereo link, and delay compensation run on that dry copy.
// 4. Output gain runs after the plugins.
// Record and live pass-through leave those two paths independent.
// Stop clears the playing flag. The dry input eases back to the live mixer
// over kHostSwitchFade samples. The chain stays where it is: no graph
// rebuild, no delay reset, and no block of silence while the play ring
// is still empty.
enum class CallbackStage
{
    recordRawInput = 0,
    replaceWithPlayback = 1,
    pluginsLinkAndDelay = 2,
    outputGain = 3
};

inline int callbackStageOrder(CallbackStage stage) noexcept
{
    return static_cast<int>(stage);
}

enum class CallbackInput
{
    live,
    playback
};

inline CallbackInput callbackInput(bool playbackRunning) noexcept
{
    return playbackRunning ? CallbackInput::playback : CallbackInput::live;
}

inline bool playbackFeedsHostChain(bool playbackRunning) noexcept
{
    return playbackRunning;
}

inline bool stopReturnsLiveInput(bool playbackRunning) noexcept
{
    return ! playbackRunning;
}

// The WAV is the device input. Playback never replaces it.
inline float recorderSample(float liveInput) noexcept
{
    return liveInput;
}

// The dry sample the host chain processes. Playing uses the take.
inline float hostDrySample(bool playbackRunning, float liveInput, float takeSample) noexcept
{
    return playbackRunning ? takeSample : liveInput;
}

// What the disk reader handed the callback.
struct PlaybackPull
{
    int samples = 0;
    bool transportPlaying = false;
    bool held = false;
    bool ended = false;
};

// 128 samples is about 2.7 ms at 48 kHz and 1.3 ms at 96 kHz. The value
// moves at most one step per sample, so a full-scale handoff stays continuous.
inline constexpr int kHostSwitchFade = 128;

struct HostFade
{
    int gain = 0;
};

struct HostFadeStep
{
    int gain0 = 0;
    int gain1 = 0;

    bool steadyLive() const noexcept { return gain0 <= 0 && gain1 <= 0; }
    bool steadyTake() const noexcept { return gain0 >= kHostSwitchFade && gain1 >= kHostSwitchFade; }
};

// Play stays on the live input until a real block of the take is ready,
// or the take is shorter than the block and has already ended. Once the
// fade has left live, only Stop comes back. An empty ring is not silence
// on the outputs, and a disk underrun does not punch the live input in.
inline bool hostWantsTake(bool transportPlaying,
                          int samplesReady,
                          int blockSamples,
                          bool holdSilence,
                          int fadeGain,
                          bool contentEnded) noexcept
{
    if (! transportPlaying || blockSamples <= 0)
        return false;
    if (holdSilence || fadeGain > 0)
        return true;
    if (samplesReady >= blockSamples)
        return true;
    return contentEnded && samplesReady > 0;
}

inline HostFadeStep stepHostFade(HostFade& fade, bool wantTake, int blockSamples) noexcept
{
    HostFadeStep step;
    step.gain0 = std::clamp(fade.gain, 0, kHostSwitchFade);
    int end = step.gain0;
    if (blockSamples > 0)
    {
        if (wantTake)
            end = std::min(kHostSwitchFade, step.gain0 + blockSamples);
        else
            end = std::max(0, step.gain0 - blockSamples);
    }
    step.gain1 = end;
    fade.gain = end;
    return step;
}

inline float mixHostSample(float live, float take, int gain) noexcept
{
    if (gain <= 0)
        return live;
    if (gain >= kHostSwitchFade)
        return take;
    const float amount = static_cast<float>(gain) / static_cast<float>(kHostSwitchFade);
    return live + (take - live) * amount;
}

// dest may alias take. A null live or take pointer is silence. The ramp
// covers the first |gain1 - gain0| samples; the rest of the block sits on gain1.
inline void mixHostBlock(float* dest,
                         const float* live,
                         const float* take,
                         int numSamples,
                         int gain0,
                         int gain1) noexcept
{
    if (dest == nullptr || numSamples <= 0)
        return;

    gain0 = std::clamp(gain0, 0, kHostSwitchFade);
    gain1 = std::clamp(gain1, 0, kHostSwitchFade);
    if (gain0 == 0 && gain1 == 0)
    {
        if (live == nullptr)
            std::memset(dest, 0, sizeof(float) * static_cast<std::size_t>(numSamples));
        else if (live != dest)
            std::memmove(dest, live, sizeof(float) * static_cast<std::size_t>(numSamples));
        return;
    }
    if (gain0 == kHostSwitchFade && gain1 == kHostSwitchFade)
    {
        if (take == nullptr)
            std::memset(dest, 0, sizeof(float) * static_cast<std::size_t>(numSamples));
        else if (take != dest)
            std::memmove(dest, take, sizeof(float) * static_cast<std::size_t>(numSamples));
        return;
    }

    const int direction = gain1 >= gain0 ? 1 : -1;
    const int changing = direction > 0 ? (gain1 - gain0) : (gain0 - gain1);
    for (int index = 0; index < numSamples; ++index)
    {
        const int gain = index < changing ? gain0 + direction * index : gain1;
        const float liveSample = live != nullptr ? live[index] : 0.0f;
        const float takeSample = take != nullptr ? take[index] : 0.0f;
        dest[index] = mixHostSample(liveSample, takeSample, gain);
    }
}

// Mix the live device input into the take buffers for the fade prefix.
// inputPacked is the live routing, before playback remaps it.
inline void blendHostChannels(float* const* dest,
                              const float* const* liveInputs,
                              int numLiveInputs,
                              const std::int16_t* inputPacked,
                              int channels,
                              int numSamples,
                              int gain0,
                              int gain1) noexcept
{
    if (dest == nullptr || channels <= 0 || numSamples <= 0)
        return;

    for (int channel = 0; channel < channels; ++channel)
    {
        float* slot = dest[channel];
        if (slot == nullptr)
            continue;

        const float* live = nullptr;
        if (liveInputs != nullptr && inputPacked != nullptr)
        {
            const int packed = inputPacked[channel];
            if (packed >= 0 && packed < numLiveInputs)
                live = liveInputs[packed];
        }
        mixHostBlock(slot, live, slot, numSamples, gain0, gain1);
    }
}

} // namespace youhost
