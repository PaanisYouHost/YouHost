#include "AudioEngine.h"
#include "AppSettings.h"
#include "MeterScale.h"
#include "SessionDocument.h"
#include "SessionFiles.h"

#include <algorithm>
#include <chrono>
#include <vector>

#if JUCE_MAC
 #include <CoreAudio/AudioHardware.h>
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace youhost
{
namespace
{

int64_t steadyNowNs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

#if JUCE_MAC
OSStatus overloadListener(AudioObjectID,
                          UInt32 numberOfAddresses,
                          const AudioObjectPropertyAddress*,
                          void* client)
{
    auto* counter = static_cast<std::atomic<std::uint32_t>*>(client);
    if (counter != nullptr)
        counter->fetch_add(numberOfAddresses == 0 ? 1u : numberOfAddresses, std::memory_order_relaxed);
    return noErr;
}

AudioObjectPropertyAddress overloadAddress()
{
    return { kAudioDeviceProcessorOverload,
             kAudioObjectPropertyScopeWildcard,
             kAudioObjectPropertyElementWildcard };
}

AudioDeviceID findCoreAudioDevice(const juce::String& name)
{
    if (name.isEmpty())
        return kAudioObjectUnknown;

    const AudioObjectPropertyAddress listAddress { kAudioHardwarePropertyDevices,
                                                    kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &listAddress, 0, nullptr, &size) != noErr
        || size < sizeof(AudioDeviceID))
        return kAudioObjectUnknown;

    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &listAddress, 0, nullptr, &size, devices.data()) != noErr)
        return kAudioObjectUnknown;

    const AudioObjectPropertyAddress nameAddress { kAudioDevicePropertyDeviceNameCFString,
                                                    kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMain };

    for (const AudioDeviceID id : devices)
    {
        CFStringRef cfName = nullptr;
        UInt32 nameSize = sizeof(cfName);
        if (AudioObjectGetPropertyData(id, &nameAddress, 0, nullptr, &nameSize, &cfName) != noErr || cfName == nullptr)
            continue;

        char buffer[512];
        const bool matched = CFStringGetCString(cfName, buffer, sizeof(buffer), kCFStringEncodingUTF8)
                             && name == juce::String::fromUTF8(buffer);
        CFRelease(cfName);
        if (matched)
            return id;
    }

    return kAudioObjectUnknown;
}
#endif

} // namespace

AudioEngine::AudioEngine(AppSettings& settings)
    : settings_(settings)
{
    catalogue_ = std::make_unique<PluginCatalogue>(settings_);
    rack_ = std::make_unique<PluginRack>(*catalogue_, compensationSamples_);
    rack_->setDirtyHandler([this] { noteSessionEdit(); });
    recorder_ = std::make_unique<Recorder>();
    recorder_->setDirtyHandler([this] { noteSessionEdit(); });
}

AudioEngine::~AudioEngine()
{
    deviceManager_.removeAudioCallback(this);
    deviceManager_.removeChangeListener(this);
    deviceManager_.closeAudioDevice();
    recorder_.reset();
    removeOverloadListener();
}

void AudioEngine::start(bool allowInput)
{
    if (started_)
    {
        deviceManager_.removeAudioCallback(this);
        deviceManager_.removeChangeListener(this);
    }

    microphoneGranted_ = allowInput;
    persistSetup_ = allowInput;
    deviceError_.store(false, std::memory_order_relaxed);

    std::unique_ptr<juce::XmlElement> saved;
    if (allowInput)
        saved = settings_.loadAudioSetup();

    const int inputs = allowInput ? kMaxChannels : 0;
    openError_ = deviceManager_.initialise(inputs, kMaxChannels, saved.get(), true);

    deviceManager_.addChangeListener(this);
    deviceManager_.addAudioCallback(this);
    started_ = true;
    pollDeviceStats();
    saveSetupIfAllowed();
    if (deviceManager_.getCurrentAudioDevice() != nullptr)
        ensureSessionFolder();
}

