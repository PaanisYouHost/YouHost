// Offline host path for Apple stereo AUs on a mono channel, at factory defaults.
// Parameters are not edited. Silence in must stay silent, including when the
// slot scratch starts full of garbage. An impulse must decay: after the loudest
// second, the peak never grows, energy falls each second, and the signal is at
// or below -60 dBFS by 10 s. macOS CI runs this.

#include "engine/RepaintCoalesce.h"
#include "engine/SignalPath.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <CoreFoundation/CoreFoundation.h>

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 32;
constexpr int kSeconds = 10;
constexpr int kBlocksPerSecond = static_cast<int>(kSampleRate) / kBlock;
constexpr float kPoison = 0.9f;
constexpr float kImpulse = 0.25f;
// -60 dBFS. At or below this, the tail has reached the floor.
constexpr float kFloor = 1.0e-3f;

struct AppleAu
{
    const char* name;
    const char* identifier;
};

constexpr AppleAu kUnits[] = {
    { "AUDelay", "AudioUnit:Effects/aufx,dely,appl" },
    { "AUDistortion", "AudioUnit:Effects/aufx,dist,appl" },
    { "AUGraphicEQ", "AudioUnit:Effects/aufx,greq,appl" },
    { "AUBandpass", "AudioUnit:Effects/aufx,bpas,appl" },
};

struct Second
{
    float peak = 0.0f;
    double energy = 0.0;
};

int fail(const std::string& message)
{
    std::cerr << "AU chain failed: " << message << "\n";
    return 1;
}

float peakDb(float peak)
{
    if (peak <= 1.0e-10f)
        return -160.0f;
    return 20.0f * std::log10(peak);
}

std::string windowText(const std::vector<Second>& windows)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    for (std::size_t index = 0; index < windows.size(); ++index)
        out << " " << index << "s:" << peakDb(windows[index].peak) << "dB";
    return out.str();
}

bool setStereoMain(juce::AudioPluginInstance& instance)
{
    juce::AudioProcessor::BusesLayout layout;
    const int inputs = instance.getBusCount(true);
    const int outputs = instance.getBusCount(false);
    layout.inputBuses.resize(inputs);
    layout.outputBuses.resize(outputs);
    for (int bus = 0; bus < inputs; ++bus)
        layout.inputBuses.getReference(bus) = bus == 0 ? juce::AudioChannelSet::stereo()
                                                       : juce::AudioChannelSet::disabled();
    for (int bus = 0; bus < outputs; ++bus)
        layout.outputBuses.getReference(bus) = bus == 0 ? juce::AudioChannelSet::stereo()
                                                        : juce::AudioChannelSet::disabled();
    if (! instance.checkBusesLayoutSupported(layout) || ! instance.setBusesLayout(layout))
        return false;
    return instance.getTotalNumInputChannels() >= 1 && instance.getTotalNumOutputChannels() >= 1;
}

float blockPeak(const float* data, int count)
{
    float peak = 0.0f;
    for (int index = 0; index < count; ++index)
    {
        const float sample = data[index];
        if (! std::isfinite(sample))
            return 1000.0f;
        peak = std::max(peak, std::fabs(sample));
    }
    return peak;
}

void logFactoryParameters(const char* name, juce::AudioPluginInstance& instance)
{
    std::ostringstream out;
    out << name << " factory defaults";
    out.setf(std::ios::fixed);
    out.precision(4);
    for (auto* param : instance.getParameters())
    {
        if (param == nullptr)
            continue;
        float shown = param->getValue();
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
            shown = ranged->convertFrom0to1(param->getValue());
        out << " | " << param->getName(32).toStdString() << "=" << shown;
    }
    std::cout << out.str() << "\n";
}

// One block through the same scratch path the audio thread uses.
// Scratch is poisoned by the caller. renderFoldedMono must replace it from
// this block's mono input before the plugin runs.
float renderBlock(juce::AudioPluginInstance& instance,
                   float* mono,
                   float* scratchL,
                   float* scratchR,
                   juce::MidiBuffer& midi)
{
    const int inputs = std::max(1, instance.getTotalNumInputChannels());
    const int outputs = std::max(1, instance.getTotalNumOutputChannels());
    youhost::renderFoldedMono(mono, kBlock, inputs, outputs, youhost::StereoFold::left,
                              scratchL, scratchR,
                              [&](float** channels, int width, int)
                              {
                                  midi.clear();
                                  juce::AudioBuffer<float> view(channels, width, kBlock);
                                  instance.processBlock(view, midi);
                              });
    return blockPeak(mono, kBlock);
}

// Factory defaults, silence in, scratch poisoned. `impulse` puts one sample
// in the first block. Windows are one second each.
bool renderSeconds(juce::AudioPluginInstance& instance,
                   bool impulse,
                   std::vector<Second>& windows,
                   float& poisonPeak)
{
    instance.prepareToPlay(kSampleRate, kBlock);
    instance.reset();

    float scratchL[kBlock];
    float scratchR[kBlock];
    std::fill(scratchL, scratchL + kBlock, kPoison);
    std::fill(scratchR, scratchR + kBlock, kPoison);
    juce::MidiBuffer midi;
    windows.assign(static_cast<std::size_t>(kSeconds), Second {});

    for (int second = 0; second < kSeconds; ++second)
    {
        auto& window = windows[static_cast<std::size_t>(second)];
        for (int block = 0; block < kBlocksPerSecond; ++block)
        {
            float mono[kBlock] {};
            if (impulse && second == 0 && block == 0)
                mono[0] = kImpulse;
            const float blockMax = renderBlock(instance, mono, scratchL, scratchR, midi);
            window.peak = std::max(window.peak, blockMax);
            for (float sample : mono)
                window.energy += static_cast<double>(sample) * static_cast<double>(sample);
        }
    }

    std::fill(scratchL, scratchL + kBlock, kPoison);
    std::fill(scratchR, scratchR + kBlock, kPoison);
    float mono[kBlock] {};
    poisonPeak = renderBlock(instance, mono, scratchL, scratchR, midi);
    return true;
}

