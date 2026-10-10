#include "AudioEngine.h"
#include "AppSettings.h"
#include "ChannelSelect.h"
#include "OutputGain.h"
#include "SignalPath.h"
#include "WaveformScale.h"
#include "DeviceWatch.h"
#include "RecordLock.h"
#include "RecordStart.h"
#include "HostLog.h"
#include "MeterScale.h"
#include "SessionDisk.h"
#include "SessionDocument.h"
#include "SessionFiles.h"
#include "SessionNames.h"
#include "StallWatch.h"
#include "TakeImport.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <vector>

#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

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

// A saved mask is ignored. Missing channel attributes make the device open its default set,
// which initialise asked for as every channel up to kMaxChannels.
void ignoreSavedChannelMask(juce::XmlElement* saved)
{
    if (saved == nullptr)
        return;
    saved->removeAttribute("audioDeviceInChans");
    saved->removeAttribute("audioDeviceOutChans");
}

std::atomic<int> stallDumpFd { -1 };
std::atomic<std::int64_t> stallLoggedNs { 0 };

void stallStackHandler(int)
{
    void* frames[32];
    const int count = ::backtrace(frames, 32);
    const int fd = stallDumpFd.load(std::memory_order_relaxed);
    if (fd >= 0 && count > 0)
        ::backtrace_symbols_fd(frames, count, fd);
}

void installStallHandler()
{
    static std::atomic<int> once { 0 };
    if (once.exchange(1, std::memory_order_relaxed) != 0)
        return;
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = stallStackHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &action, nullptr);
}

void noteMessageBeat()
{
    auto& clock = StallClock::get();
    clock.noteThread(clock.messageThread);
    clock.messageNs.store(steadyNowNs(), std::memory_order_relaxed);
    if (clock.messagePhase.load(std::memory_order_relaxed) == kPhaseIdle)
        clock.messagePhase.store(kPhaseMessage, std::memory_order_relaxed);
}

class StallThread : public juce::Thread
{
public:
    explicit StallThread(AppSettings& settings)
        : juce::Thread("youhost-watch"),
          settings_(settings)
    {
    }

    void run() override
    {
        installStallHandler();
        while (! threadShouldExit())
        {
            wait(500);
            if (threadShouldExit())
                return;
            logIfStalled();
        }
    }

private:
    void logIfStalled()
    {
        auto& clock = StallClock::get();
        const auto now = steadyNowNs();
        const auto messageBeat = clock.messageNs.load(std::memory_order_relaxed);
        const auto audioBeat = clock.audioNs.load(std::memory_order_relaxed);
        const bool message = beatIsStale(now, messageBeat, kStallLimitNs);
        const bool audio = clock.audioLive.load(std::memory_order_relaxed) != 0
                           && beatIsStale(now, audioBeat, kStallLimitNs);
        if (! message && ! audio)
            return;
        const auto previous = stallLoggedNs.load(std::memory_order_relaxed);
        if (previous > 0 && now - previous < 10000000000LL)
            return;
        stallLoggedNs.store(now, std::memory_order_relaxed);

        char names[160] {};
        clock.copyNames(names, sizeof(names));
        juce::String line = "stall";
        if (message)
            line += " message " + juce::String(static_cast<std::int64_t>((now - messageBeat) / 1000000))
                    + " ms phase=" + stallPhaseName(clock.messagePhase.load(std::memory_order_relaxed));
        if (audio)
            line += " audio " + juce::String(static_cast<std::int64_t>((now - audioBeat) / 1000000))
                    + " ms phase=" + stallPhaseName(clock.audioPhase.load(std::memory_order_relaxed));
        line += " plugins=" + juce::String(clock.activePlugins.load(std::memory_order_relaxed));
        if (names[0] != '\0')
            line += " " + juce::String(names);
        appendHostLog(settings_, line);

        const auto file = hostLogFile(settings_);
        const int fd = ::open(file.getFullPathName().toRawUTF8(), O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd < 0)
            return;
        stallDumpFd.store(fd, std::memory_order_relaxed);
        auto dump = [&](std::uintptr_t bits, const char* header)
        {
            if (bits == 0)
                return;
            pthread_t thread {};
            std::memcpy(&thread, &bits, sizeof(thread));
            if (::write(fd, header, std::strlen(header)) < 0)
                return;
            pthread_kill(thread, SIGUSR2);
            juce::Thread::sleep(40);
        };
        if (message)
            dump(clock.messageThread.load(std::memory_order_acquire), "\nmessage stack\n");
        if (audio)
            dump(clock.audioThread.load(std::memory_order_acquire), "\naudio stack\n");
        stallDumpFd.store(-1, std::memory_order_relaxed);
        ::close(fd);
    }

    AppSettings& settings_;
};

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

juce::String recordingCloseText(bool locked)
{
    return locked ? juce::String("Unlock recording first.") : juce::String("Stop recording first.");
}

AudioEngine::AudioEngine(AppSettings& settings)
    : settings_(settings)
{
    channelGroup_.fill(-1);
    listen_.fill(ChannelListen::record);
    for (auto& gain : outputGain_)
        gain.store(1.0f, std::memory_order_relaxed);
    catalogue_ = std::make_unique<PluginCatalogue>(settings_);
    rack_ = std::make_unique<PluginRack>(*catalogue_, compensationSamples_, &settings_);
    rack_->setDirtyHandler([this] { noteSessionEdit(); });
    rack_->setPluginTrace([this](int channel, int slot, const juce::String& phase, const juce::String& name, const juce::String& identifier)
    {
        notePluginTrace(channel, slot, phase, name, identifier);
    });
    recorder_ = std::make_unique<Recorder>();
    recorder_->setDirtyHandler([this] { noteSessionEdit(); });
    wavBitDepth_ = settings_.loadWavBitDepth();
    recorder_->setWavBitDepth(wavBitDepth_);
    preferredRate_ = settings_.loadPreferredSampleRate();
    preferredBuffer_ = settings_.loadPreferredBuffer();
    if (preferredRate_ < 1000.0)
        preferredRate_ = 48000.0;
    if (preferredBuffer_ < 16)
        preferredBuffer_ = kNewSessionBufferSamples;
    dropoutOriginNs_ = steadyNowNs();
    playbackMax_ = 32768;
    playbackScratch_.assign(static_cast<std::size_t>(kMaxChannels) * static_cast<std::size_t>(playbackMax_), 0.0f);
    for (int channel = 0; channel < kMaxChannels; ++channel)
        playbackPtrs_[static_cast<std::size_t>(channel)] = playbackScratch_.data()
                                                           + static_cast<std::size_t>(channel * playbackMax_);
    noteMessageBeat();
    stallThread_ = std::make_unique<StallThread>(settings_);
    stallThread_->startThread();
    sessionDisk_ = std::make_unique<SessionDisk>();
}

AudioEngine::~AudioEngine()
{
    if (alive_ != nullptr)
        alive_->store(false);
    sessionDisk_.reset();
    if (stallThread_ != nullptr)
    {
        stallThread_->stopThread(1000);
        stallThread_.reset();
    }
    prepareForQuit();
    recorder_.reset();
    rack_.reset();
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
    ignoreSavedChannelMask(saved.get());
    lastFullOpenName_.clear();
    lastFullOpenInputs_ = -1;
    lastFullOpenOutputs_ = -1;
    rememberSavedSetup(saved.get());

    const int inputs = allowInput ? kMaxChannels : 0;
    openError_ = deviceManager_.initialise(inputs, kMaxChannels, saved.get(), true);

    if (allowInput && wantedName_.isNotEmpty())
    {
        auto* device = deviceManager_.getCurrentAudioDevice();
        const auto opened = device != nullptr ? device->getName() : juce::String();
        if (opened != wantedName_)
        {
            awaitingSavedDevice_ = true;
            startupFallbackName_ = opened;
            startupDeviceNote_ = "Saved device \"" + wantedName_ + "\" is not available. YouHost will open it when it appears.";
        }
    }

    deviceManager_.addChangeListener(this);
    deviceManager_.addAudioCallback(this);
    started_ = true;
    if (allowInput)
        reconcileStartupDevice();
    pollDeviceStats();
    if (! awaitingSavedDevice_)
        saveSetupIfAllowed();
}