void AudioEngine::pollDeviceStats()
{
    if (deviceError_.exchange(false, std::memory_order_relaxed))
        openError_ = "The audio device reported an error.";

    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr || ! device->isOpen())
    {
        deviceOpen_.store(false, std::memory_order_relaxed);
        cpuUsage_.store(0.0f, std::memory_order_relaxed);
        deviceName_ = "No device";
        return;
    }

    deviceName_ = device->getName();
    cpuUsage_.store(static_cast<float>(deviceManager_.getCpuUsage()), std::memory_order_relaxed);
    const double rate = device->getCurrentSampleRate();
    const int buffer = device->getCurrentBufferSizeSamples();
    const int inputLatency = device->getInputLatencyInSamples();
    const int outputLatency = device->getOutputLatencyInSamples();
    const auto formula = formulaForDeviceType(device->getTypeName().toRawUTF8());

    sampleRate_.store(rate, std::memory_order_relaxed);
    bufferSamples_.store(buffer, std::memory_order_relaxed);
    inputLatencySamples_.store(inputLatency, std::memory_order_relaxed);
    outputLatencySamples_.store(outputLatency, std::memory_order_relaxed);
    formulaValue_.store(static_cast<int>(formula), std::memory_order_relaxed);
    deviceOpen_.store(true, std::memory_order_relaxed);
}

LatencyNumbers AudioEngine::latencyNumbers() const
{
    LatencyNumbers numbers;
    numbers.sampleRate = sampleRate_.load(std::memory_order_relaxed);
    numbers.bufferSamples = bufferSamples_.load(std::memory_order_relaxed);
    numbers.inputSamples = inputLatencySamples_.load(std::memory_order_relaxed);
    numbers.outputSamples = outputLatencySamples_.load(std::memory_order_relaxed);
    numbers.compensationSamples = compensationSamples_.load(std::memory_order_relaxed);
    const auto dropouts = dropoutCount_.load(std::memory_order_relaxed);
    numbers.xruns = static_cast<int>(std::min<std::uint32_t>(dropouts, 2147483647u));
    numbers.formula = static_cast<RoundTripFormula>(formulaValue_.load(std::memory_order_relaxed));
    numbers.deviceOpen = deviceOpen_.load(std::memory_order_relaxed);
    numbers.roundTripSamples = roundTripSamples(numbers.inputSamples,
                                                numbers.outputSamples,
                                                numbers.bufferSamples,
                                                numbers.compensationSamples,
                                                numbers.formula);
    return numbers;
}

const AudioThreadConfig& AudioEngine::currentConfig() const
{
    const int index = configIndex_.load(std::memory_order_acquire);
    return configs_[static_cast<std::size_t>(index)];
}

int AudioEngine::visibleChannels() const
{
    return currentConfig().routing.visibleChannels;
}

int AudioEngine::inputCount() const
{
    return currentConfig().routing.inputCount;
}

int AudioEngine::outputCount() const
{
    return currentConfig().routing.outputCount;
}

float AudioEngine::rmsFor(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return 0.0f;
    return strips_[static_cast<std::size_t>(channel)].meter.rms.load(std::memory_order_relaxed);
}

float AudioEngine::peakFor(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return 0.0f;
    return strips_[static_cast<std::size_t>(channel)].meter.peak.load(std::memory_order_relaxed);
}

bool AudioEngine::clipFor(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return false;
    return strips_[static_cast<std::size_t>(channel)].meter.clipped.load(std::memory_order_relaxed);
}

bool AudioEngine::inputActive(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return false;
    return currentConfig().routing.inputPacked[static_cast<std::size_t>(channel)] >= 0;
}

void AudioEngine::requestClipClear(int channel)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;

    auto& meter = strips_[static_cast<std::size_t>(channel)].meter;
    meter.clearRequested.store(true, std::memory_order_relaxed);
    // No callback is running, so nothing else will consume the request.
    if (! deviceOpen_.load(std::memory_order_relaxed))
        meter.clipped.store(false, std::memory_order_relaxed);
}

void AudioEngine::requestClipClearAll()
{
    const bool applyNow = ! deviceOpen_.load(std::memory_order_relaxed);
    for (auto& strip : strips_)
    {
        strip.meter.clearRequested.store(true, std::memory_order_relaxed);
        if (applyNow)
            strip.meter.clipped.store(false, std::memory_order_relaxed);
    }
}

void AudioEngine::resetDropouts()
{
    dropoutCount_.store(0, std::memory_order_relaxed);
}

