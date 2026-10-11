// Offline host path for Apple stereo AUs on a mono channel.
// Scratch starts full of garbage. Silence must stay silence. An impulse must
// decay, and a slot move must not feed the previous output back in.
// The message thread must return while AUDelay is loaded. macOS CI runs this.

#include "engine/RepaintCoalesce.h"
#include "engine/SignalPath.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace
{

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 32;
constexpr float kPoison = 0.9f;
constexpr float kImpulse = 0.25f;
constexpr float kSilencePeak = 1.0e-3f;
constexpr float kImpulseCeiling = 1.0f;

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

int fail(const std::string& message)
{
    std::cerr << "AU chain failed: " << message << "\n";
    return 1;
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

// Poison the slot scratch, then run the same prepare / process / fold path
// the audio thread uses. `impulse` puts one sample in the first block only.
bool runOffline(juce::AudioPluginInstance& instance, bool impulse, float& peak, float& tail)
{
    instance.prepareToPlay(kSampleRate, kBlock);
    instance.reset();

    const int inputs = std::max(1, instance.getTotalNumInputChannels());
    const int outputs = std::max(1, instance.getTotalNumOutputChannels());
    float mono[kBlock] {};
    float scratchL[kBlock];
    float scratchR[kBlock];
    std::fill(scratchL, scratchL + kBlock, kPoison);
    std::fill(scratchR, scratchR + kBlock, kPoison);
    juce::MidiBuffer midi;
    peak = 0.0f;
    tail = 0.0f;

    constexpr int kBlocks = 1500;
    constexpr int kTailBlocks = 100;
    for (int block = 0; block < kBlocks; ++block)
    {
        std::fill(mono, mono + kBlock, 0.0f);
        if (impulse && block == 0)
            mono[0] = kImpulse;
        youhost::renderFoldedMono(mono, kBlock, inputs, outputs, youhost::StereoFold::left,
                                  scratchL, scratchR,
                                  [&](float** channels, int width, int)
                                  {
                                      midi.clear();
                                      juce::AudioBuffer<float> view(channels, width, kBlock);
                                      instance.processBlock(view, midi);
                                  });
        const float blockMax = blockPeak(mono, kBlock);
        peak = std::max(peak, blockMax);
        if (block >= kBlocks - kTailBlocks)
            tail = std::max(tail, blockMax);
    }

    // A move lands the plugin on a scratch pair that still holds the last output.
    std::fill(scratchL, scratchL + kBlock, kPoison);
    std::fill(scratchR, scratchR + kBlock, kPoison);
    std::fill(mono, mono + kBlock, 0.0f);
    youhost::renderFoldedMono(mono, kBlock, inputs, outputs, youhost::StereoFold::left,
                              scratchL, scratchR,
                              [&](float** channels, int width, int)
                              {
                                  midi.clear();
                                  juce::AudioBuffer<float> view(channels, width, kBlock);
                                  instance.processBlock(view, midi);
                              });
    tail = std::max(tail, blockPeak(mono, kBlock));
    return true;
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

        float silencePeak = 0.0f;
        float silenceTail = 0.0f;
        runOffline(*instance, false, silencePeak, silenceTail);
        if (silencePeak > kSilencePeak || silenceTail > kSilencePeak)
            return fail(std::string(unit.name) + " silence built up, peak " + std::to_string(silencePeak)
                        + " tail " + std::to_string(silenceTail));

        float impulsePeak = 0.0f;
        float impulseTail = 0.0f;
        runOffline(*instance, true, impulsePeak, impulseTail);
        if (impulsePeak > kImpulseCeiling || impulseTail > kSilencePeak)
            return fail(std::string(unit.name) + " impulse did not decay, peak " + std::to_string(impulsePeak)
                        + " tail " + std::to_string(impulseTail));

        std::cout << unit.name << " offline mono passed, silence peak " << silencePeak
                  << " impulse peak " << impulsePeak << " tail " << impulseTail << "\n";
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

    const auto started = juce::Time::getMillisecondCounter();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    const auto elapsed = juce::Time::getMillisecondCounter() - started;
    if (elapsed > 1000)
        return fail("message thread stalled " + std::to_string(elapsed) + " ms with AUDelay loaded");

    std::cout << "AU chain offline passed, message thread returned in " << elapsed << " ms\n";
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