void AudioEngine::pollDeviceStats()
{
    if (auto* messages = juce::MessageManager::getInstanceWithoutCreating())
        if (! messages->isThisTheMessageThread())
            return;

    noteMessageBeat();

    if (deviceError_.exchange(false, std::memory_order_relaxed))
        openError_ = "The audio device reported an error.";

    if (offlineTemplate_)
    {
        deviceOpen_.store(false, std::memory_order_relaxed);
        deviceName_ = juce::String(kOfflineDeviceName);
        publishSessionChannelLimit();
        return;
    }

    auto* device = deviceManager_.getCurrentAudioDevice();
    const bool open = device != nullptr && device->isOpen();
    if (reopenInProgress_)
    {
        if (open)
            applyOpenDevice(*device, device->getName() == wantedName_);
        return;
    }

    if (! open)
    {
        handleDeviceDown();
        return;
    }

    juce::AudioDeviceManager::AudioDeviceSetup current;
    deviceManager_.getAudioDeviceSetup(current);
    const bool namedBySetup = device->getName() == current.outputDeviceName || device->getName() == current.inputDeviceName;
    const bool userChoseThis = namedBySetup && wantedName_.isNotEmpty() && device->getName() != wantedName_ && ! awaitingSavedDevice_;
    if (userChoseThis)
    {
        deviceLostBanner_ = false;
        lossFinalized_ = false;
        downSinceMs_ = 0;
        deviceDown_.store(false, std::memory_order_relaxed);
        wantedName_ = device->getName();
        applyOpenDevice(*device, true);
        return;
    }

    if (awaitingSavedDevice_ && device->getName() != wantedName_ && device->getName() != startupFallbackName_)
    {
        awaitingSavedDevice_ = false;
        startupDeviceNote_.clear();
        startupFallbackName_.clear();
        deviceLostBanner_ = false;
        lossFinalized_ = false;
        downSinceMs_ = 0;
        deviceDown_.store(false, std::memory_order_relaxed);
        wantedName_ = device->getName();
        applyOpenDevice(*device, true);
        return;
    }

    if ((awaitingSavedDevice_ || deviceLostBanner_) && device->getName() != wantedName_)
    {
        applyOpenDevice(*device, false);
        tryReopenWanted();
        return;
    }

    deviceLostBanner_ = false;
    lossFinalized_ = false;
    awaitingSavedDevice_ = false;
    startupDeviceNote_.clear();
    startupFallbackName_.clear();
    downSinceMs_ = 0;
    deviceDown_.store(false, std::memory_order_relaxed);
    applyOpenDevice(*device, true);
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

SessionChannelView AudioEngine::channelView() const
{
    if (offlineTemplate_)
        return sessionChannelView(0, false, true, false);
    const bool open = deviceOpen_.load(std::memory_order_relaxed);
    const int inputs = open ? inputCount() : 0;
    return sessionChannelView(inputs, open, false, revealUnsupported_);
}

void AudioEngine::setOfflineTemplate(bool offline)
{
    if (! recordActionAllowed(RecordDisrupt::offlineSwitch, isRecording(), recordLockArmed_))
        return;
    if (offlineTemplate_ == offline)
        return;
    offlineTemplate_ = offline;
    revealUnsupported_ = false;
    if (offline)
    {
        if (auto* live = deviceManager_.getCurrentAudioDevice())
        {
            if (live->getCurrentSampleRate() > 0.0)
                preferredRate_ = live->getCurrentSampleRate();
            if (live->getCurrentBufferSizeSamples() > 0)
                preferredBuffer_ = live->getCurrentBufferSizeSamples();
            settings_.savePreferredSampleRate(preferredRate_);
            settings_.savePreferredBuffer(preferredBuffer_);
        }
        closingDevice_.store(true, std::memory_order_release);
        deviceManager_.closeAudioDevice();
        closingDevice_.store(false, std::memory_order_release);
        deviceOpen_.store(false, std::memory_order_relaxed);
        deviceName_ = juce::String(kOfflineDeviceName);
    }
    publishSessionChannelLimit();
    ++displayRevision_;
}

void AudioEngine::setPreferredSampleRate(double rate)
{
    if (! recordActionAllowed(RecordDisrupt::changeRate, isRecording(), recordLockArmed_))
        return;
    if (! (rate > 0.0))
        return;
    preferredRate_ = rate;
    settings_.savePreferredSampleRate(rate);
    if (offlineTemplate_ && recorder_ != nullptr)
        recorder_->setTimelineSampleRate(rate);
}

void AudioEngine::setPreferredBuffer(int samples)
{
    if (! recordActionAllowed(RecordDisrupt::changeBuffer, isRecording(), recordLockArmed_))
        return;
    if (samples < 16)
        return;
    preferredBuffer_ = samples;
    settings_.savePreferredBuffer(samples);
}

bool AudioEngine::isRecording() const
{
    return recorder_ != nullptr && recorder_->isRecording();
}

bool AudioEngine::recordingLocked() const
{
    return recordLockEngaged(recordLockArmed_, isRecording());
}

void AudioEngine::setRecordLockArmed(bool armed)
{
    recordLockArmed_ = armed;
}

juce::String AudioEngine::takeSessionRateNotice()
{
    auto notice = sessionRateNotice_;
    sessionRateNotice_.clear();
    return notice;
}

bool AudioEngine::selectionListenNeedsConfirm(ChannelListen mode) const
{
    if (! isRecording() || mode == ChannelListen::record)
        return false;
    const auto selected = selectedChannels();
    const auto targets = channelsForGlobalListen(selected.data(), static_cast<int>(selected.size()), visibleChannels());
    for (const int channel : targets)
        if (channelListen(channel) == ChannelListen::record)
            return true;
    return false;
}

void AudioEngine::setSelectionListen(ChannelListen mode, bool confirmed)
{
    if (! recordActionAllowed(RecordDisrupt::globalListen, isRecording(), recordLockArmed_))
        return;
    if (! confirmed && selectionListenNeedsConfirm(mode))
        return;
    const auto selected = selectedChannels();
    const auto targets = channelsForGlobalListen(selected.data(), static_cast<int>(selected.size()), visibleChannels());
    for (const int channel : targets)
        setChannelListen(channel, mode);
}

void AudioEngine::publishSessionChannelLimit()
{
    const auto view = channelView();
    sessionVisible_.store(view.visible, std::memory_order_relaxed);
    sessionAudioLimit_.store(view.cardInputs, std::memory_order_relaxed);
    if (rack_ != nullptr)
        rack_->setActiveChannels(view.cardInputs);
}

void AudioEngine::setRevealUnsupportedChannels(bool reveal)
{
    if (revealUnsupported_ == reveal)
        return;
    revealUnsupported_ = reveal;
    publishSessionChannelLimit();
    ++displayRevision_;
}

int AudioEngine::visibleChannels() const
{
    return sessionVisible_.load(std::memory_order_relaxed);
}

juce::String AudioEngine::hiddenChannelNote() const
{
    return juce::String(youhost::hiddenChannelNote(channelView()));
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
    {
        auto& clock = StallClock::get();
        clock.noteThread(clock.audioThread);
        clock.audioLive.store(1, std::memory_order_relaxed);
        clock.audioNs.store(startedNs, std::memory_order_relaxed);
        clock.audioPhase.store(kPhaseAudio, std::memory_order_relaxed);
    }
    const int slot = configIndex_.load(std::memory_order_acquire);
    const AudioThreadConfig& config = configs_[static_cast<std::size_t>(slot)];

    // A late wake-up is a gap. The clock read is a vDSO call, not a lock or an allocation.
    const int64_t previousNs = lastCallbackNs_.exchange(startedNs, std::memory_order_relaxed);
    const bool skipGap = skipNextGap_.exchange(false, std::memory_order_relaxed);
    noteDropout(dropoutGapCount(previousNs, startedNs, config.expectedPeriodNs, skipGap));

    // Record tap is a lock-free copy of the raw input. Playback, when active,
    // replaces that input before meters and plugins. Neither path takes a lock.
    // The tap scans every channel. sessionAudioLimit_ is only the plugin CPU
    // limit: using it as the scan length made Play open no files.
    const int sessionChannels = sessionAudioLimit_.load(std::memory_order_relaxed);
    if (recorder_ != nullptr)
        recorder_->processRecord(inputChannelData,
                                 numInputChannels,
                                 config.routing.inputPacked.data(),
                                 kMaxChannels,
                                 numSamples,
                                 kMaxChannels);

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
                           sessionChannels,
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
                           sessionChannels,
                           enabledLow,
                           enabledHigh);
    }

    // Plugins and the alignment delay run on the dry copy. Off channels are skipped.
    if (rack_ != nullptr)
        rack_->process(outputChannelData, numOutputChannels, numSamples, config.routing, enabledLow, enabledHigh, sessionChannels);

    if (outputChannelData != nullptr && numSamples > 0)
    {
        for (int channel = 0; channel < sessionChannels; ++channel)
        {
            if (! channelIsOn(enabledLow, enabledHigh, channel))
                continue;
            const float gain = outputGain_[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed);
            if (sameFloatBits(gain, 1.0f))
                continue;
            const int packed = config.routing.outputPacked[static_cast<std::size_t>(channel)];
            if (packed < 0 || packed >= numOutputChannels)
                continue;
            float* output = outputChannelData[packed];
            if (output != nullptr)
                applyOutputTrim(output, numSamples, gain);
        }
    }

    if (recorder_ != nullptr)
        recorder_->noteCallback();

    const auto finishedNs = steadyNowNs();
    StallClock::get().audioNs.store(finishedNs, std::memory_order_relaxed);
    noteDropout(dropoutOverrunCount(finishedNs - startedNs, config.expectedPeriodNs));
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice*)
{
    for (auto& strip : strips_)
    {
        strip.meterState = {};
        strip.meter.rms.store(0.0f, std::memory_order_relaxed);
        strip.meter.peak.store(0.0f, std::memory_order_relaxed);
        strip.meter.clipped.store(false, std::memory_order_relaxed);
    }

    // The device thread only raises flags. Sample rate, plugin prepare, and the
    // CoreAudio listener run on the message thread in pollDeviceStats.
    lastCallbackNs_.store(0, std::memory_order_relaxed);
    skipNextGap_.store(true, std::memory_order_relaxed);
    deviceStarting_.store(true, std::memory_order_release);
    deviceDown_.store(false, std::memory_order_release);
    if (recorder_ != nullptr)
        recorder_->setCallbacksLive(true);
}