void AudioEngine::noteDropout(int events)
{
    if (events <= 0)
        return;
    dropoutCount_.fetch_add(static_cast<std::uint32_t>(events), std::memory_order_relaxed);
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                   int numInputChannels,
                                                   float* const* outputChannelData,
                                                   int numOutputChannels,
                                                   int numSamples,
                                                   const juce::AudioIODeviceCallbackContext& context)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(context);

    const int64_t startedNs = steadyNowNs();
    const int slot = configIndex_.load(std::memory_order_acquire);
    const AudioThreadConfig& config = configs_[static_cast<std::size_t>(slot)];

    // A late wake-up is a gap. The clock read is a vDSO call, not a lock or an allocation.
    const int64_t previousNs = lastCallbackNs_.exchange(startedNs, std::memory_order_relaxed);
    const bool skipGap = skipNextGap_.exchange(false, std::memory_order_relaxed);
    noteDropout(dropoutGapCount(previousNs, startedNs, config.expectedPeriodNs, skipGap));

    // Record tap is a lock-free copy of the raw input. Playback, when active,
    // replaces that input before meters and plugins. Neither path takes a lock.
    if (recorder_ != nullptr)
        recorder_->processRecord(inputChannelData,
                                 numInputChannels,
                                 config.routing.inputPacked.data(),
                                 kMaxChannels,
                                 numSamples);

    const bool playing = recorder_ != nullptr && numSamples > 0 && numSamples <= playbackMax_
                         && recorder_->processPlayback(playbackPtrs_.data(), numSamples);
    if (playing)
    {
        AudioThreadConfig playConfig = config;
        for (int channel = 0; channel < kMaxChannels; ++channel)
            playConfig.routing.inputPacked[static_cast<std::size_t>(channel)] = channel;
        processPassthrough(playbackPtrs_.data(),
                           kMaxChannels,
                           outputChannelData,
                           numOutputChannels,
                           numSamples,
                           playConfig,
                           strips_.data(),
                           kMaxChannels);
    }
    else
    {
        processPassthrough(inputChannelData,
                           numInputChannels,
                           outputChannelData,
                           numOutputChannels,
                           numSamples,
                           config,
                           strips_.data(),
                           kMaxChannels);
    }

    // Plugins and the alignment delay run on the dry copy. No lock and no allocation.
    if (rack_ != nullptr)
        rack_->process(outputChannelData, numOutputChannels, numSamples, config.routing);

    if (recorder_ != nullptr)
        recorder_->noteCallback();

    noteDropout(dropoutOverrunCount(steadyNowNs() - startedNs, config.expectedPeriodNs));
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    for (auto& strip : strips_)
    {
        strip.meterState = {};
        strip.meter.rms.store(0.0f, std::memory_order_relaxed);
        strip.meter.peak.store(0.0f, std::memory_order_relaxed);
        strip.meter.clipped.store(false, std::memory_order_relaxed);
    }

    // The gap between the old stream and this one is not a dropout.
    lastCallbackNs_.store(0, std::memory_order_relaxed);
    skipNextGap_.store(true, std::memory_order_relaxed);

    if (device != nullptr)
    {
        publishConfig(*device);
        playbackMax_ = std::max(8192, device->getCurrentBufferSizeSamples());
        playbackScratch_.assign(static_cast<std::size_t>(kMaxChannels * playbackMax_), 0.0f);
        for (int channel = 0; channel < kMaxChannels; ++channel)
            playbackPtrs_[static_cast<std::size_t>(channel)] = playbackScratch_.data()
                                                               + static_cast<std::size_t>(channel * playbackMax_);
        if (recorder_ != nullptr)
            recorder_->setDevice(device->getCurrentSampleRate(), true);
        if (rack_ != nullptr)
            rack_->prepare(device->getCurrentSampleRate(),
                           device->getCurrentBufferSizeSamples(),
                           currentConfig().routing);
        installOverloadListener(device->getName());
    }
}

void AudioEngine::audioDeviceStopped()
{
    deviceOpen_.store(false, std::memory_order_relaxed);
    skipNextGap_.store(true, std::memory_order_relaxed);
    if (recorder_ != nullptr)
    {
        recorder_->setDevice(0.0, false);
        recorder_->stop();
    }
    if (rack_ != nullptr)
        rack_->deviceStopped();
    removeOverloadListener();
}

void AudioEngine::audioDeviceError(const juce::String& errorMessage)
{
    juce::ignoreUnused(errorMessage);
    // This may run on any thread, so the only thing stored here is a flag.
    deviceError_.store(true, std::memory_order_relaxed);
}

