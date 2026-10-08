#include "AudioEngine.h"
#include "AppSettings.h"
#include "MeterScale.h"
#include "SessionDocument.h"
#include "SessionFiles.h"
#include "TakeImport.h"

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
    auto* engine = static_cast<AudioEngine*>(client);
    if (engine != nullptr)
        engine->noteDropout(numberOfAddresses == 0 ? 1 : static_cast<int>(numberOfAddresses));
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
    channelGroup_.fill(-1);
    catalogue_ = std::make_unique<PluginCatalogue>(settings_);
    rack_ = std::make_unique<PluginRack>(*catalogue_, compensationSamples_, &settings_);
    rack_->setDirtyHandler([this] { noteSessionEdit(); });
    recorder_ = std::make_unique<Recorder>();
    recorder_->setDirtyHandler([this] { noteSessionEdit(); });
    wavBitDepth_ = settings_.loadWavBitDepth();
    recorder_->setWavBitDepth(wavBitDepth_);
    dropoutOriginNs_ = steadyNowNs();
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
    dropoutRing_.discardPending();
    dropoutMarks_.clear();
    cpuSamples_.clear();
    dropoutOriginNs_ = steadyNowNs();
    lastCpuSampleNs_ = 0;

    if (sessionFolder_ == juce::File())
        return;

    auto file = sessionFolder_.getChildFile("dropouts.csv");
    if (! dropoutHeaderWritten_ || ! file.existsAsFile())
    {
        if (! file.existsAsFile())
            file.appendText("wall_ms,steady_ns,timeline_sample,recording\n");
        dropoutHeaderWritten_ = true;
    }
    file.appendText(juce::String(juce::Time::currentTimeMillis()) + ",reset,0,0\n");
}

void AudioEngine::noteDropout(int events)
{
    if (events <= 0)
        return;
    dropoutCount_.fetch_add(static_cast<std::uint32_t>(events), std::memory_order_relaxed);
    const bool recording = recorder_ != nullptr && recorder_->isRecording();
    const auto sample = recording ? recorder_->playhead() : static_cast<std::int64_t>(-1);
    const auto stamp = steadyNowNs();
    const int marks = std::min(events, 8);
    for (int index = 0; index < marks; ++index)
        dropoutRing_.push(stamp, sample, recording);
}

void AudioEngine::drainDropoutLog()
{
    DropoutMark batch[64];
    const int count = dropoutRing_.drain(batch, 64);
    const auto nowSteady = steadyNowNs();
    const auto nowWall = juce::Time::currentTimeMillis();

    if (dropoutOriginNs_ == 0)
        dropoutOriginNs_ = nowSteady;

    for (int index = 0; index < count; ++index)
    {
        dropoutMarks_.push_back(batch[index]);
        if (dropoutMarks_.size() > 8192)
            dropoutMarks_.erase(dropoutMarks_.begin());

        if (sessionFolder_ != juce::File())
        {
            const auto ageMs = std::max<std::int64_t>(0, (nowSteady - batch[index].steadyNs) / 1000000);
            const auto wall = nowWall - ageMs;
            auto file = sessionFolder_.getChildFile("dropouts.csv");
            if (! dropoutHeaderWritten_ || ! file.existsAsFile())
            {
                if (! file.existsAsFile())
                    file.appendText("wall_ms,steady_ns,timeline_sample,recording\n");
                dropoutHeaderWritten_ = true;
            }
            file.appendText(juce::String(wall) + ","
                            + juce::String(batch[index].steadyNs) + ","
                            + juce::String(batch[index].timelineSample) + ","
                            + juce::String(batch[index].recording ? 1 : 0) + "\n");
        }
    }

    if (lastCpuSampleNs_ == 0 || nowSteady - lastCpuSampleNs_ >= 200000000)
    {
        lastCpuSampleNs_ = nowSteady;
        cpuSamples_.push_back({ nowSteady, cpuUsage_.load(std::memory_order_relaxed) });
        if (cpuSamples_.size() > 20000)
            cpuSamples_.erase(cpuSamples_.begin(), cpuSamples_.begin() + 4000);
    }
}