void AudioEngine::audioDeviceStopped()
{
    if (catalogue_ != nullptr)
        catalogue_->setDeviceOpen(false);
    deviceOpen_.store(false, std::memory_order_relaxed);
    StallClock::get().audioLive.store(0, std::memory_order_relaxed);
    skipNextGap_.store(true, std::memory_order_relaxed);
    if (recorder_ != nullptr)
        recorder_->setCallbacksLive(false);
    if (rack_ != nullptr)
        rack_->deviceStopped();
    if (! closingDevice_.load(std::memory_order_acquire))
        deviceDown_.store(true, std::memory_order_release);
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
    publishSessionChannelLimit();
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
}

void AudioEngine::noteChannelMasks(juce::AudioIODevice& device)
{
    if (catalogue_ != nullptr)
        catalogue_->setDeviceOpen(true);
    if (forcingChannels_)
        return;

    const auto inputNames = device.getInputChannelNames();
    const auto outputNames = device.getOutputChannelNames();
    const auto inputMask = device.getActiveInputChannels();
    const auto outputMask = device.getActiveOutputChannels();
    const int inputsToOpen = microphoneGranted_ ? channelsToOpen(inputNames.size()) : 0;
    const int outputsToOpen = channelsToOpen(outputNames.size());
    bool inputsOpen[kMaxChannels] {};
    bool outputsOpen[kMaxChannels] {};
    for (int index = 0; index < inputsToOpen; ++index)
        inputsOpen[index] = inputMask[index];
    for (int index = 0; index < outputsToOpen; ++index)
        outputsOpen[index] = outputMask[index];

    const bool complete = deviceMaskIsComplete(inputsOpen, inputsToOpen)
                          && deviceMaskIsComplete(outputsOpen, outputsToOpen);
    if (lastFullOpenName_ == device.getName()
        && lastFullOpenInputs_ == inputNames.size()
        && lastFullOpenOutputs_ == outputNames.size())
        return;

    lastFullOpenName_ = device.getName();
    lastFullOpenInputs_ = inputNames.size();
    lastFullOpenOutputs_ = outputNames.size();
    juce::String line;
    line << "device " << device.getName()
         << " inputs reported " << inputNames.size()
         << " active " << inputMask.countNumberOfSetBits()
         << " outputs reported " << outputNames.size()
         << " active " << outputMask.countNumberOfSetBits();
    appendHostLog(settings_, line);
    if (complete)
        return;

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager_.getAudioDeviceSetup(setup);
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.outputChannels.clear();
    if (inputsToOpen > 0)
        setup.inputChannels.setRange(0, inputsToOpen, true);
    if (outputsToOpen > 0)
        setup.outputChannels.setRange(0, outputsToOpen, true);
    appendHostLog(settings_, "opening every channel on " + device.getName());
    forcingChannels_ = true;
    const auto error = deviceManager_.setAudioDeviceSetup(setup, true);
    forcingChannels_ = false;
    if (error.isNotEmpty())
        openError_ = error;
}

void AudioEngine::applyOpenDevice(juce::AudioIODevice& device, bool remember)
{
    if (forcingChannels_)
        return;

    // A short saved mask is reopened once here. Later polls skip this unless the open mask changes.
    const bool unseen = lastFullOpenInputs_ < 0 || device.getName() != lastFullOpenName_;
    if (unseen
        || device.getActiveInputChannels() != lastInputMask_
        || device.getActiveOutputChannels() != lastOutputMask_)
        noteChannelMasks(device);
    auto* live = deviceManager_.getCurrentAudioDevice();
    if (live == nullptr || ! live->isOpen())
        return;

    if (deviceName_ != live->getName())
        revealUnsupported_ = false;
    deviceName_ = live->getName();
    cpuUsage_.store(static_cast<float>(deviceManager_.getCpuUsage()), std::memory_order_relaxed);

    const double rate = live->getCurrentSampleRate();
    const int buffer = live->getCurrentBufferSizeSamples();
    const bool starting = deviceStarting_.exchange(false, std::memory_order_acq_rel);
    const bool rateChanged = preparedRate_ <= 0.0 || std::abs(rate - preparedRate_) >= 1.0;
    const bool bufferChanged = buffer != preparedBuffer_;
    const auto inputMask = live->getActiveInputChannels();
    const auto outputMask = live->getActiveOutputChannels();
    const bool masksChanged = inputMask != lastInputMask_ || outputMask != lastOutputMask_;
    const bool needPrepare = starting || rateChanged || bufferChanged;

    if (needPrepare || masksChanged)
    {
        publishConfig(*live);
    }
    else
    {
        sampleRate_.store(rate, std::memory_order_relaxed);
        bufferSamples_.store(buffer, std::memory_order_relaxed);
        inputLatencySamples_.store(live->getInputLatencyInSamples(), std::memory_order_relaxed);
        outputLatencySamples_.store(live->getOutputLatencyInSamples(), std::memory_order_relaxed);
        formulaValue_.store(static_cast<int>(formulaForDeviceType(live->getTypeName().toRawUTF8())),
                            std::memory_order_relaxed);
        deviceOpen_.store(true, std::memory_order_relaxed);
    }

    if (needPrepare)
    {
        if (recorder_ != nullptr)
            recorder_->setDevice(rate, true);
        if (rack_ != nullptr)
            rack_->prepare(rate, buffer > 0 ? buffer : 512, currentConfig().routing, live->getWorkgroup());
        preparedRate_ = rate;
        preparedBuffer_ = buffer;
        installOverloadListener(live->getName());
    }
    else if (masksChanged && rack_ != nullptr)
    {
        rack_->updateRouting(currentConfig().routing);
    }

    // A card change keeps the session. Visibility follows the new input count.
    // The rate rule is the same as opening a session: higher rates are announced.
    if (! restoringSession_ && ! offlineTemplate_ && hasSession() && recorder_ != nullptr && rate > 0.0)
    {
        const auto adoption = adoptCardSampleRate(recorder_->timelineSampleRate(), rate);
        if (adoption.changed)
        {
            recorder_->setTimelineSampleRate(adoption.rate);
            sessionRateNotice_ = juce::String(adoption.notice);
            appendHostLog(settings_, "device switch kept session, rate " + juce::String(formatRateKhz(adoption.rate)) + " kHz");
        }
    }

    lastInputMask_ = inputMask;
    lastOutputMask_ = outputMask;
    if (remember && live->getName() == wantedName_)
    {
        deviceLostBanner_ = false;
        lossFinalized_ = false;
        awaitingSavedDevice_ = false;
        startupDeviceNote_.clear();
        startupFallbackName_.clear();
        downSinceMs_ = 0;
        setupLocked_ = true;
    }
    const double sessionRate = recorder_ != nullptr ? recorder_->timelineSampleRate() : 0.0;
    rateWarning_ = juce::String(sampleRateWarningText(rate, sessionRate));

    if (remember && (needPrepare || masksChanged || wantedName_ != live->getName()))
    {
        deviceManager_.getAudioDeviceSetup(wantedSetup_);
        wantedName_ = live->getName();
        saveSetupIfAllowed();
    }
}