void AudioEngine::publishConfig(juce::AudioIODevice& device)
{
    std::array<bool, kMaxChannels> inputs {};
    std::array<bool, kMaxChannels> outputs {};
    const auto inputMask = device.getActiveInputChannels();
    const auto outputMask = device.getActiveOutputChannels();

    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        inputs[static_cast<std::size_t>(channel)] = inputMask[channel];
        outputs[static_cast<std::size_t>(channel)] = outputMask[channel];
        strips_[static_cast<std::size_t>(channel)].inputIndex = channel;
        strips_[static_cast<std::size_t>(channel)].outputIndex = channel;
    }

    AudioThreadConfig next;
    next.routing = makeRouting(inputs, outputs);
    next.meterTiming = meterTimingFor(device.getCurrentSampleRate());
    next.expectedPeriodNs = expectedPeriodNs(device.getCurrentSampleRate(), device.getCurrentBufferSizeSamples());

    const int current = configIndex_.load(std::memory_order_relaxed);
    const int slot = 1 - current;
    configs_[static_cast<std::size_t>(slot)] = std::move(next);
    configIndex_.store(slot, std::memory_order_release);

    sampleRate_.store(device.getCurrentSampleRate(), std::memory_order_relaxed);
    bufferSamples_.store(device.getCurrentBufferSizeSamples(), std::memory_order_relaxed);
    inputLatencySamples_.store(device.getInputLatencyInSamples(), std::memory_order_relaxed);
    outputLatencySamples_.store(device.getOutputLatencyInSamples(), std::memory_order_relaxed);
    formulaValue_.store(static_cast<int>(formulaForDeviceType(device.getTypeName().toRawUTF8())),
                        std::memory_order_relaxed);
    deviceOpen_.store(true, std::memory_order_relaxed);
}

void AudioEngine::installOverloadListener(const juce::String& deviceName)
{
    removeOverloadListener();

   #if JUCE_MAC
    const AudioDeviceID id = findCoreAudioDevice(deviceName);
    if (id == kAudioObjectUnknown)
        return;

    const AudioObjectPropertyAddress address = overloadAddress();
    if (AudioObjectAddPropertyListener(id, &address, &overloadListener, &dropoutCount_) == noErr)
        overloadDeviceId_ = static_cast<std::uint32_t>(id);
   #else
    juce::ignoreUnused(deviceName);
   #endif
}

void AudioEngine::removeOverloadListener()
{
   #if JUCE_MAC
    if (overloadDeviceId_ == 0)
        return;

    const AudioObjectPropertyAddress address = overloadAddress();
    AudioObjectRemovePropertyListener(static_cast<AudioDeviceID>(overloadDeviceId_),
                                      &address,
                                      &overloadListener,
                                      &dropoutCount_);
    overloadDeviceId_ = 0;
   #endif
}

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    juce::ignoreUnused(source);
    pollDeviceStats();
    saveSetupIfAllowed();
    if (auto* device = deviceManager_.getCurrentAudioDevice())
    {
        if (device->isOpen())
        {
            if (rack_ != nullptr)
                rack_->updateRouting(routingFromDevice(*device));
            ensureSessionFolder();
        }
    }
}

void AudioEngine::saveSetupIfAllowed()
{
    if (! persistSetup_ || deviceManager_.getCurrentAudioDevice() == nullptr)
        return;

    if (auto xml = deviceManager_.createStateXml())
        settings_.saveAudioSetup(xml.get());
}

Routing AudioEngine::routingFromDevice(const juce::AudioIODevice& device) const
{
    std::array<bool, kMaxChannels> inputs {};
    std::array<bool, kMaxChannels> outputs {};
    const auto inputMask = device.getActiveInputChannels();
    const auto outputMask = device.getActiveOutputChannels();
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        inputs[static_cast<std::size_t>(channel)] = inputMask[channel];
        outputs[static_cast<std::size_t>(channel)] = outputMask[channel];
    }
    return makeRouting(inputs, outputs);
}

void AudioEngine::pushRouting(const Routing& routing)
{
    if (rack_ != nullptr)
        rack_->updateRouting(routing);
}

ChannelSnapshot AudioEngine::channelSnapshot(int channel) const
{
    if (rack_ == nullptr)
        return {};
    return rack_->snapshot(channel);
}

void AudioEngine::loadPlugin(int channel, int slot, const juce::PluginDescription& description, bool openEditor)
{
    if (rack_ != nullptr)
        rack_->loadPlugin(channel, slot, description, {}, false, true, openEditor);
}

void AudioEngine::removePlugin(int channel, int slot)
{
    if (rack_ != nullptr)
        rack_->removePlugin(channel, slot);
}

void AudioEngine::setSlotBypassed(int channel, int slot, bool bypassed)
{
    if (rack_ != nullptr)
        rack_->setBypassed(channel, slot, bypassed);
}