int checkSilence(const char* name, const std::vector<Second>& windows, float poisonPeak)
{
    float peak = poisonPeak;
    for (const auto& window : windows)
        peak = std::max(peak, window.peak);
    if (peak > kFloor || poisonPeak > kFloor)
        return fail(std::string(name) + " factory silence built up, peak " + std::to_string(peak)
                    + " poison " + std::to_string(poisonPeak) + windowText(windows));
    std::cout << name << " factory silence passed, peak " << peak << "\n";
    return 0;
}

int checkImpulse(const char* name, const std::vector<Second>& windows, float poisonPeak)
{
    if (windows.empty())
        return fail(std::string(name) + " impulse produced no windows");

    std::size_t loudest = 0;
    for (std::size_t index = 1; index < windows.size(); ++index)
        if (windows[index].peak > windows[loudest].peak)
            loudest = index;

    for (std::size_t index = loudest; index + 1 < windows.size(); ++index)
    {
        const auto& current = windows[index];
        const auto& next = windows[index + 1];
        if (current.peak <= kFloor)
        {
            if (next.peak > kFloor)
                return fail(std::string(name) + " factory tail grew after reaching -60 dBFS"
                            + windowText(windows));
            continue;
        }
        if (next.peak > current.peak + 1.0e-5f)
            return fail(std::string(name) + " factory tail peak grew"
                        + windowText(windows));
        if (! (next.energy < current.energy))
            return fail(std::string(name) + " factory tail energy did not fall"
                        + windowText(windows));
    }

    if (windows.back().peak > kFloor)
        return fail(std::string(name) + " factory impulse still above -60 dBFS at "
                    + std::to_string(kSeconds) + " s" + windowText(windows));

    // The scratch from a slot move must not come back once the tail is on the floor.
    if (poisonPeak > kFloor)
        return fail(std::string(name) + " factory scratch poison returned after the impulse, peak "
                    + std::to_string(poisonPeak));

    std::cout << name << " factory impulse passed" << windowText(windows) << "\n";
    return 0;
}

int run()
{
    juce::AudioUnitPluginFormat format;
    for (const auto& unit : kUnits)
    {
        if (! format.fileMightContainThisPluginType(unit.identifier))
            return fail(std::string(unit.name) + " was not found at " + unit.identifier);

        juce::PluginDescription description;
        description.pluginFormatName = "AudioUnit";
        description.fileOrIdentifier = unit.identifier;
        description.name = unit.name;
        description.manufacturerName = "Apple";

        juce::String error;
        auto instance = format.createInstanceFromDescription(description, kSampleRate, kBlock, error);
        if (instance == nullptr)
            return fail(std::string("could not load ") + unit.name + ": " + error.toStdString());
        if (! setStereoMain(*instance))
            return fail(std::string(unit.name) + " rejected a stereo main bus");
        instance->setNonRealtime(true);
        logFactoryParameters(unit.name, *instance);

        std::vector<Second> silence;
        float silencePoison = 0.0f;
        renderSeconds(*instance, false, silence, silencePoison);
        if (const int code = checkSilence(unit.name, silence, silencePoison))
            return code;

        std::vector<Second> impulse;
        float impulsePoison = 0.0f;
        renderSeconds(*instance, true, impulse, impulsePoison);
        if (const int code = checkImpulse(unit.name, impulse, impulsePoison))
            return code;

        instance.reset();
    }

    youhost::RepaintCoalescer paints;
    for (int index = 0; index < 1000; ++index)
        paints.note(1, 0, youhost::kRepaintLatency);
    int channel = -1;
    int slot = -1;
    int trigger = 0;
    if (! paints.take(1000, channel, slot, trigger) || paints.wantsFullWindow() || paints.paints() != 1)
        return fail("latency callbacks were not coalesced to one slot paint");
    if (paints.take(1010, channel, slot, trigger))
        return fail("a second slot paint happened inside 33 ms");
    paints.note(0, 0, youhost::kRepaintMove);
    if (! paints.take(1040, channel, slot, trigger) || trigger != youhost::kRepaintMove || channel != 0)
        return fail("moving AUDelay did not coalesce to that slot");

    // JUCE builds with JUCE_MODAL_LOOPS_PERMITTED at 0, so runDispatchLoopUntil
    // is not part of MessageManager. Pump the same run loop it would have used.
    const auto started = juce::Time::getMillisecondCounter();
    const auto end = started + 200u;
    while (juce::Time::getMillisecondCounter() < end)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, TRUE);
    const auto elapsed = juce::Time::getMillisecondCounter() - started;
    if (elapsed > 1000u)
        return fail("message thread stalled " + std::to_string(elapsed) + " ms with AUDelay loaded");

    std::cout << "AU chain offline passed at factory defaults, message thread returned in " << elapsed << " ms\n";
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
