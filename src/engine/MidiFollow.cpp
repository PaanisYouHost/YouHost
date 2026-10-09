#include "MidiFollow.h"
#include "SceneRecall.h"

namespace youhost
{

MidiFollow::MidiFollow(AppSettings& settings)
    : settings_(settings)
{
    channel_.store(clampMidiChannel(settings_.loadMidiFollowChannel()), std::memory_order_relaxed);
    deviceId_ = settings_.loadMidiFollowDevice();
    const bool enabled = settings_.loadMidiFollow();
    enabled_.store(enabled ? 1 : 0, std::memory_order_relaxed);
    if (enabled)
        reopen();
}

MidiFollow::~MidiFollow()
{
    input_.reset();
}

void MidiFollow::setEnabled(bool enabled)
{
    enabled_.store(enabled ? 1 : 0, std::memory_order_relaxed);
    if (! enabled)
        pendingProgram_.store(-1, std::memory_order_relaxed);
    settings_.saveMidiFollow(enabled);
    reopen();
}

bool MidiFollow::enabled() const noexcept
{
    return enabled_.load(std::memory_order_relaxed) != 0;
}

void MidiFollow::setChannel(int channel)
{
    const int next = clampMidiChannel(channel);
    channel_.store(next, std::memory_order_relaxed);
    settings_.saveMidiFollowChannel(next);
}

int MidiFollow::channel() const noexcept
{
    return clampMidiChannel(channel_.load(std::memory_order_relaxed));
}

void MidiFollow::setDevice(const juce::String& identifier)
{
    if (deviceId_ == identifier)
        return;
    deviceId_ = identifier;
    settings_.saveMidiFollowDevice(identifier);
    reopen();
}

juce::String MidiFollow::device() const
{
    return deviceId_;
}

bool MidiFollow::isOpen() const noexcept
{
    return input_ != nullptr;
}

juce::Array<juce::MidiDeviceInfo> MidiFollow::devices() const
{
    return juce::MidiInput::getAvailableDevices();
}

int MidiFollow::takePending() noexcept
{
    return pendingProgram_.exchange(-1, std::memory_order_acq_rel);
}

bool MidiFollow::activityLit() const noexcept
{
    const auto stamp = activityMs_.load(std::memory_order_relaxed);
    if (stamp == 0)
        return false;
    const auto now = juce::Time::getMillisecondCounter();
    return static_cast<std::uint32_t>(now - stamp) < 180u;
}

void MidiFollow::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message)
{
    if (message.getRawDataSize() < 2)
        return;
    const auto* raw = message.getRawData();
    const int status = static_cast<int>(static_cast<unsigned char>(raw[0]));
    const int data1 = static_cast<int>(static_cast<unsigned char>(raw[1]));
    int program = 0;
    if (! takeProgramChange(status, data1, channel_.load(std::memory_order_relaxed), program))
        return;
    pendingProgram_.store(program, std::memory_order_relaxed);
    activityMs_.store(juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
}

void MidiFollow::reopen()
{
    input_.reset();
    if (enabled_.load(std::memory_order_relaxed) == 0 || deviceId_.isEmpty())
        return;
    input_ = juce::MidiInput::openDevice(deviceId_, this);
    if (input_ != nullptr)
        input_->start();
}

} // namespace youhost