void AudioEngine::setChannelExcluded(int channel, bool excluded)
{
    if (channel >= 0 && channel < kMaxChannels)
        strips_[static_cast<std::size_t>(channel)].excludeFromCompensation = excluded;
    if (rack_ != nullptr)
        rack_->setExcluded(channel, excluded);
}

void AudioEngine::openPluginEditor(int channel, int slot)
{
    if (rack_ != nullptr)
        rack_->openEditor(channel, slot);
}

void AudioEngine::togglePluginEditor(int channel, int slot)
{
    if (rack_ != nullptr)
        rack_->toggleEditor(channel, slot);
}

bool AudioEngine::isPluginEditorOpen(int channel, int slot) const
{
    return rack_ != nullptr && rack_->isEditorOpen(channel, slot);
}

void AudioEngine::setRecordArmed(int channel, bool armed)
{
    if (recorder_ != nullptr)
        recorder_->setArmed(channel, armed);
}

bool AudioEngine::isRecordArmed(int channel) const
{
    return recorder_ != nullptr && recorder_->isArmed(channel);
}

void AudioEngine::setChannelName(int channel, const juce::String& name)
{
    if (recorder_ != nullptr)
        recorder_->setChannelName(channel, name);
}

juce::String AudioEngine::channelName(int channel) const
{
    return recorder_ != nullptr ? recorder_->channelName(channel) : juce::String();
}

void AudioEngine::transportRecord()
{
    ensureSessionFolder();
    if (recorder_ == nullptr)
        return;
    const auto routing = currentConfig().routing;
    recorder_->record(routing.inputPacked.data(), kMaxChannels);
    noteSessionEdit();
}

void AudioEngine::transportStop()
{
    if (recorder_ == nullptr)
        return;
    const bool active = recorder_->isRecording() || recorder_->isPlaying();
    recorder_->stop();
    if (active)
        noteSessionEdit();
}

void AudioEngine::transportPlay()
{
    if (recorder_ != nullptr)
        recorder_->play();
}

void AudioEngine::transportLocate(std::int64_t sample)
{
    if (recorder_ != nullptr)
        recorder_->locate(sample);
}

void AudioEngine::transportJump(int direction)
{
    if (recorder_ != nullptr)
        recorder_->jumpMarker(direction);
}

void AudioEngine::transportNudge(double seconds)
{
    if (recorder_ != nullptr)
        recorder_->nudgeSeconds(seconds);
}

TransportView AudioEngine::transportView() const
{
    if (recorder_ == nullptr)
        return {};
    return recorder_->view();
}

void AudioEngine::startNewSession()
{
    if (recorder_ != nullptr)
        recorder_->clearTakes();
    sessionFolder_ = juce::File();
    ensureSessionFolder();
}

void AudioEngine::setSessionMeters(bool peak, int rmsReferenceDb)
{
    sessionPeak_ = peak;
    sessionReferenceDb_ = normaliseRmsReferenceDb(rmsReferenceDb);
}

void AudioEngine::noteSessionEdit()
{
    if (restoringSession_)
        return;
    sessionDirty_ = true;
    sessionDirtyAtMs_ = juce::Time::getMillisecondCounter();
}

void AudioEngine::touchSession()
{
    if (restoringSession_ || sessionDirty_ || sessionFolder_ == juce::File())
        return;
    sessionDirty_ = true;
    sessionDirtyAtMs_ = juce::Time::getMillisecondCounter();
}

void AudioEngine::setSessionPage(int page)
{
    const int next = page == 2 ? 2 : 1;
    if (next == sessionPage_)
        return;
    sessionPage_ = next;
    noteSessionEdit();
}

void AudioEngine::setPageRestoreHandler(std::function<void(int)> handler)
{
    pageRestoreHandler_ = std::move(handler);
}

void AudioEngine::syncRecorderFolder()
{
    if (recorder_ == nullptr || sessionFolder_ == juce::File())
        return;
    const auto layout = sessionLayoutFor(sessionFolder_.getFullPathName().toStdString());
    recorder_->setAudioFolder(juce::File(layout.audioFolder));
}

void AudioEngine::ensureSessionFolder()
{
    if (sessionFolder_.getFullPathName().isNotEmpty())
    {
        syncRecorderFolder();
        return;
    }

    auto root = juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("YouHost");
    root.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M-%S");
    auto folder = root.getChildFile(stamp);
    int suffix = 2;
    while (folder.exists())
        folder = root.getChildFile(stamp + "-" + juce::String(suffix++));

    saveSessionToFolder(folder);
    sessionMessage_ = "Recording to " + folder.getFullPathName();
}