void AudioEngine::handleDeviceDown()
{
    deviceOpen_.store(false, std::memory_order_relaxed);
    cpuUsage_.store(0.0f, std::memory_order_relaxed);
    deviceName_ = "No device";
    publishSessionChannelLimit();
    if (closingDevice_.load(std::memory_order_acquire) || ! started_)
        return;

    if (! deviceDown_.load(std::memory_order_acquire) && ! awaitingSavedDevice_)
        return;

    if (downSinceMs_ == 0)
        downSinceMs_ = juce::Time::getMillisecondCounter();

    const auto elapsed = juce::Time::getMillisecondCounter() - downSinceMs_;
    if (deviceDown_.load(std::memory_order_acquire) && elapsed >= 400 && ! lossFinalized_)
    {
        lossFinalized_ = true;
        deviceLostBanner_ = true;
        removeOverloadListener();
        if (recorder_ != nullptr)
            recorder_->stop();
    }

    if (deviceLostBanner_ || awaitingSavedDevice_)
        tryReopenWanted();
}

void AudioEngine::tryReopenWanted()
{
    if (reopenInProgress_ || closingDevice_.load(std::memory_order_acquire) || wantedName_.isEmpty())
        return;

    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastDeviceScanMs_ < 500)
        return;
    lastDeviceScanMs_ = now;
    if (! deviceNameListed(wantedName_))
        return;

    reopenInProgress_ = true;
    auto setup = wantedSetup_;
    if (setup.outputDeviceName.isEmpty())
        setup.outputDeviceName = wantedName_;
    if (setup.inputDeviceName.isEmpty())
        setup.inputDeviceName = wantedName_;
    const auto error = deviceManager_.setAudioDeviceSetup(setup, true);
    reopenInProgress_ = false;
    if (error.isNotEmpty())
        openError_ = error;
}

bool AudioEngine::deviceNameListed(const juce::String& name)
{
    if (name.isEmpty())
        return false;

    for (auto* type : deviceManager_.getAvailableDeviceTypes())
    {
        if (type == nullptr)
            continue;
        type->scanForDevices();
        if (type->getDeviceNames(false).contains(name) || type->getDeviceNames(true).contains(name))
            return true;
    }
    return false;
}

void AudioEngine::rememberSavedSetup(const juce::XmlElement* saved)
{
    wantedSetup_.useDefaultInputChannels = true;
    wantedSetup_.useDefaultOutputChannels = true;
    wantedSetup_.inputChannels.clear();
    wantedSetup_.outputChannels.clear();
    if (saved == nullptr)
        return;

    const auto legacy = saved->getStringAttribute("audioDeviceName");
    wantedSetup_.outputDeviceName = saved->getStringAttribute("audioOutputDeviceName");
    wantedSetup_.inputDeviceName = saved->getStringAttribute("audioInputDeviceName");
    if (wantedSetup_.outputDeviceName.isEmpty() && legacy.isNotEmpty())
    {
        wantedSetup_.outputDeviceName = legacy;
        wantedSetup_.inputDeviceName = legacy;
    }
    wantedSetup_.bufferSize = saved->getIntAttribute("audioDeviceBufferSize", wantedSetup_.bufferSize);
    wantedSetup_.sampleRate = saved->getDoubleAttribute("audioDeviceRate", wantedSetup_.sampleRate);
    wantedName_ = wantedSetup_.outputDeviceName.isNotEmpty() ? wantedSetup_.outputDeviceName
                                                             : wantedSetup_.inputDeviceName;
}

void AudioEngine::saveSetupIfAllowed()
{
    if (! persistSetup_ || awaitingSavedDevice_ || deviceLostBanner_ || deviceManager_.getCurrentAudioDevice() == nullptr)
        return;

    if (auto xml = deviceManager_.createStateXml())
        settings_.saveAudioSetup(xml.get());
}

bool AudioEngine::takeUncleanShutdown()
{
    journal_ = loadCrashJournal(settings_);
    const bool dirty = ! journal_.clean && ! journal_.marks.empty();
    if (! dirty)
    {
        journal_.clean = false;
        journal_.marks.clear();
        storeCrashJournal(settings_, journal_);
        appendHostLog(settings_, "start");
        return false;
    }

    crashChoicePending_ = true;
    appendHostLog(settings_, "unclean start");
    const auto last = juce::File(settings_.loadLastSessionFolder());
    const auto sessionFile = last.getChildFile(kSessionFileName);
    if (sessionDisk_ != nullptr && sessionFile.existsAsFile())
    {
        const auto now = juce::Time::getCurrentTime();
        const auto name = crashRecoverySessionName(now.getDayOfMonth(),
                                                    now.getMonth() + 1,
                                                    now.getYear(),
                                                    now.getHours(),
                                                    now.getMinutes());
        sessionDisk_->startCrashCopy(sessionFile, juce::String(name));
    }
    return true;
}

juce::String AudioEngine::uncleanPluginMessage() const
{
    juce::String text;
    for (const auto& mark : journal_.marks)
    {
        const auto name = mark.name.empty() ? std::string("A plugin") : mark.name;
        text << juce::String::fromUTF8(name.c_str()) << " may have crashed YouHost.\n";
    }
    text << "\nLeave them off, or load them anyway. Suspicious plugins are listed in the scanner.\n";
    text << "Crash notes are in youhost.log and crash-journal.txt in the YouHost support folder.";
    return text;
}

void AudioEngine::acceptCrashChoice(bool leaveOff)
{
    juce::StringArray blocked;
    for (const auto& mark : journal_.marks)
    {
        if (mark.identifier.empty())
            continue;
        const auto identifier = juce::String::fromUTF8(mark.identifier.c_str());
        blocked.addIfNotAlreadyThere(identifier);
        settings_.addSuspiciousPlugin(identifier);
    }

    if (leaveOff && rack_ != nullptr)
        rack_->setBlockedIdentifiers(blocked);

    journal_.marks.clear();
    journal_.clean = false;
    crashChoicePending_ = false;
    storeCrashJournal(settings_, journal_);
    appendHostLog(settings_, leaveOff ? "left crashed plugins off" : "load crashed plugins anyway");
}

void AudioEngine::prepareForQuit()
{
    if (quitPrepared_)
        return;
    quitPrepared_ = true;
    closingDevice_.store(true, std::memory_order_release);
    deviceManager_.removeAudioCallback(this);
    deviceManager_.removeChangeListener(this);
    deviceManager_.closeAudioDevice();
    removeOverloadListener();
    if (recorder_ != nullptr)
        recorder_->stop();
    if (rack_ != nullptr)
        rack_->releaseForQuit();

    if (! crashChoicePending_)
    {
        journal_.clean = true;
        journal_.marks.clear();
        storeCrashJournal(settings_, journal_);
    }
    appendHostLog(settings_, "quit");
}

void AudioEngine::notePluginTrace(int channel,
                                  int slot,
                                  const juce::String& phase,
                                  const juce::String& name,
                                  const juce::String& identifier)
{
    if (quitPrepared_)
        return;

    if (phase == "unload" || phase == "moved-from")
        eraseCrashMark(journal_, channel, slot);
    else
    {
        CrashMark mark;
        mark.phase = phase.toStdString();
        mark.channel = channel;
        mark.slot = slot;
        mark.name = name.toStdString();
        mark.identifier = identifier.toStdString();
        upsertCrashMark(journal_, mark);
    }

    journal_.clean = false;
    storeCrashJournal(settings_, journal_);
    appendHostLog(settings_,
                  phase + "  ch " + juce::String(channel + 1) + " slot " + juce::String(slot + 1) + "  " + name);
}

juce::String AudioEngine::dspLoadText() const
{
    if (rack_ == nullptr)
        return "DSP callback 0%   plugin CPU 0%";

    const auto load = rack_->dspLoad();
    auto lines = load.lines;
    const int count = std::clamp(load.count, 0, static_cast<int>(lines.size()));
    std::sort(lines.begin(), lines.begin() + count, [](const DspLoadLine& left, const DspLoadLine& right)
    {
        return left.percent > right.percent;
    });

    const auto percent = [](float value)
    {
        return juce::String(juce::roundToInt(std::clamp(value, 0.0f, 9999.0f)));
    };

    juce::String text = "DSP callback " + percent(load.callbackPercent) + "%   plugin CPU " + percent(load.pluginPercent) + "%";
    for (int index = 0; index < count; ++index)
    {
        const auto& line = lines[static_cast<std::size_t>(index)];
        text << "\nch " << line.channel << "  " << juce::String(line.name) << "  " << percent(line.percent) << "%";
    }
    return text;
}

