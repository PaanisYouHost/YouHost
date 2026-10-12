// Compare a real Apple AU opened 1/1 with the same AU opened 2/2 and folded
// to the left. Factory defaults stay put. The level is the RMS after the
// plugin's own latency.
//
// Within 0.1 dB on every AU that accepts both layouts, unlinked mono channels
// use 1/1 (kUnlinkedMonoUsesMonoBus). A larger difference keeps 2/2. A linked
// pair is unchanged either way: layoutForChannel leaves it stereo.

#include "engine/HostPath.h"
#include "engine/SignalPath.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <time.h>

namespace
{

constexpr double kRate = 48000.0;
constexpr int kBlock = 32;
constexpr double kMatchDb = 0.1;

struct Unit
{
    const char* name;
    const char* identifier;
};

constexpr Unit kUnits[] = {
    { "AUDelay", "AudioUnit:Effects/aufx,dely,appl" },
    { "AUDistortion", "AudioUnit:Effects/aufx,dist,appl" },
    { "AUGraphicEQ", "AudioUnit:Effects/aufx,greq,appl" },
    { "AUBandpass", "AudioUnit:Effects/aufx,bpas,appl" },
};

int fail(const std::string& message)
{
    std::cerr << "mono null failed: " << message << "\n";
    return 1;
}

std::int64_t threadCpuNs()
{
    timespec ts {};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return -1;
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + static_cast<std::int64_t>(ts.tv_nsec);
}

bool setLayout(juce::AudioPluginInstance& instance, int inCh, int outCh)
{
    juce::AudioProcessor::BusesLayout layout;
    const int inputs = instance.getBusCount(true);
    const int outputs = instance.getBusCount(false);
    layout.inputBuses.resize(inputs);
    layout.outputBuses.resize(outputs);
    const auto inSet = inCh > 1 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono();
    const auto outSet = outCh > 1 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono();
    for (int bus = 0; bus < inputs; ++bus)
        layout.inputBuses.getReference(bus) = bus == 0 ? inSet : juce::AudioChannelSet::disabled();
    for (int bus = 0; bus < outputs; ++bus)
        layout.outputBuses.getReference(bus) = bus == 0 ? outSet : juce::AudioChannelSet::disabled();
    if (! instance.checkBusesLayoutSupported(layout) || ! instance.setBusesLayout(layout))
        return false;
    return instance.getTotalNumInputChannels() == inCh && instance.getTotalNumOutputChannels() == outCh;
}

struct Source
{
    std::uint32_t noise = 0xA341316Cu;
    double phase = 0.0;