DropoutSnapshot AudioEngine::dropoutSnapshot() const
{
    DropoutSnapshot snapshot;
    snapshot.nowNs = steadyNowNs();
    snapshot.originNs = dropoutOriginNs_;
    snapshot.total = static_cast<int>(std::min<std::uint32_t>(dropoutCount_.load(std::memory_order_relaxed), 2147483647u));
    snapshot.marks = dropoutMarks_;
    snapshot.cpu = cpuSamples_;
    snapshot.lastSteadyNs = latestMarkNs(snapshot.marks.data(), static_cast<int>(snapshot.marks.size()));
    return snapshot;
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

    const auto enabledLow = channelOnLo_.load(std::memory_order_relaxed);
    const auto enabledHigh = channelOnHi_.load(std::memory_order_relaxed);

    const bool playing = recorder_ != nullptr && numSamples > 0 && numSamples <= playbackMax_
                         && recorder_->processPlayback(playbackPtrs_.data(), numSamples);
    if (playing)
    {
        AudioThreadConfig playConfig = config;
        for (int channel = 0; channel < kMaxChannels; ++channel)
            playConfig.routing.inputPacked[static_cast<std::size_t>(channel)] = channel;
        // Every channel that is on, including ones with no plugin, is copied onto
        // its matching USB output here. Off channels are silenced. The rack then
        // runs plugins and the alignment delay, so a dry channel stays in time.
        processPassthrough(playbackPtrs_.data(),
                           kMaxChannels,
                           outputChannelData,
                           numOutputChannels,
                           numSamples,
                           playConfig,
                           strips_.data(),
                           kMaxChannels,
                           enabledLow,
                           enabledHigh);
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
                           kMaxChannels,
                           enabledLow,
                           enabledHigh);
    }

    // Plugins and the alignment delay run on the dry copy. Off channels are skipped.
    if (rack_ != nullptr)
        rack_->process(outputChannelData, numOutputChannels, numSamples, config.routing, enabledLow, enabledHigh);

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
    if (AudioObjectAddPropertyListener(id, &address, &overloadListener, this) == noErr)
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
                                      this);
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

void AudioEngine::transferPlugin(int fromChannel, int fromSlot, int toChannel, int toSlot, bool copy)
{
    if (rack_ != nullptr)
        rack_->transferPlugin(fromChannel, fromSlot, toChannel, toSlot, copy);
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
    if (channel < 0 || channel >= kMaxChannels)
        return;
    if (recorder_ != nullptr)
        recorder_->setArmed(channel, armed);
    storeChannelOn(channel, armed);
    if (rack_ != nullptr)
        rack_->setAudible(channel, armed);
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
    const bool recording = recorder_->isRecording();
    const bool active = recording || recorder_->isPlaying();
    recorder_->stop();
    if (recording)
        saveSession();
    else if (active)
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
        recorder_->stop();
    if (sessionFolder_ != juce::File())
        saveSession();
    if (recorder_ != nullptr)
        recorder_->clearTakes();
    sessionFolder_ = juce::File();
    sessionDirty_ = false;
    ensureSessionFolder();
}

void AudioEngine::setSessionMeters(bool peak, int rmsReferenceDb)
{
    sessionPeak_ = peak;
    sessionReferenceDb_ = normaliseRmsReferenceDb(rmsReferenceDb);
}

void AudioEngine::setWavBitDepth(int bits, bool markDirty)
{
    const int next = normaliseWavBitDepth(bits);
    const bool changed = next != wavBitDepth_;
    wavBitDepth_ = next;
    if (recorder_ != nullptr)
        recorder_->setWavBitDepth(next);
    settings_.saveWavBitDepth(next);
    if (markDirty && changed)
        noteSessionEdit();
}

juce::String AudioEngine::wavBitDepthLabel() const
{
    if (wavBitDepth_ == 16)
        return "16-bit";
    if (wavBitDepth_ == 32)
        return "32-bit float";
    return "24-bit";
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
    sessionMessage_ = "Session folder " + folder.getFullPathName();
}

void AudioEngine::setMeterRestoreHandler(std::function<void(bool, int)> handler)
{
    meterRestoreHandler_ = std::move(handler);
}