void AudioEngine::visitTimelineLanes(const std::function<void(const std::vector<TimelineLaneView>&)>& fn) const
{
    if (fn == nullptr || recorder_ == nullptr)
        return;

    recorder_->visitRecordedTakes([this, &fn](const RecordedTakeView* takes,
                                             int count,
                                             const RecordedTakeView* live,
                                             const std::array<std::string, kMaxChannels>& names)
    {
        const int shown = visibleChannels();
        std::array<TimelineTakeSource, 64> smallTakes;
        std::vector<TimelineTakeSource> largeTakes;
        const TimelineTakeSource* sources = nullptr;
        if (count > 0 && takes != nullptr)
        {
            TimelineTakeSource* storage = nullptr;
            if (count <= static_cast<int>(smallTakes.size()))
            {
                storage = smallTakes.data();
            }
            else
            {
                largeTakes.resize(static_cast<std::size_t>(count));
                storage = largeTakes.data();
            }
            for (int index = 0; index < count; ++index)
            {
                const auto& take = takes[index];
                auto& source = storage[index];
                source.number = take.number;
                source.start = take.start;
                source.length = take.length;
                source.peaks = take.peaks;
                source.recorded = take.recorded;
            }
            sources = storage;
        }

        TimelineTakeSource liveSource;
        const TimelineTakeSource* livePtr = nullptr;
        if (live != nullptr)
        {
            liveSource.number = live->number;
            liveSource.start = live->start;
            liveSource.length = live->length;
            liveSource.peaks = live->peaks;
            liveSource.recorded = live->recorded;
            livePtr = &liveSource;
        }

        std::array<TimelineChannelInfo, kMaxChannels> channels {};
        for (int channel = 0; channel < shown; ++channel)
        {
            const auto index = static_cast<std::size_t>(channel);
            channels[index].name = names[index];
            channels[index].color = channelColor_[index];
            channels[index].group = channelGroup_[index];
        }
        std::array<TimelineGroupInfo, kMaxDisplayGroups> groups {};
        for (int group = 0; group < kMaxDisplayGroups; ++group)
        {
            const auto index = static_cast<std::size_t>(group);
            groups[index].collapsed = groups_[index].collapsed;
            groups[index].color = groups_[index].color;
            groups[index].name = groups_[index].name.toStdString();
        }
        buildTimelineLanes(sources, count, livePtr, channels.data(), shown, groups.data(), kMaxDisplayGroups, fn);
    });
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

void AudioEngine::setStereoFold(int channel, int slot, int fold)
{
    if (rack_ != nullptr)
        rack_->setStereoFold(channel, slot, fold);
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
    setChannelListen(channel, armed ? ChannelListen::record : ChannelListen::off);
}

ChannelListen AudioEngine::channelListen(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return ChannelListen::off;
    return listen_[static_cast<std::size_t>(channel)];
}

void AudioEngine::setChannelListen(int channel, ChannelListen mode)
{
    if (! recordActionAllowed(RecordDisrupt::channelListen, isRecording(), recordLockArmed_))
        return;
    if (channel < 0 || channel >= kMaxChannels)
        return;
    listen_[static_cast<std::size_t>(channel)] = mode;
    if (recorder_ != nullptr)
        recorder_->setArmed(channel, channelListenRecords(mode));
    storeChannelOn(channel, channelListenAudible(mode));
    if (rack_ != nullptr)
        rack_->setAudible(channel, channelListenAudible(mode));
    noteSessionEdit();
}

void AudioEngine::cycleChannelListen(int channel)
{
    setChannelListen(channel, youhost::cycleChannelListen(channelListen(channel)));
}

float AudioEngine::outputDb(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return 0.0f;
    return outputDb_[static_cast<std::size_t>(channel)];
}

void AudioEngine::setOutputDb(int channel, float db)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    const float snapped = snapOutputDb(db);
    if (sameFloatBits(snapped, outputDb_[static_cast<std::size_t>(channel)]))
        return;
    outputDb_[static_cast<std::size_t>(channel)] = snapped;
    outputGain_[static_cast<std::size_t>(channel)].store(outputDbToLinear(snapped), std::memory_order_relaxed);
    noteSessionEdit();
}

void AudioEngine::setBypassAll(bool bypass)
{
    if (rack_ != nullptr)
        rack_->setBypassAll(bypass);
}

bool AudioEngine::bypassAll() const
{
    return rack_ != nullptr && rack_->bypassAll();
}

void AudioEngine::setWaveformGain(float gain)
{
    const float clamped = clampWaveformGain(gain);
    if (sameFloatBits(clamped, waveformGain_))
        return;
    waveformGain_ = clamped;
    noteSessionEdit();
}

void AudioEngine::nudgeWaveformGain(int direction)
{
    setWaveformGain(stepWaveformGain(waveformGain_, direction));
}

void AudioEngine::setAlignGroup(int perGroup)
{
    const int mode = perGroup == 1 ? 1 : 0;
    if (alignGroup_ == mode)
        return;
    alignGroup_ = mode;
    syncCompensation();
    noteSessionEdit();
}

int AudioEngine::copyGroupLatency(GroupLatencyLine* out, int capacity) const
{
    if (out == nullptr || capacity <= 0 || alignGroup_ != 1 || rack_ == nullptr)
        return 0;
    std::array<ChannelLatencyInput, kMaxChannels> inputs {};
    rack_->copyLatencyInputs(inputs.data(), kMaxChannels);
    GroupCompensationRow rows[kMaxDisplayGroups];
    const int count = groupCompensationRows(inputs.data(), kMaxChannels, rows, std::min(capacity, kMaxDisplayGroups));
    for (int index = 0; index < count; ++index)
    {
        out[index] = {};
        out[index].group = rows[index].group;
        out[index].alignSamples = rows[index].alignSamples;
        out[index].members = rows[index].members;
        writeGroupLatencyName(out[index], groupName(rows[index].group).toRawUTF8());
    }
    return count;
}

CpuMeters AudioEngine::cpuMeters() const
{
    if (rack_ == nullptr)
        return {};
    return rack_->cpuMeters();
}

juce::String AudioEngine::channelPdcText(int channel) const
{
    if (rack_ == nullptr || channel < 0 || channel >= kMaxChannels)
        return {};
    const auto snap = rack_->snapshot(channel);
    if (snap.chainSamples <= 0 && snap.delaySamples <= 0)
        return {};
    const double rate = sampleRate_.load(std::memory_order_relaxed);
    const double ms = samplesToMilliseconds(snap.delaySamples, rate > 0.0 ? rate : 48000.0);
    return "FX " + juce::String(snap.chainSamples) + " smp   PDC " + juce::String(snap.delaySamples)
           + " smp / " + juce::String(ms, 1) + " ms";
}

void AudioEngine::syncCompensation()
{
    if (rack_ == nullptr)
        return;
    rack_->setAlignMode(alignGroup_);
    rack_->setChannelGroups(channelGroup_.data(), kMaxChannels);
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

void AudioEngine::toggleRecordReady()
{
    if (! recordActionAllowed(RecordDisrupt::recToggle, isRecording(), recordLockArmed_))
        return;
    if (recorder_ != nullptr && recorder_->isRecording())
        return;
    recordReady_ = ! recordReady_;
}

void AudioEngine::logTransport(const juce::String& line)
{
    if (line.isNotEmpty())
        appendHostLog(settings_, line);
}

RecordAttempt AudioEngine::makeRecordAttempt()
{
    RecordAttempt attempt;
    attempt.buttonArmed = recordReady_;
    attempt.lockArmed = recordLockArmed_;
    attempt.recording = isRecording();
    attempt.playing = recorder_ != nullptr && recorder_->isPlaying();
    attempt.hasSession = hasSession();
    const auto folderProblem = sessionRecordProblem();
    attempt.folderWritable = attempt.hasSession && folderProblem.isEmpty();
    if (folderProblem.isNotEmpty())
        attempt.folderProblem = folderProblem.toStdString();
    attempt.offline = offlineTemplate_;
    attempt.deviceLive = ! attempt.offline && deviceOpen_.load(std::memory_order_relaxed);
    attempt.copyBusy = sessionCopyProgress() >= 0.0f;
    attempt.deviceRate = sampleRate_.load(std::memory_order_relaxed);
    attempt.preferredRate = preferredRate_;
    if (recorder_ != nullptr)
        attempt.timelineRate = recorder_->timelineSampleRate();

    auto packed = currentConfig().routing.inputPacked;
    int openInputs = 0;
    for (const auto index : packed)
        if (index >= 0)
            ++openInputs;
    if (openInputs == 0 && attempt.deviceLive)
    {
        if (auto* live = deviceManager_.getCurrentAudioDevice())
        {
            const auto mask = live->getActiveInputChannels();
            packed.fill(-1);
            std::int16_t next = 0;
            for (int channel = 0; channel < kMaxChannels; ++channel)
                if (mask[channel])
                    packed[static_cast<std::size_t>(channel)] = next++;
        }
    }

    const auto transport = transportView();
    attempt.hasTakes = ! transport.takes.empty();
    attempt.takeNumber = static_cast<int>(transport.takes.size()) + 1;
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        const auto index = static_cast<std::size_t>(channel);
        attempt.channels[index].rec = channelListen(channel) == ChannelListen::record;
        attempt.channels[index].inputOpen = packed[index] >= 0;
        attempt.names[index] = channelName(channel).toStdString();
    }
    return attempt;
}

