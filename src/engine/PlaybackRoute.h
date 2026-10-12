#pragma once

namespace youhost
{

// One callback, in this order:
// 1. The recorder copies the device input. It never sees the take.
// 2. While a take is playing, that take replaces the live input per channel.
// 3. Plugins, stereo link, and delay compensation run on that dry copy.
// 4. Output gain runs after the plugins.
// Stop clears the playing flag, so the next callback uses the live input
// and the same chain.
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

} // namespace youhost