void AudioEngine::maintainSession()
{
    if (catalogue_ != nullptr)
        catalogue_->flushSave();
    drainDropoutLog();

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
        ensureSessionFolder();
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
    data.wavBitDepth = wavBitDepth_;
    if (persistSetup_)
        data.device = deviceManager_.createStateXml();
    if (rack_ != nullptr)
        rack_->captureSession(data);
    if (recorder_ != nullptr)
        recorder_->captureSession(data);
    captureDisplay(data);
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
    settings_.rememberRecentSession(sessionFolder_.getFullPathName());
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
        sessionMessage_ = "Could not open that session. The session file is missing or unreadable.";
        return false;
    }

    if (recorder_ != nullptr)
        recorder_->stop();

    restoringSession_ = true;
    sessionFolder_ = juce::File(layout.folder);
    sessionPeak_ = data.peakMeter;
    sessionReferenceDb_ = data.rmsReferenceDb;
    setWavBitDepth(data.wavBitDepth, false);
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

    applyDisplay(data);

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
    settings_.rememberRecentSession(sessionFolder_.getFullPathName());
    sessionMessage_ = "Opened " + sessionFolder_.getFileName();
    sessionDirty_ = false;
    restoringSession_ = false;
    dropoutHeaderWritten_ = false;
    return true;
}

bool AudioEngine::saveSessionAs(const juce::File& folder)
{
    if (folder.getFullPathName().isEmpty())
        return false;

    if (recorder_ != nullptr)
        recorder_->stop();
    if (sessionFolder_ == juce::File())
        ensureSessionFolder();
    else if (! saveSession())
        return false;

    if (folder.getFullPathName() == sessionFolder_.getFullPathName())
        return true;

    folder.createDirectory();
    for (const auto& child : sessionFolder_.findChildFiles(juce::File::findFiles, false))
        child.copyFileTo(folder.getChildFile(child.getFileName()));

    const auto audio = sessionFolder_.getChildFile(kAudioFolderName);
    const auto destinationAudio = folder.getChildFile(kAudioFolderName);
    destinationAudio.createDirectory();
    if (audio.isDirectory())
    {
        for (const auto& wav : audio.findChildFiles(juce::File::findFiles, false))
        {
            if (! wav.copyFileTo(destinationAudio.getChildFile(wav.getFileName())))
            {
                sessionMessage_ = "Could not copy " + wav.getFileName();
                return false;
            }
        }
    }

    sessionFolder_ = folder;
    dropoutHeaderWritten_ = false;
    syncRecorderFolder();
    if (! saveSession())
        return false;
    sessionMessage_ = "Saved a copy in " + folder.getFullPathName() + ". The original folder is unchanged.";
    return true;
}

bool AudioEngine::importRecordingFolder(const juce::File& folder)
{
    if (folder.getChildFile(kSessionFileName).existsAsFile())
        return loadSessionFrom(folder);
    if (folder.getFileName() == juce::String(kSessionFileName) && folder.existsAsFile())
        return loadSessionFrom(folder);

    auto source = folder.isDirectory() ? folder : folder.getParentDirectory();
    juce::Array<juce::File> wavs = source.findChildFiles(juce::File::findFiles, false, "*.wav");
    if (wavs.isEmpty())
        wavs = source.findChildFiles(juce::File::findFiles, true, "*.wav");
    if (wavs.isEmpty())
    {
        sessionMessage_ = "That folder has no WAV files.";
        return false;
    }

    if (recorder_ != nullptr)
        recorder_->stop();
    ensureSessionFolder();
    const auto layout = sessionLayoutFor(sessionFolder_.getFullPathName().toStdString());
    const juce::File audioFolder(layout.audioFolder);
    audioFolder.createDirectory();

    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(wavs.size()));
    for (const auto& wav : wavs)
        names.push_back(wav.getFileName().toStdString());
    const auto groups = groupImportedRecordings(names);

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    int added = 0;
    double importedRate = 0.0;
    for (const auto& group : groups)
    {
        std::array<juce::String, kMaxChannels> files {};
        std::int64_t length = 0;
        for (const auto& channel : group.channels)
        {
            if (channel.channel < 0 || channel.channel >= kMaxChannels)
                continue;

            juce::File sourceFile;
            for (const auto& wav : wavs)
            {
                if (wav.getFileName() == juce::String(channel.fileName))
                {
                    sourceFile = wav;
                    break;
                }
            }
            if (sourceFile == juce::File())
                continue;

            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(sourceFile));
            if (reader == nullptr || reader->lengthInSamples <= 0)
                continue;
            if (importedRate <= 0.0)
                importedRate = reader->sampleRate;
            else if (reader->sampleRate < importedRate - 1.0 || reader->sampleRate > importedRate + 1.0)
                continue;

            auto storedName = sourceFile.getFileName();
            const auto alreadyThere = audioFolder.getChildFile(storedName);
            if (sourceFile != alreadyThere)
            {
                if (alreadyThere.existsAsFile())
                    storedName = "import_" + storedName;
                if (! sourceFile.copyFileTo(audioFolder.getChildFile(storedName)))
                    continue;
            }

            files[static_cast<std::size_t>(channel.channel)] = storedName;
            length = std::max(length, static_cast<std::int64_t>(reader->lengthInSamples));
        }

        if (length > 0 && recorder_ != nullptr)
        {
            recorder_->addImportedTake(length, files, importedRate);
            ++added;
        }
    }

    if (added == 0)
    {
        sessionMessage_ = "Could not import those WAV files. Check the file names and that they share one sample rate.";
        return false;
    }

    saveSession();
    sessionMessage_ = "Imported " + juce::String(added) + (added == 1 ? " take." : " takes.");
    return true;
}