juce::String AudioEngine::pressTransport(TransportPress press)
{
    if (press == TransportPress::commandSpace)
        recordReady_ = true;

    const auto decision = resolveTransport(makeRecordAttempt(), press);
    if (decision.startRecording)
    {
        recordReady_ = true;
        if (recorder_ == nullptr)
        {
            logTransport("record start failed: recorder is missing");
            sessionMessage_ = "Recording did not start.";
            return sessionMessage_;
        }
        std::array<bool, kMaxChannels> armed {};
        for (int channel = 0; channel < kMaxChannels; ++channel)
            armed[static_cast<std::size_t>(channel)] = channelListen(channel) == ChannelListen::record;
        recorder_->replaceArmed(armed);
        if (offlineTemplate_)
            recorder_->setDevice(preferredRate_ > 0.0 ? preferredRate_ : 48000.0, false);
        else if (deviceOpen_.load(std::memory_order_relaxed))
        {
            const double rate = sampleRate_.load(std::memory_order_relaxed);
            if (rate > 0.0)
                recorder_->setDevice(rate, true);
        }

        auto packed = currentConfig().routing.inputPacked;
        if (offlineTemplate_)
        {
            packed.fill(-1);
            for (const int channel : decision.channels)
                if (channel >= 0 && channel < kMaxChannels)
                    packed[static_cast<std::size_t>(channel)] = static_cast<std::int16_t>(channel);
        }
        else
        {
            int openInputs = 0;
            for (const auto index : packed)
                if (index >= 0)
                    ++openInputs;
            if (openInputs == 0)
            {
                if (auto* live = deviceManager_.getCurrentAudioDevice())
                {
                    const auto mask = live->getActiveInputChannels();
                    packed.fill(-1);
                    std::int16_t next = 0;
                    for (int channel = 0; channel < kMaxChannels; ++channel)
                        if (mask[channel])
                            packed[static_cast<std::size_t>(channel)] = next++;
                }
            }
        }

        // The array length is kMaxChannels. A card count of 0 used to mark every input closed.
        const auto error = recorder_->record(packed.data(), kMaxChannels, offlineTemplate_);
        if (error.isNotEmpty())
        {
            logTransport("record start failed: " + error);
            sessionMessage_ = error;
            return error;
        }
        logTransport(juce::String(decision.log));
        noteSessionEdit();
        return {};
    }

    if (decision.stop)
    {
        logTransport(juce::String(decision.log));
        finishTransport();
        recordReady_ = false;
        return {};
    }

    if (decision.startPlayback)
    {
        logTransport(juce::String(decision.log));
        if (recorder_ != nullptr)
            recorder_->play();
        const bool playing = recorder_ != nullptr && recorder_->isPlaying();
        if (! playing)
        {
            const auto status = recorder_ != nullptr ? transportView().status : juce::String("Playback did not start.");
            const auto reason = status.isNotEmpty() ? status : juce::String("Playback did not start.");
            logTransport("play failed: " + reason);
            sessionMessage_ = reason;
            return reason;
        }
        return {};
    }

    logTransport(juce::String(decision.log));
    if (decision.alert.empty())
        return {};
    sessionMessage_ = juce::String(decision.alert);
    return sessionMessage_;
}

juce::String AudioEngine::transportRecord()
{
    recordReady_ = true;
    return pressTransport(TransportPress::commandSpace);
}

juce::String AudioEngine::transportStop()
{
    return pressTransport(TransportPress::stop);
}

void AudioEngine::finishTransport()
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

juce::String AudioEngine::transportPlay()
{
    return pressTransport(TransportPress::play);
}

std::vector<ListedDevice> AudioEngine::connectedDevices()
{
    const bool force = inventoryForce_;
    inventoryForce_ = false;
    const bool lost = deviceDown_.load(std::memory_order_relaxed);
    if (! deviceInventoryQueryAllowed(setupLocked_, lost, force, deviceInventory_.empty()))
        return deviceInventory_;

    const auto now = juce::Time::getMillisecondCounter();
    if (! force && ! deviceInventory_.empty() && now - deviceInventoryMs_ < 1500u)
        return deviceInventory_;

    std::vector<ListedDevice> found;
    for (auto* type : deviceManager_.getAvailableDeviceTypes())
    {
        if (type == nullptr)
            continue;
        type->scanForDevices();
        juce::StringArray names = type->getDeviceNames(false);
        if (type->hasSeparateInputsAndOutputs())
        {
            for (const auto& inputName : type->getDeviceNames(true))
                if (! names.contains(inputName))
                    names.add(inputName);
        }
        for (const auto& name : names)
        {
            if (name.isEmpty())
                continue;
            ListedDevice item;
            item.name = name.toStdString();
            if (auto* live = deviceManager_.getCurrentAudioDevice())
            {
                if (live->getName() == name && live->isOpen())
                {
                    item.inputs = live->getInputChannelNames().size();
                    item.outputs = live->getOutputChannelNames().size();
                    found.push_back(std::move(item));
                    continue;
                }
            }
            const std::unique_ptr<juce::AudioIODevice> probe(type->createDevice(name, name));
            if (probe != nullptr)
            {
                item.inputs = probe->getInputChannelNames().size();
                item.outputs = probe->getOutputChannelNames().size();
            }
            found.push_back(std::move(item));
        }
    }
    deviceInventory_ = std::move(found);
    deviceInventoryMs_ = now;
    return deviceInventory_;
}

void AudioEngine::openNamedDevice(const juce::String& name)
{
    if (name.isEmpty() || isOfflineDeviceName(name.toStdString()))
        return;
    if (! recordActionAllowed(RecordDisrupt::changeDevice, isRecording(), recordLockArmed_))
    {
        sessionMessage_ = juce::String(deviceSwitchBlockedReason(isRecording(), recordLockArmed_));
        return;
    }
    offlineTemplate_ = false;
    revealUnsupported_ = false;
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager_.getAudioDeviceSetup(setup);
    setup.inputDeviceName = name;
    setup.outputDeviceName = name;
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;
    wantedName_ = name;
    awaitingSavedDevice_ = false;
    startupDeviceNote_.clear();
    setupLocked_ = false;
    inventoryForce_ = true;
    const auto error = deviceManager_.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty())
        openError_ = error;
    deviceInventoryMs_ = 0;
}

void AudioEngine::noteUserChoseDevice(const juce::String& name)
{
    if (name.isEmpty() || isOfflineDeviceName(name.toStdString()))
        return;
    settings_.saveUserChosenDevice(name);
}

void AudioEngine::reconcileStartupDevice()
{
    const auto present = connectedDevices();
    const auto choice = chooseStartupDevice(wantedName_.toStdString(),
                                            settings_.loadUserChosenDevice().toStdString(),
                                            present);
    auto* device = deviceManager_.getCurrentAudioDevice();
    const auto opened = device != nullptr ? device->getName().toStdString() : std::string();
    if (choice.empty())
    {
        if (audioCardKind(opened) == AudioCardKind::virtualDevice)
        {
            closingDevice_.store(true, std::memory_order_release);
            deviceManager_.closeAudioDevice();
            closingDevice_.store(false, std::memory_order_release);
            appendHostLog(settings_, "startup left virtual device \"" + juce::String(opened) + "\" closed");
        }
        return;
    }
    if (opened == choice)
        return;
    appendHostLog(settings_, "startup device \"" + juce::String(choice) + "\"");
    openNamedDevice(juce::String(choice));
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

void AudioEngine::resetToCleanSession()
{
    if (isRecording())
    {
        sessionMessage_ = recordingCloseText(recordingLocked());
        return;
    }
    if (recorder_ != nullptr)
    {
        recorder_->stop();
        recorder_->clearTakes();
        recorder_->clearChannelNames();
    }
    recordReady_ = false;
    if (rack_ != nullptr)
        rack_->clearAll(false);
    setBypassAll(false);

    std::uint64_t low = 0;
    std::uint64_t high = 0;
    std::array<bool, kMaxChannels> audible {};
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        const auto index = static_cast<std::size_t>(channel);
        listen_[index] = ChannelListen::record;
        outputDb_[index] = 0.0f;
        outputGain_[index].store(1.0f, std::memory_order_relaxed);
        channelColor_[index] = 0;
        channelGroup_[index] = -1;
        strips_[index].excludeFromCompensation = false;
        audible[index] = true;
        setChannelOnBit(low, high, channel, true);
        if (recorder_ != nullptr)
            recorder_->setArmed(channel, true);
    }
    channelOnLo_.store(low, std::memory_order_relaxed);
    channelOnHi_.store(high, std::memory_order_relaxed);
    if (rack_ != nullptr)
        rack_->setAudibleAll(audible);

    groups_ = {};
    selection_.clear();
    selectionAnchor_ = 0;
    alignGroup_ = 0;
    waveformGain_ = 1.0f;
    sessionPeak_ = false;
    sessionReferenceDb_ = kDefaultRmsReferenceDb;
    publishSessionChannelLimit();
    sessionFolder_ = juce::File();
    sessionOnInternalDisk_ = false;
    hasPreservedSession_ = false;
    preservedSession_ = {};
    sessionDirty_ = false;
    if (meterRestoreHandler_ != nullptr)
        meterRestoreHandler_(sessionPeak_, sessionReferenceDb_);
    if (timelineHandler_ != nullptr)
        timelineHandler_({});
    syncCompensation();
    ++displayRevision_;
    sessionMessage_ = "New session. Choose a name and a folder.";
}