void AudioEngine::setMeterRestoreHandler(std::function<void(bool, int)> handler)
{
    meterRestoreHandler_ = std::move(handler);
}

void AudioEngine::maintainSession()
{
    if (catalogue_ != nullptr)
        catalogue_->flushSave();

    if (! sessionDirty_ || sessionFolder_ == juce::File())
        return;
    if (juce::Time::getMillisecondCounter() - sessionDirtyAtMs_ < 1500u)
        return;

    sessionDirty_ = false;
    saveSession();
}

juce::File AudioEngine::suggestedSessionFolder() const
{
    if (sessionFolder_ != juce::File())
        return sessionFolder_;
    const auto last = settings_.loadLastSessionFolder();
    if (last.isNotEmpty())
        return juce::File(last);
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}

bool AudioEngine::saveSession()
{
    if (sessionFolder_ == juce::File())
        return false;
    return saveSessionToFolder(sessionFolder_);
}

bool AudioEngine::saveSessionToFolder(const juce::File& folder)
{
    if (folder == juce::File())
        return false;

    const auto layout = sessionLayoutFor(folder.getFullPathName().toStdString());
    sessionFolder_ = juce::File(layout.folder);
    sessionFolder_.createDirectory();
    juce::File(layout.audioFolder).createDirectory();

    SessionData data;
    data.peakMeter = sessionPeak_;
    data.rmsReferenceDb = sessionReferenceDb_;
    if (persistSetup_)
        data.device = deviceManager_.createStateXml();
    if (rack_ != nullptr)
        rack_->captureSession(data);
    if (recorder_ != nullptr)
        recorder_->captureSession(data);
    data.page = sessionPage_;

    const juce::File file(layout.sessionFile);
    if (! writeSessionFile(file, data))
    {
        sessionMessage_ = "Could not write the session file.";
        sessionDirty_ = true;
        return false;
    }

    syncRecorderFolder();
    settings_.saveLastSessionFolder(sessionFolder_.getFullPathName());
    if (sessionMessage_.isEmpty() || sessionMessage_.startsWith("Saved "))
        sessionMessage_ = "Saved " + sessionFolder_.getFileName();
    sessionDirty_ = false;
    return true;
}

bool AudioEngine::loadSessionFrom(const juce::File& fileOrFolder)
{
    juce::File folder = fileOrFolder;
    if (fileOrFolder.existsAsFile())
        folder = fileOrFolder.getParentDirectory();

    const auto layout = sessionLayoutFor(folder.getFullPathName().toStdString());
    SessionData data;
    if (! readSessionFile(juce::File(layout.sessionFile), data))
    {
        sessionMessage_ = "Could not read session.youhost.";
        return false;
    }

    if (recorder_ != nullptr)
        recorder_->stop();

    restoringSession_ = true;
    sessionFolder_ = juce::File(layout.folder);
    sessionPeak_ = data.peakMeter;
    sessionReferenceDb_ = data.rmsReferenceDb;
    sessionPage_ = data.page == 2 ? 2 : 1;
    if (meterRestoreHandler_ != nullptr)
        meterRestoreHandler_(sessionPeak_, sessionReferenceDb_);
    if (pageRestoreHandler_ != nullptr)
        pageRestoreHandler_(sessionPage_);

    if (rack_ != nullptr)
        rack_->restoreSession(data);
    if (recorder_ != nullptr)
        recorder_->restoreSession(data, juce::File(layout.audioFolder));

    for (int channel = 0; channel < kMaxChannels; ++channel)
        strips_[static_cast<std::size_t>(channel)].excludeFromCompensation =
            data.channels[static_cast<std::size_t>(channel)].excludeFromCompensation;

    if (microphoneGranted_ && data.device != nullptr)
    {
        deviceManager_.removeAudioCallback(this);
        deviceManager_.removeChangeListener(this);
        openError_ = deviceManager_.initialise(kMaxChannels, kMaxChannels, data.device.get(), true);
        deviceManager_.addChangeListener(this);
        deviceManager_.addAudioCallback(this);
        pollDeviceStats();
        saveSetupIfAllowed();
    }

    settings_.saveLastSessionFolder(sessionFolder_.getFullPathName());
    sessionMessage_ = "Opened " + sessionFolder_.getFileName();
    sessionDirty_ = false;
    restoringSession_ = false;
    return true;
}

} // namespace youhost