void AudioEngine::clearTimeline()
{
    if (recorder_ != nullptr)
    {
        recorder_->stop();
        recorder_->clearTakes();
    }
    saveSession();
    sessionMessage_ = "Timeline cleared. The WAV files are still in the audio folder.";
}

juce::StringArray AudioEngine::recentSessions() const
{
    return settings_.loadRecentSessions();
}

void AudioEngine::storeChannelOn(int channel, bool on)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    auto& word = channel < 64 ? channelOnLo_ : channelOnHi_;
    const auto bit = 1ull << (channel & 63);
    if (on)
        word.fetch_or(bit, std::memory_order_relaxed);
    else
        word.fetch_and(~bit, std::memory_order_relaxed);
}

void AudioEngine::bumpDisplay()
{
    ++displayRevision_;
    noteSessionEdit();
}

void AudioEngine::captureDisplay(SessionData& data) const
{
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        data.channels[static_cast<std::size_t>(channel)].color = channelColor_[static_cast<std::size_t>(channel)];
        data.channels[static_cast<std::size_t>(channel)].group = channelGroup_[static_cast<std::size_t>(channel)];
    }
    data.groups = groups_;
}

void AudioEngine::applyDisplay(const SessionData& data)
{
    std::array<bool, kMaxChannels> audible {};
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        const auto& source = data.channels[static_cast<std::size_t>(channel)];
        channelColor_[static_cast<std::size_t>(channel)] = normaliseX32Colour(source.color);
        const int group = source.group;
        channelGroup_[static_cast<std::size_t>(channel)] = (group >= 0 && group < kMaxDisplayGroups) ? group : -1;
        audible[static_cast<std::size_t>(channel)] = source.recordEnabled;
        setChannelOnBit(low, high, channel, source.recordEnabled);
    }
    channelOnLo_.store(low, std::memory_order_relaxed);
    channelOnHi_.store(high, std::memory_order_relaxed);
    groups_ = data.groups;
    selection_.clear();
    selectionAnchor_ = 0;
    if (rack_ != nullptr)
        rack_->setAudibleAll(audible);
    ++displayRevision_;
}

int AudioEngine::channelColor(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return 0;
    return channelColor_[static_cast<std::size_t>(channel)];
}

void AudioEngine::setChannelColor(int channel, int color)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    channelColor_[static_cast<std::size_t>(channel)] = normaliseX32Colour(color);
    bumpDisplay();
}

int AudioEngine::channelGroup(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return -1;
    return channelGroup_[static_cast<std::size_t>(channel)];
}

void AudioEngine::assignChannelsToGroup(const std::vector<int>& channels, int group)
{
    if (group >= kMaxDisplayGroups)
        return;
    if (group >= 0)
    {
        auto& stored = groups_[static_cast<std::size_t>(group)];
        stored.used = true;
        if (stored.name.isEmpty())
            stored.name = "Group " + juce::String(group + 1);
    }

    for (int channel : channels)
    {
        if (channel < 0 || channel >= kMaxChannels)
            continue;
        channelGroup_[static_cast<std::size_t>(channel)] = group;
    }
    bumpDisplay();
}

void AudioEngine::clearGroup(int group)
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return;
    for (int channel = 0; channel < kMaxChannels; ++channel)
        if (channelGroup_[static_cast<std::size_t>(channel)] == group)
            channelGroup_[static_cast<std::size_t>(channel)] = -1;
    groups_[static_cast<std::size_t>(group)] = {};
    bumpDisplay();
}