void AudioEngine::startNewSession()
{
    resetToCleanSession();
}

bool AudioEngine::isInternalFallback(const juce::File& folder) const
{
    const auto root = juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("YouHost");
    return folder.isAChildOf(root);
}

void AudioEngine::rememberSessionParent(const juce::File& sessionFolder)
{
    if (sessionFolder == juce::File() || isInternalFallback(sessionFolder))
        return;
    settings_.saveSessionParentFolder(sessionFolder.getParentDirectory().getFullPathName());
}

juce::File AudioEngine::defaultSessionParent() const
{
    const auto saved = juce::File(settings_.loadSessionParentFolder());
    if (saved.isDirectory())
        return saved;
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory);
}

juce::String AudioEngine::missingSessionParentNote() const
{
    const auto saved = settings_.loadSessionParentFolder();
    if (saved.isNotEmpty() && ! juce::File(saved).isDirectory())
        return "The last drive is not available. Choose a folder.";
    return {};
}

juce::String AudioEngine::sessionRecordProblem() const
{
    if (sessionFolder_ == juce::File())
        return {};

    const auto parent = sessionFolder_.getParentDirectory();
    if (! parent.isDirectory())
        return "The session drive is not available. " + sessionFolder_.getFullPathName()
               + " may be unmounted. Recording did not start.";
    if (sessionFolder_.existsAsFile())
        return sessionFolder_.getFullPathName() + " is not a folder. Recording did not start.";

    const auto probe = sessionFolder_.isDirectory() ? sessionFolder_ : parent;
    if (! probe.hasWriteAccess())
        return "The session folder is not writable. " + sessionFolder_.getFullPathName()
               + " Recording did not start.";
    return {};
}

bool AudioEngine::placeNewSession(const juce::File& folder, bool internalDisk, bool clean)
{
    if (folder.getFullPathName().isEmpty())
        return false;
    if (isRecording())
    {
        sessionMessage_ = recordingCloseText(recordingLocked());
        return false;
    }

    if (recorder_ != nullptr)
        recorder_->stop();
    recordReady_ = false;
    if (! clean && sessionFolder_ != juce::File() && sessionFolder_ != folder)
    {
        if (! saveSession())
            return false;
    }
    if (clean)
        resetToCleanSession();
    else if (recorder_ != nullptr)
        recorder_->clearTakes();

    sessionOnInternalDisk_ = internalDisk;
    lastBackupMs_ = 0;
    if (! saveSessionToFolder(folder))
        return false;

    if (internalDisk)
        sessionMessage_ = "This session is on the internal disk: " + sessionFolder_.getFullPathName();
    else
    {
        rememberSessionParent(sessionFolder_);
        sessionMessage_ = "Session folder " + sessionFolder_.getFullPathName();
    }
    return true;
}

bool AudioEngine::createInternalSession()
{
    auto root = juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("YouHost");
    root.createDirectory();
    if (! root.isDirectory() || ! root.hasWriteAccess())
    {
        sessionMessage_ = "The internal Music folder is not writable.";
        return false;
    }

    auto folder = root.getChildFile(suggestedNewSessionName(root));

    if (! placeNewSession(folder, true))
        return false;
    sessionMessage_ = "This session is on the internal disk: " + sessionFolder_.getFullPathName();
    return true;
}

