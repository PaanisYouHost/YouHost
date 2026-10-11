// Loads Apple AUDelay, feeds silence to both inputs, and exits non-zero if the
// output is non-finite or not near silence. macOS CI runs this binary.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace
{

constexpr const char* kAuDelayId = "AudioUnit:Effects/aufx,dely,appl";
constexpr double kSampleRate = 48000.0;
constexpr float kSilencePeak = 1.0e-3f;

int fail(const std::string& message)
{
    std::cerr << "AUDelay silent input failed: " << message << "\n";
    return 1;
}

bool samplesAreSilent(const juce::AudioBuffer<float>& buffer, int count, float& peak)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const float* data = buffer.getReadPointer(channel);
        for (int index = 0; index < count; ++index)
        {
            const float sample = data[index];
            if (! std::isfinite(sample))
                return false;
            const float magnitude = std::fabs(sample);
            if (magnitude > peak)
                peak = magnitude;
        }
    }
    return true;
}

// Both inputs receive the same mono buffer. Silence is the mono signal.
bool renderSilence(juce::AudioPluginInstance& instance, int block, int blocks, float& peak)
{
    instance.prepareToPlay(kSampleRate, block);
    instance.reset();

    const int channels = std::max(instance.getTotalNumInputChannels(), instance.getTotalNumOutputChannels());
    if (channels < 2)
        return false;

    juce::AudioBuffer<float> buffer(channels, block);
    juce::MidiBuffer midi;
    for (int pass = 0; pass < blocks; ++pass)
    {
        buffer.clear();
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getWritePointer(1);
        for (int index = 0; index < block; ++index)
            right[index] = left[index];
        midi.clear();
        instance.processBlock(buffer, midi);
        if (! samplesAreSilent(buffer, block, peak))
            return false;
    }
    return peak <= kSilencePeak;
}

int run()
{
    juce::AudioUnitPluginFormat format;
    if (! format.fileMightContainThisPluginType(kAuDelayId))
        return fail(std::string("Apple AUDelay was not found at ") + kAuDelayId);

    juce::PluginDescription description;
    description.pluginFormatName = "AudioUnit";
    description.fileOrIdentifier = kAuDelayId;
    description.name = "AUDelay";
    description.manufacturerName = "Apple";

    juce::String error;
    auto instance = format.createInstanceFromDescription(description, kSampleRate, 32, error);
    if (instance == nullptr)
        return fail("could not load AUDelay: " + error.toStdString());

    juce::AudioProcessor::BusesLayout stereo;
    stereo.inputBuses.add(juce::AudioChannelSet::stereo());
    stereo.outputBuses.add(juce::AudioChannelSet::stereo());
    if (! instance->checkBusesLayoutSupported(stereo) || ! instance->setBusesLayout(stereo))
        return fail("AUDelay rejected a stereo layout");

    instance->setNonRealtime(true);

    float peak = 0.0f;
    if (! renderSilence(*instance, 32, 16, peak))
        return fail("32-sample silence produced a non-finite or loud sample, peak " + std::to_string(peak));
    if (! renderSilence(*instance, 512, 8, peak))
        return fail("512-sample silence produced a non-finite or loud sample, peak " + std::to_string(peak));

    std::cout << "AUDelay silent input passed, peak " << peak << "\n";
    instance.reset();
    return 0;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    const int code = run();
    juce::DeletedAtShutdown::deleteAll();
    return code;
}
