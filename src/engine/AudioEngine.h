#pragma once

#include "DropoutDetect.h"
#include "LatencyMath.h"
#include "Passthrough.h"
#include "PluginCatalogue.h"
#include "PluginRack.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace youhost
{

class AppSettings;

// Owns the device, the realtime callback, and the in-process plugin rack.
// Record rings are still later. Order in the callback: meters on the raw input,
// dry copy, then the published plugin graph and compensation delay.
class AudioEngine : private juce::AudioIODeviceCallback,
                    private juce::ChangeListener
{
public:
    explicit AudioEngine(AppSettings& settings);
    ~AudioEngine() override;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    juce::AudioDeviceManager& deviceManager() noexcept { return deviceManager_; }

    // Opens the saved device, or a default with every channel up to kMaxChannels.
    // When allowInput is false the saved setup is left untouched and inputs stay closed.
    void start(bool allowInput);

    bool microphoneGranted() const noexcept { return microphoneGranted_; }
    bool isRunning() const noexcept { return started_; }

    void pollDeviceStats();
    LatencyNumbers latencyNumbers() const;
    int visibleChannels() const;
    int inputCount() const;
    int outputCount() const;
    juce::String deviceName() const { return deviceName_; }
    juce::String openError() const { return openError_; }

    float rmsFor(int channel) const;
    float peakFor(int channel) const;
    bool clipFor(int channel) const;
    bool inputActive(int channel) const;
    void requestClipClear(int channel);
    void requestClipClearAll();
    void resetDropouts();
    float cpuUsage() const noexcept { return cpuUsage_.load(std::memory_order_relaxed); }

    PluginCatalogue& pluginCatalogue() noexcept { return *catalogue_; }
    ChannelSnapshot channelSnapshot(int channel) const;
    void loadPlugin(int channel, int slot, const juce::PluginDescription& description);
    void removePlugin(int channel, int slot);
    void setSlotBypassed(int channel, int slot, bool bypassed);
    void setChannelExcluded(int channel, bool excluded);
    void openPluginEditor(int channel, int slot);

    void setSessionMeters(bool peak, int rmsReferenceDb);
    void noteSessionEdit();
    void maintainSession();
    bool hasSession() const noexcept { return sessionFolder_.getFullPathName().isNotEmpty(); }
    juce::String sessionName() const { return sessionFolder_.getFileName(); }
    juce::File suggestedSessionFolder() const;
    bool saveSession();
    bool saveSessionToFolder(const juce::File& folder);
    bool loadSessionFrom(const juce::File& fileOrFolder);
    juce::String sessionMessage() const { return sessionMessage_; }
    void setMeterRestoreHandler(std::function<void(bool peak, int referenceDb)> handler);

private:
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& errorMessage) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    void publishConfig(juce::AudioIODevice& device);
    void saveSetupIfAllowed();
    void pushRouting(const Routing& routing);
    Routing routingFromDevice(const juce::AudioIODevice& device) const;
    void installOverloadListener(const juce::String& deviceName);
    void removeOverloadListener();
    void noteDropout(int events);
    const AudioThreadConfig& currentConfig() const;

    AppSettings& settings_;
    juce::AudioDeviceManager deviceManager_;
    std::array<ChannelStrip, kMaxChannels> strips_ {};
    std::array<AudioThreadConfig, 2> configs_ {};
    std::atomic<int> configIndex_ { 0 };

    std::atomic<int> compensationSamples_ { 0 };
    std::atomic<int> bufferSamples_ { 0 };
    std::atomic<int> inputLatencySamples_ { 0 };
    std::atomic<int> outputLatencySamples_ { 0 };
    std::atomic<std::uint32_t> dropoutCount_ { 0 };
    std::atomic<std::int64_t> lastCallbackNs_ { 0 };
    std::atomic<bool> skipNextGap_ { true };
    std::uint32_t overloadDeviceId_ = 0;
    std::atomic<int> formulaValue_ { static_cast<int>(RoundTripFormula::driverSum) };
    std::atomic<double> sampleRate_ { 0.0 };
    std::atomic<bool> deviceOpen_ { false };
    std::atomic<bool> deviceError_ { false };
    std::atomic<float> cpuUsage_ { 0.0f };

    std::unique_ptr<PluginCatalogue> catalogue_;
    std::unique_ptr<PluginRack> rack_;
    bool sessionPeak_ = false;
    int sessionReferenceDb_ = kDefaultRmsReferenceDb;
    juce::File sessionFolder_;
    juce::String sessionMessage_;
    bool sessionDirty_ = false;
    bool restoringSession_ = false;
    juce::uint32 sessionDirtyAtMs_ = 0;
    std::function<void(bool, int)> meterRestoreHandler_;

    juce::String deviceName_ { "No device" };
    juce::String openError_;
    bool microphoneGranted_ = false;
    bool persistSetup_ = false;
    bool started_ = false;
};

} // namespace youhost
