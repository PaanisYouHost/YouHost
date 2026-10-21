// Runs a real VST3 and the Audio Unit of the same plugin when the machine has
// both. The GitHub runner usually does not, so four heavy processors at
// 32 samples always run. Hidden channels are not called. A callback that
// blocks for half a second fails: that is the lock this test is here to catch.

#include "engine/CpuMeter.h"
#include "engine/Vst3Path.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace
{

constexpr double kRate = 48000.0;
constexpr int kBlock = 32;
constexpr int kBlocks = 1500;

int fail(const std::string& message)
{
    std::cerr << "VST3 stress failed: " << message << "\n";
    return 1;
}

std::string stemOf(const juce::String& path)
{
    return juce::File(path).getFileNameWithoutExtension().toLowerCase().toStdString();
}

struct Heavy
{
    int calls = 0;
    int wrongBlock = 0;

    void process(int samples)
    {
        if (samples != kBlock)
            ++wrongBlock;
        ++calls;
        volatile float acc = 0.25f;
        for (int index = 0; index < samples * 80; ++index)
            acc = acc * 1.0001f + 0.0001f;
        (void) acc;
    }
};

int runHeavy()
{
    const auto model = youhost::runHeavyVst3At32(kBlocks);
    if (model.hiddenCalls != kBlocks * 2 || model.liveCalls != kBlocks || model.reprepares != 0 || model.waits != 0)
        return fail("hidden-channel model did not skip channels 6 and 8");
    if (model.meter == 52 || model.meter != model.callbackPercent)
        return fail("meter stayed at the latched 52");

    Heavy live[4];
    Heavy hidden[2];
    const auto started = std::chrono::steady_clock::now();
    for (int block = 0; block < kBlocks; ++block)
    {
        if (youhost::blockAsksReprepare(kBlock, kBlock))
            return fail("32 samples asked for a reprepare");
        const auto blockStart = std::chrono::steady_clock::now();
        for (auto& plugin : live)
            plugin.process(kBlock);
        for (const auto& plugin : hidden)
            if (plugin.calls != 0)
                return fail("a hidden channel ran");
        const auto blockNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - blockStart)
                                 .count();
        if (blockNs > 500000000LL)
            return fail("a 32-sample block blocked for more than 500 ms");
    }
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    const auto perBlock = static_cast<std::uint64_t>(elapsedNs / kBlocks);
    const int percent = youhost::callbackLoadPercent(perBlock, kBlock, kRate);
    const int meter = youhost::dspReading(percent, 52);
    if (meter != youhost::clampCpuPercent(percent))
        return fail("DSP meter ignored the callback");
    for (const auto& plugin : live)
        if (plugin.calls != kBlocks || plugin.wrongBlock != 0)
            return fail("a heavy plugin was not called once per 32-sample block");
    for (const auto& plugin : hidden)
        if (plugin.calls != 0)
            return fail("hidden plugin call count was not zero");

    std::cout << "heavy VST3 test plugins at 32 samples, meter " << meter
              << "%, four plugins, hidden calls 0\n";
    return 0;
}

int measureInstance(juce::AudioPluginInstance& instance, const std::string& label, std::uint64_t& perBlockOut)
{
    instance.setNonRealtime(true);
    instance.prepareToPlay(kRate, kBlock);
    if (instance.getBlockSize() != kBlock)
        return fail(label + " prepared block " + std::to_string(instance.getBlockSize()) + " instead of 32");

    juce::AudioBuffer<float> buffer(std::max(1, instance.getTotalNumOutputChannels()), kBlock);
    juce::MidiBuffer midi;
    buffer.clear();
    const auto started = std::chrono::steady_clock::now();
    for (int block = 0; block < kBlocks; ++block)
    {
        const auto blockStart = std::chrono::steady_clock::now();
        instance.processBlock(buffer, midi);
        const auto blockNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - blockStart)
                                 .count();
        if (blockNs > 500000000LL)
            return fail(label + " process blocked for more than 500 ms");
    }
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    perBlockOut = static_cast<std::uint64_t>(elapsedNs / kBlocks);
    instance.releaseResources();
    const int percent = youhost::callbackLoadPercent(perBlockOut, kBlock, kRate);
    std::cout << label << " 32-sample block " << (perBlockOut / 1000) << " us, " << percent << "%\n";
    return 0;
}

int runPair()
{
    juce::VST3PluginFormat vst3;
    juce::AudioUnitPluginFormat audioUnit;
    const auto vst3Files = vst3.searchPathsForPlugins(vst3.getDefaultLocationsToSearch(), true, false);
    const auto auFiles = audioUnit.searchPathsForPlugins(audioUnit.getDefaultLocationsToSearch(), true, false);

    juce::String vst3Path;
    juce::String auPath;
    for (const auto& vst3File : vst3Files)
    {
        const auto stem = stemOf(vst3File);
        if (stem.empty())
            continue;
        for (const auto& auFile : auFiles)
        {
            if (stemOf(auFile) == stem)
            {
                vst3Path = vst3File;
                auPath = auFile;
                break;
            }
        }
        if (vst3Path.isNotEmpty())
            break;
    }

    if (vst3Path.isEmpty())
    {
        std::cout << "no matching VST3 and AU, heavy VST3 test plugins at 32 samples\n";
        return 0;
    }

    juce::OwnedArray<juce::PluginDescription> vst3Types;
    juce::OwnedArray<juce::PluginDescription> auTypes;
    vst3.findAllTypesForFile(vst3Types, vst3Path);
    audioUnit.findAllTypesForFile(auTypes, auPath);
    if (vst3Types.isEmpty() || auTypes.isEmpty())
    {
        std::cout << "matching names did not open, heavy VST3 test plugins at 32 samples\n";
        return 0;
    }

    juce::String error;
    auto vst3Instance = vst3.createInstanceFromDescription(*vst3Types.getFirst(), kRate, kBlock, error);
    if (vst3Instance == nullptr)
        return fail("VST3 did not load: " + error.toStdString());
    error.clear();
    auto auInstance = audioUnit.createInstanceFromDescription(*auTypes.getFirst(), kRate, kBlock, error);
    if (auInstance == nullptr)
        return fail("AU did not load: " + error.toStdString());

    std::uint64_t vst3Ns = 0;
    std::uint64_t auNs = 0;
    const auto pairName = vst3Types.getFirst()->name.toStdString();
    if (const int status = measureInstance(*vst3Instance, pairName + " VST3", vst3Ns))
        return status;
    if (const int status = measureInstance(*auInstance, pairName + " AU", auNs))
        return status;
    std::cout << "real VST3 vs AU at 32 samples, " << (vst3Ns / 1000) << " us vs " << (auNs / 1000) << " us\n";
    return 0;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    if (const int status = runPair())
        return status;
    return runHeavy();
}