void AudioEngine::setGroupName(int group, const juce::String& name)
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return;
    auto& stored = groups_[static_cast<std::size_t>(group)];
    stored.used = true;
    const auto trimmed = name.trim().substring(0, 40);
    stored.name = trimmed.isEmpty() ? "Group " + juce::String(group + 1) : trimmed;
    bumpDisplay();
}

void AudioEngine::setGroupColor(int group, int color)
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return;
    groups_[static_cast<std::size_t>(group)].used = true;
    groups_[static_cast<std::size_t>(group)].color = normaliseX32Colour(color);
    bumpDisplay();
}

juce::String AudioEngine::groupName(int group) const
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return {};
    const auto& stored = groups_[static_cast<std::size_t>(group)];
    if (stored.name.isNotEmpty())
        return stored.name;
    return "Group " + juce::String(group + 1);
}

int AudioEngine::groupColor(int group) const
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return 0;
    return groups_[static_cast<std::size_t>(group)].color;
}

bool AudioEngine::groupCollapsed(int group) const
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return false;
    return groups_[static_cast<std::size_t>(group)].collapsed;
}

bool AudioEngine::groupHasMembers(int group) const
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return false;
    for (int channel : channelGroup_)
        if (channel == group)
            return true;
    return false;
}

void AudioEngine::toggleGroupCollapsed(int group)
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return;
    groups_[static_cast<std::size_t>(group)].collapsed = ! groups_[static_cast<std::size_t>(group)].collapsed;
    bumpDisplay();
}

void AudioEngine::expandAllGroups()
{
    for (auto& group : groups_)
        group.collapsed = false;
    bumpDisplay();
}

void AudioEngine::hideGroupedChannels()
{
    for (int group = 0; group < kMaxDisplayGroups; ++group)
        if (groupHasMembers(group))
            groups_[static_cast<std::size_t>(group)].collapsed = true;
    bumpDisplay();
}

bool AudioEngine::groupsAreExpanded() const
{
    for (int group = 0; group < kMaxDisplayGroups; ++group)
        if (groupHasMembers(group) && groups_[static_cast<std::size_t>(group)].collapsed)
            return false;
    return true;
}

bool AudioEngine::groupsAreHidden() const
{
    bool any = false;
    for (int group = 0; group < kMaxDisplayGroups; ++group)
    {
        if (! groupHasMembers(group))
            continue;
        any = true;
        if (! groups_[static_cast<std::size_t>(group)].collapsed)
            return false;
    }
    return any;
}

std::vector<StripItem> AudioEngine::displayStrips(int channelCount) const
{
    std::array<int, kMaxChannels> membership {};
    std::array<bool, kMaxDisplayGroups> collapsed {};
    membership.fill(-1);
    const int count = std::clamp(channelCount, 0, kMaxChannels);
    for (int channel = 0; channel < count; ++channel)
        membership[static_cast<std::size_t>(channel)] = channelGroup_[static_cast<std::size_t>(channel)];
    for (int group = 0; group < kMaxDisplayGroups; ++group)
        collapsed[static_cast<std::size_t>(group)] = groups_[static_cast<std::size_t>(group)].collapsed;

    std::vector<StripItem> items(static_cast<std::size_t>(count + kMaxDisplayGroups));
    const int written = layoutChannelStrips(count, membership.data(), collapsed.data(), items.data(), static_cast<int>(items.size()));
    if (written < static_cast<int>(items.size()))
        items.resize(static_cast<std::size_t>(written));
    return items;
}

void AudioEngine::selectChannel(int channel, bool extend)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    if (! extend)
    {
        selection_.clear();
        selection_.push_back(channel);
        selectionAnchor_ = channel;
        return;
    }

    selection_.clear();
    const int first = std::min(selectionAnchor_, channel);
    const int last = std::max(selectionAnchor_, channel);
    for (int index = first; index <= last; ++index)
        selection_.push_back(index);
}

bool AudioEngine::isChannelSelected(int channel) const
{
    return std::find(selection_.begin(), selection_.end(), channel) != selection_.end();
}

std::vector<int> AudioEngine::selectedChannels() const
{
    return selection_;
}

} // namespace youhost