void AudioEngine::setGlobalKeyListener(juce::KeyListener* listener)
{
    if (rack_ != nullptr)
        rack_->setGlobalKeyListener(listener);
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

void AudioEngine::setTimelineStateProvider(std::function<SessionTimelineState()> provider)
{
    timelineProvider_ = std::move(provider);
}

void AudioEngine::setTimelineStateHandler(std::function<void(const SessionTimelineState&)> handler)
{
    timelineHandler_ = std::move(handler);
}

void AudioEngine::syncRecorderFolder()
{
    if (recorder_ == nullptr || sessionFolder_ == juce::File())
        return;
    const auto layout = sessionLayoutFor(sessionFolder_.getFullPathName().toStdString());
    recorder_->setAudioFolder(juce::File(layout.audioFolder));
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

    if (sessionDirty_ && sessionFolder_ != juce::File()
        && juce::Time::getMillisecondCounter() - sessionDirtyAtMs_ >= 1500u)
    {
        sessionDirty_ = false;
        saveSession();
    }
    maybeBackupSession();
}

void AudioEngine::maybeBackupSession()
{
    if (sessionFolder_ == juce::File() || sessionDisk_ == nullptr)
        return;
    const auto now = juce::Time::getMillisecondCounter();
    if (lastBackupMs_ == 0)
    {
        lastBackupMs_ = now;
        return;
    }
    if (now - lastBackupMs_ < static_cast<juce::uint32>(kSessionBackupIntervalMs))
        return;
    if (recorder_ != nullptr && recorder_->isRecording())
        return;
    if (sessionDisk_->busy() || sessionCopyProgress() >= 0.0f)
        return;

    const auto clock = juce::Time::getCurrentTime();
    const auto stem = europeanSessionDate(clock.getDayOfMonth(), clock.getMonth() + 1, clock.getYear());
    if (! sessionDisk_->startBackup(captureSessionData(), sessionFolder_, juce::String(stem)))
        return;
    lastBackupMs_ = now;
}

juce::String suggestedNewSessionName(const juce::File& parent)
{
    const auto now = juce::Time::getCurrentTime();
    const auto base = europeanSessionDate(now.getDayOfMonth(), now.getMonth() + 1, now.getYear());
    return juce::String(nextFreeSessionName(base, [&parent](const std::string& name)
    {
        return parent.getChildFile(juce::String(name)).exists();
    }));
}

SessionData AudioEngine::captureSessionData()
{
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
    data.channelCount = kMaxChannels;
    data.page = sessionPage_;
    data.waveformGain = waveformGain_;
    data.alignGroup = alignGroup_;
    data.hasPreserved = hasPreservedSession_;
    data.preserved = preservedSession_;
    if (timelineProvider_)
        data.timeline = timelineProvider_();
    return data;
}

juce::File AudioEngine::suggestedSessionFolder() const
{
    if (sessionFolder_ != juce::File())
        return sessionFolder_;
    const auto last = settings_.loadLastSessionFolder();
    if (last.isNotEmpty())
        return juce::File(last);
    return defaultSessionParent();
}

bool AudioEngine::saveSession()
{
    if (sessionFolder_ == juce::File())
    {
        sessionMessage_ = "Choose where to save this session.";
        return false;
    }
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

    const SessionData data = captureSessionData();

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
    if (isRecording())
    {
        sessionMessage_ = recordingCloseText(recordingLocked());
        return false;
    }

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
    setBypassAll(false);
    sessionFolder_ = juce::File(layout.folder);
    sessionPeak_ = data.peakMeter;
    sessionReferenceDb_ = data.rmsReferenceDb;
    setWavBitDepth(data.wavBitDepth, false);
    sessionPage_ = data.page == 2 ? 2 : 1;
    sessionChannelCount_ = kMaxChannels;
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

    waveformGain_ = clampWaveformGain(data.waveformGain);
    alignGroup_ = data.alignGroup == 1 ? 1 : 0;
    hasPreservedSession_ = data.hasPreserved;
    preservedSession_ = data.preserved;
    if (timelineHandler_)
        timelineHandler_(data.timeline);
    applyDisplay(data);
    syncCompensation();

    double cardRate = 0.0;
    if (offlineTemplate_)
        cardRate = preferredRate_;
    else if (deviceOpen_.load(std::memory_order_relaxed))
        cardRate = sampleRate_.load(std::memory_order_relaxed);
    const auto adoption = adoptCardSampleRate(data.sampleRate, cardRate);
    if (recorder_ != nullptr && adoption.rate > 0.0 && (adoption.changed || ! (data.sampleRate > 0.0)))
        recorder_->setTimelineSampleRate(adoption.rate);
    sessionRateNotice_ = juce::String(adoption.notice);
    if (rack_ != nullptr && cardRate > 0.0)
    {
        const int liveBuffer = bufferSamples_.load(std::memory_order_relaxed);
        const int buffer = liveBuffer > 0 ? liveBuffer : preferredBuffer_;
        juce::AudioWorkgroup group;
        if (auto* live = deviceManager_.getCurrentAudioDevice())
            group = live->getWorkgroup();
        rack_->prepare(cardRate, buffer > 0 ? buffer : 512, currentConfig().routing, group);
    }
    publishSessionChannelLimit();

    settings_.saveLastSessionFolder(sessionFolder_.getFullPathName());
    settings_.rememberRecentSession(sessionFolder_.getFullPathName());
    rememberSessionParent(sessionFolder_);
    sessionOnInternalDisk_ = isInternalFallback(sessionFolder_);
    recordReady_ = false;
    sessionMessage_ = "Opened " + sessionFolder_.getFileName();
    sessionDirty_ = false;
    if (adoption.changed)
    {
        saveSession();
        sessionMessage_ = "Opened " + sessionFolder_.getFileName();
        sessionDirty_ = false;
    }
    lastBackupMs_ = 0;
    restoringSession_ = false;
    dropoutHeaderWritten_ = false;
    if (rack_ != nullptr)
        rack_->pumpSessionLoads();
    return true;
}

bool AudioEngine::isLoadingPlugins() const
{
    return rack_ != nullptr && rack_->isLoadingPlugins();
}

juce::String AudioEngine::pluginLoadProgress() const
{
    if (rack_ == nullptr)
        return {};
    return rack_->pluginLoadProgress();
}

void AudioEngine::setPluginSlotHandler(std::function<void(int)> handler)
{
    pluginSlotHandler_ = std::move(handler);
    if (rack_ != nullptr)
        rack_->setPluginSlotHandler(pluginSlotHandler_);
}

bool AudioEngine::saveSessionAs(const juce::File& folder)
{
    return beginSessionCopy(folder);
}

bool AudioEngine::beginSessionCopy(const juce::File& folder)
{
    if (folder.getFullPathName().isEmpty())
        return false;
    if (recorder_ != nullptr && recorder_->isRecording())
    {
        sessionMessage_ = "Stop recording before saving a copy.";
        return false;
    }
    if (sessionCopyProgress() >= 0.0f)
    {
        sessionMessage_ = "A copy is already being saved.";
        return false;
    }
    if (sessionFolder_ == juce::File())
    {
        sessionOnInternalDisk_ = false;
        if (! saveSessionToFolder(folder))
            return false;
        rememberSessionParent(sessionFolder_);
        sessionMessage_ = "Session folder " + sessionFolder_.getFullPathName();
        return true;
    }
    if (! saveSession())
        return false;
    if (folder.getFullPathName() == sessionFolder_.getFullPathName())
        return true;
    if (folder.isAChildOf(sessionFolder_) || sessionFolder_.isAChildOf(folder))
    {
        sessionMessage_ = "Choose a folder that is not inside this session.";
        return false;
    }
    if (sessionDisk_ == nullptr)
        return false;

    auto life = alive_;
    const auto destination = folder;
    if (! sessionDisk_->startCopy(sessionFolder_, destination, [life, this, destination](bool ok, juce::String message)
    {
        if (life == nullptr || ! life->load())
            return;
        if (! ok)
        {
            copyFailed_ = true;
            copyFailure_ = message.isNotEmpty() ? message : juce::String("Could not save the copy.");
            sessionMessage_ = copyFailure_;
            return;
        }
        sessionFolder_ = destination;
        dropoutHeaderWritten_ = false;
        lastBackupMs_ = 0;
        syncRecorderFolder();
        rememberSessionParent(sessionFolder_);
        sessionOnInternalDisk_ = isInternalFallback(sessionFolder_);
        settings_.saveLastSessionFolder(sessionFolder_.getFullPathName());
        settings_.rememberRecentSession(sessionFolder_.getFullPathName());
        sessionMessage_ = "Continuing in " + sessionFolder_.getFullPathName() + ". The original folder is unchanged.";
    }))
    {
        sessionMessage_ = "A copy is already being saved.";
        return false;
    }
    sessionMessage_ = "Saving a copy…";
    return true;
}

void AudioEngine::setCopyFinishedHandler(std::function<void(bool ok)> handler)
{
    copyFinishedHandler_ = std::move(handler);
}

void AudioEngine::notifyCopyFinished(bool ok)
{
    auto handler = std::move(copyFinishedHandler_);
    if (handler != nullptr)
        handler(ok);
}

float AudioEngine::sessionCopyProgress() const
{
    return sessionDisk_ != nullptr ? sessionDisk_->copyProgress() : -1.0f;
}

juce::String AudioEngine::backupStatusText() const
{
    if (sessionDisk_ == nullptr)
        return {};
    const int hour = sessionDisk_->backupHour();
    const int minute = sessionDisk_->backupMinute();
    if (hour < 0 || minute < 0)
        return {};
    return juce::String::formatted("Backup %02d:%02d", hour, minute);
}

juce::String AudioEngine::takeCopyFailure()
{
    if (! copyFailed_)
        return {};
    copyFailed_ = false;
    return copyFailure_;
}

bool AudioEngine::importRecordingFolder(const juce::File& folder)
{
    if (isRecording())
    {
        sessionMessage_ = recordingCloseText(recordingLocked());
        return false;
    }

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
    if (sessionFolder_ == juce::File())
    {
        sessionMessage_ = "Save the session before importing.";
        return false;
    }
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
    if (isRecording())
    {
        sessionMessage_ = recordingCloseText(recordingLocked());
        return;
    }
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
        auto& destination = data.channels[static_cast<std::size_t>(channel)];
        destination.color = channelColor_[static_cast<std::size_t>(channel)];
        destination.group = channelGroup_[static_cast<std::size_t>(channel)];
        destination.listen = listen_[static_cast<std::size_t>(channel)];
        destination.recordEnabled = destination.listen == ChannelListen::record;
        destination.outputDb = outputDb_[static_cast<std::size_t>(channel)];
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
        listen_[static_cast<std::size_t>(channel)] = source.listen;
        if (recorder_ != nullptr)
            recorder_->setArmed(channel, channelListenRecords(source.listen));
        outputDb_[static_cast<std::size_t>(channel)] = snapOutputDb(source.outputDb);
        outputGain_[static_cast<std::size_t>(channel)].store(outputDbToLinear(outputDb_[static_cast<std::size_t>(channel)]),
                                                            std::memory_order_relaxed);
        const bool live = channelListenAudible(source.listen);
        audible[static_cast<std::size_t>(channel)] = live;
        setChannelOnBit(low, high, channel, live);
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
        stored.collapsed = true;
        if (stored.name.isEmpty())
            stored.name = "Group " + juce::String(group + 1);
    }

    for (int channel : channels)
    {
        if (channel < 0 || channel >= kMaxChannels)
            continue;
        channelGroup_[static_cast<std::size_t>(channel)] = group;
    }
    syncCompensation();
    bumpDisplay();
}

void AudioEngine::clearGroup(int group)
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return;
    for (int channel = 0; channel < kMaxChannels; ++channel)
        if (channelGroup_[static_cast<std::size_t>(channel)] == group)
            channelGroup_[static_cast<std::size_t>(channel)] = -1;
    syncCompensation();
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

void AudioEngine::selectChannel(int channel, bool extend, bool toggle)
{
    ChannelSelection current;
    current.channels = selection_;
    current.anchor = selectionAnchor_;
    const auto pick = toggle ? ChannelPick::toggle : extend ? ChannelPick::range : ChannelPick::replace;
    current = pickChannels(current, channel, kMaxChannels, pick);
    selection_ = std::move(current.channels);
    selectionAnchor_ = current.anchor;
    if (selectionHandler_ != nullptr)
        selectionHandler_(channel);
}

void AudioEngine::setSelectionHandler(std::function<void(int)> handler)
{
    selectionHandler_ = std::move(handler);
}

int AudioEngine::selectedChannel() const
{
    if (selection_.empty())
        return -1;
    if (isChannelSelected(selectionAnchor_))
        return selectionAnchor_;
    return selection_.front();
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