    void fill(float* dest, int count)
    {
        const double step = 2.0 * 3.14159265358979323846 * 1000.0 / kRate;
        for (int index = 0; index < count; ++index)
        {
            noise ^= noise << 13;
            noise ^= noise >> 17;
            noise ^= noise << 5;
            const float hashed = static_cast<float>(noise & 0x00ffffffu) / static_cast<float>(0x00ffffffu) - 0.5f;
            dest[index] = 0.15f * static_cast<float>(std::sin(phase)) + 0.05f * hashed;
            phase += step;
        }
    }
};

struct Level
{
    bool ok = false;
    bool supported = false;
    double db = -160.0;
    double cpuPercent = 0.0;
    int latency = 0;
};

Level render(juce::AudioPluginInstance& instance, int inCh, int outCh)
{
    Level level;
    if (! setLayout(instance, inCh, outCh))
        return level;
    level.supported = true;
    instance.prepareToPlay(kRate, kBlock);
    instance.reset();
    level.latency = std::max(0, instance.getLatencySamples());

    int skip = level.latency + static_cast<int>(kRate * 0.25);
    if (skip > static_cast<int>(kRate))
        skip = static_cast<int>(kRate);
    const int measure = static_cast<int>(kRate * 2.0);
    const int total = skip + measure;

    Source source;
    juce::MidiBuffer midi;
    double sum = 0.0;
    int counted = 0;
    std::int64_t cpu = 0;
    bool finite = true;

    for (int done = 0; done < total; done += kBlock)
    {
        float mono[kBlock];
        source.fill(mono, kBlock);
        float scratchL[kBlock];
        float scratchR[kBlock];
        const bool timed = done >= skip;
        const auto cpu0 = timed ? threadCpuNs() : 0;
        youhost::renderFoldedMono(mono, kBlock, inCh, outCh, youhost::StereoFold::left,
                                  scratchL, scratchR,
                                  [&](float** channels, int width, int)
                                  {
                                      midi.clear();
                                      juce::AudioBuffer<float> view(channels, width, kBlock);
                                      instance.processBlock(view, midi);
                                  });
        const auto cpu1 = timed ? threadCpuNs() : 0;
        if (timed)
        {
            if (cpu0 < 0 || cpu1 < cpu0)
            {
                finite = false;
                break;
            }
            cpu += cpu1 - cpu0;
            for (float sample : mono)
            {
                if (! std::isfinite(sample))
                    finite = false;
                else
                    sum += static_cast<double>(sample) * static_cast<double>(sample);
            }
            counted += kBlock;
        }
    }

    if (! finite || counted <= 0)
        return level;
    level.ok = true;
    level.db = sum > 0.0 ? 10.0 * std::log10(sum / static_cast<double>(counted)) : -160.0;
    const double audioSeconds = static_cast<double>(counted) / kRate;
    level.cpuPercent = audioSeconds > 0.0 ? (static_cast<double>(cpu) / 1.0e9) / audioSeconds * 100.0 : 0.0;
    return level;
}

int run()
{
    juce::AudioUnitPluginFormat format;
    double worst = 0.0;
    std::string worstName;
    bool anyPair = false;
    double cpuStereo = 0.0;
    double cpuMono = 0.0;
    int compared = 0;

    std::cout.setf(std::ios::fixed);
    std::cout.precision(3);

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
        auto stereo = format.createInstanceFromDescription(description, kRate, kBlock, error);
        if (stereo == nullptr)
            return fail(std::string("could not load ") + unit.name + " 2/2: " + error.toStdString());
        auto mono = format.createInstanceFromDescription(description, kRate, kBlock, error);
        if (mono == nullptr)
            return fail(std::string("could not load ") + unit.name + " 1/1: " + error.toStdString());

        const Level folded = render(*stereo, 2, 2);
        const Level direct = render(*mono, 1, 1);
        if (! folded.supported || ! folded.ok)
            return fail(std::string(unit.name) + " rejected 2/2 or produced a non-finite output");
        if (! direct.supported)
        {
            std::cout << unit.name << " 1/1 not supported, stays 2/2, level " << folded.db << " dB\n";
            continue;
        }
        if (! direct.ok)
            return fail(std::string(unit.name) + " 1/1 output was not finite");
        if (folded.db < -60.0 && direct.db < -60.0)
            return fail(std::string(unit.name) + " both paths were below -60 dBFS");

        const double delta = direct.db - folded.db;
        const double absDelta = std::fabs(delta);
        if (worstName.empty() || absDelta > worst)
        {
            worst = absDelta;
            worstName = unit.name;
        }
        anyPair = true;
        ++compared;
        cpuStereo += folded.cpuPercent;
        cpuMono += direct.cpuPercent;
        std::cout << unit.name
                  << " 2/2 " << folded.db << " dB cpu " << folded.cpuPercent << "%"
                  << " 1/1 " << direct.db << " dB cpu " << direct.cpuPercent << "%"
                  << " delta " << delta << " dB"
                  << " latency " << folded.latency << "/" << direct.latency
                  << "\n";
    }

    if (! anyPair)
        return fail("no Apple AU accepted both 1/1 and 2/2");

    const double meanStereo = cpuStereo / static_cast<double>(compared);
    const double meanMono = cpuMono / static_cast<double>(compared);
    const bool equal = worst <= kMatchDb;
    std::cout << "max |delta| " << worst << " dB (" << worstName << ")\n";
    std::cout << "mean CPU 2/2 " << meanStereo << "%  1/1 " << meanMono << "%\n";
    std::cout << "kUnlinkedMonoUsesMonoBus " << (youhost::kUnlinkedMonoUsesMonoBus ? "true" : "false") << "\n";

    if (equal && ! youhost::kUnlinkedMonoUsesMonoBus)
        return fail("1/1 matches Fold L within 0.1 dB (max " + std::to_string(worst) + " dB on " + worstName
                    + "). Enable kUnlinkedMonoUsesMonoBus. CPU 2/2 " + std::to_string(meanStereo)
                    + "% 1/1 " + std::to_string(meanMono) + "%");
    if (! equal && youhost::kUnlinkedMonoUsesMonoBus)
        return fail("1/1 differs from Fold L by " + std::to_string(worst) + " dB on " + worstName + ". Keep 2/2.");

    if (equal)
        std::cout << "enabled 1/1 for unlinked mono channels. CPU before (2/2) " << meanStereo
                  << "% after (1/1) " << meanMono << "%\n";
    else
        std::cout << "kept 2/2. measured difference " << worst << " dB on " << worstName << "\n";
    std::cout << "mono null passed\n";
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
