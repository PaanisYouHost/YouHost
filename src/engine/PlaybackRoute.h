#pragma once

namespace youhost
{

// processPlayback returns true only while a take is playing. Stop sets the
// mode to stopped, so the next callback takes the live input and still runs
// the same plugin chain.
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

} // namespace youhost
