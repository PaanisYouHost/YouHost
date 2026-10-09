#include "engine/ChannelEnable.h"
#include "engine/ChannelSelect.h"
#include "engine/SessionNames.h"
#include "engine/Shortcuts.h"
#include "engine/ChannelListen.h"
#include "engine/OutputGain.h"
#include "engine/SignalPath.h"
#include "engine/WaveformScale.h"
#include "engine/DisplayLayout.h"
#include "engine/DropoutDetect.h"
#include "engine/DropoutLog.h"
#include "engine/LatencyCompensation.h"
#include "engine/X32Colours.h"
#include "engine/LatencyMath.h"
#include "engine/MeterLayout.h"
#include "engine/MeterScale.h"
#include "engine/Passthrough.h"
#include "engine/ScanJobs.h"
#include "engine/SessionFiles.h"
#include "engine/TakeImport.h"
#include "engine/TakePlan.h"
#include "engine/CrashJournal.h"
#include "engine/DeviceWatch.h"
#include "engine/HostPath.h"
#include "engine/InsertMenu.h"
#include "engine/PluginLoadPace.h"
#include "engine/PluginMoves.h"
#include "engine/StallWatch.h"
#include "engine/TimelineLanes.h"
#include "engine/TimelineZoom.h"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line)
{
    if (condition)
        return;

    std::cerr << file << ":" << line << "  " << expression << "\n";
    ++failures;
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

bool near(float actual, float expected, float tolerance)
{
    return std::fabs(actual - expected) <= tolerance;
}

youhost::AudioThreadConfig configAt(double sampleRate, const youhost::Routing& routing)
{
    youhost::AudioThreadConfig config;
    config.routing = routing;
    config.meterTiming = youhost::meterTimingFor(sampleRate);
    return config;
}

void runBlocks(std::array<youhost::ChannelStrip, youhost::kMaxChannels>& strips,
               const youhost::AudioThreadConfig& config,
               const std::vector<std::vector<float>>& inputs,
               int times)
{
    std::vector<const float*> inputPointers(inputs.size());
    for (int n = 0; n < times; ++n)
    {
        for (std::size_t channel = 0; channel < inputs.size(); ++channel)
            inputPointers[channel] = inputs[channel].data();

        youhost::processPassthrough(inputPointers.data(),
                                    static_cast<int>(inputPointers.size()),
                                    nullptr,
                                    0,
                                    static_cast<int>(inputs.empty() ? 0 : inputs[0].size()),
                                    config,
                                    strips.data(),
                                    youhost::kMaxChannels);
    }
}

void testPassthroughCopiesMatchingChannels()
{
    std::array<bool, youhost::kMaxChannels> inputs {};
    std::array<bool, youhost::kMaxChannels> outputs {};
    inputs[1] = true;
    inputs[3] = true;
    outputs[1] = true;
    outputs[3] = true;
    outputs[4] = true;

    const auto routing = youhost::makeRouting(inputs, outputs);
    CHECK(routing.visibleChannels == 5);
    CHECK(routing.inputCount == 2);
    CHECK(routing.outputCount == 3);
    CHECK(routing.inputPacked[1] == 0);
    CHECK(routing.inputPacked[3] == 1);
    CHECK(routing.outputPacked[4] == 2);

    constexpr int frames = 8;
    float in0[frames];
    float in1[frames];
    float out0[frames];
    float out1[frames];
    float out2[frames];
    for (int i = 0; i < frames; ++i)
    {
        in0[i] = 0.25f;
        in1[i] = -0.5f;
        out0[i] = 7.0f;
        out1[i] = 7.0f;
        out2[i] = 7.0f;
    }

    const float* inputsPacked[] = { in0, in1 };
    float* outputsPacked[] = { out0, out1, out2 };
    std::array<youhost::ChannelStrip, youhost::kMaxChannels> strips {};
    const auto config = configAt(48000.0, routing);

    youhost::processPassthrough(inputsPacked, 2, outputsPacked, 3, frames, config, strips.data(), youhost::kMaxChannels);

    for (int i = 0; i < frames; ++i)
    {
        CHECK(out0[i] == 0.25f);
        CHECK(out1[i] == -0.5f);
        CHECK(out2[i] == 0.0f);
    }

    CHECK(strips[1].meter.rms.load() > 0.0f);
    CHECK(strips[0].meter.rms.load() == 0.0f);
    CHECK(strips[3].meter.clipped.load() == false);
}

void testMetersSettleClipAndClear()
{
    std::array<bool, youhost::kMaxChannels> active {};
    active[0] = true;
    const auto config = configAt(48000.0, youhost::makeRouting(active, active));
    std::array<youhost::ChannelStrip, youhost::kMaxChannels> strips {};

    constexpr int frames = 480;
    std::vector<float> constant(static_cast<std::size_t>(frames), 0.5f);
    runBlocks(strips, config, { constant }, 200);
    CHECK(near(strips[0].meter.rms.load(), 0.5f, 0.01f));
    CHECK(near(strips[0].meter.peak.load(), 0.5f, 0.001f));
    CHECK(strips[0].meter.clipped.load() == false);

    std::vector<float> hot(static_cast<std::size_t>(frames), 0.0f);
    hot[0] = 1.0f;
    runBlocks(strips, config, { hot }, 1);
    CHECK(strips[0].meter.clipped.load() == true);
    CHECK(strips[0].meter.peak.load() >= 0.99f);

    std::vector<float> silence(static_cast<std::size_t>(frames), 0.0f);
    runBlocks(strips, config, { silence }, 100);
    CHECK(strips[0].meter.clipped.load() == true);
    CHECK(strips[0].meter.peak.load() > 0.95f);

    // Four seconds of silence must leave the clip latched. It used to time out after two.
    runBlocks(strips, config, { silence }, 300);
    CHECK(strips[0].meter.clipped.load() == true);
    CHECK(strips[0].meter.peak.load() < 0.5f);

    strips[0].meter.clearRequested.store(true);
    runBlocks(strips, config, { silence }, 1);
    CHECK(strips[0].meter.clipped.load() == false);

    runBlocks(strips, config, { silence }, 50);
    CHECK(strips[0].meter.clipped.load() == false);

    std::vector<float> almost(static_cast<std::size_t>(frames), 0.999f);
    runBlocks(strips, config, { almost }, 1);
    CHECK(strips[0].meter.clipped.load() == false);
}

void testClipClearWithoutAnOpenInput()
{
    youhost::Routing routing;
    auto config = configAt(48000.0, routing);
    std::array<youhost::ChannelStrip, youhost::kMaxChannels> strips {};
    strips[2].meterState.clipped = true;
    strips[2].meter.clipped.store(true);
    strips[2].meter.clearRequested.store(true);

    float frame[4] = {};
    const float* inputs[] = { frame };
    youhost::processPassthrough(inputs, 1, nullptr, 0, 4, config, strips.data(), youhost::kMaxChannels);
    CHECK(strips[2].meter.clipped.load() == false);
    CHECK(strips[2].meter.clearRequested.load() == false);
}

void testUnwrittenOutputsAreCleared()
{
    youhost::Routing routing;
    float out[4] = { 3.0f, 3.0f, 3.0f, 3.0f };
    float* outputs[] = { out };
    std::array<youhost::ChannelStrip, youhost::kMaxChannels> strips {};
    youhost::processPassthrough(nullptr, 0, outputs, 1, 4, configAt(48000.0, routing), strips.data(), youhost::kMaxChannels);
    for (float sample : out)
        CHECK(sample == 0.0f);

    youhost::processPassthrough(nullptr, 0, nullptr, 0, 0, configAt(48000.0, routing), strips.data(), youhost::kMaxChannels);
}

void testLatencyFormulas()
{
    using youhost::RoundTripFormula;
    CHECK(youhost::formulaForDeviceType("CoreAudio") == RoundTripFormula::coreAudioSubtractOneBuffer);
    CHECK(youhost::formulaForDeviceType("ALSA") == RoundTripFormula::alsaAddOneBuffer);
    CHECK(youhost::formulaForDeviceType("Windows Audio") == RoundTripFormula::driverSum);

    CHECK(youhost::roundTripSamples(210, 180, 128, 0, RoundTripFormula::coreAudioSubtractOneBuffer) == 262);
    CHECK(youhost::roundTripSamples(384, 384, 128, 0, RoundTripFormula::alsaAddOneBuffer) == 896);
    CHECK(youhost::roundTripSamples(100, 80, 64, 32, RoundTripFormula::driverSum) == 212);
    CHECK(youhost::roundTripSamples(10, 10, 128, 0, RoundTripFormula::coreAudioSubtractOneBuffer) == 0);
    CHECK(near(static_cast<float>(youhost::samplesToMilliseconds(48, 48000.0)), 1.0f, 0.0001f));
    CHECK(youhost::samplesToMilliseconds(48, 0.0) == 0.0);
}

void testTakePlan()
{
    youhost::TakeSpan takes[] = { { 0, 1000 }, { 1000, 500 } };
    CHECK(youhost::timelineEnd(takes, 2) == 1500);
    const auto marks = youhost::takeMarkers(takes, 2);
    CHECK(marks.size() == 3);
    CHECK(youhost::previousMarker(1000, marks) == 0);
    CHECK(youhost::nextMarker(1000, marks) == 1500);
    CHECK(youhost::previousMarker(0, marks) == 0);
    CHECK(youhost::nudgeSamples(100, -500, 1500) == 0);
    CHECK(youhost::nudgeSamples(1400, 500, 1500) == 1500);
    const auto code = youhost::timecodeFromSamples(48000 * 3661, 48000.0);
    CHECK(code.hours == 1);
    CHECK(code.minutes == 1);
    CHECK(code.seconds == 1);
    CHECK(youhost::takeWaveName(1, 3, "Kick Drum") == "3_1_Kick_Drum.wav");
    CHECK(youhost::takeWaveName(12, 1, "Kick/Snare") == "1_12_KickSnare.wav");
    CHECK(youhost::takeWaveName(1, 1, "") == "1_1.wav");
    CHECK(youhost::takeWaveName(2, 2, "BD") == "2_2_BD.wav");
    CHECK(youhost::takeWaveName(1, 1, "  ") == "1_1.wav");
    CHECK(youhost::sanitiseChannelName("  ") == "");
    CHECK(youhost::meterColourForDb(-21.0f, -20) == youhost::MeterColour::green);
    CHECK(youhost::meterColourForDb(-20.0f, -20) == youhost::MeterColour::yellow);
    CHECK(youhost::meterColourForDb(-6.0f, -20) == youhost::MeterColour::yellow);
    CHECK(youhost::meterColourForDb(0.0f, -20) == youhost::MeterColour::red);
    CHECK(youhost::meterColourForDb(-15.0f, -14) == youhost::MeterColour::green);
    CHECK(youhost::meterColourForDb(-13.0f, -14) == youhost::MeterColour::yellow);
    CHECK(youhost::meterColourForDb(-15.0f, -20) == youhost::MeterColour::yellow);
    CHECK(youhost::normaliseWavBitDepth(16) == 16);
    CHECK(youhost::normaliseWavBitDepth(24) == 24);
    CHECK(youhost::normaliseWavBitDepth(32) == 32);
    CHECK(youhost::normaliseWavBitDepth(8) == 24);
    CHECK(youhost::wavBitDepthIsFloat(32));
    CHECK(! youhost::wavBitDepthIsFloat(24));
}

void testMeterLayoutScales()
{
    const auto stereo = youhost::layoutMeters(2, 800.0f, 300.0f);
    CHECK(stereo.columns == 2);
    CHECK(stereo.rows == 1);
    CHECK(stereo.cellWidth <= 68.0f);
    CHECK(stereo.contentWidth <= 800.0f);

    const auto desk = youhost::layoutMeters(32, 1100.0f, 400.0f);
    CHECK(desk.columns == 32);
    CHECK(desk.rows == 1);

    const auto full = youhost::layoutMeters(128, 1100.0f, 400.0f);
    CHECK(full.columns == 128);
    CHECK(full.rows == 1);
    CHECK(full.cellWidth >= 36.0f);
    CHECK(full.contentWidth > 1100.0f);

    const auto none = youhost::layoutMeters(0, 400.0f, 200.0f);
    CHECK(none.cellWidth == 0.0f);

    const auto bridge = youhost::layoutBridge(32, 2, 1100.0f);
    CHECK(bridge.channelWidth >= 36.0f);
    CHECK(bridge.contentWidth > 1100.0f);

    std::array<bool, youhost::kMaxChannels> all {};
    all.fill(true);
    const auto routing = youhost::makeRouting(all, all);
    CHECK(routing.inputCount == 128);
    CHECK(routing.outputCount == 128);
    CHECK(routing.visibleChannels == 128);
    CHECK(routing.inputPacked[127] == 127);
}

void testMeterScales()
{
    CHECK(youhost::normaliseRmsReferenceDb(-20) == -20);
    CHECK(youhost::normaliseRmsReferenceDb(-19) == -20);
    CHECK(youhost::normaliseRmsReferenceDb(-15) == -14);
    CHECK(youhost::normaliseRmsReferenceDb(0) == -14);

    youhost::MeterTick ticks[9];
    CHECK(youhost::rmsTicks(-20, ticks, 9) == 9);
    CHECK(ticks[0].label == 20);
    CHECK(ticks[0].dbFs == 0.0f);
    CHECK(ticks[3].label == 0);
    CHECK(ticks[3].dbFs == -20.0f);
    CHECK(ticks[8].label == -40);
    CHECK(ticks[8].dbFs == -60.0f);

    CHECK(youhost::rmsTicks(-18, ticks, 9) == 9);
    CHECK(ticks[0].dbFs == 2.0f);
    CHECK(ticks[3].dbFs == -18.0f);

    CHECK(youhost::rmsTicks(-14, ticks, 9) == 9);
    CHECK(ticks[0].dbFs == 6.0f);
    CHECK(ticks[3].dbFs == -14.0f);
    CHECK(ticks[8].dbFs == -54.0f);

    CHECK(youhost::peakTicks(ticks, 9) == 8);
    CHECK(ticks[0].label == 0);
    CHECK(ticks[0].dbFs == 0.0f);
    CHECK(ticks[2].label == -6);
    CHECK(ticks[7].label == -60);
    CHECK(ticks[7].dbFs == -60.0f);

    const float fullScale = 1.0f;
    const float line = 0.1f; // -20 dBFS
    const float floor = 0.001f; // -60 dBFS
    CHECK(near(youhost::meterNormal(fullScale, false, -20), 1.0f, 0.001f));
    CHECK(near(youhost::meterNormal(line, false, -20), 40.0f / 60.0f, 0.001f));
    CHECK(near(youhost::meterNormal(floor, false, -20), 0.0f, 0.001f));
    // -14 dBFS reference: full scale is only +14 VU, short of the +20 tick.
    CHECK(near(youhost::meterNormal(fullScale, false, -14), 54.0f / 60.0f, 0.001f));
    CHECK(near(youhost::meterNormal(fullScale, true, -14), 1.0f, 0.001f));
    CHECK(near(youhost::meterNormal(floor, true, -20), 0.0f, 0.001f));
}

void testDropoutDecisions()
{
    const int64_t period = youhost::expectedPeriodNs(48000.0, 128);
    CHECK(period == 2666667);
    CHECK(youhost::expectedPeriodNs(0.0, 128) == 0);
    CHECK(youhost::dropoutGapCount(0, period, period, false) == 0);
    CHECK(youhost::dropoutGapCount(1000, 1000 + period, period, true) == 0);
    CHECK(youhost::dropoutGapCount(1000, 1000 + period, period, false) == 0);
    CHECK(youhost::dropoutGapCount(1000, 1000 + period + 1000000, period, false) == 0);
    CHECK(youhost::dropoutGapCount(1000, 1000 + period * 2, period, false) == 1);
    CHECK(youhost::dropoutOverrunCount(period, period) == 0);
    CHECK(youhost::dropoutOverrunCount(period + 1, period) == 1);
    CHECK(youhost::dropoutOverrunCount(period, 0) == 0);
}

void testLatencyCompensation()
{
    youhost::ChannelLatencyInput channels[4] {};
    channels[0] = { 100, true };
    channels[1] = { 40, true };
    channels[2] = { 500, false };
    channels[3] = { 0, true };

    const auto plan = youhost::planCompensation(channels, 4);
    CHECK(plan.alignmentSamples == 100);
    CHECK(plan.delaySamples[0] == 0);
    CHECK(plan.delaySamples[1] == 60);
    CHECK(plan.delaySamples[2] == 0);
    CHECK(plan.delaySamples[3] == 100);

    youhost::ChannelLatencyInput matched[2] {};
    matched[0] = { 100, true };
    matched[1] = { 100, true };
    const auto even = youhost::planCompensation(matched, 2);
    CHECK(even.alignmentSamples == 100);
    CHECK(even.delaySamples[0] == 0);
    CHECK(even.delaySamples[1] == 0);

    youhost::ChannelLatencyInput huge[1] {};
    huge[0] = { 10000000, true };
    const auto clamped = youhost::planCompensation(huge, 1);
    CHECK(clamped.alignmentSamples == youhost::kMaxCompensationSamples);

    const int latencies[] = { 10, 20, 30, 40 };
    const bool occupied[] = { true, true, true, false };
    const bool bypassed[] = { false, true, false, false };
    CHECK(youhost::sumSlotLatency(latencies, occupied, bypassed, 4) == 40);

    youhost::ChannelLatencyInput grouped[4] {};
    grouped[0] = { 100, true, 0 };
    grouped[1] = { 300, true, 0 };
    grouped[2] = { 1000, true, -1 };
    grouped[3] = { 50, true, 1 };
    const auto perGroup = youhost::planCompensation(grouped, 4, youhost::AlignMode::group);
    CHECK(perGroup.delaySamples[0] == 200);
    CHECK(perGroup.delaySamples[1] == 0);
    CHECK(perGroup.delaySamples[2] == 0);
    CHECK(perGroup.delaySamples[3] == 0);
    CHECK(perGroup.alignmentSamples == 300);

    youhost::ChannelLatencyInput pair[2] {};
    pair[0] = { 64, true, -1 };
    pair[1] = { 64, true, -1 };
    const auto together = youhost::planCompensation(pair, 2, youhost::AlignMode::all);
    CHECK(together.delaySamples[0] == together.delaySamples[1]);
    CHECK(together.delaySamples[0] == 0);
}

void testOffChannelStaysSilent()
{
    std::array<bool, youhost::kMaxChannels> inputs {};
    std::array<bool, youhost::kMaxChannels> outputs {};
    inputs[0] = true;
    inputs[2] = true;
    outputs[0] = true;
    outputs[2] = true;
    auto routing = youhost::makeRouting(inputs, outputs);

    float in0[4] = { 0.5f, 0.0f, 0.0f, 0.0f };
    float in2[4] = { -0.25f, 0.0f, 0.0f, 0.0f };
    const float* inputPointers[2] = { in0, in2 };
    float out0[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float out2[4] {};
    float* outputsPacked[2] = { out0, out2 };
    std::array<youhost::ChannelStrip, youhost::kMaxChannels> strips {};

    std::uint64_t low = ~std::uint64_t { 0 };
    std::uint64_t high = ~std::uint64_t { 0 };
    youhost::setChannelOnBit(low, high, 0, false);
    CHECK(! youhost::channelIsOn(low, high, 0));
    CHECK(youhost::channelIsOn(low, high, 70));
    youhost::setChannelOnBit(low, high, 70, false);
    CHECK(! youhost::channelIsOn(low, high, 70));

    youhost::processPassthrough(inputPointers,
                                2,
                                outputsPacked,
                                2,
                                4,
                                configAt(48000.0, routing),
                                strips.data(),
                                youhost::kMaxChannels,
                                low,
                                high);
    CHECK(out0[0] == 0.0f);
    CHECK(near(out2[0], -0.25f, 0.0001f));
    CHECK(strips[0].meter.rms.load() == 0.0f);
    CHECK(strips[2].meter.rms.load() > 0.0f);
}

void testChannelPick()
{
    youhost::ChannelSelection selection;
    selection = youhost::pickChannels(selection, 4, 32, youhost::ChannelPick::replace);
    CHECK(selection.channels.size() == 1);
    CHECK(selection.channels[0] == 4);
    CHECK(selection.anchor == 4);

    selection = youhost::pickChannels(selection, 8, 32, youhost::ChannelPick::range);
    CHECK(selection.channels.size() == 5);
    CHECK(selection.channels.front() == 4);
    CHECK(selection.channels.back() == 8);
    CHECK(selection.anchor == 4);

    selection = youhost::pickChannels(selection, 6, 32, youhost::ChannelPick::toggle);
    CHECK(std::find(selection.channels.begin(), selection.channels.end(), 6) == selection.channels.end());
    CHECK(selection.anchor == 6);
    selection = youhost::pickChannels(selection, 6, 32, youhost::ChannelPick::toggle);
    CHECK(std::find(selection.channels.begin(), selection.channels.end(), 6) != selection.channels.end());

    youhost::ChannelSelection sparse;
    sparse = youhost::pickChannels(sparse, 1, 32, youhost::ChannelPick::replace);
    sparse = youhost::pickChannels(sparse, 3, 32, youhost::ChannelPick::toggle);
    sparse = youhost::pickChannels(sparse, 7, 32, youhost::ChannelPick::toggle);
    sparse = youhost::pickChannels(sparse, 9, 32, youhost::ChannelPick::toggle);
    CHECK(sparse.channels.size() == 4);
}

void testSessionNames()
{
    CHECK(youhost::europeanSessionDate(9, 10, 2026) == "09.10.2026");
    CHECK(youhost::crashRecoverySessionName(9, 10, 2026, 14, 32) == "09.10.2026_crash_14-32");
    CHECK(youhost::nextFreeSessionName("09.10.2026", std::vector<std::string> {}) == "09.10.2026");
    CHECK(youhost::nextFreeSessionName("09.10.2026", std::vector<std::string> { "09.10.2026" }) == "09.10.2026_1");
    CHECK(youhost::nextFreeSessionName("09.10.2026", std::vector<std::string> { "09.10.2026", "09.10.2026_1" })
          == "09.10.2026_2");
    CHECK(youhost::kSessionBackupIntervalMs == 5 * 60 * 1000);
    CHECK(youhost::kSessionBackupsToKeep == 10);

    std::vector<youhost::BackupStamp> stamps {
        { "old", 10 },
        { "newer", 50 },
        { "mid", 30 },
    };
    const auto drop = youhost::backupsToRemove(stamps, 2);
    CHECK(drop.size() == 1);
    CHECK(drop[0] == "old");
    CHECK(youhost::groupFoldLabel(true, 8) == "\u25B8 8 ch");
    CHECK(youhost::groupFoldLabel(false, 8) == "\u25BE 8 ch");
}

void testPluginLoadPace()
{
    using youhost::PluginInstantiateWhere;
    CHECK(youhost::isAudioUnitFormat("AudioUnit"));
    CHECK(! youhost::isAudioUnitFormat("VST3"));
    CHECK(youhost::pluginInstantiateWhere(true, false) == PluginInstantiateWhere::messageAsync);
    CHECK(youhost::pluginInstantiateWhere(false, true) == PluginInstantiateWhere::messageAsync);
    CHECK(youhost::pluginInstantiateWhere(false, false) == PluginInstantiateWhere::background);

    char text[64];
    CHECK(! youhost::formatPluginLoadProgress(0, 0, false, text, sizeof(text)));
    CHECK(youhost::formatPluginLoadProgress(0, 5, false, text, sizeof(text)));
    CHECK(std::string(text) == "Loading plugins 1/5\xE2\x80\xA6");
    CHECK(youhost::formatPluginLoadProgress(2, 5, true, text, sizeof(text)));
    CHECK(std::string(text) == "Loading plugins 3/5\xE2\x80\xA6");
    CHECK(! youhost::formatPluginLoadProgress(5, 5, false, text, sizeof(text)));

    youhost::SessionLoadCursor cursor;
    cursor.total = 5;
    int started = 0;
    while (! cursor.done())
    {
        CHECK(cursor.startOne());
        CHECK(! cursor.startOne());
        ++started;
        cursor.completeOne();
    }
    CHECK(started == 5);
    CHECK(cursor.done());
    CHECK(! cursor.startOne());
}

void testShortcutsMatchTheHelp()
{
    const auto help = youhost::shortcutHelpText();
    const auto expect = [&help](youhost::ShortcutId id, char character, youhost::KeyKind kind, bool shift, bool command, bool alt)
    {
        youhost::KeyQuery query;
        query.kind = kind;
        query.character = character;
        query.shift = shift;
        query.command = command;
        query.alt = alt;
        const auto matched = youhost::matchShortcut(query);
        CHECK(matched.has_value());
        if (matched.has_value())
            CHECK(*matched == id);
        const auto line = youhost::shortcutChord(id) + "  " + youhost::shortcutMeaning(id);
        CHECK(help.find(line) != std::string::npos);
    };

    expect(youhost::ShortcutId::recPage, '1', youhost::KeyKind::character, false, false, false);
    expect(youhost::ShortcutId::hostPage, '2', youhost::KeyKind::character, false, false, false);
    expect(youhost::ShortcutId::dropouts, '3', youhost::KeyKind::character, false, false, false);
    expect(youhost::ShortcutId::dropouts, 'd', youhost::KeyKind::character, false, false, false);
    expect(youhost::ShortcutId::cpu, '4', youhost::KeyKind::character, false, false, false);
    expect(youhost::ShortcutId::latency, '5', youhost::KeyKind::character, false, false, false);
    expect(youhost::ShortcutId::scanner, 's', youhost::KeyKind::character, false, false, false);
    expect(youhost::ShortcutId::zoomIn, 't', youhost::KeyKind::character, false, false, false);
    expect(youhost::ShortcutId::zoomOut, 0, youhost::KeyKind::letterR, false, false, false);
    expect(youhost::ShortcutId::fit, 0, youhost::KeyKind::letterR, false, false, true);
    expect(youhost::ShortcutId::lanesTaller, 0, youhost::KeyKind::bracketLeft, false, true, false);
    expect(youhost::ShortcutId::lanesShorter, 0, youhost::KeyKind::bracketRight, false, true, false);
    expect(youhost::ShortcutId::playOrStop, 0, youhost::KeyKind::space, false, false, false);
    expect(youhost::ShortcutId::recordNow, 0, youhost::KeyKind::space, false, true, false);
    expect(youhost::ShortcutId::previousTake, 0, youhost::KeyKind::left, false, false, false);
    expect(youhost::ShortcutId::nextTake, 0, youhost::KeyKind::right, false, false, false);
    expect(youhost::ShortcutId::nudgeBack, 0, youhost::KeyKind::left, true, false, false);
    expect(youhost::ShortcutId::nudgeForward, 0, youhost::KeyKind::right, true, false, false);
    expect(youhost::ShortcutId::save, 's', youhost::KeyKind::character, false, true, false);
    expect(youhost::ShortcutId::saveAs, 's', youhost::KeyKind::character, true, true, false);

    CHECK(help.find("5  Open or close SCAN") == std::string::npos);
    CHECK(help.find("5  Open or close LATENCY") != std::string::npos);
    CHECK(help.find("S  Open or close SCAN") != std::string::npos);
    CHECK(help.find("3 or D  Open or close DROPOUTS") != std::string::npos);
    CHECK(help.find("W+") != std::string::npos);
    CHECK(help.find("Null test") != std::string::npos);
    CHECK(help.find("Save As") != std::string::npos);
    CHECK(help.find("Backup") != std::string::npos);
    CHECK(help.find("Shift+click") != std::string::npos);
    CHECK(help.find("Cmd+click") != std::string::npos);
    CHECK(help.find("Option-drag") != std::string::npos);

    CHECK(youhost::matchNameKey(false, true, false, false, false, false, false) == youhost::NameKey::commit);
    CHECK(youhost::matchNameKey(false, false, true, false, false, false, false) == youhost::NameKey::cancel);
    CHECK(youhost::matchNameKey(true, false, false, false, false, false, false) == youhost::NameKey::next);
    CHECK(youhost::matchNameKey(true, false, false, true, false, false, false) == youhost::NameKey::previous);
    CHECK(youhost::matchNameKey(true, false, false, false, true, false, false) == youhost::NameKey::none);
    for (const auto& nameKey : youhost::kNameKeyHelp)
        CHECK(help.find(nameKey.line) != std::string::npos);
    CHECK(std::string(youhost::kNameKeyHelp[0].line).find("Enter") == 0);
    CHECK(std::string(youhost::kNameKeyHelp[1].line).find("Esc") == 0);
}

void testGroupsFoldAndPalette()
{
    CHECK(youhost::kX32ColourCount == 16);
    CHECK(std::string(youhost::kX32Colours[1].code) == "RD");
    CHECK(youhost::kX32Colours[9].inverted);
    CHECK(! youhost::kX32Colours[0].inverted);
    CHECK(youhost::normaliseX32Colour(40) == 0);

    int membership[8] = { 0, 0, 0, 0, -1, -1, 1, 1 };
    bool collapsed[youhost::kMaxDisplayGroups] = {};
    collapsed[0] = true;
    youhost::StripItem items[16];
    const int folded = youhost::layoutChannelStrips(8, membership, collapsed, items, 16);
    CHECK(folded == 6);
    CHECK(items[0].kind == youhost::StripKind::groupHeader);
    CHECK(items[0].group == 0);
    CHECK(items[1].channel == 4);
    CHECK(items[3].kind == youhost::StripKind::groupHeader);
    CHECK(items[3].group == 1);
    CHECK(items[4].channel == 6);

    collapsed[0] = false;
    const int open = youhost::layoutChannelStrips(8, membership, collapsed, items, 16);
    CHECK(open == 10);
    CHECK(items[1].channel == 0);
    CHECK(items[4].channel == 3);
    CHECK(items[5].channel == 4);

    collapsed[0] = true;
    const int hidden = youhost::layoutChannelStrips(8, membership, collapsed, items, 16);
    CHECK(youhost::adjacentVisibleChannel(items, hidden, 4, 1) == 5);
    CHECK(youhost::adjacentVisibleChannel(items, hidden, 5, 1) == 6);
    CHECK(youhost::adjacentVisibleChannel(items, hidden, 4, -1) == -1);
    CHECK(youhost::adjacentVisibleChannel(items, hidden, 7, 1) == -1);
    CHECK(youhost::adjacentVisibleChannel(items, hidden, 0, 1) == -1);
}

void testPlaybackCopiesDryChannels()
{
    std::array<bool, youhost::kMaxChannels> inputs {};
    std::array<bool, youhost::kMaxChannels> outputs {};
    inputs[0] = true;
    inputs[2] = true;
    outputs[0] = true;
    outputs[2] = true;
    auto routing = youhost::makeRouting(inputs, outputs);
    for (int channel = 0; channel < youhost::kMaxChannels; ++channel)
        routing.inputPacked[static_cast<std::size_t>(channel)] = static_cast<std::int16_t>(channel);

    float playback[youhost::kMaxChannels][4] {};
    playback[0][0] = 0.25f;
    playback[2][0] = -0.5f;
    const float* inputPointers[youhost::kMaxChannels] {};
    for (int channel = 0; channel < youhost::kMaxChannels; ++channel)
        inputPointers[channel] = playback[channel];

    float out0[4] {};
    float out2[4] {};
    float* outputsPacked[2] = { out0, out2 };
    std::array<youhost::ChannelStrip, youhost::kMaxChannels> strips {};
    youhost::processPassthrough(inputPointers,
                                youhost::kMaxChannels,
                                outputsPacked,
                                2,
                                4,
                                configAt(48000.0, routing),
                                strips.data(),
                                youhost::kMaxChannels);
    CHECK(near(out0[0], 0.25f, 0.0001f));
    CHECK(near(out2[0], -0.5f, 0.0001f));
    CHECK(out0[1] == 0.0f);
    CHECK(out2[1] == 0.0f);
}

void testDryChannelDelayMatchesPluginChannel()
{
    youhost::ChannelLatencyInput channels[2] {};
    channels[0] = { 8, true };
    channels[1] = { 0, true };
    const auto plan = youhost::planCompensation(channels, 2);
    CHECK(plan.alignmentSamples == 8);
    CHECK(plan.delaySamples[0] == 0);
    CHECK(plan.delaySamples[1] == 8);

    float plugin[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 };
    float dry[12] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    float line[8] {};
    int write = 0;
    youhost::delayInPlace(line, plan.delaySamples[1], write, dry, 12);
    CHECK(dry[8] == 1.0f);
    CHECK(plugin[8] == 1.0f);
    CHECK(dry[0] == 0.0f);
}

void testScanOrderSkipsWavesAndHidesBuiltIns()
{
    auto waves = youhost::makeScanCandidate("AudioUnit", "AudioUnit:Effects/aumf,CLSM,ksWV");
    auto feedback = youhost::makeScanCandidate("VST3", "/Library/Audio/Plug-Ins/VST3/Defeedback.vst3");
    auto shell = youhost::makeScanCandidate("VST3", "/Library/Audio/Plug-Ins/VST3/WaveShell1-VST3.vst3");
    CHECK(waves.waves);
    CHECK(waves.shell);
    CHECK(! feedback.shell);
    CHECK(shell.waves);
    CHECK(youhost::skipBecauseWaves(waves, false));
    CHECK(! youhost::skipBecauseWaves(waves, true));
    CHECK(! youhost::skipBecauseWaves(feedback, false));

    std::vector<youhost::ScanCandidate> jobs { waves, feedback, shell };
    youhost::orderScanCandidates(jobs);
    CHECK(jobs[0].identifier.find("Defeedback") != std::string::npos);

    CHECK(youhost::isAppleBuiltIn("Apple", "AudioUnit:Effects/aufx,greq,appl"));
    CHECK(youhost::showInInsertList("AudioUnit:Effects/aufx,greq,appl", "Effect", false, false));
    CHECK(! youhost::showInInsertList("AudioUnit:Synths/aumu,dls ,appl", "Synth", true, false));
    CHECK(youhost::showInInsertList("AudioUnit:Synths/aumu,dls ,appl", "Synth", true, true));
    CHECK(! youhost::showInInsertList("AudioUnit:Generators/augn,afil,appl", "Generator", false, false));
    CHECK(youhost::showInInsertList("/Library/Audio/Plug-Ins/VST3/Defeedback.vst3", "Fx|Delay", false, false));
    CHECK(youhost::showInInsertList("/Library/Audio/Plug-Ins/VST3/Defeedback.vst3", "", false, false));
    CHECK(! youhost::showInInsertList("/Library/Audio/Plug-Ins/VST3/Synth.vst3", "Instrument|Synth", false, false));
}

void testTakeImportGroups()
{
    const auto kick = youhost::parseRecordingName("Take01_Ch03_Kick.wav");
    CHECK(kick.takeNumber == 1);
    CHECK(kick.channelNumber == 3);
    const auto slash = youhost::parseRecordingName("Take12_Ch01_KickSnare.wav");
    CHECK(slash.takeNumber == 12);
    CHECK(slash.channelNumber == 1);
    const auto current = youhost::parseRecordingName("1_1_BD.wav");
    CHECK(current.takeNumber == 1);
    CHECK(current.channelNumber == 1);
    const auto plain = youhost::parseRecordingName("3_2.wav");
    CHECK(plain.takeNumber == 2);
    CHECK(plain.channelNumber == 3);
    const auto wide = youhost::parseRecordingName("12_3_Kick.wav");
    CHECK(wide.takeNumber == 3);
    CHECK(wide.channelNumber == 12);
    const auto loose = youhost::parseRecordingName("Track 4 snare.wav");
    CHECK(loose.takeNumber == 0);
    CHECK(loose.channelNumber == 4);

    const auto takes = youhost::groupImportedRecordings({ "Take02_Ch01_A.wav", "Kick.wav", "Take01_Ch02_B.wav" });
    CHECK(takes.size() == 3);
    CHECK(takes[0].number == 1);
    CHECK(takes[0].channels[0].channel == 1);
    CHECK(takes[1].number == 2);
    CHECK(takes[2].number == 0);
    CHECK(takes[2].channels[0].channel == 0);
}

void testDropoutWindow()
{
    youhost::DropoutRing ring;
    ring.push(1000, 10, true);
    ring.push(5000, -1, false);
    youhost::DropoutMark marks[4];
    CHECK(ring.drain(marks, 4) == 2);
    CHECK(marks[0].recording);
    CHECK(marks[0].timelineSample == 10);
    CHECK(! marks[1].recording);
    CHECK(ring.drain(marks, 4) == 0);

    CHECK(youhost::countMarksInWindow(marks, 2, 6000, 2000) == 1);
    CHECK(youhost::countMarksInWindow(marks, 2, 6000, 0) == 2);
    CHECK(youhost::windowIsStable(marks, 2, 6000, 500));
    CHECK(! youhost::windowIsStable(marks, 2, 6000, 0));
    CHECK(youhost::latestMarkNs(marks, 2) == 5000);
}

void testSessionLayout()
{
    const auto layout = youhost::sessionLayoutFor("/tmp/My Session/");
    CHECK(layout.folder == "/tmp/My Session");
    CHECK(layout.sessionFile == "/tmp/My Session/session.youhost");
    CHECK(layout.audioFolder == "/tmp/My Session/audio");
    CHECK(youhost::sanitiseSessionName("Friday/show") == "Fridayshow");
    CHECK(youhost::sanitiseSessionName("  ") == "Session");
    CHECK(youhost::sanitiseSessionName("Ok.") == "Ok");
}

void testTimelineZoom()
{
    CHECK(youhost::zoomVisibleSamples(48000, 0) == 48000);
    CHECK(youhost::zoomVisibleSamples(48000, 1) == 24000);
    const auto hour = static_cast<std::int64_t>(48000) * 3600;
    CHECK(youhost::clampZoomStep(99, hour) == youhost::maxZoomStepForSpan(hour));
    CHECK(youhost::maxZoomStepForSpan(hour) > 10);
    CHECK(youhost::zoomVisibleSamples(hour, 0) == hour);
    CHECK(youhost::zoomVisibleSamples(hour, youhost::maxZoomStepForSpan(hour)) == youhost::kMinTimelineZoomSamples);
    CHECK(youhost::lanesShownForVerticalStep(128, 0) == 128);
    CHECK(youhost::lanesShownForVerticalStep(128, 100) == 1);
    CHECK(youhost::laneNumberVisible(12.0f));
    CHECK(! youhost::laneNumberVisible(11.0f));
    const auto start = youhost::viewStartKeepingPlayhead(1000, 0, 1000, 100, 400);
    CHECK(start <= 400);
    CHECK(start + 100 > 400);
    CHECK(youhost::followPlayhead(1000, 0, 1000, 400) == 0);
    const auto followed = youhost::followPlayhead(10000, 0, 1000, 5000);
    CHECK(followed > 0);
    CHECK(5000 >= followed);
    CHECK(5000 < followed + 1000);

    CHECK(youhost::fitSpanSamples(0, 48000.0) == 48000 * 60);
    CHECK(youhost::fitSpanSamples(-5, 48000.0) == 48000 * 60);
    const auto oneSecond = youhost::fitSpanSamples(48000, 48000.0);
    CHECK(oneSecond - 48000 == 24000);
    CHECK(oneSecond < 48000 * 30);
    const auto longTake = static_cast<std::int64_t>(48000) * 100;
    CHECK(youhost::fitSpanSamples(longTake, 48000.0) - longTake >= longTake / 50);

    float x1 = 0.0f;
    float x2 = 0.0f;
    youhost::timelineRegionPixels(0.0f, 1000.0f, 0, 48000 * 60, 0, 48000 * 2, x1, x2);
    CHECK(x2 - x1 > 30.0f);
    CHECK(x2 - x1 < 40.0f);
    CHECK(x2 < 1000.0f);
}

void testInsertMenuAndStall()
{
    CHECK(! youhost::beatIsStale(100, 0, youhost::kStallLimitNs));
    CHECK(! youhost::beatIsStale(100, 100, youhost::kStallLimitNs));
    CHECK(! youhost::beatIsStale(youhost::kStallLimitNs, 1, youhost::kStallLimitNs));
    CHECK(youhost::beatIsStale(youhost::kStallLimitNs + 2, 1, youhost::kStallLimitNs));
    CHECK(std::string(youhost::stallPhaseName(youhost::kPhaseLoad)) == "load");

    std::vector<youhost::CatalogPlugin> plugins {
        { "Pro-Q 3", "FabFilter", "AudioUnit" },
        { "Pro-Q 3", "FabFilter", "VST3" },
        { "De-Feedback", "Alpha Labs", "AudioUnit" },
        { "Saturn 2", "FabFilter", "VST3" }
    };
    const auto all = youhost::groupInsertPlugins(plugins, "");
    CHECK(all.size() == 2);
    CHECK(all[0].manufacturer == "Alpha Labs");
    CHECK(all[0].plugins.size() == 1);
    CHECK(all[0].plugins[0].label == "De-Feedback");
    CHECK(all[1].manufacturer == "FabFilter");
    CHECK(all[1].plugins.size() == 3);
    CHECK(all[1].plugins[0].label == "Pro-Q 3 (AU)");
    CHECK(all[1].plugins[1].label == "Pro-Q 3 (VST3)");
    CHECK(all[1].plugins[2].label.find("Saturn") != std::string::npos);

    const auto fab = youhost::groupInsertPlugins(plugins, "FAB");
    CHECK(fab.size() == 1);
    CHECK(fab[0].plugins.size() == 3);
    const auto proq = youhost::groupInsertPlugins(plugins, "pro q");
    CHECK(proq.size() == 1);
    CHECK(proq[0].plugins.size() == 2);

    std::vector<youhost::CatalogPlugin> unknown { { "Thing", "  ", "VST3" } };
    const auto grouped = youhost::groupInsertPlugins(unknown, "");
    CHECK(grouped.size() == 1);
    CHECK(grouped[0].manufacturer == "Unknown");

    const auto auOnly = youhost::groupInsertPlugins(plugins, "", youhost::InsertFormatFilter::audioUnit);
    CHECK(auOnly.size() == 2);
    CHECK(auOnly[1].plugins.size() == 1);
    CHECK(auOnly[1].plugins[0].label == "Pro-Q 3 (AU)");
    const auto vstOnly = youhost::groupInsertPlugins(plugins, "", youhost::InsertFormatFilter::vst3);
    CHECK(vstOnly.size() == 1);
    CHECK(vstOnly[0].plugins.size() == 2);
    CHECK(vstOnly[0].plugins[0].label == "Pro-Q 3 (VST3)");
}

void testCrashJournal()
{
    youhost::CrashJournal journal;
    CHECK(youhost::readCrashJournal("").clean);
    youhost::CrashMark mark;
    mark.phase = "loading";
    mark.channel = 3;
    mark.slot = 1;
    mark.name = "VM Transient Shaper";
    mark.identifier = "au.vm";
    youhost::upsertCrashMark(journal, mark);
    CHECK(! journal.clean);
    const auto text = youhost::writeCrashJournal(journal);
    const auto back = youhost::readCrashJournal(text);
    CHECK(! back.clean);
    CHECK(back.marks.size() == 1);
    CHECK(back.marks[0].name == "VM Transient Shaper");
    CHECK(back.marks[0].channel == 3);
    auto cleared = back;
    youhost::eraseCrashMark(cleared, 3, 1);
    CHECK(cleared.marks.empty());
    const auto dirty = youhost::readCrashJournal("mark\tactive\t1\t0\tName\tid\n");
    CHECK(! dirty.clean);
    CHECK(dirty.marks.size() == 1);
}

void testDeviceWatch()
{
    CHECK(! youhost::sampleRatesDiffer(48000.0, 48000.0));
    CHECK(youhost::sampleRatesDiffer(48000.0, 44100.0));
    CHECK(! youhost::sampleRatesDiffer(0.0, 48000.0));
    CHECK(youhost::sampleRateWarningText(44100.0, 48000.0).find("44100") != std::string::npos);
    CHECK(youhost::usableChannelCount(256) == 128);
    CHECK(youhost::usableChannelCount(48) == 48);
    CHECK(youhost::usableChannelCount(-1) == 0);

    bool wing[48];
    for (int index = 0; index < 48; ++index)
        wing[index] = index < 47;
    const auto shortOut = youhost::inspectChannelMask(wing, 48);
    CHECK(shortOut.reported == 48);
    CHECK(shortOut.active == 47);
    CHECK(shortOut.solidPrefix);
    CHECK(youhost::trailingOutputMissing(shortOut));
    wing[10] = false;
    const auto hole = youhost::inspectChannelMask(wing, 48);
    CHECK(! hole.solidPrefix);
    CHECK(! youhost::trailingOutputMissing(hole));
    bool full[48];
    for (bool& bit : full)
        bit = true;
    const auto allOut = youhost::inspectChannelMask(full, 48);
    CHECK(allOut.active == 48);
    CHECK(! youhost::trailingOutputMissing(allOut));
}

void testMergePeaks()
{
    std::vector<youhost::WavePeak> first { { -0.2f, 0.2f }, { -0.1f, 0.4f } };
    std::vector<youhost::WavePeak> second { { -0.5f, 0.1f } };
    std::vector<const std::vector<youhost::WavePeak>*> layers { &first, &second };
    const auto merged = youhost::mergePeakLayers(layers);
    CHECK(merged.size() == 2);
    CHECK(near(merged[0].low, -0.5f, 0.0001f));
    CHECK(near(merged[0].high, 0.2f, 0.0001f));
    CHECK(near(merged[1].high, 0.4f, 0.0001f));
}

void testDryPathIsBitIdentical()
{
    std::array<bool, youhost::kMaxChannels> active {};
    active[0] = true;
    active[1] = true;
    const auto routing = youhost::makeRouting(active, active);

    const float pattern[8] = { 1.0f, -1.0f, 0.0f, 0.1f, -0.25f, 0.0001f, 0.5f, -0.5f };
    float in0[8];
    float in1[8];
    std::memcpy(in0, pattern, sizeof(pattern));
    std::memcpy(in1, pattern, sizeof(pattern));
    const float* inputs[2] = { in0, in1 };
    float out0[8];
    float out1[8];
    std::memset(out0, 0x5a, sizeof(out0));
    std::memset(out1, 0x5a, sizeof(out1));
    float* outputs[2] = { out0, out1 };
    std::array<youhost::ChannelStrip, youhost::kMaxChannels> strips {};

    youhost::processPassthrough(inputs, 2, outputs, 2, 8, configAt(48000.0, routing), strips.data(), youhost::kMaxChannels);
    CHECK(std::memcmp(out0, pattern, sizeof(pattern)) == 0);
    CHECK(std::memcmp(out1, pattern, sizeof(pattern)) == 0);
    CHECK(out0[0] == 1.0f);
    CHECK(out0[1] == -1.0f);

    float same[8];
    std::memcpy(same, pattern, sizeof(pattern));
    const float* sameIn[1] = { same };
    float* sameOut[1] = { same };
    std::array<bool, youhost::kMaxChannels> one {};
    one[0] = true;
    youhost::processPassthrough(sameIn, 1, sameOut, 1, 8, configAt(48000.0, youhost::makeRouting(one, one)), strips.data(), youhost::kMaxChannels);
    CHECK(std::memcmp(same, pattern, sizeof(pattern)) == 0);

    CHECK(youhost::outputDbToLinear(0.0f) == 1.0f);
    float trimmed[8];
    std::memcpy(trimmed, pattern, sizeof(pattern));
    youhost::applyOutputTrim(trimmed, 8, youhost::outputDbToLinear(0.0f));
    CHECK(std::memcmp(trimmed, pattern, sizeof(pattern)) == 0);

    float delayed[8];
    std::memcpy(delayed, pattern, sizeof(pattern));
    float line[3] {};
    int write = 0;
    youhost::delayInPlace(line, 3, write, delayed, 8);
    CHECK(std::memcmp(delayed + 3, pattern, sizeof(float) * 5) == 0);
    CHECK(delayed[0] == 0.0f);
    CHECK(delayed[1] == 0.0f);
    CHECK(delayed[2] == 0.0f);
    bool notAHighPass = false;
    for (int index = 1; index < 8; ++index)
        if (delayed[index] != pattern[index] - pattern[index - 1])
            notAHighPass = true;
    CHECK(notAHighPass);

    float left[4];
    float right[4] = { 7.0f, 7.0f, 7.0f, 7.0f };
    float* staged[2] = { left, right };
    youhost::stagePluginChannels(staged, 1, 2, pattern, 4);
    CHECK(std::memcmp(left, pattern, sizeof(float) * 4) == 0);
    CHECK(right[0] == 0.0f);
    CHECK(right[1] == 0.0f);
    CHECK(right[2] == 0.0f);
    CHECK(right[3] == 0.0f);

    youhost::stagePluginChannels(staged, 2, 2, pattern, 4);
    CHECK(std::memcmp(left, pattern, sizeof(float) * 4) == 0);
    CHECK(std::memcmp(right, pattern, sizeof(float) * 4) == 0);

    float side[4];
    side[0] = 0.0f;
    for (int index = 1; index < 4; ++index)
        side[index] = -pattern[index - 1];
    const float* pluginOut[2] = { left, side };
    float folded[4];
    std::memcpy(folded, pattern, sizeof(float) * 4);
    youhost::takePluginChannel(folded, pluginOut, 2, 4);
    CHECK(std::memcmp(folded, left, sizeof(float) * 4) == 0);
    bool notADifference = false;
    for (int index = 0; index < 4; ++index)
        if (folded[index] != left[index] - side[index])
            notADifference = true;
    CHECK(notADifference);
    bool notASum = false;
    for (int index = 0; index < 4; ++index)
        if (folded[index] != (left[index] + side[index]) * 0.5f)
            notASum = true;
    CHECK(notASum);

    CHECK(! youhost::pluginSlotRuns(true, true, false));
    CHECK(! youhost::pluginSlotRuns(true, false, true));
    CHECK(youhost::pluginSlotRuns(true, false, false));
    CHECK(! youhost::pluginSlotRuns(false, false, false));

    float signal[4] = { 1.0f, 0.5f, -0.25f, 0.0f };
    float silent[4] = {};
    float inverted[4] = { -1.0f, -0.5f, 0.25f, 0.0f };
    const float* identical[2] = { signal, signal };
    float foldedStereo[4];
    youhost::takeFoldedChannel(foldedStereo, identical, 2, 4, youhost::StereoFold::left);
    CHECK(youhost::sameFloatBits(foldedStereo[0], 1.0f));
    CHECK(youhost::sameFloatBits(foldedStereo[1], 0.5f));
    youhost::takeFoldedChannel(foldedStereo, identical, 2, 4, youhost::StereoFold::sum);
    CHECK(youhost::sameFloatBits(foldedStereo[0], 1.0f));
    const float* half[2] = { signal, silent };
    youhost::takeFoldedChannel(foldedStereo, half, 2, 4, youhost::StereoFold::left);
    CHECK(youhost::sameFloatBits(foldedStereo[0], 1.0f));
    youhost::takeFoldedChannel(foldedStereo, half, 2, 4, youhost::StereoFold::sum);
    CHECK(youhost::sameFloatBits(foldedStereo[0], 0.5f));
    const float* opposite[2] = { signal, inverted };
    youhost::takeFoldedChannel(foldedStereo, opposite, 2, 4, youhost::StereoFold::left);
    CHECK(youhost::sameFloatBits(foldedStereo[0], 1.0f));
    youhost::takeFoldedChannel(foldedStereo, opposite, 2, 4, youhost::StereoFold::sum);
    CHECK(youhost::sameFloatBits(foldedStereo[0], 0.0f));

    youhost::OpenedLayout opened;
    opened.openInputs = 2;
    opened.openOutputs = 2;
    opened.mono = true;
    opened.side = true;
    opened.stereo = true;
    CHECK(youhost::chooseOpenedLayout(opened) == youhost::ChosenLayout::stereo);
    opened.openInputs = 1;
    opened.openOutputs = 1;
    CHECK(youhost::chooseOpenedLayout(opened) == youhost::ChosenLayout::mono);
    opened.openInputs = 0;
    opened.openOutputs = 0;
    opened.mono = false;
    CHECK(youhost::chooseOpenedLayout(opened) == youhost::ChosenLayout::stereo);

    CHECK(youhost::pluginBlockFeedsDirect(128, 128));
    CHECK(youhost::pluginBlockFeedsDirect(256, 128));
    CHECK(! youhost::pluginBlockFeedsDirect(64, 128));
    CHECK(youhost::fedSamplesAfter(128, 64, 1) == 0);
    CHECK(youhost::fedSamplesAfter(128, 64, 2) == 64);
    CHECK(youhost::fedSamplesAfter(128, 64, 4) == 192);

    float ring[4] = {};
    int ringWrite = 0;
    int count = 0;
    const float pushed[3] = { 1.0f, 2.0f, 3.0f };
    youhost::ringPush(ring, 4, ringWrite, count, pushed, 3);
    float popped[3] = {};
    int read = 0;
    CHECK(youhost::ringPop(ring, 4, read, count, popped, 2) == 2);
    CHECK(youhost::sameFloatBits(popped[0], 1.0f));
    CHECK(youhost::sameFloatBits(popped[1], 2.0f));
    CHECK(count == 1);
}

void testOutputGainAndListen()
{
    CHECK(youhost::snapOutputDb(0.2f) == 0.0f);
    CHECK(youhost::snapOutputDb(0.3f) == 0.5f);
    CHECK(youhost::snapOutputDb(-20.0f) == -9.0f);
    CHECK(youhost::snapOutputDb(20.0f) == 9.0f);
    CHECK(youhost::outputDbIsUnity(0.0f));
    CHECK(near(youhost::outputDbToLinear(6.0f), 1.995262f, 0.001f));
    CHECK(youhost::channelListenFromName("input") == youhost::ChannelListen::input);
    CHECK(youhost::channelListenFromName("off") == youhost::ChannelListen::off);
    CHECK(youhost::channelListenFromName("rec") == youhost::ChannelListen::record);
    CHECK(std::string(youhost::channelListenLabel(youhost::ChannelListen::input)) == "INPUT");
    CHECK(youhost::cycleChannelListen(youhost::ChannelListen::record) == youhost::ChannelListen::input);
    CHECK(youhost::cycleChannelListen(youhost::ChannelListen::input) == youhost::ChannelListen::off);
    CHECK(youhost::cycleChannelListen(youhost::ChannelListen::off) == youhost::ChannelListen::record);
    CHECK(youhost::channelListenAudible(youhost::ChannelListen::input));
    CHECK(! youhost::channelListenRecords(youhost::ChannelListen::input));
    CHECK(youhost::channelListenRecords(youhost::ChannelListen::record));
}

void testWaveformAndAnchor()
{
    CHECK(youhost::waveformDisplayLevel(1.0f, 1.0f) == 1.0f);
    CHECK(youhost::waveformDisplayLevel(0.0f, 1.0f) == 0.0f);
    CHECK(youhost::waveformDisplayLevel(0.1f, 1.0f) > 0.5f);
    CHECK(youhost::stepWaveformGain(1.0f, 1) > 1.0f);
    CHECK(youhost::stepWaveformGain(1.0f, -1) < 1.0f);
    CHECK(youhost::anchorPlayhead(10000, 4000, 0) == 0);
    CHECK(youhost::anchorPlayhead(10000, 4000, 3000) == 0);
    CHECK(youhost::anchorPlayhead(10000, 4000, 4000) == 1000);
    CHECK(youhost::anchorPlayhead(4000, 4000, 2000) == 0);
}

void testPluginMovesDoNotReload()
{
    youhost::SlotMoveState slots[4] {};
    for (int index = 0; index < 4; ++index)
        slots[index].instance = index + 1;

    CHECK(youhost::moveSlotInstance(slots[0], slots[1]));
    CHECK(youhost::moveSlotInstance(slots[1], slots[2]));
    CHECK(youhost::moveSlotInstance(slots[2], slots[3]));

    bool seen[5] = {};
    for (int index = 0; index < 4; ++index)
    {
        CHECK(slots[index].instance >= 1 && slots[index].instance <= 4);
        CHECK(! seen[slots[index].instance]);
        seen[slots[index].instance] = true;
        CHECK(! slots[index].loading);
    }

    const int parked = slots[2].instance;
    slots[3].loading = true;
    CHECK(! youhost::slotDragAccepted(slots[2], slots[3], false));
    CHECK(! youhost::moveSlotInstance(slots[2], slots[3]));
    CHECK(slots[2].instance == parked);
    slots[3].loading = false;

    CHECK(! youhost::slotDragAccepted(slots[0], slots[1], true));

    int next = 4;
    const int source = slots[0].instance;
    const auto copied = youhost::copySlotInstance(slots[0], slots[1], next);
    CHECK(copied.ok);
    CHECK(copied.created == 5);
    CHECK(copied.created != source);
    CHECK(slots[0].instance == source);
    CHECK(slots[1].instance == 5);
    CHECK(slots[1].loading);
    slots[1].loading = false;

    youhost::ChannelOpQueue queue;
    CHECK(youhost::beginQueuedLoad(queue));
    CHECK(! youhost::beginQueuedLoad(queue));
    youhost::finishQueuedLoad(queue);
    CHECK(youhost::beginQueuedLoad(queue));
    CHECK(queue.started == 2);
    CHECK(queue.finished == 1);

    int hops = youhost::pluginGraveHops();
    int aliveTurns = 0;
    bool editorDropped = false;
    bool instanceDropped = false;
    for (int guard = 0; guard < 12 && ! instanceDropped; ++guard)
    {
        const auto step = youhost::graveStep(hops);
        if (step == youhost::GraveStep::keep)
        {
            CHECK(! editorDropped);
            ++aliveTurns;
            hops = youhost::graveNextHops(hops);
        }
        else if (step == youhost::GraveStep::dropEditor)
        {
            CHECK(! instanceDropped);
            editorDropped = true;
            hops = 0;
        }
        else
        {
            CHECK(editorDropped);
            instanceDropped = true;
        }
    }
    CHECK(aliveTurns >= 3);
    CHECK(instanceDropped);
}

void testRaiseUnit()
{
    CHECK(near(youhost::raiseUnit(0.5f, 4), 0.0625f, 0.00001f));
    CHECK(youhost::raiseUnit(0.5f, 0) == 1.0f);
    CHECK(youhost::raiseUnit(0.0f, 3) == 0.0f);
}

} // namespace

int main()
{
    testDryPathIsBitIdentical();
    testOutputGainAndListen();
    testWaveformAndAnchor();
    testRaiseUnit();
    testPluginMovesDoNotReload();
    testPassthroughCopiesMatchingChannels();
    testMetersSettleClipAndClear();
    testClipClearWithoutAnOpenInput();
    testMeterScales();
    testDropoutDecisions();
    testLatencyCompensation();
    testPlaybackCopiesDryChannels();
    testDryChannelDelayMatchesPluginChannel();
    testScanOrderSkipsWavesAndHidesBuiltIns();
    testTakeImportGroups();
    testDropoutWindow();
    testSessionLayout();
    testTimelineZoom();
    testInsertMenuAndStall();
    testCrashJournal();
    testDeviceWatch();
    testMergePeaks();
    testUnwrittenOutputsAreCleared();
    testLatencyFormulas();
    testTakePlan();
    testMeterLayoutScales();
    testOffChannelStaysSilent();
    testChannelPick();
    testSessionNames();
    testPluginLoadPace();
    testShortcutsMatchTheHelp();
    testGroupsFoldAndPalette();

    if (failures != 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "YouHost engine tests passed\n";
    return EXIT_SUCCESS;
}
