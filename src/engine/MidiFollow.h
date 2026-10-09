#pragma once

#include "AppSettings.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstdint>

namespace youhost
{

// CoreMIDI (and the JUCE MIDI input on other hosts) delivers on its own thread.
// That callback only stores two atomics. The message thread polls them.
// Nothing here is read by the audio callback.
class MidiFollow : private juce::MidiInputCallback
{
public:
    explicit MidiFollow(AppSettings& settings);
    ~MidiFollow() override;

    MidiFollow(const MidiFollow&) = delete;
    MidiFollow& operator=(const MidiFollow&) = delete;

    void setEnabled(bool enabled);
    bool enabled() const noexcept;
    void setChannel(int channel);
    int channel() const noexcept;
    void setDevice(const juce::String& identifier);
    juce::String device() const;
    bool isOpen() const noexcept;
    juce::Array<juce::MidiDeviceInfo> devices() const;

    // -1 when no Program Change has arrived since the last take.
    int takePending() noexcept;
    bool activityLit() const noexcept;

private:
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
    void reopen();

    AppSettings& settings_;
    std::unique_ptr<juce::MidiInput> input_;
    juce::String deviceId_;
    std::atomic<int> enabled_ { 0 };
    std::atomic<int> channel_ { 1 };
    std::atomic<int> pendingProgram_ { -1 };
    std::atomic<std::uint32_t> activityMs_ { 0 };
};

} // namespace youhost
