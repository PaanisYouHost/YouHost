#pragma once

#include "ChannelEnable.h"
#include "ChannelListen.h"
#include "CrashJournal.h"
#include "DisplayLayout.h"
#include "DropoutDetect.h"
#include "DropoutLog.h"
#include "LatencyMath.h"
#include "Passthrough.h"
#include "X32Colours.h"
#include "PluginCatalogue.h"
#include "PluginRack.h"
#include "Recorder.h"
#include "TimelineLanes.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace youhost
{

class AppSettings;
class MidiFollow;

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
    void noteDropout(int events);
    void drainDropoutLog();
    DropoutSnapshot dropoutSnapshot() const;
    float cpuUsage() const noexcept { return cpuUsage_.load(std::memory_order_relaxed); }

    PluginCatalogue& pluginCatalogue() noexcept { return *catalogue_; }
    ChannelSnapshot channelSnapshot(int channel) const;
    void loadPlugin(int channel, int slot, const juce::PluginDescription& description, bool openEditor = false);
    void removePlugin(int channel, int slot);
    void transferPlugin(int fromChannel, int fromSlot, int toChannel, int toSlot, bool copy);
    void setSlotBypassed(int channel, int slot, bool bypassed);
    void setStereoFold(int channel, int slot, int fold);
    void setChannelExcluded(int channel, bool excluded);
    void openPluginEditor(int channel, int slot);
    void togglePluginEditor(int channel, int slot);
    bool isPluginEditorOpen(int channel, int slot) const;

    void setRecordArmed(int channel, bool armed);
    bool isRecordArmed(int channel) const;
    ChannelListen channelListen(int channel) const;
    void setChannelListen(int channel, ChannelListen mode);
    void cycleChannelListen(int channel);
    float outputDb(int channel) const;
    void setOutputDb(int channel, float db);
    void setBypassAll(bool bypass);
    bool bypassAll() const;

    int sceneCount() const;
    int sceneRevision() const noexcept { return sceneRevision_; }
    juce::String sceneName(int index) const;
    int sceneRemote(int index) const;
    void setSceneRemote(int index, int program);
    int recalledScene() const noexcept { return recalledScene_; }
    bool sceneDrift() const noexcept { return sceneDrift_; }
    void storeScene(int index);
    void storeNewScene();
    void recallScene(int index);
    void renameScene(int index, const juce::String& name);
    void deleteScene(int index);
    bool channelSafe(int channel) const;
    void setChannelSafe(int channel, bool safe);

    void pollMidiFollow();
    void setMidiFollow(bool enabled);
    bool midiFollow() const;
    void setMidiFollowChannel(int channel);
    int midiFollowChannel() const;
    void setMidiFollowDevice(const juce::String& identifier);
    juce::String midiFollowDevice() const;
    bool midiFollowOpen() const;
    bool midiActivityLit() const;
    juce::Array<juce::MidiDeviceInfo> midiInputs() const;
    float waveformGain() const noexcept { return waveformGain_; }
    void setWaveformGain(float gain);
    void nudgeWaveformGain(int direction);
    int alignGroup() const noexcept { return alignGroup_; }
    void setAlignGroup(int perGroup);
    CpuMeters cpuMeters() const;
    juce::String channelPdcText(int channel) const;
    void setChannelName(int channel, const juce::String& name);
    juce::String channelName(int channel) const;

    int channelColor(int channel) const;
    void setChannelColor(int channel, int color);
    int channelGroup(int channel) const;
    void assignChannelsToGroup(const std::vector<int>& channels, int group);
    void clearGroup(int group);
    void setGroupName(int group, const juce::String& name);
    void setGroupColor(int group, int color);
    juce::String groupName(int group) const;
    int groupColor(int group) const;
    bool groupCollapsed(int group) const;
    bool groupHasMembers(int group) const;
    void toggleGroupCollapsed(int group);
    void expandAllGroups();
    void hideGroupedChannels();
    bool groupsAreExpanded() const;
    bool groupsAreHidden() const;
    int displayRevision() const noexcept { return displayRevision_; }
    std::vector<StripItem> displayStrips(int channelCount) const;

    void selectChannel(int channel, bool extend);
    bool isChannelSelected(int channel) const;
    std::vector<int> selectedChannels() const;
    void toggleRecordReady();
    bool isRecordReady() const noexcept { return recordReady_; }
    void transportRecord();
    void transportStop();
    void transportPlay();
    void transportLocate(std::int64_t sample);
    void transportJump(int direction);
    void transportNudge(double seconds);
    TransportView transportView() const;
    void startNewSession();
    bool placeNewSession(const juce::File& folder, bool internalDisk);
    bool createInternalSession();
    juce::String sessionRecordProblem() const;
    bool sessionIsOnInternalDisk() const noexcept { return sessionOnInternalDisk_; }
    juce::File sessionFolder() const { return sessionFolder_; }
    juce::File defaultSessionParent() const;
    juce::String missingSessionParentNote() const;

    void setSessionMeters(bool peak, int rmsReferenceDb);
    void setWavBitDepth(int bits, bool markDirty);
    int wavBitDepth() const noexcept { return wavBitDepth_; }
    juce::String wavBitDepthLabel() const;
    void noteSessionEdit();
    void touchSession();
    void maintainSession();
    void setSessionPage(int page);
    int sessionPage() const noexcept { return sessionPage_; }
    void setPageRestoreHandler(std::function<void(int)> handler);
    bool hasSession() const noexcept { return sessionFolder_.getFullPathName().isNotEmpty(); }
    juce::String sessionName() const { return sessionFolder_.getFileName(); }
    juce::File suggestedSessionFolder() const;
    bool saveSession();
    bool saveSessionToFolder(const juce::File& folder);
    bool saveSessionAs(const juce::File& folder);
    bool loadSessionFrom(const juce::File& fileOrFolder);
    bool importRecordingFolder(const juce::File& folder);
    void clearTimeline();
    juce::StringArray recentSessions() const;
    juce::String sessionMessage() const { return sessionMessage_; }
    void setMeterRestoreHandler(std::function<void(bool peak, int referenceDb)> handler);
    void setGlobalKeyListener(juce::KeyListener* listener);

    bool deviceLost() const noexcept { return deviceLostBanner_; }
    juce::String rateWarning() const { return rateWarning_; }
    juce::String startupDeviceNote() const { return startupDeviceNote_; }
    bool takeUncleanShutdown();
    juce::String uncleanPluginMessage() const;
    void acceptCrashChoice(bool leaveOff);
    void prepareForQuit();
    void visitTimelineLanes(const std::function<void(const std::vector<TimelineLaneView>&)>& fn) const;
    juce::String dspLoadText() const;

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
    void syncRecorderFolder();
    void rememberSessionParent(const juce::File& sessionFolder);
    bool isInternalFallback(const juce::File& folder) const;
    void pushRouting(const Routing& routing);
    void syncCompensation();
    Routing routingFromDevice(const juce::AudioIODevice& device) const;
    void installOverloadListener(const juce::String& deviceName);
    void removeOverloadListener();
    const AudioThreadConfig& currentConfig() const;
    void captureDisplay(SessionData& data) const;
    void applyDisplay(const SessionData& data);
    void captureScenes(SessionData& data) const;
    void applyScenes(const SessionData& data);
    SessionScene captureCurrentScene(const juce::String& name, int remote);
    void markSceneDrift();
    void storeChannelOn(int channel, bool on);
    void bumpDisplay();
    void applyOpenDevice(juce::AudioIODevice& device, bool remember);
    void noteChannelMasks(juce::AudioIODevice& device);
    void handleDeviceDown();
    void tryReopenWanted();
    bool deviceNameListed(const juce::String& name);
    void notePluginTrace(int channel, int slot, const juce::String& phase, const juce::String& name, const juce::String& identifier);
    void rememberSavedSetup(const juce::XmlElement* saved);

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
    DropoutRing dropoutRing_;
    std::vector<DropoutMark> dropoutMarks_;
    std::vector<CpuSample> cpuSamples_;
    std::int64_t dropoutOriginNs_ = 0;
    std::int64_t lastCpuSampleNs_ = 0;
    bool dropoutHeaderWritten_ = false;
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
    std::unique_ptr<Recorder> recorder_;
    std::vector<float> playbackScratch_;
    std::array<float*, kMaxChannels> playbackPtrs_ {};
    int playbackMax_ = 0;
    bool sessionPeak_ = false;
    int sessionReferenceDb_ = kDefaultRmsReferenceDb;
    int wavBitDepth_ = kDefaultWavBitDepth;
    int sessionPage_ = 1;
    bool recordReady_ = false;
    bool sessionOnInternalDisk_ = false;
    juce::File sessionFolder_;
    juce::String sessionMessage_;
    bool sessionDirty_ = false;
    bool restoringSession_ = false;
    juce::uint32 sessionDirtyAtMs_ = 0;
    std::function<void(bool, int)> meterRestoreHandler_;
    std::function<void(int)> pageRestoreHandler_;
    std::atomic<std::uint64_t> channelOnLo_ { ~std::uint64_t { 0 } };
    std::atomic<std::uint64_t> channelOnHi_ { ~std::uint64_t { 0 } };
    std::unique_ptr<MidiFollow> midi_;
    std::vector<SessionScene> scenes_;
    std::array<bool, kMaxChannels> sceneSafe_ {};
    int recalledScene_ = -1;
    bool sceneDrift_ = false;
    bool recallingScene_ = false;
    int sceneRevision_ = 0;
    std::array<int, kMaxChannels> channelColor_ {};
    std::array<int, kMaxChannels> channelGroup_ {};
    std::array<ChannelListen, kMaxChannels> listen_ {};
    std::array<float, kMaxChannels> outputDb_ {};
    std::array<std::atomic<float>, kMaxChannels> outputGain_ {};
    float waveformGain_ = 1.0f;
    int alignGroup_ = 0;
    std::array<SessionGroup, kMaxDisplayGroups> groups_ {};
    std::vector<int> selection_;
    int selectionAnchor_ = 0;
    int displayRevision_ = 0;

    juce::String deviceName_ { "No device" };
    juce::String openError_;
    bool microphoneGranted_ = false;
    bool persistSetup_ = false;
    bool started_ = false;

    std::atomic<bool> deviceStarting_ { false };
    std::atomic<bool> deviceDown_ { false };
    std::atomic<bool> closingDevice_ { false };
    bool deviceLostBanner_ = false;
    bool reopenInProgress_ = false;
    bool lossFinalized_ = false;
    bool awaitingSavedDevice_ = false;
    bool wideningOutputs_ = false;
    bool quitPrepared_ = false;
    bool crashChoicePending_ = false;
    std::uint32_t downSinceMs_ = 0;
    std::uint32_t lastDeviceScanMs_ = 0;
    double preparedRate_ = 0.0;
    int preparedBuffer_ = 0;
    juce::AudioDeviceManager::AudioDeviceSetup wantedSetup_;
    juce::String wantedName_;
    juce::String startupFallbackName_;
    juce::String startupDeviceNote_;
    juce::String rateWarning_;
    juce::BigInteger lastInputMask_;
    juce::BigInteger lastOutputMask_;
    CrashJournal journal_;
    std::unique_ptr<juce::Thread> stallThread_;
};

} // namespace youhost
