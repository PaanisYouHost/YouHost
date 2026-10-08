#include "AudioEngine.h"
#include "AppSettings.h"

namespace youhost
{

AudioEngine::AudioEngine(AppSettings& settings)
    : settings_(settings)
{
}

AudioEngine::~AudioEngine()
{
    deviceManager_.removeAudioCallback(this);
    deviceManager_.removeChangeListener(this);
    deviceManager_.closeAudioDevice();
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
}

void AudioEngine::pollDeviceStats()
{
    if (deviceError_.exchange(false, std::memory_order_relaxed))
        openError_ = "The audio device reported an error.";

    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr || ! device->isOpen())
    {
        deviceOpen_.store(false, std::memory_order_relaxed);
        xrunCount_.store(-1, std::memory_order_relaxed);
        deviceName_ = "No device";
        return;
    }

    deviceName_ = device->getName();
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
    xrunCount_.store(device->getXRunCount(), std::memory_order_relaxed);
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
    numbers.xruns = xrunCount_.load(std::memory_order_relaxed);
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
    strips_[static_cast<std::size_t>(channel)].meter.clearRequested.store(true, std::memory_order_relaxed);
}

void AudioEngine::requestClipClearAll()
{
    for (auto& strip : strips_)
        strip.meter.clearRequested.store(true, std::memory_order_relaxed);
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

    // Later phases, still on this thread and still without locks:
    //   1. copy raw inputs into the recorder rings (record-enabled strips only)
    //   2. meters, already inside processPassthrough
    //   3. hand each strip's four slots to its sandbox and wait within a deadline
    //   4. write latency-compensated audio to the matching output
    const int slot = configIndex_.load(std::memory_order_acquire);
    processPassthrough(inputChannelData,
                       numInputChannels,
                       outputChannelData,
                       numOutputChannels,
                       numSamples,
                       configs_[static_cast<std::size_t>(slot)],
                       strips_.data(),
                       kMaxChannels);
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

    if (device != nullptr)
        publishConfig(*device);
}

void AudioEngine::audioDeviceStopped()
{
    deviceOpen_.store(false, std::memory_order_relaxed);
    xrunCount_.store(-1, std::memory_order_relaxed);
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
    xrunCount_.store(device.getXRunCount(), std::memory_order_relaxed);
    deviceOpen_.store(true, std::memory_order_relaxed);
}

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    juce::ignoreUnused(source);
    pollDeviceStats();
    saveSetupIfAllowed();
}

void AudioEngine::saveSetupIfAllowed()
{
    if (! persistSetup_ || deviceManager_.getCurrentAudioDevice() == nullptr)
        return;

    if (auto xml = deviceManager_.createStateXml())
        settings_.saveAudioSetup(xml.get());
}

} // namespace youhost
