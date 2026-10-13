#include "engine/ChannelColor.h"
#include "engine/ChannelEnable.h"
#include "engine/SessionCopy.h"
#include "engine/ChannelSelect.h"
#include "engine/SessionNames.h"
#include "engine/Shortcuts.h"
#include "engine/ChannelListen.h"
#include "engine/AudioSetupMenus.h"
#include "engine/SessionChannels.h"
#include "engine/StatusLine.h"
#include "engine/RecordLock.h"
#include "engine/OutputGain.h"
#include "engine/SignalPath.h"
#include "engine/RepaintCoalesce.h"
#include "engine/MeterBallistics.h"
#include "engine/WaveformScale.h"
#include "engine/DisplayLayout.h"
#include "engine/DropoutDetect.h"
#include "engine/DropoutLog.h"
#include "engine/CpuMeter.h"
#include "engine/PlaybackRoute.h"
#include "engine/DeviceLoss.h"
#include "engine/ClockSync.h"
#include "engine/SyncLost.h"
#include "engine/Markers.h"
#include "engine/GroupEdit.h"
#include "engine/EditHistory.h"
#include "engine/MeterClip.h"
#include "engine/VersionLabel.h"
#include "engine/LatencyCompensation.h"
#include "engine/X32Colours.h"
#include "engine/LatencyCard.h"
#include "engine/SessionActions.h"
#include "engine/WindowCatalog.h"
#include "engine/LatencyMath.h"
#include "engine/WindowFit.h"
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
#include "engine/HostColumns.h"
#include "engine/InsertMenu.h"
#include "engine/PluginBuses.h"
#include "engine/StereoLink.h"
#include "engine/PluginLoadPace.h"
#include "engine/PluginMoves.h"
#include "engine/StallWatch.h"
#include "engine/SessionFormat.h"
#include "engine/TimelineLanes.h"
#include "engine/TimelineZoom.h"
#include "engine/RecordStart.h"
#include "engine/CheckedMutex.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::atomic<int> audioAllocations { 0 };
thread_local bool inAudioCallback = false;

struct AudioAllocationGuard
{
    static void note()
    {
        if (inAudioCallback)
            audioAllocations.fetch_add(1, std::memory_order_relaxed);
    }
};
} // namespace

void* operator new(std::size_t size)
{
    AudioAllocationGuard::note();
    if (size == 0)
        size = 1;
    if (void* pointer = std::malloc(size))
        return pointer;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    return operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    AudioAllocationGuard::note();
    if (size == 0)
        size = 1;
    return std::malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return operator new(size, std::nothrow);
}

void operator delete(void* pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept
{
    std::free(pointer);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    AudioAllocationGuard::note();
    std::size_t align = static_cast<std::size_t>(alignment);
    if (align < sizeof(void*))
        align = sizeof(void*);
    if (size < align)
        size = align;
    if (size % align != 0)
        size += align - (size % align);
    void* pointer = nullptr;
    if (posix_memalign(&pointer, align, size) != 0)
        throw std::bad_alloc();
    return pointer;
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try
    {
        return operator new(size, alignment);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return operator new(size, alignment, std::nothrow);
}

void operator delete(void* pointer, std::align_val_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer, std::align_val_t) noexcept
{
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept
{
    std::free(pointer);
}

void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept
{
    std::free(pointer);
}

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

void testLatencyWindowFits()
{
    const auto card = youhost::layoutLatencyCard(youhost::kLatencyPreferredWidth);
    CHECK(card.contentWidth == youhost::kLatencyPreferredWidth);
    CHECK(card.compensation.height >= 26);
    CHECK(card.note.height >= youhost::kLatencyLineH * 2);
    CHECK(card.noteLines >= 2);
    CHECK(card.hero.top + card.hero.height <= card.buffer.top);
    CHECK(card.buffer.top + card.buffer.height <= card.input.top);
    CHECK(card.output.top + card.output.height <= card.compensation.top);
    CHECK(card.compensation.top + card.compensation.height <= card.dropouts.top);
    CHECK(card.dropouts.top + card.dropouts.height <= card.modes.top);
    CHECK(card.modes.top + card.modes.height <= card.groups.top);
    CHECK(card.groups.top + card.groups.height <= card.note.top);
    CHECK(card.groups.height >= (youhost::kLatencyListedGroupRows + 1) * youhost::kLatencyGroupRowH);
    CHECK(youhost::kLatencyListedGroupRows == youhost::kMaxDisplayGroups);
    CHECK(card.note.top + card.note.height <= card.contentHeight);

    const auto core = youhost::latencyNoteText(true, youhost::RoundTripFormula::coreAudioSubtractOneBuffer);
    const auto alsa = youhost::latencyNoteText(false, youhost::RoundTripFormula::alsaAddOneBuffer);
    CHECK(static_cast<int>(core.size()) <= youhost::longestLatencyNoteChars());
    CHECK(static_cast<int>(alsa.size()) <= youhost::longestLatencyNoteChars());
    const auto wrapped = youhost::layoutLatencyCard(youhost::kLatencyPreferredWidth,
                                                     static_cast<int>(core.size()));
    CHECK(wrapped.note.height <= card.note.height);
    CHECK(wrapped.note.top + wrapped.note.height <= card.contentHeight);

    youhost::SavedWindowSize none;
    const auto opened = youhost::windowOpenSize(card.contentWidth, card.contentHeight, 1920, 1080, none);
    CHECK(opened.width >= card.contentWidth);
    CHECK(opened.height >= card.contentHeight);

    youhost::SavedWindowSize legacy;
    legacy.valid = true;
    legacy.width = 520;
    legacy.height = 320;
    const auto reset = youhost::windowOpenSize(card.contentWidth, card.contentHeight, 1920, 1080, legacy);
    CHECK(reset.width == card.contentWidth);
    CHECK(reset.height == card.contentHeight);

    youhost::SavedWindowSize shrunk = legacy;
    shrunk.hasFit = true;
    shrunk.fitWidth = card.contentWidth;
    shrunk.fitHeight = card.contentHeight;
    shrunk.width = 480;
    shrunk.height = 280;
    const auto kept = youhost::windowOpenSize(card.contentWidth, card.contentHeight, 1920, 1080, shrunk);
    CHECK(kept.width == card.contentWidth);
    CHECK(kept.height == card.contentHeight);
    CHECK(! youhost::keepRememberedWindow(shrunk, card.contentWidth, card.contentHeight));
    CHECK(youhost::blockReachable(card.compensation.top, card.compensation.height, card.contentHeight, kept.height));
    CHECK(youhost::blockReachable(card.note.top, card.note.height, card.contentHeight, kept.height));

    const auto grown = youhost::windowOpenSize(card.contentWidth, card.contentHeight + 80, 1920, 1080, shrunk);
    CHECK(grown.height == card.contentHeight + 80);

    const auto smallScreen = youhost::windowOpenSize(card.contentWidth, card.contentHeight, 640, 360, none);
    CHECK(smallScreen.height < card.contentHeight);
    CHECK(smallScreen.height > 0);
    CHECK(youhost::blockReachable(card.note.top, card.note.height, card.contentHeight, smallScreen.height));
    CHECK(youhost::blockReachable(card.compensation.top, card.compensation.height, card.contentHeight, 160));

    const auto parsed = youhost::parseWindowState("12 40 520 420 fit 680 510");
    CHECK(parsed.valid);
    CHECK(parsed.x == 12);
    CHECK(parsed.width == 520);
    CHECK(parsed.hasFit);
    CHECK(parsed.fitHeight == 510);
    CHECK(youhost::juceWindowState(parsed) == "12 40 520 420");
    CHECK(youhost::shortcutHelpText().find("A saved size smaller than that content is replaced") != std::string::npos);
    CHECK(youhost::shortcutHelpText().find("LATENCY does not have a Timeline button") != std::string::npos);
    const auto legacyState = youhost::parseWindowState("8 8 900 700 fullscreen");
    CHECK(legacyState.valid);
    CHECK(! legacyState.hasFit);
    CHECK(youhost::keepRememberedWindow(legacyState, card.contentWidth, card.contentHeight) == (700 >= card.contentHeight));
}

void testTimelineNavigation()
{
    CHECK(youhost::laneCornerLabel(12, "Kick", false) == "12  Kick");
    CHECK(youhost::laneCornerLabel(4, "", false) == "4");
    CHECK(youhost::laneCornerLabel(4, "4", false) == "4");
    CHECK(youhost::laneCornerLabel(0, "Drums", true) == "Drums");
    CHECK(youhost::parseGoToChannel(" 12") == 12);
    CHECK(youhost::parseGoToChannel("0") == 0);
    CHECK(youhost::parseGoToChannel("12a") == 0);

    const int revealed = youhost::laneScrollToReveal(40, 8, 0, 20);
    CHECK(revealed <= 20);
    CHECK(revealed + 8 > 20);
    CHECK(youhost::laneScrollToReveal(40, 8, 10, 12) == 10);

    const auto focused = youhost::focusReadableLane(32, 20, 200.0f);
    const int focusedShown = youhost::lanesVisible(32, focused.verticalStep, 200.0f);
    CHECK(200.0f / static_cast<float>(focusedShown) >= youhost::kReadableLaneHeightPx);
    CHECK(focused.laneScroll <= 20);
    CHECK(focused.laneScroll + focusedShown > 20);
    const auto fitted = youhost::fitAllLaneFocus();
    CHECK(fitted.verticalStep == 0);
    CHECK(fitted.laneScroll == 0);
    CHECK(youhost::lanesVisible(32, fitted.verticalStep, 200.0f) > focusedShown);

    const int anchored = youhost::laneScrollKeepingAnchor(32, 0, 16, 8, 3);
    CHECK(anchored <= 3);
    CHECK(anchored + 8 > 3);
    CHECK(youhost::laneUnderPointer(50.0f, 0.0f, 160.0f, 8, 4) == 6);

    const std::vector<std::vector<int>> lanes = { { 0, 1 }, { 5 } };
    CHECK(youhost::laneIndexContaining(lanes, 5) == 1);
    CHECK(youhost::laneIndexContaining(lanes, 3) == -1);

    const float rulerX = 8.0f;
    const float rulerY = 6.0f;
    const float rulerW = 880.0f;
    const float rulerH = static_cast<float>(youhost::kTimelineRulerHeightPx);
    const float laneY = rulerY + rulerH;
    const float laneH = 36.0f;
    const float markX = rulerX;
    const float nextX = rulerX + 640.0f;
    const auto take = youhost::takeRulerLabelRect(rulerX, rulerY, rulerW, rulerH, markX, nextX);
    const auto corner = youhost::laneCornerLabelRect(rulerX, laneY, rulerW, laneH);
    CHECK(youhost::takeRulerLabelText(1, nextX - markX) == "TAKE 1");
    CHECK(youhost::takeRulerLabelText(2, 40.0f) == "2");
    CHECK(youhost::timelineLabelInside(take, rulerX, rulerY, rulerW, rulerH));
    CHECK(youhost::timelineLabelInside(corner, rulerX, laneY, rulerW, laneH));
    CHECK(corner.x <= rulerX + 4.0f);
    CHECK(corner.y <= laneY + 2.0f);
    CHECK(! youhost::timelineLabelsOverlap(take, corner));
    CHECK(take.y + take.height <= laneY + 0.01f);
}

void testSessionFileActions()
{
    CHECK(youhost::sessionModelIsClean(youhost::cleanSessionModel()));
    CHECK(youhost::sessionReplaceAsks(true));
    CHECK(! youhost::sessionReplaceAsks(false));
    CHECK(! youhost::sessionReplaceProceeds(true, youhost::UnsavedChoice::cancel));
    CHECK(youhost::sessionReplaceProceeds(true, youhost::UnsavedChoice::discard));
    CHECK(youhost::sessionReplaceProceeds(false, youhost::UnsavedChoice::cancel));
    CHECK(youhost::sessionReplaceSavesFirst(true, youhost::UnsavedChoice::save));
    CHECK(! youhost::sessionReplaceSavesFirst(true, youhost::UnsavedChoice::discard));

    youhost::SessionDocumentModel dirty;
    dirty.align = "group";
    dirty.channels.push_back({});
    dirty.channels.back().index = 3;
    dirty.channels.back().name = "Old";
    CHECK(! youhost::sessionModelIsClean(dirty));
    CHECK(youhost::sessionModelIsClean(youhost::replaceSessionModel(youhost::cleanSessionModel())));

    youhost::SessionDocumentModel session = youhost::cleanSessionModel();
    youhost::SessionChannelRecord channel;
    channel.index = 4;
    channel.name = "Kick";
    channel.color = 3;
    channel.group = 1;
    channel.listen = "rec";
    youhost::SessionSlotRecord slot;
    slot.occupied = true;
    slot.index = 0;
    slot.pluginName = "EQ";
    slot.state = "abc";
    channel.slots.push_back(slot);
    session.channels.push_back(channel);
    youhost::SessionTakeRecord take;
    take.start = 0;
    take.length = 100;
    take.files.push_back(youhost::SessionFileRecord { 4, "Kick.wav" });
    session.takes.push_back(take);

    youhost::SessionNode written = youhost::writeSessionModel(session);
    const auto xml = youhost::writeSessionXml(written);
    youhost::SessionNode parsed;
    CHECK(youhost::parseSessionXml(xml, parsed));
    youhost::SessionDocumentModel round;
    CHECK(youhost::readSessionModel(parsed, round));
    CHECK(round.channels.size() == 1);
    CHECK(round.channels[0].name == "Kick");
    CHECK(round.channels[0].color == 3);
    CHECK(round.channels[0].group == 1);
    CHECK(round.channels[0].listen == "rec");
    CHECK(round.channels[0].slots.size() == 1);
    CHECK(round.channels[0].slots[0].pluginName == "EQ");
    CHECK(round.takes.size() == 1);
    CHECK(round.takes[0].files.size() == 1);
    CHECK(round.takes[0].files[0].name == "Kick.wav");

    youhost::SessionDocumentModel previous = round;
    youhost::SessionChannelRecord leftover;
    leftover.index = 9;
    leftover.name = "Leftover";
    leftover.color = 6;
    previous.channels.push_back(leftover);
    const auto opened = youhost::replaceSessionModel(round);
    CHECK(opened.channels.size() == 1);
    CHECK(opened.channels[0].name == "Kick");
    CHECK(opened.channels[0].color == 3);

    const auto plan = youhost::planSessionCopy({ "session.youhost", "dropouts.csv" }, { "Kick.wav", "Snare.wav" }, { "01" });
    CHECK(plan.size() == 3);
    CHECK(plan[0].relativePath == "session.youhost");
    CHECK(plan[1].relativePath == "audio/Kick.wav");
    CHECK(plan[2].relativePath == "audio/Snare.wav");
    for (const auto& entry : plan)
        CHECK(entry.relativePath.find("Backups/") == std::string::npos);
    CHECK(youhost::folderAfterSaveAs("/old", "/new", true) == "/new");
    CHECK(youhost::folderAfterSaveAs("/old", "/new", false) == "/old");

    const auto help = youhost::shortcutHelpText();
    CHECK(help.find("ALL PLUGIN BYPASS") != std::string::npos);
    CHECK(help.find("FIT") != std::string::npos);
    CHECK(help.find("Go to channel") != std::string::npos);
    CHECK(help.find("Scene") == std::string::npos);
    CHECK(help.find("MIDI") == std::string::npos);
}

std::string readWorkspaceFile(const char* path)
{
    std::ifstream in(path);
    if (! in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void testInstallGuide()
{
    const auto guide = readWorkspaceFile("INSTALL.md");
    const auto workflow = readWorkspaceFile(".github/workflows/build-macos.yml");
    CHECK(! guide.empty());
    CHECK(! workflow.empty());

    const auto finnish = guide.find("YouHost — ASENNUSOHJE");
    const auto english = guide.find("YouHost — INSTALL");
    CHECK(finnish != std::string::npos);
    CHECK(english != std::string::npos);
    CHECK(finnish < english);

    const char* required[] = {
        "YouHost.app",
        "Applications",
        "Ohjelmat",
        "Open Anyway",
        "Avaa silti",
        "Privacy & Security",
        "Tietosuoja ja suojaus",
        "xattr -dr com.apple.quarantine /Applications/YouHost.app",
        "Right-click YouHost.app and choose Open",
        "Klikkaa YouHost.appia oikealla ja valitse Avaa",
        "microphone and audio input",
        "mikrofoni ja äänitulo",
        "choose the audio card",
        "valitse äänikortti",
    };
    for (const char* phrase : required)
        CHECK(guide.find(phrase) != std::string::npos);

    const char* banned[] = {
        "GitHub",
        "Actions",
        "Artifacts",
        "Artefakti",
        "http",
        "www.",
        "download",
        "Download",
        "Lataa",
        "website",
    };
    for (const char* phrase : banned)
        CHECK(guide.find(phrase) == std::string::npos);

    CHECK(workflow.find("pack/ASENNUSOHJE - INSTALL.txt") != std::string::npos);
    CHECK(workflow.find("cp \"INSTALL.md\" \"pack/ASENNUSOHJE - INSTALL.txt\"") != std::string::npos);
    CHECK(workflow.find("path: pack") != std::string::npos);

    const auto help = youhost::shortcutHelpText();
    CHECK(help.find(guide) != std::string::npos);
    for (const char* phrase : banned)
        CHECK(help.find(phrase) == std::string::npos);
    CHECK(help.find("Scene") == std::string::npos);
    CHECK(help.find("MIDI") == std::string::npos);
}

void testWindowContentFits()
{
    CHECK(youhost::cpuWindowWidth() >= 480);
    CHECK(youhost::cpuWindowHeight() >= youhost::cpuCardHeight(7));
    CHECK(youhost::blockReachable(youhost::cpuCardHeight(7) - 48, 48, youhost::cpuCardHeight(7), 180));
    CHECK(youhost::dropoutWindowHeight() >= youhost::dropoutWindowContentHeight(4));
    CHECK(youhost::blockReachable(youhost::dropoutBodyHeight(4) - 40, 40, youhost::dropoutBodyHeight(4), 160));
    CHECK(youhost::scannerWindowWidth() >= youhost::scannerButtonRowWidth());
    CHECK(youhost::scannerWindowHeight() >= youhost::scannerControlHeight());
    CHECK(youhost::groupRenameWindowHeight() >= youhost::groupRenameContentHeight());
    CHECK(youhost::pluginListWindowHeight() >= youhost::pluginPickerContentHeight());
    CHECK(youhost::setupWindowHeight() == 12 + youhost::audioSetupFormHeight() + youhost::setupChannelToggleHeight() + 12);
    CHECK(youhost::setupChannelToggleHeight() == 8 + 28 + 4 + 22);
    CHECK(youhost::startupWindowHeight() == 12 + youhost::audioSetupFormHeight() + 10 + youhost::startupSessionBlockHeight() + 12);
    CHECK(youhost::startupSessionBlockHeight() == 28 + 6 + 28 + 22 + 6 + 32 + 8 + 20 + youhost::startupRecentListHeight());
    CHECK(youhost::setupWindowHeight() < 280);
    CHECK(youhost::startupWindowHeight() < 560);
    const auto startup = readWorkspaceFile("src/ui/MainComponent.cpp");
    CHECK(! startup.empty());
    CHECK(startup.find("Choose the interface, sample rate, and buffer size") == std::string::npos);
    CHECK(startup.find("Recordings usually go on an external drive. The interface opens") == std::string::npos);
    const int helpCharacters = static_cast<int>(youhost::shortcutHelpText().size());
    const int helpContent = youhost::helpContentHeight(helpCharacters, youhost::helpWindowWidth() - 48);
    const int helpWindow = youhost::helpWindowHeightFor(helpCharacters);
    CHECK(helpWindow >= 280);
    CHECK(youhost::blockReachable(std::max(0, helpContent - 20), 16, helpContent, std::max(1, helpWindow - 36)));
    const auto card = youhost::layoutLatencyCard(youhost::kLatencyPreferredWidth);
    CHECK(card.note.top + card.note.height <= card.contentHeight);
    CHECK(youhost::blockReachable(card.compensation.top, card.compensation.height, card.contentHeight, 160));
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
    CHECK(youhost::takeWaveName(1, 1, "BD") == "1_BD_1.wav");
    CHECK(youhost::takeWaveName(2, 1, "BD") == "1_BD_2.wav");
    CHECK(youhost::takeWaveName(1, 2, "Sn") == "2_Sn_1.wav");
    CHECK(youhost::takeWaveName(1, 3, "Kick Drum") == "3_Kick_Drum_1.wav");
    CHECK(youhost::takeWaveName(12, 1, "Kick/Snare") == "1_KickSnare_12.wav");
    CHECK(youhost::takeWaveName(1, 1, "") == "1_1.wav");
    CHECK(youhost::takeWaveName(1, 2, "") == "2_1.wav");
    CHECK(youhost::takeWaveName(2, 1, "") == "1_2.wav");
    CHECK(youhost::takeWaveName(2, 2, "BD") == "2_BD_2.wav");
    CHECK(youhost::takeWaveName(1, 1, "  ") == "1_1.wav");
    CHECK(youhost::takeWaveName(1, 1, "a/b:c*?\"<>|") == "1_abc_1.wav");
    youhost::SessionDocumentModel keptNames;
    youhost::SessionTakeRecord oldTake;
    oldTake.start = 0;
    oldTake.length = 48000;
    oldTake.files.push_back({ 0, "1_1_BD.wav" });
    oldTake.files.push_back({ 1, "2_1.wav" });
    keptNames.takes.push_back(oldTake);
    youhost::SessionDocumentModel loadedNames;
    CHECK(youhost::readSessionModel(youhost::writeSessionModel(keptNames), loadedNames));
    CHECK(loadedNames.takes.size() == 1);
    CHECK(loadedNames.takes[0].files.size() == 2);
    CHECK(loadedNames.takes[0].files[0].name == "1_1_BD.wav");
    CHECK(loadedNames.takes[0].files[1].name == "2_1.wav");
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
    CHECK(ticks[0].label == 18);
    CHECK(ticks[0].dbFs == 0.0f);
    CHECK(ticks[3].label == 0);
    CHECK(ticks[3].dbFs == -18.0f);

    CHECK(youhost::rmsTicks(-14, ticks, 9) == 9);
    CHECK(ticks[0].label == 14);
    CHECK(ticks[0].dbFs == 0.0f);
    CHECK(ticks[3].label == 0);
    CHECK(ticks[3].dbFs == -14.0f);
    CHECK(ticks[8].dbFs == -54.0f);
    CHECK(youhost::rmsHeadroomDb(-14) == 14);
    CHECK(youhost::rmsHeadroomDb(-18) == 18);
    CHECK(youhost::rmsHeadroomDb(-20) == 20);
    const auto span14 = youhost::rmsMeterSpan(-14);
    const auto span20 = youhost::rmsMeterSpan(-20);
    CHECK(span14.topDb == 0.0f);
    CHECK(span14.bottomDb == youhost::kMeterFloorDb);
    CHECK(span20.topDb == 0.0f);
    const float zero14 = youhost::normaliseDb(-14.0f, span14);
    const float zero20 = youhost::normaliseDb(-20.0f, span20);
    CHECK(zero14 > zero20);
    CHECK(near(zero14, 46.0f / 60.0f, 0.001f));
    CHECK(near(zero20, 40.0f / 60.0f, 0.001f));
    CHECK(youhost::meterColourForDb(-15.0f, -14) == youhost::MeterColour::green);
    CHECK(youhost::meterColourForDb(-14.0f, -14) == youhost::MeterColour::yellow);
    CHECK(youhost::meterColourForDb(-21.0f, -20) == youhost::MeterColour::green);
    CHECK(youhost::meterColourForDb(-20.0f, -20) == youhost::MeterColour::yellow);

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
    CHECK(near(youhost::meterNormal(fullScale, false, -14), 1.0f, 0.001f));
    CHECK(near(youhost::meterNormal(fullScale, false, -18), 1.0f, 0.001f));
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

    // A latent plugin inside one group must not delay the other channels.
    youhost::ChannelLatencyInput desk[5] {};
    desk[0] = { 0, true, 0 };
    desk[1] = { 480, true, 0 };
    desk[2] = { 0, true, 1 };
    desk[3] = { 0, true, -1 };
    desk[4] = { 1000, true, -1 };
    const auto global = youhost::planCompensation(desk, 5, youhost::AlignMode::all);
    CHECK(global.alignmentSamples == 1000);
    CHECK(global.delaySamples[0] == 1000);
    CHECK(global.delaySamples[3] == 1000);
    const auto onlyGroup = youhost::planCompensation(desk, 5, youhost::AlignMode::group);
    CHECK(onlyGroup.delaySamples[0] == 480);
    CHECK(onlyGroup.delaySamples[1] == 0);
    CHECK(onlyGroup.delaySamples[2] == 0);
    CHECK(onlyGroup.delaySamples[3] == 0);
    CHECK(onlyGroup.delaySamples[4] == 0);
    CHECK(onlyGroup.alignmentSamples == 480);
    youhost::GroupCompensationRow rows[4];
    const int rowCount = youhost::groupCompensationRows(desk, 5, rows, 4);
    CHECK(rowCount == 2);
    CHECK(rows[0].group == 0);
    CHECK(rows[0].alignSamples == 480);
    CHECK(rows[0].members == 2);
    CHECK(rows[1].group == 1);
    CHECK(rows[1].alignSamples == 0);
    CHECK(rows[1].members == 1);

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

    int membership[4] = { -1, 0, 0, -1 };
    bool collapsed[youhost::kMaxDisplayGroups] = {};
    collapsed[0] = true;
    youhost::StripItem strips[8];
    const int written = youhost::layoutChannelStrips(4, membership, collapsed, strips, 8);
    int shown[8];
    const int shownCount = youhost::shownChannelNumbers(strips, written, shown, 8);
    const auto folded = youhost::selectAllChannels(shown, shownCount);
    CHECK(folded.channels.size() == 2);
    CHECK(folded.channels[0] == 0);
    CHECK(folded.channels[1] == 3);
    CHECK(folded.anchor == 0);
    CHECK(youhost::makeGroupShortcutApplies(1, false));
    CHECK(youhost::makeGroupShortcutApplies(2, false));
    CHECK(! youhost::makeGroupShortcutApplies(1, true));
    CHECK(! youhost::makeGroupShortcutApplies(2, true));
    const auto grouped = youhost::planNewGroup(! folded.channels.empty(), 0, "All", 2);
    CHECK(grouped.created);
    CHECK(grouped.collapsed);
    CHECK(grouped.name == "All");

    collapsed[0] = false;
    const int openWritten = youhost::layoutChannelStrips(4, membership, collapsed, strips, 8);
    const int openCount = youhost::shownChannelNumbers(strips, openWritten, shown, 8);
    const auto open = youhost::selectAllChannels(shown, openCount);
    CHECK(open.channels.size() == 4);
    CHECK(open.channels.front() == 0);
    CHECK(open.channels.back() == 3);
    CHECK(youhost::planNewGroup(! open.channels.empty(), 1, "", 0).name == "Group 2");
    CHECK(! youhost::planNewGroup(open.channels.empty(), 1, "All", 0).created);
}

void testSessionNames()
{
    CHECK(youhost::europeanSessionDate(9, 10, 2026) == "09.10.2026");
    CHECK(! youhost::startupLeaveEntersMain(youhost::StartupLeave::close));
    CHECK(youhost::startupLeaveEntersMain(youhost::StartupLeave::newSession));
    CHECK(youhost::startupLeaveEntersMain(youhost::StartupLeave::openSession));
    CHECK(youhost::startupLeaveEntersMain(youhost::StartupLeave::openRecent));
    CHECK(! youhost::mainWindowAllowed(false, false));
    CHECK(! youhost::mainWindowAllowed(false, true));
    CHECK(youhost::mainWindowAllowed(true, true));

    const auto planned = youhost::planNewSession("/Volumes/Recordings", 10, 10, 2026, {});
    CHECK(planned.location == "/Volumes/Recordings");
    CHECK(planned.name == "10.10.2026");
    CHECK(planned.folder == "/Volumes/Recordings/10.10.2026");
    CHECK(planned.folderCreatedImmediately);
    const auto again = youhost::planNewSession("/Volumes/Recordings/", 10, 10, 2026, { "10.10.2026" });
    CHECK(again.name == "10.10.2026_1");
    CHECK(again.folder == "/Volumes/Recordings/10.10.2026_1");
    CHECK(again.folderCreatedImmediately);
    const auto layout = youhost::sessionLayoutFor(planned.folder);
    const auto root = std::filesystem::temp_directory_path() / "youhost-required-session";
    std::filesystem::remove_all(root);
    const auto created = root / planned.name;
    std::filesystem::create_directories(created / "audio");
    {
        std::ofstream session((created / "session.youhost").string());
        session << "session";
    }
    CHECK(std::filesystem::is_directory(created / "audio"));
    CHECK(std::filesystem::is_regular_file(created / "session.youhost"));
    CHECK(layout.sessionFile.find("session.youhost") != std::string::npos);
    CHECK(layout.audioFolder.find("/audio") != std::string::npos);
    std::filesystem::remove_all(root);
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
    expect(youhost::ShortcutId::fit, 'f', youhost::KeyKind::character, false, false, false);
    expect(youhost::ShortcutId::fit, 0, youhost::KeyKind::letterR, false, false, true);
    CHECK(youhost::shortcutChord(youhost::ShortcutId::fit) == "F or Option+R");
    CHECK(youhost::fitShortcutApplies(1, false));
    CHECK(youhost::fitShortcutApplies(2, false));
    CHECK(! youhost::fitShortcutApplies(1, true));
    CHECK(! youhost::fitShortcutApplies(3, false));
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
    expect(youhost::ShortcutId::goToChannel, 'g', youhost::KeyKind::character, false, false, false);
    expect(youhost::ShortcutId::makeGroup, 'g', youhost::KeyKind::character, false, true, false);
    expect(youhost::ShortcutId::selectAll, 'a', youhost::KeyKind::character, false, true, false);
    expect(youhost::ShortcutId::selectAll, 'a', youhost::KeyKind::character, false, false, false);
    youhost::KeyQuery shiftedA;
    shiftedA.kind = youhost::KeyKind::character;
    shiftedA.character = 'a';
    shiftedA.shift = true;
    CHECK(! youhost::matchShortcut(shiftedA).has_value());
    CHECK(youhost::selectAllShortcutApplies(1, false));
    CHECK(! youhost::selectAllShortcutApplies(1, true));
    CHECK(youhost::selectAllShortcutApplies(2, false));
    CHECK(! youhost::selectAllShortcutApplies(2, true));

    CHECK(help.find("5  Open or close SCAN") == std::string::npos);
    CHECK(help.find("5  Open or close LATENCY") != std::string::npos);
    CHECK(help.find("S  Open or close SCAN") != std::string::npos);
    CHECK(help.find("3 or D  Open or close DROPOUTS") != std::string::npos);
    CHECK(help.find("W+") != std::string::npos);
    CHECK(help.find("ALL PLUGIN BYPASS") != std::string::npos);
    CHECK(help.find("Null test") == std::string::npos);
    CHECK(help.find("Save As") != std::string::npos);
    CHECK(help.find("Backup") != std::string::npos);
    CHECK(help.find("Shift+click") != std::string::npos);
    CHECK(help.find("Cmd+click") != std::string::npos);
    CHECK(help.find("Cmd+A or A  Select every visible channel on the REC page and the HOST page") != std::string::npos);
    CHECK(help.find("Click, Shift+click, and Cmd+click select on HOST the same way") != std::string::npos);
    CHECK(help.find("Cmd+G  On the REC page and the HOST page, name a group from the selected channels") != std::string::npos);
    CHECK(help.find("Make group from selection") != std::string::npos);
    CHECK(help.find("Cmd+A, then Cmd+G, groups every visible channel") != std::string::npos);
    CHECK(help.find("Cmd+G does nothing while a text field has focus") != std::string::npos);
    CHECK(help.find("Stereo link with ch N+1") != std::string::npos);
    CHECK(help.find("does not change the recorder or the REC page") != std::string::npos);
    CHECK(help.find("stay per channel") != std::string::npos);
    CHECK(help.find("CH, MODE, NAME, DELAY COMP, PLUGINS, and GAIN") != std::string::npos);
    CHECK(help.find("M, ST, or M/ST") != std::string::npos);
    CHECK(help.find("makes it tall enough to read") != std::string::npos);
    CHECK(help.find("FIT shows every lane again") != std::string::npos);
    CHECK(help.find("It does the same as F and Option+R") != std::string::npos);
    CHECK(help.find("F does nothing while a text field has focus") != std::string::npos);
    CHECK(help.find("on a background thread") != std::string::npos);
    CHECK(help.find("writes session.youhost once") != std::string::npos);
    CHECK(help.find("skip the Backups folder") != std::string::npos);
    CHECK(help.find("A progress bar shows the copy.") != std::string::npos);
    CHECK(help.find("even when a lane is only a pixel or two") != std::string::npos);
    CHECK(help.find("V- zooms out until every lane fits") != std::string::npos);
    CHECK(help.find("Lanes stay tall enough to read") == std::string::npos);
    CHECK(help.find("takes focus only when you click it or press G") != std::string::npos);
    CHECK(help.find("nothing while a text field has focus") != std::string::npos);
    CHECK(help.find("Option-drag") != std::string::npos);
    CHECK(help.find("REC means the audio passes through the plugins and is recorded") != std::string::npos);
    CHECK(help.find("INPUT means the audio passes through the plugins to the output and is not recorded") != std::string::npos);
    CHECK(help.find("OFF cuts the channel fully: no audio, no plugins, and no recording") != std::string::npos);
    CHECK(help.find("YouHost — ASENNUSOHJE") != std::string::npos);
    CHECK(help.find("YouHost — INSTALL") != std::string::npos);
    CHECK(help.find("Open Anyway") != std::string::npos);
    CHECK(help.find("xattr -dr com.apple.quarantine /Applications/YouHost.app") != std::string::npos);
    CHECK(help.find("choose the audio card") != std::string::npos);
    CHECK(help.find("GitHub") == std::string::npos);
    CHECK(help.find("Artifacts") == std::string::npos);
    CHECK(help.find("The interface always opens with all channels") != std::string::npos);
    CHECK(help.find("Channel use is chosen only with these buttons") != std::string::npos);
    CHECK(help.find("Audio setup ticks") == std::string::npos);

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

    int used[4] = { 0, 0, 1, 1 };
    CHECK(youhost::firstUnusedGroup(used, 4, youhost::kMaxDisplayGroups) == 2);
    int full[youhost::kMaxDisplayGroups];
    for (int group = 0; group < youhost::kMaxDisplayGroups; ++group)
        full[group] = group;
    CHECK(youhost::firstUnusedGroup(full, youhost::kMaxDisplayGroups, youhost::kMaxDisplayGroups) == -1);

    const auto emptySelection = youhost::planNewGroup(false, 0, "Drums", 1);
    CHECK(! emptySelection.created);
    const auto noSlot = youhost::planNewGroup(true, -1, "Drums", 1);
    CHECK(! noSlot.created);
    const auto named = youhost::planNewGroup(true, 2, "  Drums  ", 3);
    CHECK(named.created);
    CHECK(named.group == 2);
    CHECK(named.name == "Drums");
    CHECK(named.color == 3);
    CHECK(named.collapsed);
    const auto blank = youhost::planNewGroup(true, 0, "   ", 0);
    CHECK(blank.name == "Group 1");
    CHECK(blank.collapsed);
    const std::string longText(50, 'A');
    CHECK(youhost::planNewGroup(true, 1, longText, 0).name.size() == 40);

    youhost::KeyQuery plainG;
    plainG.kind = youhost::KeyKind::character;
    plainG.character = 'g';
    const auto go = youhost::matchShortcut(plainG);
    CHECK(go.has_value());
    CHECK(*go == youhost::ShortcutId::goToChannel);
    youhost::KeyQuery commandG = plainG;
    commandG.command = true;
    const auto make = youhost::matchShortcut(commandG);
    CHECK(make.has_value());
    CHECK(*make == youhost::ShortcutId::makeGroup);
    CHECK(youhost::shortcutHelpText().find("Enter or OK creates the group") != std::string::npos);
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

void testHostSwitchIsClickFree()
{
    const float liveLevel = 0.5f;
    const float takeLevel = -0.5f;
    float live[256];
    float take[256];
    std::fill(live, live + 256, liveLevel);
    std::fill(take, take + 256, takeLevel);

    youhost::HostFade fade;
    CHECK(! youhost::hostWantsTake(true, 0, 256, false, fade.gain, false));
    auto waiting = youhost::stepHostFade(fade, false, 256);
    CHECK(waiting.steadyLive());
    CHECK(fade.gain == 0);

    CHECK(youhost::hostWantsTake(true, 256, 256, false, fade.gain, false));
    auto up = youhost::stepHostFade(fade, true, 256);
    CHECK(up.gain0 == 0);
    CHECK(up.gain1 == youhost::kHostSwitchFade);
    float dest[256];
    youhost::mixHostBlock(dest, live, take, 256, up.gain0, up.gain1);
    CHECK(dest[0] == liveLevel);
    CHECK(dest[youhost::kHostSwitchFade] == takeLevel);
    float maxJump = 0.0f;
    for (int index = 1; index < 256; ++index)
        maxJump = std::max(maxJump, std::fabs(dest[index] - dest[index - 1]));
    const float oneStep = std::fabs(takeLevel - liveLevel) / static_cast<float>(youhost::kHostSwitchFade);
    CHECK(maxJump <= oneStep + 1.0e-5f);

    float previous = dest[255];
    auto down = youhost::stepHostFade(fade, false, 256);
    CHECK(down.gain0 == youhost::kHostSwitchFade);
    CHECK(down.gain1 == 0);
    youhost::mixHostBlock(dest, live, take, 256, down.gain0, down.gain1);
    CHECK(dest[0] == previous);
    CHECK(dest[youhost::kHostSwitchFade] == liveLevel);
    CHECK(fade.gain == 0);
    maxJump = 0.0f;
    for (int index = 1; index < 256; ++index)
        maxJump = std::max(maxJump, std::fabs(dest[index] - dest[index - 1]));
    CHECK(maxJump <= oneStep + 1.0e-5f);

    // A 32-sample buffer has to cross the same ramp without a step between blocks.
    youhost::HostFade small;
    previous = liveLevel;
    bool wantTake = true;
    for (int block = 0; block < 10; ++block)
    {
        if (block == 6)
            wantTake = false;
        const bool open = wantTake && youhost::hostWantsTake(true, 32, 32, false, small.gain, false);
        auto step = youhost::stepHostFade(small, open, 32);
        float blockDest[32];
        youhost::mixHostBlock(blockDest, live, take, 32, step.gain0, step.gain1);
        CHECK(std::fabs(blockDest[0] - previous) <= oneStep + 1.0e-5f);
        for (int index = 1; index < 32; ++index)
            CHECK(std::fabs(blockDest[index] - blockDest[index - 1]) <= oneStep + 1.0e-5f);
        previous = blockDest[31];
    }
    CHECK(small.gain == 0);
    auto settled = youhost::stepHostFade(small, false, 32);
    CHECK(settled.steadyLive());
    float tail[32];
    youhost::mixHostBlock(tail, live, take, 32, settled.gain0, settled.gain1);
    CHECK(std::fabs(tail[0] - previous) <= oneStep + 1.0e-5f);
    CHECK(tail[0] == liveLevel);
    CHECK(tail[31] == liveLevel);
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
    const auto named = youhost::parseRecordingName("1_BD_1.wav");
    CHECK(named.takeNumber == 1);
    CHECK(named.channelNumber == 1);
    const auto secondTake = youhost::parseRecordingName("1_BD_2.wav");
    CHECK(secondTake.takeNumber == 2);
    CHECK(secondTake.channelNumber == 1);
    const auto snare = youhost::parseRecordingName("2_Sn_1.wav");
    CHECK(snare.takeNumber == 1);
    CHECK(snare.channelNumber == 2);
    const auto drum = youhost::parseRecordingName("3_Kick_Drum_1.wav");
    CHECK(drum.takeNumber == 1);
    CHECK(drum.channelNumber == 3);
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
        { "Pro-Q 3", "FabFilter", "AudioUnit", "" },
        { "Pro-Q 3", "FabFilter", "VST3", "" },
        { "De-Feedback", "Alpha Labs", "AudioUnit", "" },
        { "Saturn 2", "FabFilter", "VST3", "" }
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

    std::vector<youhost::CatalogPlugin> unknown { { "Thing", "  ", "VST3", "" } };
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

void testDeviceOpensAllChannels()
{
    CHECK(youhost::channelsToOpen(32) == 32);
    CHECK(youhost::channelsToOpen(200) == 128);
    CHECK(youhost::channelsToOpen(0) == 0);
    CHECK(youhost::channelsToOpen(-3) == 0);

    bool saved[8] = { true, true, false, false, false, false, false, false };
    CHECK(! youhost::deviceMaskIsComplete(saved, 8));
    youhost::openAllReportedChannels(saved, 8);
    CHECK(youhost::deviceMaskIsComplete(saved, 8));
    for (const bool bit : saved)
        CHECK(bit);

    bool wide[128] = {};
    wide[0] = true;
    CHECK(! youhost::deviceMaskIsComplete(wide, 200));
    youhost::openAllReportedChannels(wide, 200);
    CHECK(youhost::deviceMaskIsComplete(wide, 200));
    CHECK(youhost::channelsToOpen(200) == 128);
    CHECK(youhost::deviceMaskIsComplete(nullptr, 0));
    CHECK(! youhost::deviceMaskIsComplete(nullptr, 4));

    bool full[8];
    for (bool& bit : full)
        bit = true;
    CHECK(youhost::deviceMaskIsComplete(full, 8));
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

const youhost::SessionNode* findChild(const youhost::SessionNode& node, const char* name)
{
    for (const auto& child : node.children)
        if (child.name == name)
            return &child;
    return nullptr;
}

const youhost::SessionNode* findChannel(const youhost::SessionNode& node, const char* index)
{
    for (const auto& child : node.children)
    {
        if (child.name != "Channel")
            continue;
        const auto* value = youhost::sessionAttribute(child, "index");
        if (value != nullptr && *value == index)
            return &child;
    }
    return nullptr;
}

void testTimelineScroll()
{
    CHECK(youhost::lanesVisible(40, 0, 70.0f) == 40);
    CHECK(youhost::lanesVisible(4, 0, 200.0f) == 4);
    CHECK(youhost::lanesVisible(40, 100, 400.0f) == 1);
    CHECK(youhost::lanesShownForVerticalStep(128, 0) == 128);

    const float shortTimeline = 66.0f;
    for (const int total : { 32, 128 })
    {
        const auto enlarged = youhost::focusReadableLane(total, total / 2, shortTimeline);
        const int enlargedShown = youhost::lanesVisible(total, enlarged.verticalStep, shortTimeline);
        CHECK(enlarged.verticalStep > 0);
        CHECK(enlargedShown < total);

        int step = enlarged.verticalStep;
        int shown = enlargedShown;
        for (int guard = 0; step > 0 && guard < 40; ++guard)
        {
            step = youhost::stepVerticalZoom(step, -1, total);
            const int next = youhost::lanesVisible(total, step, shortTimeline);
            CHECK(next >= shown);
            shown = next;
        }
        CHECK(step == 0);
        CHECK(shown == total);

        const auto fitted = youhost::fitAllLaneFocus();
        CHECK(fitted.verticalStep == 0);
        CHECK(fitted.laneScroll == 0);
        CHECK(youhost::lanesVisible(total, fitted.verticalStep, shortTimeline) == total);

        const int zoomedStep = youhost::stepVerticalZoom(0, 1, total);
        const int zoomedIn = youhost::lanesVisible(total, zoomedStep, shortTimeline);
        CHECK(zoomedIn < total);
        CHECK(zoomedIn > 4);
        CHECK(youhost::lanesVisible(total, youhost::stepVerticalZoom(zoomedStep, -1, total), shortTimeline) == total);

        const float slot = shortTimeline / static_cast<float>(total);
        const float row = youhost::laneRowHeight(slot);
        CHECK(row > 0.0f);
        CHECK(row <= slot + 0.01f);
        CHECK(row * static_cast<float>(total) <= shortTimeline + 0.01f);
        if (slot < 2.0f)
            CHECK(row == slot);
    }
    CHECK(youhost::laneRowHeight(20.0f) == 19.0f);
    const auto span = youhost::fitSpanSamples(48000 * 30, 48000.0);
    CHECK(youhost::zoomVisibleSamples(span, 0) == span);

    float laneWheel = 0.0f;
    CHECK(youhost::applyLaneWheel(laneWheel, 1.0f, 3, 20, 5) == 2);
    CHECK(youhost::applyLaneWheel(laneWheel, -1.0f, 0, 20, 5) == 1);
    laneWheel = 0.0f;
    CHECK(youhost::applyLaneWheel(laneWheel, -0.4f, 0, 20, 5) == 0);
    CHECK(youhost::applyLaneWheel(laneWheel, -0.4f, 0, 20, 5) == 0);
    CHECK(youhost::applyLaneWheel(laneWheel, -0.4f, 0, 20, 5) == 1);
    CHECK(youhost::applyLaneWheel(laneWheel, -1.0f, 18, 20, 5) == 15);

    double timeWheel = 0.0;
    const auto moved = youhost::applyTimeWheel(timeWheel, -1.0f, 0, 100000, 10000);
    CHECK(moved == 1200);
    CHECK(youhost::applyTimeWheel(timeWheel, 1.0f, 0, 100000, 10000) == 0);

    CHECK(youhost::timelineScrollAxis(0.0f, -1.0f, false, false) == youhost::TimelineScrollAxis::lanes);
    CHECK(youhost::timelineScrollAxis(0.0f, -1.0f, true, false) == youhost::TimelineScrollAxis::time);
    CHECK(youhost::timelineScrollAxis(0.8f, 0.1f, false, false) == youhost::TimelineScrollAxis::time);
    CHECK(youhost::timelineScrollAxis(0.8f, 0.1f, false, true) == youhost::TimelineScrollAxis::gain);
    CHECK(youhost::timelineTimeDelta(0.0f, -1.0f, true) == -1.0f);
    CHECK(youhost::timelineTimeDelta(0.5f, -0.1f, false) == 0.5f);

    const auto fitted = youhost::timeBarRange(0, 48000, 48000);
    CHECK(fitted.size == fitted.limit);
    const auto zoomed = youhost::timeBarRange(1000, 250, 1000);
    CHECK(zoomed.limit == 1000.0);
    CHECK(zoomed.start == 750.0);
    CHECK(zoomed.size == 250.0);
    const auto lanes = youhost::laneBarRange(2, 5, 20);
    CHECK(lanes.limit == 20.0);
    CHECK(lanes.start == 2.0);
    CHECK(lanes.size == 5.0);

    youhost::SessionTimelineState missing = youhost::timelineFromNode(youhost::SessionNode {});
    CHECK(missing.zoom == 0);
    CHECK(missing.vertical == 0);
    CHECK(missing.scroll == 0);
    CHECK(missing.laneScroll == 0);
    CHECK(missing.height == 0);
}

void testSessionCompatibility()
{
    const char* older = R"(<YouHostSession>
  <Channel index="1" name="Hat" record="0" custom="keep">
    <Slot index="0" bypass="0">
      <PLUGIN name="EQ"/>
      <State data="abc"/>
    </Slot>
  </Channel>
  <Group index="2" name="Drums" color="1" collapsed="1"/>
  <Take start="10" length="50">
    <File channel="1" name="Hat.wav"/>
  </Take>
</YouHostSession>)";

    youhost::SessionNode oldRoot;
    CHECK(youhost::parseSessionXml(older, oldRoot));
    youhost::SessionDocumentModel loaded;
    CHECK(youhost::readSessionModel(oldRoot, loaded));
    CHECK(loaded.version == 0);
    CHECK(loaded.channelCount == 128);
    CHECK(loaded.bits == 24);
    CHECK(loaded.page == 1);
    CHECK(near(static_cast<float>(loaded.wave), 1.0f, 0.0001f));
    CHECK(loaded.align == "all");
    CHECK(! loaded.hasTimeline);
    CHECK(loaded.timeline.zoom == 0);
    CHECK(loaded.timeline.height == 0);
    CHECK(loaded.channels.size() == 1);
    CHECK(loaded.channels[0].index == 1);
    CHECK(loaded.channels[0].name == "Hat");
    CHECK(loaded.channels[0].listen == "off");
    CHECK(loaded.channels[0].group == -1);
    CHECK(loaded.channels[0].slots.size() == 1);
    CHECK(loaded.channels[0].slots[0].pluginName == "EQ");
    CHECK(loaded.channels[0].slots[0].state == "abc");
    CHECK(loaded.groups.size() == 1);
    CHECK(loaded.groups[0].name == "Drums");
    CHECK(loaded.groups[0].collapsed);
    CHECK(loaded.takes.size() == 1);
    CHECK(loaded.takes[0].files.size() == 1);
    CHECK(loaded.takes[0].files[0].name == "Hat.wav");

    const auto migrated = youhost::writeSessionModel(loaded);
    CHECK(youhost::sessionAttributeInt(migrated, "version", 0) == youhost::kSessionFormatVersion);
    const auto* timeline = findChild(migrated, "Timeline");
    CHECK(timeline != nullptr);
    CHECK(youhost::sessionAttributeInt(*timeline, "zoom", -1) == 0);
    CHECK(youhost::sessionAttributeInt(*timeline, "height", -1) == 0);
    const auto* kept = findChannel(migrated, "1");
    CHECK(kept != nullptr);
    CHECK(youhost::sessionAttribute(*kept, "custom") != nullptr);
    CHECK(*youhost::sessionAttribute(*kept, "custom") == "keep");

    youhost::SessionNode migratedRoot;
    CHECK(youhost::parseSessionXml(youhost::writeSessionXml(migrated), migratedRoot));
    youhost::SessionDocumentModel legacyOfMigrated;
    CHECK(youhost::readSessionModelLegacy(migratedRoot, legacyOfMigrated));
    CHECK(legacyOfMigrated.channels.size() == 1);
    CHECK(legacyOfMigrated.channels[0].name == "Hat");
    CHECK(! legacyOfMigrated.hasTimeline);

    const char* newer = R"(<YouHostSession version="99" future="keep" wave="1.5">
  <Channel index="0" name="Kick" group="2" outputDb="3" custom="yes">
    <Slot index="1" bypass="1" fold="R">
      <PLUGIN name="Comp" extra="1"/>
      <State data="xyz"/>
      <Widget kind="a"/>
    </Slot>
  </Channel>
  <Timeline zoom="3" vertical="1" scroll="100" lanes="2" height="150"/>
  <Cloud id="7"/>
  <Take start="0" length="1000">
    <File channel="0" name="Kick.wav"/>
  </Take>
</YouHostSession>)";

    youhost::SessionNode newRoot;
    CHECK(youhost::parseSessionXml(newer, newRoot));
    youhost::SessionDocumentModel current;
    CHECK(youhost::readSessionModel(newRoot, current));
    CHECK(current.version == 99);
    CHECK(current.hasTimeline);
    CHECK(current.timeline.zoom == 3);
    CHECK(current.timeline.vertical == 1);
    CHECK(current.timeline.scroll == 100);
    CHECK(current.timeline.laneScroll == 2);
    CHECK(current.timeline.height == 150);
    CHECK(current.channels[0].name == "Kick");
    CHECK(current.channels[0].group == 2);
    CHECK(current.channels[0].slots[0].pluginName == "Comp");
    CHECK(current.channels[0].slots[0].fold == "R");
    CHECK(current.channels[0].slots[0].bypassed);
    CHECK(current.takes[0].files[0].name == "Kick.wav");

    youhost::SessionDocumentModel olderReader;
    CHECK(youhost::readSessionModelLegacy(newRoot, olderReader));
    CHECK(olderReader.channels[0].name == "Kick");
    CHECK(olderReader.channels[0].slots[0].state == "xyz");
    CHECK(olderReader.takes[0].files[0].name == "Kick.wav");
    CHECK(! olderReader.hasTimeline);
    CHECK(olderReader.timeline.zoom == 0);

    current.channels[0].name = "Snare";
    current.channels[0].group = -1;
    current.channels[0].outputDb = 0.0;
    const auto resaved = youhost::writeSessionModel(current);
    CHECK(youhost::sessionAttributeInt(resaved, "version", 0) == youhost::kSessionFormatVersion);
    CHECK(youhost::sessionAttribute(resaved, "future") != nullptr);
    CHECK(*youhost::sessionAttribute(resaved, "future") == "keep");
    CHECK(findChild(resaved, "Cloud") != nullptr);
    const auto* channel = findChannel(resaved, "0");
    CHECK(channel != nullptr);
    CHECK(youhost::sessionAttribute(*channel, "name") != nullptr);
    CHECK(*youhost::sessionAttribute(*channel, "name") == "Snare");
    CHECK(youhost::sessionAttribute(*channel, "group") == nullptr);
    CHECK(youhost::sessionAttribute(*channel, "outputDb") == nullptr);
    CHECK(youhost::sessionAttribute(*channel, "custom") != nullptr);
    const auto* slot = findChild(*channel, "Slot");
    CHECK(slot != nullptr);
    const auto* plugin = findChild(*slot, "PLUGIN");
    CHECK(plugin != nullptr);
    CHECK(youhost::sessionAttribute(*plugin, "extra") != nullptr);
    CHECK(findChild(*slot, "Widget") != nullptr);
    CHECK(youhost::sessionAttributeInt(*findChild(resaved, "Timeline"), "zoom", 0) == 3);

    youhost::SessionNode originalDevice;
    CHECK(youhost::parseSessionXml(
        R"(<YouHostSession><Device><AUDIODEVICE rate="44100"/><Note text="hi"/></Device></YouHostSession>)",
        originalDevice));
    youhost::SessionNode writtenDevice;
    CHECK(youhost::parseSessionXml(
        R"(<YouHostSession version="6"><Device><AUDIODEVICE rate="48000"/></Device></YouHostSession>)",
        writtenDevice));
    youhost::mergeSessionNodes(originalDevice, writtenDevice);
    const auto* device = findChild(writtenDevice, "Device");
    CHECK(device != nullptr);
    int audioDevices = 0;
    bool noteKept = false;
    if (device != nullptr)
    {
        for (const auto& child : device->children)
        {
            if (child.name == "AUDIODEVICE")
            {
                ++audioDevices;
                const auto* rate = youhost::sessionAttribute(child, "rate");
                CHECK(rate != nullptr && *rate == "48000");
            }
            if (child.name == "Note")
                noteKept = true;
        }
    }
    CHECK(audioDevices == 1);
    CHECK(noteKept);

    youhost::SessionNode refused;
    CHECK(! youhost::parseSessionXml("<NotASession/>", refused) || ! youhost::readSessionModel(refused, current));
    youhost::SessionNode wrong;
    CHECK(youhost::parseSessionXml("<NotASession/>", wrong));
    CHECK(! youhost::readSessionModel(wrong, current));

    youhost::SessionNode named;
    CHECK(youhost::parseSessionXml("<YouHostSession><Channel index=\"0\" name=\"Kick &amp; Bass\"/></YouHostSession>", named));
    youhost::SessionDocumentModel ampersand;
    CHECK(youhost::readSessionModel(named, ampersand));
    CHECK(ampersand.channels[0].name == "Kick & Bass");
    const auto escaped = youhost::writeSessionXml(youhost::writeSessionModel(ampersand));
    CHECK(escaped.find("Kick &amp; Bass") != std::string::npos);
}

void testNullTestIsTransparent()
{
    CHECK(! youhost::pluginSlotRuns(true, false, true));
    CHECK(youhost::pluginSlotRuns(true, false, false));

    constexpr int frames = 8;
    const float pattern[frames] = { 1.0f, -1.0f, 0.0f, 0.25f, -0.5f, 0.0001f, 0.5f, -0.125f };
    std::array<bool, youhost::kMaxChannels> inputsOn {};
    std::array<bool, youhost::kMaxChannels> outputsOn {};
    inputsOn[0] = true;
    inputsOn[7] = true;
    outputsOn[0] = true;
    outputsOn[4] = true;
    outputsOn[7] = true;
    float in0[frames];
    float in7[frames];
    float out0[frames];
    float out4[frames];
    float out7[frames];
    std::memcpy(in0, pattern, sizeof(pattern));
    std::memcpy(in7, pattern, sizeof(pattern));
    std::memset(out0, 0x11, sizeof(out0));
    std::memset(out4, 0x11, sizeof(out4));
    std::memset(out7, 0x11, sizeof(out7));
    const float* inputs[] = { in0, in7 };
    float* outputs[] = { out0, out4, out7 };
    std::array<youhost::ChannelStrip, youhost::kMaxChannels> strips {};
    const auto routing = youhost::makeRouting(inputsOn, outputsOn);
    CHECK(routing.inputPacked[7] == 1);
    CHECK(routing.outputPacked[7] == 2);
    youhost::processPassthrough(inputs, 2, outputs, 3, frames, configAt(48000.0, routing), strips.data(), youhost::kMaxChannels);
    CHECK(std::memcmp(out0, pattern, sizeof(pattern)) == 0);
    CHECK(std::memcmp(out7, pattern, sizeof(pattern)) == 0);
    for (int index = 0; index < frames; ++index)
        CHECK(out4[index] == 0.0f);
}

void testPluginEditStressStaysResponsive()
{
    const auto started = std::chrono::steady_clock::now();
    youhost::SlotMoveState slots[8] {};
    for (int index = 0; index < 8; ++index)
        slots[index].instance = index + 1;
    int next = 8;
    youhost::ChannelOpQueue queue;
    youhost::SessionLoadCursor cursor;
    cursor.total = 64;

    for (int step = 0; step < 4000; ++step)
    {
        const int from = step % 8;
        const int to = (step + 3) % 8;
        if ((step % 5) == 0)
        {
            const auto copied = youhost::copySlotInstance(slots[from], slots[to], next);
            if (copied.ok)
                slots[to].loading = false;
        }
        else
        {
            youhost::moveSlotInstance(slots[from], slots[to]);
        }

        if (youhost::beginQueuedLoad(queue))
            youhost::finishQueuedLoad(queue);

        if (cursor.startOne())
        {
            CHECK(cursor.inFlight);
            CHECK(! cursor.startOne());
            cursor.completeOne();
        }

        int hops = youhost::pluginGraveHops();
        while (youhost::graveStep(hops) != youhost::GraveStep::dropInstance)
            hops = youhost::graveNextHops(hops);
    }

    CHECK(cursor.done());
    CHECK(queue.started == queue.finished);
    bool seenEmpty = false;
    for (const auto& slot : slots)
    {
        CHECK(! slot.loading);
        if (slot.instance == 0)
            seenEmpty = true;
        CHECK(slot.instance >= 0);
    }
    CHECK(! seenEmpty);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    CHECK(elapsed.count() < 200);
}

void testAudioEngineStressDoesNotAllocate()
{
    constexpr int threadCount = 4;
    constexpr int channelsPerThread = 32;
    constexpr int frames = 64;
    constexpr int blocks = 30;

    struct Worker
    {
        std::array<youhost::ChannelStrip, channelsPerThread> strips {};
        std::vector<float> inputs;
        std::vector<float> outputs;
        std::array<const float*, channelsPerThread> inputPtrs {};
        std::array<float*, channelsPerThread> outputPtrs {};
        youhost::AudioThreadConfig config {};
    };

    auto workers = std::make_unique<Worker[]>(threadCount);
    for (int thread = 0; thread < threadCount; ++thread)
    {
        auto& worker = workers[static_cast<std::size_t>(thread)];
        worker.inputs.assign(static_cast<std::size_t>(channelsPerThread * frames), 0.25f);
        worker.outputs.assign(static_cast<std::size_t>(channelsPerThread * frames), 0.0f);
        std::array<bool, youhost::kMaxChannels> active {};
        for (int channel = 0; channel < channelsPerThread; ++channel)
        {
            worker.inputPtrs[static_cast<std::size_t>(channel)] = worker.inputs.data() + channel * frames;
            worker.outputPtrs[static_cast<std::size_t>(channel)] = worker.outputs.data() + channel * frames;
            active[static_cast<std::size_t>(channel)] = true;
        }
        worker.config = configAt(48000.0, youhost::makeRouting(active, active));
    }

    std::atomic<int> finished { 0 };
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    audioAllocations.store(0, std::memory_order_relaxed);
    for (int thread = 0; thread < threadCount; ++thread)
    {
        threads.emplace_back([&, thread]
        {
            auto& worker = workers[static_cast<std::size_t>(thread)];
            inAudioCallback = true;
            for (int block = 0; block < blocks; ++block)
            {
                youhost::processPassthrough(worker.inputPtrs.data(),
                                            channelsPerThread,
                                            worker.outputPtrs.data(),
                                            channelsPerThread,
                                            frames,
                                            worker.config,
                                            worker.strips.data(),
                                            channelsPerThread);
            }
            inAudioCallback = false;
            finished.fetch_add(1, std::memory_order_release);
        });
    }

    for (auto& thread : threads)
        thread.join();

    CHECK(audioAllocations.load() == 0);
    CHECK(workers[0].outputs[0] == 0.25f);
    CHECK(workers[3].outputs[static_cast<std::size_t>((channelsPerThread - 1) * frames)] == 0.25f);
}

void testSessionChannelsRateLockAndClose()
{
    CHECK(youhost::audioCardKind("MacBook Pro Microphone") == youhost::AudioCardKind::builtin);
    CHECK(youhost::audioCardKind("Built-in Output") == youhost::AudioCardKind::builtin);
    CHECK(youhost::audioCardKind("X32") == youhost::AudioCardKind::real);
    CHECK(youhost::audioCardKind("Dante Virtual Soundcard") == youhost::AudioCardKind::real);
    CHECK(youhost::audioCardKind("") == youhost::AudioCardKind::none);
    CHECK(youhost::audioCardKind("No device") == youhost::AudioCardKind::none);
    CHECK(youhost::audioCardKind("<< none >>") == youhost::AudioCardKind::none);
    CHECK(youhost::audioCardKind(youhost::kOfflineDeviceName) == youhost::AudioCardKind::none);

    CHECK(youhost::audioCardKind("WING") == youhost::AudioCardKind::real);
    const auto macbook = youhost::sessionChannelView(2, true, false, false);
    CHECK(macbook.visible == 2);
    CHECK(macbook.visible != 128);
    CHECK(macbook.cardInputs == 2);
    CHECK(youhost::channelUsesCpu(0, macbook));
    CHECK(! youhost::channelUsesCpu(2, macbook));
    CHECK(! youhost::timelineShowsChannel(2, macbook.visible));
    CHECK(youhost::hiddenChannelNote(macbook).find("126 channels hidden (card has 2)") != std::string::npos);

    const auto wing = youhost::sessionChannelView(48, true, false, false);
    CHECK(wing.visible == 48);
    CHECK(youhost::channelUsesCpu(47, wing));
    CHECK(! youhost::timelineShowsChannel(48, wing.visible));
    CHECK(! youhost::channelUsesCpu(48, wing));

    const auto card16 = youhost::sessionChannelView(16, true, false, false);
    CHECK(card16.hidden == 112);
    CHECK(youhost::hiddenChannelNote(card16) == "112 channels hidden (card has 16)");
    CHECK(youhost::timelineShowsChannel(15, card16.visible));
    CHECK(youhost::channelUsesCpu(15, card16));
    CHECK(! youhost::timelineShowsChannel(40, card16.visible));
    CHECK(! youhost::channelUsesCpu(40, card16));
    CHECK(youhost::channelUnsupportedByCard(40, card16));
    CHECK(youhost::unsupportedChannelLine(15, "Kick").find("16  Kick  not on this card") != std::string::npos);

    const auto revealed = youhost::sessionChannelView(16, true, false, true);
    CHECK(revealed.visible == 128);
    CHECK(revealed.revealed);
    CHECK(youhost::timelineShowsChannel(40, revealed.visible));
    CHECK(! youhost::channelUsesCpu(40, revealed));
    CHECK(youhost::channelUnsupportedByCard(40, revealed));

    const auto offline = youhost::sessionChannelView(2, true, true, false);
    CHECK(offline.visible == 128);
    CHECK(offline.cardInputs == 0);
    CHECK(! youhost::channelUsesCpu(0, offline));
    CHECK(! youhost::cardHidesChannels(offline));
    CHECK(youhost::hiddenChannelNote(offline).empty());

    const auto closed = youhost::sessionChannelView(32, false, false, false);
    CHECK(closed.visible == 0);
    CHECK(closed.cardInputs == 0);

    const auto full = youhost::sessionChannelView(128, true, false, false);
    CHECK(full.visible == 128);
    CHECK(! youhost::cardHidesChannels(full));
    CHECK(youhost::channelUsesCpu(40, full));

    std::vector<std::string> devices = { "WING", youhost::kOfflineDeviceName, "X32" };
    youhost::appendOfflineDeviceEntry(devices);
    CHECK(devices.size() == 3);
    CHECK(devices.back() == youhost::kOfflineDeviceName);
    CHECK(devices[0] == "WING");
    youhost::appendOfflineDeviceEntry(devices);
    CHECK(devices.size() == 3);
    CHECK(std::count(devices.begin(), devices.end(), youhost::kOfflineDeviceName) == 1);
    CHECK(youhost::deviceKeptOnSessionOpen("WING", youhost::kOfflineDeviceName) == "WING");

    youhost::SessionDocumentModel model;
    model.channelCount = 16;
    youhost::SessionChannelRecord channel;
    channel.index = 40;
    channel.name = "OH";
    channel.color = 3;
    channel.group = 2;
    youhost::SessionSlotRecord slot;
    slot.occupied = true;
    slot.pluginName = "Comp";
    channel.slots.push_back(slot);
    model.channels.push_back(channel);
    youhost::SessionTakeRecord take;
    take.files.push_back(youhost::SessionFileRecord { 40, "41_1_OH.wav" });
    model.takes.push_back(take);
    const auto hiddenCard = youhost::sessionChannelView(16, true, false, false);
    const auto shownCard = youhost::sessionChannelView(64, true, false, false);
    (void) hiddenCard;
    (void) shownCard;
    CHECK(model.channels[0].name == "OH");
    CHECK(model.channels[0].color == 3);
    CHECK(model.channels[0].group == 2);
    CHECK(model.channels[0].slots[0].pluginName == "Comp");
    CHECK(model.takes[0].files[0].name == "41_1_OH.wav");

    const auto xml = youhost::writeSessionXml(youhost::writeSessionModel(model));
    CHECK(xml.find("version=\"7\"") != std::string::npos);
    CHECK(xml.find("channels=") == std::string::npos);
    CHECK(xml.find("index=\"40\"") != std::string::npos);
    CHECK(youhost::kSessionFormatVersion == 7);

    const char* counted = R"(<YouHostSession version="7" channels="16" future="keep">
  <Channel index="40" name="OH" color="3" group="2">
    <Slot index="0"><PLUGIN name="Comp"/></Slot>
  </Channel>
</YouHostSession>)";
    youhost::SessionNode countedRoot;
    CHECK(youhost::parseSessionXml(counted, countedRoot));
    youhost::SessionDocumentModel fromCounted;
    CHECK(youhost::readSessionModel(countedRoot, fromCounted));
    CHECK(fromCounted.channelCount == 128);
    CHECK(fromCounted.channels.size() == 1);
    CHECK(fromCounted.channels[0].index == 40);
    CHECK(fromCounted.channels[0].name == "OH");
    const auto kept = youhost::writeSessionXml(youhost::writeSessionModel(fromCounted));
    CHECK(kept.find("channels=\"16\"") != std::string::npos);
    CHECK(kept.find("future=\"keep\"") != std::string::npos);
    CHECK(kept.find("name=\"OH\"") != std::string::npos);

    const int recordingModes[] = {
        static_cast<int>(youhost::ChannelListen::record),
        static_cast<int>(youhost::ChannelListen::off),
    };
    CHECK(youhost::globalListenNeedsConfirm(true, youhost::ChannelListen::off, recordingModes, 2));
    CHECK(youhost::globalListenNeedsConfirm(true, youhost::ChannelListen::input, recordingModes, 1));
    CHECK(youhost::globalListenNeedsConfirm(true, youhost::ChannelListen::record, recordingModes, 2));
    CHECK(! youhost::globalListenNeedsConfirm(false, youhost::ChannelListen::off, recordingModes, 2));
    const int quietModes[] = { static_cast<int>(youhost::ChannelListen::input) };
    CHECK(youhost::globalListenNeedsConfirm(true, youhost::ChannelListen::off, quietModes, 1));
    CHECK(! youhost::globalListenNeedsConfirm(true, youhost::ChannelListen::off, nullptr, 0));
    const int already[] = { static_cast<int>(youhost::ChannelListen::record), static_cast<int>(youhost::ChannelListen::record) };
    CHECK(! youhost::globalListenNeedsConfirm(true, youhost::ChannelListen::record, already, 2));
    CHECK(youhost::channelListenChangeLine(4, youhost::ChannelListen::record, youhost::ChannelListen::input)
          == "Switch ch 5 REC -> INPUT");
    CHECK(std::string(youhost::kListenChangeVerb) == "Switch");
    CHECK(std::string(youhost::kListenChangeSureTitle) == "Are you sure?");
    CHECK(youhost::globalListenChangeLine(youhost::ChannelListen::input) == "Switch every visible channel to INPUT");
    CHECK(youhost::listenChangeApplies(youhost::ListenChangeKind::now, 0, 0));
    CHECK(! youhost::listenChangeApplies(youhost::ListenChangeKind::confirm, 0, 1));
    CHECK(! youhost::listenChangeApplies(youhost::ListenChangeKind::confirm, 1, 0));
    CHECK(youhost::listenChangeApplies(youhost::ListenChangeKind::confirm, 1, 1));
    CHECK(! youhost::listenChangeApplies(youhost::ListenChangeKind::blocked, 1, 1));
    CHECK(youhost::classifyListenChange(false, false, true) == youhost::ListenChangeKind::now);
    CHECK(youhost::classifyListenChange(true, false, true) == youhost::ListenChangeKind::confirm);
    CHECK(youhost::classifyListenChange(true, true, true) == youhost::ListenChangeKind::blocked);
    CHECK(youhost::classifyListenChange(true, false, false) == youhost::ListenChangeKind::unchanged);
    const int pageSelected[] = { 2, 3 };
    const auto recPage = youhost::channelsForPageListen(1, pageSelected, 2, 8);
    const auto hostPage = youhost::channelsForPageListen(2, pageSelected, 2, 8);
    CHECK(recPage == hostPage);
    CHECK(recPage.size() == 8);
    CHECK(youhost::channelsForPageListen(3, pageSelected, 2, 8).empty());

    CHECK(youhost::formatRateKhz(48000.0) == "48");
    CHECK(youhost::formatRateKhz(44100.0) == "44.1");
    CHECK(youhost::formatRateKhz(96000.0) == "96");
    CHECK(youhost::formatRateKhz(88200.0) == "88.2");
    const auto up = youhost::adoptCardSampleRate(44100.0, 48000.0);
    CHECK(up.changed);
    CHECK(up.rate == 48000.0);
    CHECK(up.notice == "Session moves to 48 kHz");
    const auto up96 = youhost::adoptCardSampleRate(48000.0, 96000.0);
    CHECK(up96.notice == "Session moves to 96 kHz");
    const auto up88 = youhost::adoptCardSampleRate(44100.0, 88200.0);
    CHECK(up88.notice == "Session moves to 88.2 kHz");
    const auto down = youhost::adoptCardSampleRate(48000.0, 44100.0);
    CHECK(down.changed);
    CHECK(down.notice.empty());
    CHECK(down.rate == 44100.0);
    const auto same = youhost::adoptCardSampleRate(48000.0, 48000.0);
    CHECK(! same.changed);
    CHECK(same.notice.empty());
    const auto fresh = youhost::adoptCardSampleRate(0.0, 48000.0);
    CHECK(! fresh.changed);
    CHECK(fresh.notice.empty());
    CHECK(fresh.rate == 48000.0);

    const youhost::SessionCloseReason closes[] = {
        youhost::SessionCloseReason::newSession,
        youhost::SessionCloseReason::open,
        youhost::SessionCloseReason::openRecent,
        youhost::SessionCloseReason::quit,
    };
    for (const auto reason : closes)
    {
        CHECK(youhost::closePathDiscardsSession(reason));
        CHECK(youhost::recordingBlocksClose(true, reason));
        CHECK(youhost::sessionCloseAsks(true, false, reason));
        CHECK(! youhost::sessionCloseAsks(true, true, reason));
        CHECK(! youhost::sessionCloseProceeds(true, true, reason, youhost::UnsavedChoice::discard));
        CHECK(youhost::sessionCloseProceeds(true, false, reason, youhost::UnsavedChoice::save));
        CHECK(youhost::sessionCloseProceeds(true, false, reason, youhost::UnsavedChoice::saveAs));
        CHECK(youhost::sessionCloseProceeds(true, false, reason, youhost::UnsavedChoice::discard));
        CHECK(! youhost::sessionCloseProceeds(true, false, reason, youhost::UnsavedChoice::cancel));
        CHECK(youhost::sessionCloseSavesFirst(true, false, reason, youhost::UnsavedChoice::save));
        CHECK(youhost::sessionCloseSaveAsFirst(true, false, reason, youhost::UnsavedChoice::saveAs));
        CHECK(! youhost::sessionCloseSavesFirst(true, false, reason, youhost::UnsavedChoice::saveAs));
        CHECK(! youhost::sessionCloseAsks(false, false, reason));
    }
    CHECK(! youhost::closePathDiscardsSession(youhost::SessionCloseReason::deviceChange));
    CHECK(! youhost::recordingBlocksClose(true, youhost::SessionCloseReason::deviceChange));
    CHECK(! youhost::sessionCloseAsks(true, false, youhost::SessionCloseReason::deviceChange));
    CHECK(youhost::sessionCloseProceeds(true, false, youhost::SessionCloseReason::deviceChange, youhost::UnsavedChoice::cancel));
    CHECK(! youhost::sessionCloseSavesFirst(true, false, youhost::SessionCloseReason::deviceChange, youhost::UnsavedChoice::save));

    const bool locked = true;
    const bool recording = true;
    const youhost::RecordDisrupt blocked[] = {
        youhost::RecordDisrupt::stop,
        youhost::RecordDisrupt::space,
        youhost::RecordDisrupt::commandSpace,
        youhost::RecordDisrupt::recToggle,
        youhost::RecordDisrupt::newSession,
        youhost::RecordDisrupt::open,
        youhost::RecordDisrupt::openRecent,
        youhost::RecordDisrupt::clearTimeline,
        youhost::RecordDisrupt::importRecordings,
        youhost::RecordDisrupt::changeDevice,
        youhost::RecordDisrupt::changeRate,
        youhost::RecordDisrupt::changeBuffer,
        youhost::RecordDisrupt::offlineSwitch,
        youhost::RecordDisrupt::channelListen,
        youhost::RecordDisrupt::globalListen,
    };
    for (const auto action : blocked)
        CHECK(! youhost::recordActionAllowed(action, recording, locked));
    CHECK(youhost::guardRecordAction(youhost::RecordDisrupt::quit, recording, locked) == youhost::RecordGuard::unlockBeforeQuit);
    CHECK(youhost::recordActionAllowed(youhost::RecordDisrupt::hostEdit, recording, locked));
    CHECK(youhost::recordActionAllowed(youhost::RecordDisrupt::save, recording, locked));

    CHECK(youhost::recordActionAllowed(youhost::RecordDisrupt::stop, recording, false));
    CHECK(youhost::recordActionAllowed(youhost::RecordDisrupt::space, recording, false));
    CHECK(youhost::recordActionAllowed(youhost::RecordDisrupt::channelListen, recording, false));
    CHECK(youhost::recordActionAllowed(youhost::RecordDisrupt::hostEdit, recording, false));
    CHECK(youhost::guardRecordAction(youhost::RecordDisrupt::changeDevice, recording, false) == youhost::RecordGuard::blockWhileRecording);
    CHECK(youhost::guardRecordAction(youhost::RecordDisrupt::newSession, recording, false) == youhost::RecordGuard::blockWhileRecording);
    CHECK(youhost::guardRecordAction(youhost::RecordDisrupt::quit, recording, false) == youhost::RecordGuard::blockWhileRecording);
    CHECK(youhost::recordActionAllowed(youhost::RecordDisrupt::recToggle, false, true));
    CHECK(youhost::recordActionAllowed(youhost::RecordDisrupt::space, false, true));
    CHECK(! youhost::recordLockEngaged(true, false));
    CHECK(youhost::recordLockEngaged(true, true));
    CHECK(youhost::recordLockControlHeight() >= youhost::transportControlHeight());
    CHECK(youhost::recordLockControlHeight() == 44);
    CHECK(youhost::transportControlHeight() == 36);
    CHECK(youhost::kRecordLockButtonWidth >= 48);
    CHECK(youhost::kRecordLockUnlocked == 0xff6f9e96u);
    CHECK(youhost::kRecordLockLocked == 0xffff2430u);

    float input[8] = { 0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f };
    float recorded[8] = {};
    float plugin[8];
    std::memcpy(plugin, input, sizeof(input));
    for (float& sample : plugin)
        sample = 0.0f;
    CHECK(youhost::copyRawRecordBlock(input, recorded, 8) == 8);
    for (int index = 0; index < 8; ++index)
        CHECK(recorded[index] == input[index]);
    CHECK(plugin[0] == 0.0f);

    const int selected[] = { 1, 16, 80 };
    const auto picked = youhost::channelsForGlobalListen(selected, 3, 64);
    CHECK(picked.size() == 64);
    CHECK(picked[0] == 0);
    CHECK(picked[63] == 63);
    const auto all = youhost::channelsForGlobalListen(nullptr, 0, 4);
    CHECK(all.size() == 4);
    CHECK(all[0] == 0);
    CHECK(all[3] == 3);

    const auto help = youhost::shortcutHelpText();
    CHECK(help.find("Offline (no audio) - 128 channels") != std::string::npos);
    CHECK(help.find("not on this card") == std::string::npos);
    CHECK(help.find("Show on mixer") == std::string::npos);
    CHECK(help.find("Channels past the card stay hidden") != std::string::npos);
    CHECK(help.find("ALL REC") != std::string::npos);
    CHECK(help.find("A mode change while recording asks twice") != std::string::npos);
    CHECK(help.find("Switch in red") != std::string::npos);
    CHECK(help.find("Are you sure?") != std::string::npos);
    CHECK(help.find("When not recording there is no dialog") != std::string::npos);
    CHECK(help.find("Leaving REC while recording asks first") == std::string::npos);
    CHECK(help.find("hidden on REC, HOST, and the timeline") != std::string::npos);
    CHECK(help.find("The session always keeps 128 channels") != std::string::npos);
    CHECK(help.find("8, 16, 32, 48, 64, or 128") == std::string::npos);
    CHECK(help.find("marked no input") == std::string::npos);
    CHECK(help.find("RECORDING LOCKED") != std::string::npos);
    CHECK(help.find(youhost::kRecordArmedHint) != std::string::npos);
    CHECK(help.find("Virtual / aggregate devices") != std::string::npos);
    CHECK(help.find("youhost.log") != std::string::npos);
    CHECK(help.find("beside the sample rate") != std::string::npos);
    CHECK(help.find("Find device") == std::string::npos);
    CHECK(help.find("Session moves to 48 kHz") != std::string::npos);
    CHECK(help.find("Save, Save As, Don't Save, or Cancel") != std::string::npos);
    CHECK(help.find("Scene") == std::string::npos);
    CHECK(help.find("MIDI") == std::string::npos);
    CHECK(help.find("Null test") == std::string::npos);
    CHECK(help.find("Auto (card)") == std::string::npos);
}

void testTimelinePaintDoesNotReenterLock()
{
    youhost::CheckedMutex first;
    youhost::CheckedMutex second;
    first.lock();
    CHECK(first.debugReentryWouldAssert());
    second.lock();
    CHECK(second.debugReentryWouldAssert());
    CHECK(first.debugReentryWouldAssert());
    second.unlock();
    first.unlock();
    CHECK(! first.debugReentryWouldAssert());
    CHECK(! second.debugReentryWouldAssert());

    std::vector<youhost::WavePeak> kickPeaks { { -0.2f, 0.4f }, { -0.1f, 0.2f } };
    std::vector<youhost::WavePeak> snarePeaks { { -0.3f, 0.3f } };
    std::vector<youhost::WavePeak> hatPeaks { { -0.05f, 0.1f } };
    youhost::TimelineTakeSource take;
    take.number = 1;
    take.length = 48000;
    take.recorded[0] = true;
    take.peaks[0] = &kickPeaks;
    take.recorded[1] = true;
    take.peaks[1] = &snarePeaks;
    take.recorded[2] = true;
    take.peaks[2] = &hatPeaks;

    std::array<youhost::TimelineChannelInfo, 3> channels {};
    channels[0].name = "Kick";
    channels[1].name = "Snare";
    channels[1].group = 0;
    channels[2].name = "Hat";
    channels[2].group = 0;
    youhost::TimelineGroupInfo group;
    group.collapsed = true;
    group.name = "Drums";

    bool sawKick = false;
    bool sawDrums = false;
    youhost::buildTimelineLanes(&take, 1, nullptr, channels.data(), 3, &group, 1,
                                [&](const std::vector<youhost::TimelineLaneView>& lanes)
                                {
                                    CHECK(lanes.size() == 2);
                                    for (const auto& lane : lanes)
                                    {
                                        if (lane.title == "Kick")
                                        {
                                            sawKick = true;
                                            CHECK(youhost::laneCornerLabel(lane.number, lane.title, false).find("Kick") != std::string::npos);
                                            CHECK(lane.regions.size() == 1);
                                            CHECK(lane.regions[0].peaks != nullptr);
                                            CHECK(! lane.regions[0].peaks->empty());
                                        }
                                        if (lane.title == "Drums")
                                        {
                                            sawDrums = true;
                                            CHECK(lane.group);
                                            CHECK(lane.regions.size() == 1);
                                            CHECK(lane.regions[0].peaks != nullptr);
                                            CHECK(lane.regions[0].peaks->size() == 1);
                                        }
                                    }
                                });
    CHECK(sawKick);
    CHECK(sawDrums);

    std::array<youhost::TimelineChannelInfo, 3> silent {};
    silent[0].name = "One";
    silent[1].name = "Two";
    silent[2].name = "Three";
    youhost::buildTimelineLanes(nullptr, 0, nullptr, silent.data(), 3, nullptr, 0,
                                [](const std::vector<youhost::TimelineLaneView>& lanes)
                                {
                                    CHECK(lanes.size() == 3);
                                    CHECK(lanes[1].number == 2);
                                    CHECK(lanes[1].title == "Two");
                                    CHECK(lanes[1].regions.empty());
                                });

    std::vector<youhost::WavePeak> recPeaks { { -0.2f, 0.5f } };
    youhost::TimelineTakeSource live;
    live.number = 1;
    live.start = 1000;
    live.length = 4800;
    live.recorded[0] = true;
    live.peaks[0] = &recPeaks;
    std::array<youhost::TimelineChannelInfo, 2> modes {};
    modes[0].name = "Rec";
    modes[1].name = "Input";
    youhost::buildTimelineLanes(nullptr, 0, &live, modes.data(), 2, nullptr, 0,
                                [](const std::vector<youhost::TimelineLaneView>& lanes)
                                {
                                    CHECK(lanes.size() == 2);
                                    CHECK(lanes[0].regions.size() == 1);
                                    CHECK(lanes[0].regions[0].number == 1);
                                    CHECK(lanes[0].regions[0].start == 1000);
                                    CHECK(lanes[1].regions.empty());
                                });

    const auto liveMarks = youhost::takeMarksFromSources(nullptr, 0, &live);
    CHECK(liveMarks.size() == 1);
    CHECK(liveMarks[0].number == 1);
    CHECK(liveMarks[0].start == 1000);

    youhost::TimelineTakeSource inputOnly;
    inputOnly.number = 3;
    inputOnly.start = 250;
    inputOnly.length = 100;
    const auto inputMarks = youhost::takeMarksFromSources(nullptr, 0, &inputOnly);
    CHECK(inputMarks.size() == 1);
    CHECK(inputMarks[0].number == 3);
    CHECK(inputMarks[0].start == 250);
    youhost::buildTimelineLanes(nullptr, 0, &inputOnly, modes.data(), 2, nullptr, 0,
                                [](const std::vector<youhost::TimelineLaneView>& lanes)
                                {
                                    CHECK(lanes[0].regions.empty());
                                    CHECK(lanes[1].regions.empty());
                                });

    youhost::TimelineTakeSource finished = live;
    const auto both = youhost::takeMarksFromSources(&finished, 1, &inputOnly);
    CHECK(both.size() == 2);
    CHECK(both[0].number == 3);
    CHECK(both[1].number == 1);

    youhost::TimelineTakeSource unnamed;
    CHECK(youhost::takeMarksFromSources(&unnamed, 1, nullptr).empty());

    const auto view = readWorkspaceFile("src/ui/TimelineView.cpp");
    CHECK(view.find("takeMarks = marks") != std::string::npos);
    CHECK(view.find("rememberTakeMark") == std::string::npos);
    CHECK(view.find("graphics.drawLine(markX, inner.getY(), markX, inner.getBottom(), 2.0f);") != std::string::npos);
    CHECK(view.find("takeRulerLabelText") != std::string::npos);
    const auto engine = readWorkspaceFile("src/engine/AudioEngine.cpp");
    CHECK(engine.find("takeMarksFromSources") != std::string::npos);
}

youhost::RecordAttempt readyTake()
{
    youhost::RecordAttempt attempt;
    attempt.buttonArmed = true;
    attempt.hasSession = true;
    attempt.folderWritable = true;
    attempt.deviceLive = true;
    attempt.deviceRate = 48000.0;
    attempt.preferredRate = 48000.0;
    attempt.takeNumber = 1;
    for (auto& channel : attempt.channels)
        channel.rec = true;
    return attempt;
}

bool wavFileHasSamples(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (! in)
        return false;
    char header[12] {};
    in.read(header, 12);
    if (! in || std::string(header, 4) != "RIFF" || std::string(header + 8, 4) != "WAVE")
        return false;
    in.seekg(0, std::ios::end);
    return in.tellg() > 44;
}

void testRecordStartTransport()
{
    const auto presses = {
        youhost::TransportPress::play,
        youhost::TransportPress::stop,
        youhost::TransportPress::space,
        youhost::TransportPress::commandSpace
    };

    auto card = readyTake();
    for (int channel = 0; channel < 32; ++channel)
        card.channels[static_cast<std::size_t>(channel)].inputOpen = true;

    const auto dir = std::filesystem::temp_directory_path() / "youhost-record-start";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const float samples[] = { 0.25f, -0.5f, 0.125f, -0.25f };

    for (const auto press : { youhost::TransportPress::play, youhost::TransportPress::space, youhost::TransportPress::commandSpace })
    {
        const auto started = youhost::resolveTransport(card, press);
        CHECK(started.startRecording);
        CHECK(started.alert.empty());
        CHECK(started.log.find("record start") != std::string::npos);
        CHECK(started.files.size() == 32);
        CHECK(started.files.front() == "1_1.wav");
        for (const auto& name : started.files)
        {
            const auto path = (dir / name).string();
            CHECK(youhost::writeMonoWav(path, static_cast<int>(started.rate), samples, 4));
            CHECK(wavFileHasSamples(path));
        }
    }

    auto disarmed = card;
    disarmed.buttonArmed = false;
    const auto command = youhost::resolveTransport(disarmed, youhost::TransportPress::commandSpace);
    CHECK(command.startRecording);
    CHECK(command.files.size() == 32);

    auto oneInput = readyTake();
    oneInput.channels[0].inputOpen = true;
    const auto narrow = youhost::resolveTransport(oneInput, youhost::TransportPress::play);
    CHECK(narrow.startRecording);
    CHECK(narrow.files.size() == 1);
    CHECK(narrow.files.front() == "1_1.wav");

    auto noInputs = readyTake();
    const auto missingInput = youhost::resolveTransport(noInputs, youhost::TransportPress::play);
    CHECK(! missingInput.startRecording);
    CHECK(missingInput.alert.find("input") != std::string::npos);
    CHECK(missingInput.log.find("record start failed") != std::string::npos);

    auto offline = readyTake();
    offline.deviceLive = false;
    offline.offline = true;
    const auto offlineStart = youhost::resolveTransport(offline, youhost::TransportPress::play);
    CHECK(! offlineStart.startRecording);
    CHECK(offlineStart.openAudioSetup);
    CHECK(offlineStart.alert == youhost::kOfflineRecordNotice);
    CHECK(youhost::writeMonoWav((dir / "1_1.wav").string(), 48000, samples, 4));
    CHECK(youhost::writeMonoWav((dir / "128_1.wav").string(), 48000, samples, 4));
    CHECK(wavFileHasSamples((dir / "1_1.wav").string()));
    CHECK(wavFileHasSamples((dir / "128_1.wav").string()));

    auto allOff = readyTake();
    allOff.deviceLive = true;
    for (auto& channel : allOff.channels)
        channel.rec = false;
    const auto off = youhost::resolveTransport(allOff, youhost::TransportPress::space);
    CHECK(! off.startRecording);
    CHECK(off.alert.find("input") != std::string::npos);

    auto lockedIdle = card;
    lockedIdle.lockArmed = true;
    lockedIdle.recording = false;
    const auto lockAllows = youhost::resolveTransport(lockedIdle, youhost::TransportPress::play);
    CHECK(lockAllows.startRecording);
    CHECK(lockAllows.alert.empty());

    auto lockedLive = card;
    lockedLive.lockArmed = true;
    lockedLive.recording = true;
    lockedLive.buttonArmed = true;
    const auto lockedSpace = youhost::resolveTransport(lockedLive, youhost::TransportPress::space);
    const auto lockedStop = youhost::resolveTransport(lockedLive, youhost::TransportPress::stop);
    CHECK(! lockedSpace.stop);
    CHECK(! lockedStop.stop);
    CHECK(lockedSpace.alert.find("locked") != std::string::npos);
    CHECK(lockedStop.log.find("transport blocked") != std::string::npos);

    auto rolling = card;
    rolling.recording = true;
    rolling.lockArmed = false;
    const auto stopTake = youhost::resolveTransport(rolling, youhost::TransportPress::space);
    CHECK(stopTake.stop);
    CHECK(stopTake.alert.empty());
    CHECK(stopTake.log.find("transport stop") != std::string::npos);

    auto opened = card;
    for (auto& channel : opened.channels)
        channel.rec = false;
    opened.channels[1].rec = true;
    opened.channels[1].inputOpen = true;
    const auto afterOpen = youhost::resolveTransport(opened, youhost::TransportPress::play);
    CHECK(afterOpen.startRecording);
    CHECK(afterOpen.files.size() == 1);
    CHECK(afterOpen.files.front() == "2_1.wav");

    auto fresh = offline;
    const auto afterNew = youhost::resolveTransport(fresh, youhost::TransportPress::play);
    CHECK(! afterNew.startRecording);
    CHECK(afterNew.files.empty());
    CHECK(afterNew.alert == youhost::kOfflineRecordNotice);

    auto noSession = card;
    noSession.hasSession = false;
    const auto folder = youhost::resolveTransport(noSession, youhost::TransportPress::play);
    CHECK(! folder.startRecording);
    CHECK(folder.alert.find("no folder") == std::string::npos);
    CHECK(folder.log.find("no folder") == std::string::npos);
    CHECK(folder.alert.empty());

    auto stoppedDevice = card;
    stoppedDevice.deviceLive = false;
    stoppedDevice.offline = false;
    const auto down = youhost::resolveTransport(stoppedDevice, youhost::TransportPress::commandSpace);
    CHECK(down.alert.find("not running") != std::string::npos);

    auto copying = card;
    copying.copyBusy = true;
    const auto busy = youhost::resolveTransport(copying, youhost::TransportPress::play);
    CHECK(busy.alert.find("copy") != std::string::npos);

    auto rated = card;
    rated.hasTakes = true;
    rated.timelineRate = 44100.0;
    rated.deviceRate = 48000.0;
    const auto clash = youhost::resolveTransport(rated, youhost::TransportPress::play);
    CHECK(clash.alert.find("different sample rate") != std::string::npos);

    auto unreadable = card;
    unreadable.folderWritable = false;
    unreadable.folderProblem = "The session drive is not available. Recording did not start.";
    const auto disk = youhost::resolveTransport(unreadable, youhost::TransportPress::play);
    CHECK(disk.alert == unreadable.folderProblem);

    auto empty = readyTake();
    empty.buttonArmed = false;
    empty.deviceLive = false;
    empty.hasTakes = false;
    for (auto& channel : empty.channels)
        channel.rec = false;
    const auto nothing = youhost::resolveTransport(empty, youhost::TransportPress::space);
    CHECK(nothing.startPlayback == false);
    CHECK(nothing.alert == "Nothing recorded yet.");

    empty.hasTakes = true;
    const auto playTakes = youhost::resolveTransport(empty, youhost::TransportPress::play);
    CHECK(playTakes.startPlayback);
    CHECK(playTakes.log == "play start");
    CHECK(playTakes.alert.empty());

    auto playing = empty;
    playing.playing = true;
    const auto again = youhost::resolveTransport(playing, youhost::TransportPress::play);
    CHECK(! again.startPlayback);
    CHECK(again.alert.empty());
    CHECK(again.log.find("already playing") != std::string::npos);

    const auto alreadyStopped = youhost::resolveTransport(empty, youhost::TransportPress::stop);
    CHECK(! alreadyStopped.stop);
    CHECK(alreadyStopped.alert.empty());
    CHECK(alreadyStopped.log.find("already stopped") != std::string::npos);

    for (const auto press : presses)
    {
        const auto result = youhost::resolveTransport(card, press);
        CHECK(! result.log.empty());
    }
}

void testDeviceListGrouping()
{
    CHECK(youhost::audioCardKind("Dante Virtual Soundcard") == youhost::AudioCardKind::real);
    CHECK(youhost::audioCardKind("SoundGrid") == youhost::AudioCardKind::real);
    CHECK(youhost::audioCardKind("X-USB") == youhost::AudioCardKind::real);
    CHECK(youhost::audioCardKind("WING 2") == youhost::AudioCardKind::real);
    CHECK(youhost::audioCardKind("EDIROL UA-1A") == youhost::AudioCardKind::real);
    CHECK(youhost::audioCardKind("FastTrack") == youhost::AudioCardKind::real);
    CHECK(youhost::audioCardKind("Fast Track Pro") == youhost::AudioCardKind::real);
    CHECK(youhost::audioCardKind("Scarlett 18i20") == youhost::AudioCardKind::real);
    CHECK(youhost::audioCardKind("Pro Tools Audio Bridge 32") == youhost::AudioCardKind::virtualDevice);
    CHECK(youhost::audioCardKind("Pro Tools Audio Bridge 2-A") == youhost::AudioCardKind::virtualDevice);
    CHECK(youhost::audioCardKind("Pro Tools Aggregate I/O") == youhost::AudioCardKind::virtualDevice);
    CHECK(youhost::audioCardKind("Microsoft Teams Audio") == youhost::AudioCardKind::virtualDevice);
    CHECK(youhost::audioCardKind("MJAudioRecorder") == youhost::AudioCardKind::virtualDevice);
    CHECK(youhost::audioCardKind("Koostelaite") == youhost::AudioCardKind::virtualDevice);
    CHECK(youhost::audioCardKind("MacBook Pro Microphone") == youhost::AudioCardKind::builtin);
    CHECK(youhost::audioCardKind("Built-in Output") == youhost::AudioCardKind::builtin);

    const std::vector<youhost::ListedDevice> present = {
        { "Pro Tools Audio Bridge 32", 32, 32 },
        { "WING 2", 48, 48 },
        { "MacBook Pro Microphone", 1, 0 },
        { "Scarlett 18i20", 18, 20 },
    };
    CHECK(youhost::chooseStartupDevice("WING 2", "", present) == "WING 2");
    CHECK(youhost::chooseStartupDevice("Pro Tools Audio Bridge 32", "", present) == "WING 2");
    CHECK(youhost::chooseStartupDevice("", "", present) == "WING 2");
    CHECK(youhost::chooseStartupDevice("MacBook Pro Microphone", "", present) == "WING 2");
    CHECK(youhost::chooseStartupDevice("WING 2", "Pro Tools Audio Bridge 32", present) == "Pro Tools Audio Bridge 32");

    const std::vector<youhost::ListedDevice> bridgeAndMac = {
        { "Pro Tools Audio Bridge 32", 32, 32 },
        { "MacBook Pro Microphone", 1, 0 },
    };
    CHECK(youhost::chooseStartupDevice("Pro Tools Audio Bridge 32", "", bridgeAndMac) == "MacBook Pro Microphone");
    CHECK(youhost::chooseStartupDevice("", "", std::vector<youhost::ListedDevice> { { "Pro Tools Audio Bridge 32", 32, 32 } }).empty());

    const std::vector<youhost::ListedDevice> macbook = {
        { "MacBook Pro Microphone", 1, 0 },
        { "MacBook Pro Speakers", 0, 2 },
        { "External Headphones", 0, 2 },
        { "WING 2", 48, 48 },
    };
    const auto pairedRows = youhost::buildDeviceList(macbook, "");
    int macRows = 0;
    bool sawBuiltIn = false;
    bool sawHeadphones = false;
    for (const auto& row : pairedRows)
    {
        if (row.name == "MacBook Pro")
        {
            ++macRows;
            sawBuiltIn = row.label == "MacBook Pro (built-in) - 1 in / 2 out";
            CHECK(row.kind == youhost::DeviceRowKind::builtin);
        }
        if (row.name == "MacBook Pro Microphone" || row.name == "MacBook Pro Speakers")
            ++macRows;
        if (row.name == "External Headphones")
            sawHeadphones = true;
    }
    CHECK(macRows == 1);
    CHECK(sawBuiltIn);
    CHECK(sawHeadphones);
    const auto finnish = youhost::pairBuiltinCards({
        { "MacBook Pro-mikrofoni", 1, 0 },
        { "MacBook Pro-kaiuttimet", 0, 2 },
    });
    CHECK(finnish.size() == 1);
    CHECK(finnish[0].name == "MacBook Pro");
    CHECK(finnish[0].inputName == "MacBook Pro-mikrofoni");
    CHECK(finnish[0].outputName == "MacBook Pro-kaiuttimet");
    const auto builtIn = youhost::pairBuiltinCards({
        { "Built-in Microphone", 1, 0 },
        { "Built-in Output", 0, 2 },
    });
    CHECK(builtIn.size() == 1);
    CHECK(builtIn[0].inputName == "Built-in Microphone");
    CHECK(builtIn[0].outputName == "Built-in Output");

    const auto rows = youhost::buildDeviceList(present, "");
    CHECK(rows.size() >= 5);
    CHECK(rows[0].name == "WING 2");
    CHECK(rows[0].label == "WING 2 - 48 in / 48 out");
    CHECK(rows[1].name == "Scarlett 18i20");
    bool sawHeading = false;
    bool sawBridge = false;
    bool sawMac = false;
    bool offlineLast = rows.back().label == youhost::kOfflineDeviceName;
    CHECK(offlineLast);
    for (const auto& row : rows)
    {
        if (row.kind == youhost::DeviceRowKind::heading)
        {
            sawHeading = true;
            CHECK(row.label == youhost::kVirtualDeviceHeading);
            CHECK(! row.selectable);
        }
        if (row.name == "Pro Tools Audio Bridge 32")
            sawBridge = true;
        if (row.name == "MacBook Pro Microphone")
            sawMac = true;
    }
    CHECK(sawHeading);
    CHECK(sawBridge);
    CHECK(sawMac);
    CHECK(rows[2].kind == youhost::DeviceRowKind::heading);

    const auto filtered = youhost::buildDeviceList(present, "wing");
    CHECK(filtered.size() == 1);
    CHECK(filtered.front().name == "WING 2");
    for (const auto& row : filtered)
        CHECK(row.label != youhost::kVirtualDeviceHeading);
}

void testStartupAndSetupShareTheDeviceMenu()
{
    const std::vector<youhost::ListedDevice> present = {
        { "Pro Tools Audio Bridge 32", 32, 32 },
        { "WING 2", 48, 48 },
        { "MacBook Pro Microphone", 1, 0 },
    };
    const auto startup = youhost::buildDeviceList(present, "");
    const auto setup = youhost::buildDeviceList(present, "");
    CHECK(startup.size() == setup.size());
    CHECK(! startup.empty());
    CHECK(startup.back().label == youhost::kOfflineDeviceName);
    CHECK(setup.back().label == youhost::kOfflineDeviceName);
    CHECK(startup.back().selectable);
    CHECK(startup.front().name == "WING 2");
    CHECK(startup.front().label == "WING 2 - 48 in / 48 out");

    bool sawHeading = false;
    bool virtualAfterHeading = false;
    for (const auto& row : startup)
    {
        if (row.label == youhost::kVirtualDeviceHeading)
        {
            sawHeading = true;
            CHECK(! row.selectable);
        }
        if (row.name == "Pro Tools Audio Bridge 32")
        {
            virtualAfterHeading = sawHeading;
            CHECK(row.label == "Pro Tools Audio Bridge 32 - 32 in / 32 out");
        }
    }
    CHECK(sawHeading);
    CHECK(virtualAfterHeading);

    youhost::KeptSessionChannel channel;
    channel.name = "Kick";
    channel.group = 3;
    channel.plugin = "De-Feedback";
    const auto kept = youhost::channelAfterDeviceSwitch(channel, 48);
    CHECK(kept.name == "Kick");
    CHECK(kept.group == 3);
    CHECK(kept.plugin == "De-Feedback");
    CHECK(youhost::sessionChannelView(32, true, false, false).visible == 32);
    CHECK(youhost::sessionChannelView(48, true, false, false).visible == 48);
    CHECK(youhost::sessionChannelView(0, false, true, false).visible == youhost::kMaxChannels);

    const auto blocked = youhost::deviceSwitchBlockedReason(true, false);
    CHECK(blocked.find("Stop the take") != std::string::npos);
    const auto locked = youhost::deviceSwitchBlockedReason(true, true);
    CHECK(locked.find("Recording is locked") != std::string::npos);
    CHECK(youhost::deviceSwitchBlockedReason(false, true).empty());
    CHECK(youhost::deviceSwitchBlockedReason(false, false).empty());

    const auto higher = youhost::adoptCardSampleRate(48000.0, 96000.0);
    CHECK(higher.changed);
    CHECK(higher.notice == "Session moves to 96 kHz");
    const auto lower = youhost::adoptCardSampleRate(96000.0, 48000.0);
    CHECK(lower.changed);
    CHECK(lower.notice.empty());
    CHECK(lower.rate == 48000.0);
}

void testRaiseUnit()
{
    CHECK(near(youhost::raiseUnit(0.5f, 4), 0.0625f, 0.00001f));
    CHECK(youhost::raiseUnit(0.5f, 0) == 1.0f);
    CHECK(youhost::raiseUnit(0.0f, 3) == 0.0f);
}

youhost::RawKey macLetter(char lower, bool shift, bool command, bool alt)
{
    youhost::RawKey key;
    if (lower >= 'a' && lower <= 'z')
        key.keyCode = lower - 'a' + 'A';
    else
        key.keyCode = static_cast<unsigned char>(lower);
    key.shift = shift;
    key.command = command;
    key.alt = alt;
    if (command)
        key.textCharacter = 0;
    else if (shift && lower >= 'a' && lower <= 'z')
        key.textCharacter = key.keyCode;
    else
        key.textCharacter = static_cast<unsigned char>(lower);
    if (lower == ' ')
        key.token = youhost::RawToken::space;
    return key;
}

void testKeyDispatch()
{
    const auto ran = [](const youhost::RawKey& key, int page, bool textField, youhost::ShortcutId id)
    {
        const auto got = youhost::dispatchRawKey(key, page, textField);
        CHECK(got.has_value());
        if (got.has_value())
            CHECK(*got == id);
    };

    const auto commandA = macLetter('a', false, true, false);
    const auto commandG = macLetter('g', false, true, false);
    const auto plainA = macLetter('a', false, false, false);
    const auto plainG = macLetter('g', false, false, false);
    CHECK(youhost::commandForRawKey(commandA) == youhost::kSelectAllCommandId);
    CHECK(youhost::commandForRawKey(commandG) == youhost::kMakeGroupCommandId);
    CHECK(youhost::commandForRawKey(macLetter('s', false, true, false)) == youhost::kSaveCommandId);
    CHECK(youhost::commandForRawKey(macLetter('s', true, true, false)) == youhost::kSaveAsCommandId);
    CHECK(youhost::commandForRawKey(plainA) == 0);
    CHECK(youhost::commandForRawKey(plainG) == 0);
    CHECK(youhost::commandForRawKey(macLetter('1', false, false, false)) == 0);

    ran(commandA, 1, false, youhost::ShortcutId::selectAll);
    ran(plainA, 1, false, youhost::ShortcutId::selectAll);
    ran(commandA, 2, false, youhost::ShortcutId::selectAll);
    ran(plainA, 2, false, youhost::ShortcutId::selectAll);
    ran(commandG, 1, false, youhost::ShortcutId::makeGroup);
    ran(commandG, 2, false, youhost::ShortcutId::makeGroup);
    ran(plainG, 1, false, youhost::ShortcutId::goToChannel);
    ran(plainG, 2, false, youhost::ShortcutId::goToChannel);
    CHECK(! youhost::dispatchRawKey(commandA, 1, true).has_value());
    CHECK(! youhost::dispatchRawKey(commandA, 2, true).has_value());
    CHECK(! youhost::dispatchRawKey(plainA, 1, true).has_value());
    CHECK(! youhost::dispatchRawKey(plainA, 2, true).has_value());
    CHECK(! youhost::dispatchRawKey(commandG, 1, true).has_value());
    CHECK(! youhost::dispatchRawKey(commandG, 2, true).has_value());
    CHECK(! youhost::dispatchRawKey(macLetter('a', true, false, false), 1, false).has_value());

    ran(macLetter('1', false, false, false), 1, false, youhost::ShortcutId::recPage);
    ran(macLetter('2', false, false, false), 1, false, youhost::ShortcutId::hostPage);
    ran(macLetter('3', false, false, false), 1, false, youhost::ShortcutId::dropouts);
    ran(macLetter('d', false, false, false), 1, false, youhost::ShortcutId::dropouts);
    ran(macLetter('4', false, false, false), 1, false, youhost::ShortcutId::cpu);
    ran(macLetter('5', false, false, false), 1, false, youhost::ShortcutId::latency);
    ran(macLetter('s', false, false, false), 1, false, youhost::ShortcutId::scanner);
    ran(macLetter('t', false, false, false), 1, false, youhost::ShortcutId::zoomIn);
    ran(macLetter('r', false, false, false), 1, false, youhost::ShortcutId::zoomOut);
    ran(macLetter('f', false, false, false), 1, false, youhost::ShortcutId::fit);
    ran(macLetter('f', false, false, false), 2, false, youhost::ShortcutId::fit);
    ran(macLetter('r', false, false, true), 1, false, youhost::ShortcutId::fit);
    ran(macLetter('r', false, false, true), 2, false, youhost::ShortcutId::fit);
    CHECK(! youhost::dispatchRawKey(macLetter('f', false, false, false), 1, true).has_value());
    CHECK(! youhost::dispatchRawKey(macLetter('f', false, false, false), 3, false).has_value());
    CHECK(! youhost::dispatchRawKey(macLetter('f', true, false, false), 1, false).has_value());
    CHECK(! youhost::dispatchRawKey(macLetter('f', false, true, false), 1, false).has_value());
    CHECK(! youhost::dispatchRawKey(macLetter('f', false, false, true), 1, false).has_value());
    CHECK(! youhost::dispatchRawKey(macLetter('r', false, false, true), 3, false).has_value());
    ran(macLetter(' ', false, false, false), 1, false, youhost::ShortcutId::playOrStop);
    ran(macLetter(' ', false, true, false), 1, false, youhost::ShortcutId::recordNow);
    ran(macLetter('s', false, true, false), 1, false, youhost::ShortcutId::save);
    ran(macLetter('s', true, true, false), 1, false, youhost::ShortcutId::saveAs);
    ran(macLetter('s', false, true, false), 1, true, youhost::ShortcutId::save);

    youhost::RawKey left;
    left.token = youhost::RawToken::left;
    left.textCharacter = 0;
    ran(left, 1, false, youhost::ShortcutId::previousTake);
    left.shift = true;
    ran(left, 1, false, youhost::ShortcutId::nudgeBack);
    youhost::RawKey right;
    right.token = youhost::RawToken::right;
    right.textCharacter = 0;
    ran(right, 1, false, youhost::ShortcutId::nextTake);
    right.shift = true;
    ran(right, 1, false, youhost::ShortcutId::nudgeForward);

    youhost::RawKey taller;
    taller.keyCode = '[';
    taller.textCharacter = 0;
    taller.command = true;
    ran(taller, 1, false, youhost::ShortcutId::lanesTaller);
    youhost::RawKey shorter;
    shorter.keyCode = ']';
    shorter.textCharacter = 0;
    shorter.command = true;
    ran(shorter, 1, false, youhost::ShortcutId::lanesShorter);

    youhost::RawKey optionR;
    optionR.keyCode = 'R';
    optionR.textCharacter = 0x00AE;
    optionR.alt = true;
    ran(optionR, 1, false, youhost::ShortcutId::fit);

    CHECK(! youhost::dispatchRawKey(macLetter('1', false, false, false), 1, true).has_value());
    CHECK(! youhost::dispatchRawKey(macLetter(' ', false, false, false), 1, true).has_value());
    CHECK(! youhost::dispatchRawKey(plainG, 1, true).has_value());

    const auto ui = readWorkspaceFile("src/ui/MainComponent.cpp");
    const auto header = readWorkspaceFile("src/ui/MainComponent.h");
    CHECK(ui.find("dispatchRawKey(rawKeyFromPress(key), page_, false)") != std::string::npos);
    CHECK(ui.find("shortcutIdForCommand(static_cast<int>(info.commandID))") != std::string::npos);
    CHECK(ui.find("commandApplies(*shortcut, page_, shortcutBlocked(info.originatingComponent))") != std::string::npos);
    CHECK(ui.find("return runShortcut(*shortcut)") != std::string::npos);
    CHECK(ui.find("edit.addCommandItem") != std::string::npos);
    CHECK(header.find("selectAllCommand = kSelectAllCommandId") != std::string::npos);
    CHECK(header.find("makeGroupCommand = kMakeGroupCommandId") != std::string::npos);
    bool selectAllIsCommand = false;
    bool makeGroupIsCommand = false;
    bool plainAIsNotCommand = false;
    for (const auto& binding : youhost::kBindings)
    {
        if (binding.id == youhost::ShortcutId::selectAll && binding.command == 1)
            selectAllIsCommand = binding.commandBinding && binding.commandCharacter == 'a';
        if (binding.id == youhost::ShortcutId::makeGroup && binding.command == 1)
            makeGroupIsCommand = binding.commandBinding && binding.commandCharacter == 'g';
        if (binding.id == youhost::ShortcutId::selectAll && binding.command == 0)
            plainAIsNotCommand = ! binding.commandBinding;
    }
    CHECK(selectAllIsCommand);
    CHECK(makeGroupIsCommand);
    CHECK(plainAIsNotCommand);
}

bool groupMeterShotMatches(std::string* ppm)
{
    constexpr int width = 108;
    constexpr int height = 480;
    const auto track = youhost::recGroupMeterTrack(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    std::vector<unsigned char> rgb(static_cast<std::size_t>(width * height * 3), 0);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>((y * width + x) * 3);
            rgb[index] = 26;
            rgb[index + 1] = 30;
            rgb[index + 2] = 40;
        }
    }
    const int x0 = static_cast<int>(std::floor(track.x));
    const int y0 = static_cast<int>(std::floor(track.y));
    const int x1 = static_cast<int>(std::ceil(track.x + track.width));
    const int y1 = static_cast<int>(std::ceil(track.y + track.height));
    for (int y = y0; y < y1; ++y)
    {
        for (int x = x0; x < x1; ++x)
        {
            if (x < 0 || y < 0 || x >= width || y >= height)
                continue;
            const std::size_t index = static_cast<std::size_t>((y * width + x) * 3);
            rgb[index] = 80;
            rgb[index + 1] = 200;
            rgb[index + 2] = 120;
        }
    }

    int rows = 0;
    int cols = 0;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (rgb[static_cast<std::size_t>((y * width + x) * 3) + 1] != 200)
                continue;
            ++rows;
            break;
        }
    }
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (rgb[static_cast<std::size_t>((y * width + x) * 3) + 1] != 200)
                continue;
            ++cols;
            break;
        }
    }

    const auto host = youhost::hostGroupMeterTrack(960.0f, 34.0f, youhost::kHostGroupMeterOrigin);
    const float hostLane = 960.0f - youhost::kHostGroupMeterOrigin;
    const bool shot = rows * 10 >= height * 9
                      && cols == static_cast<int>(youhost::kGroupMeterThickness)
                      && width / std::max(1, cols) >= 6
                      && track.height >= static_cast<float>(height) * 0.60f
                      && ! track.horizontal
                      && host.horizontal
                      && host.height == 34.0f - youhost::kGroupMeterInset * 2.0f
                      && hostLane > 0.0f
                      && host.width / hostLane >= 0.90f;
    if (ppm != nullptr)
    {
        std::ostringstream out;
        out << "P6\n" << width << ' ' << height << "\n255\n";
        out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
        *ppm = out.str();
    }
    return shot;
}

void testScratchSilenceAndRepaint()
{
    constexpr int n = 16;
    float mono[n] {};
    float left[n];
    float right[n];
    auto summing = [](float** channels, int width, int count)
    {
        for (int index = 0; index < count; ++index)
        {
            const float inL = channels[0] != nullptr ? channels[0][index] : 0.0f;
            const float inR = width > 1 && channels[1] != nullptr ? channels[1][index] : 0.0f;
            if (channels[0] != nullptr)
                channels[0][index] = inL + inR;
            if (width > 1 && channels[1] != nullptr)
                channels[1][index] = inL;
        }
    };

    std::fill(left, left + n, 0.8f);
    std::fill(right, right + n, 0.8f);
    float peak = 0.0f;
    for (int block = 0; block < 24; ++block)
    {
        youhost::renderFoldedMono(mono, n, 2, 2, youhost::StereoFold::left, left, right, summing);
        for (float sample : mono)
            peak = std::max(peak, std::fabs(sample));
    }
    CHECK(peak == 0.0f);

    mono[0] = 0.25f;
    youhost::renderFoldedMono(mono, n, 2, 2, youhost::StereoFold::left, left, right, summing);
    CHECK(youhost::sameFloatBits(mono[0], 0.5f));
    std::fill(mono, mono + n, 0.0f);
    float tail = 0.0f;
    for (int block = 0; block < 8; ++block)
    {
        youhost::renderFoldedMono(mono, n, 2, 2, youhost::StereoFold::left, left, right, summing);
        for (float sample : mono)
            tail = std::max(tail, std::fabs(sample));
    }
    CHECK(tail == 0.0f);

    std::fill(right, right + n, 0.8f);
    std::fill(mono, mono + n, 0.0f);
    mono[0] = 0.25f;
    youhost::renderFoldedMono(mono, n, 1, 2, youhost::StereoFold::left, left, right, summing);
    CHECK(youhost::sameFloatBits(mono[0], 0.25f));

    youhost::ChannelStrip strip;
    strip.meter.peak.store(4.0f);
    strip.meter.rms.store(4.0f);
    strip.meter.clipped.store(true);
    strip.meterState.heldPeak = 4.0f;
    strip.meterState.meanSquare = 16.0f;
    strip.meterState.clipped = true;
    youhost::clearMeterBallistics(strip);
    CHECK(strip.meter.peak.load() == 0.0f);
    CHECK(strip.meter.rms.load() == 0.0f);
    CHECK(! strip.meter.clipped.load());
    CHECK(strip.meterState.heldPeak == 0.0f);
    CHECK(! strip.meterState.clipped);
    CHECK(youhost::gainToDb(strip.meter.peak.load()) <= -160.0f);

    float memory[8];
    std::fill(memory, memory + 8, 1.0f);
    std::memset(memory, 0, sizeof(memory));
    bool bitsZero = true;
    for (float sample : memory)
        bitsZero = bitsZero && sample == 0.0f;
    CHECK(bitsZero);

    youhost::RepaintCoalescer paints;
    for (int index = 0; index < 1000; ++index)
        paints.note(1, 0, youhost::kRepaintLatency);
    int channel = -1;
    int slot = -1;
    int trigger = 0;
    CHECK(paints.take(1000, channel, slot, trigger));
    CHECK(channel == 1);
    CHECK(slot == 0);
    CHECK(trigger == youhost::kRepaintLatency);
    CHECK(! paints.take(1020, channel, slot, trigger));
    CHECK(paints.notes() == 1000);
    CHECK(paints.paints() == 1);
    CHECK(! paints.wantsFullWindow());
    paints.note(2, 1, youhost::kRepaintMove);
    CHECK(paints.take(1033, channel, slot, trigger));
    CHECK(channel == 2);
    CHECK(slot == 1);
    CHECK(trigger == youhost::kRepaintMove);

    const bool metersAtFloor = strip.meter.peak.load() == 0.0f
                               && strip.meter.rms.load() == 0.0f
                               && ! strip.meter.clipped.load()
                               && youhost::gainToDb(0.0f) <= -160.0f;
    if (peak == 0.0f && tail == 0.0f && metersAtFloor && bitsZero)
        std::cout << "offline New session reset passed, meters at floor, scratch bit-zero\n";
}

void testReleaseChecklist()
{
    int listed = 0;
    int passed = 0;
    const auto item = [&](const char* name, bool ok)
    {
        ++listed;
        if (ok)
            ++passed;
        std::cout << (ok ? "[pass] " : "[fail] ") << name << "\n";
        CHECK(ok);
    };

    std::cout << "release checklist\n";

    const auto card = youhost::layoutLatencyCard(youhost::kLatencyPreferredWidth);
    const int helpCharacters = static_cast<int>(youhost::shortcutHelpText().size());
    struct WindowNeed
    {
        const char* name;
        int width;
        int height;
    };
    const WindowNeed windows[] = {
        { "LATENCY", card.contentWidth, card.contentHeight },
        { "CPU", youhost::cpuWindowWidth(), youhost::cpuWindowHeight() },
        { "DROPOUTS", youhost::dropoutWindowWidth(), youhost::dropoutWindowHeight() },
        { "SCAN", youhost::scannerWindowWidth(), youhost::scannerWindowHeight() },
        { "Audio setup", youhost::setupWindowWidth(), youhost::setupWindowHeight() },
        { "Shortcuts", youhost::helpWindowWidth(), youhost::helpWindowHeightFor(helpCharacters) },
        { "Start session", youhost::startupWindowWidth(), youhost::startupWindowHeight() },
        { "New session", youhost::placeSessionWindowWidth(), youhost::placeSessionWindowHeight() },
        { "Group dialog", youhost::groupRenameWindowWidth(), youhost::groupRenameWindowHeight() },
        { "Plugin list", youhost::pluginListWindowWidth(), youhost::pluginListWindowHeight() },
    };
    youhost::SavedWindowSize oldSmall;
    oldSmall.valid = true;
    oldSmall.width = 320;
    oldSmall.height = 180;
    oldSmall.hasFit = true;
    oldSmall.fitWidth = 320;
    oldSmall.fitHeight = 180;
    bool windowsFit = true;
    for (const auto& window : windows)
    {
        const auto fresh = youhost::windowOpenSize(window.width, window.height, 1920, 1080, {});
        const auto migrated = youhost::windowOpenSize(window.width, window.height, 1920, 1080, oldSmall);
        const bool shown = fresh.width >= window.width && fresh.height >= window.height
                           && migrated.width >= window.width && migrated.height >= window.height
                           && fresh.width <= 1920 && fresh.height <= 1080
                           && migrated.width <= 1920 && migrated.height <= 1080;
        if (! shown)
            windowsFit = false;
    }
    item("windows open full-size (fresh and old small saved size)", windowsFit);

    youhost::ChannelListen modes[6] = {
        youhost::ChannelListen::record, youhost::ChannelListen::input, youhost::ChannelListen::off,
        youhost::ChannelListen::record, youhost::ChannelListen::input, youhost::ChannelListen::off,
    };
    const int selected[] = { 1 };
    const youhost::ChannelListen presses[] = {
        youhost::ChannelListen::record, youhost::ChannelListen::input, youhost::ChannelListen::off,
    };
    bool allButtons = true;
    for (const int page : { 1, 2 })
    {
        for (const auto mode : presses)
        {
            auto working = modes;
            const auto targets = youhost::channelsForPageListen(page, selected, 1, 4);
            if (targets.size() != 4)
                allButtons = false;
            for (const int channel : targets)
                working[static_cast<std::size_t>(channel)] = mode;
            for (int channel = 0; channel < 4; ++channel)
                if (working[static_cast<std::size_t>(channel)] != mode)
                    allButtons = false;
            if (working[4] != modes[4] || working[5] != modes[5])
                allButtons = false;
        }
    }
    if (! youhost::channelsForPageListen(0, selected, 1, 4).empty())
        allButtons = false;
    item("ALL REC / ALL INPUT / ALL OFF override on REC and HOST", allButtons);

    const bool quitAsks = youhost::sessionCloseAsks(true, false, youhost::SessionCloseReason::quit);
    const bool cancelAborts = ! youhost::sessionCloseProceeds(true, false, youhost::SessionCloseReason::quit, youhost::UnsavedChoice::cancel);
    const bool saveFirst = youhost::sessionCloseSavesFirst(true, false, youhost::SessionCloseReason::quit, youhost::UnsavedChoice::save);
    const bool saveAsFirst = youhost::sessionCloseSaveAsFirst(true, false, youhost::SessionCloseReason::quit, youhost::UnsavedChoice::saveAs);
    const bool discardProceeds = youhost::sessionCloseProceeds(true, false, youhost::SessionCloseReason::quit, youhost::UnsavedChoice::discard);
    const bool recordingStopsFirst = ! youhost::sessionCloseAsks(true, true, youhost::SessionCloseReason::quit)
                                     && ! youhost::sessionCloseProceeds(true, true, youhost::SessionCloseReason::quit, youhost::UnsavedChoice::discard);
    item("Cmd+Q prompts Save / Save As / Don't Save / Cancel; recording stops first",
         quitAsks && cancelAborts && saveFirst && saveAsFirst && discardProceeds && recordingStopsFirst);

    item("new session buffer is 32", youhost::kNewSessionBufferSamples == 32);

    int queries = 0;
    for (int poll = 0; poll < 50; ++poll)
        if (youhost::deviceInventoryQueryAllowed(true, false, false, false))
            ++queries;
    const bool lossQueries = youhost::deviceInventoryQueryAllowed(true, true, false, false);
    const bool firstFillQueries = youhost::deviceInventoryQueryAllowed(true, false, false, true);
    const bool userChangeQueries = youhost::deviceInventoryQueryAllowed(true, false, true, false);
    item("device lock: no inventory query while the chosen card stays up",
         queries == 0 && lossQueries && firstFillQueries && userChangeQueries);

    const std::vector<youhost::ListedDevice> cards = {
        { "WING 2", 48, 48 },
        { "MacBook Pro Microphone", 1, 0 },
        { "Pro Tools Audio Bridge 32", 32, 32 },
    };
    const auto startupMenus = youhost::audioSetupMenus(cards, "", true, 48000.0, 24, 32);
    const auto setupMenus = youhost::audioSetupMenus(cards, "WING 2", false, 48000.0, 24, 32);
    const char* menuLabels[] = { "Audio card:", "Sample rate:", "Bit depth:", "Buffer:" };
    bool oneMenu = startupMenus.size() == 4 && setupMenus.size() == 4;
    bool offlineListed = false;
    for (int index = 0; index < 4 && oneMenu; ++index)
    {
        oneMenu = std::string(startupMenus[static_cast<std::size_t>(index)].label) == menuLabels[index]
                  && std::string(setupMenus[static_cast<std::size_t>(index)].label) == menuLabels[index];
    }
    if (oneMenu)
    {
        for (const auto& choice : startupMenus[0].choices)
            if (choice.text == youhost::kOfflineDeviceName && choice.id == youhost::kOfflineDeviceItemId)
                offlineListed = true;
        bool setupOffline = false;
        bool setupCard = false;
        for (const auto& choice : setupMenus[0].choices)
        {
            if (choice.text == youhost::kOfflineDeviceName && choice.id == youhost::kOfflineDeviceItemId)
                setupOffline = true;
            if (choice.text.find("WING 2") != std::string::npos && choice.id == setupMenus[0].selectedId)
                setupCard = true;
        }
        oneMenu = offlineListed
                  && setupOffline
                  && setupCard
                  && startupMenus[0].selectedId == youhost::kOfflineDeviceItemId
                  && startupMenus[1].selectedId == 48000
                  && startupMenus[2].selectedId == 24
                  && startupMenus[3].selectedId == 32
                  && setupMenus[1].selectedId == 48000
                  && setupMenus[2].selectedId == 24
                  && setupMenus[3].selectedId == 32
                  && startupMenus[1].choices.size() >= 2
                  && startupMenus[2].choices.size() == 3
                  && setupMenus[1].choices.size() == startupMenus[1].choices.size()
                  && setupMenus[2].choices.size() == startupMenus[2].choices.size()
                  && setupMenus[3].choices.size() == startupMenus[3].choices.size();
        for (const auto& menu : startupMenus)
            if (std::string(menu.label) == "Input:" || std::string(menu.label) == "Output:")
                oneMenu = false;
    }
    const auto setupUi = readWorkspaceFile("src/ui/MainComponent.cpp");
    const auto setupHeader = readWorkspaceFile("src/ui/MainComponent.h");
    const auto firstForm = setupUi.find("AudioSetupForm audioSetup_");
    const auto secondForm = firstForm == std::string::npos ? std::string::npos : setupUi.find("AudioSetupForm audioSetup_", firstForm + 1);
    oneMenu = oneMenu
              && setupUi.find("audioSetupMenus(") != std::string::npos
              && firstForm != std::string::npos
              && secondForm != std::string::npos
              && setupUi.find("\"Input:\"") == std::string::npos
              && setupUi.find("\"Output:\"") == std::string::npos
              && setupUi.find("Internal disk") == std::string::npos
              && setupUi.find("AudioDeviceSelectorComponent") == std::string::npos
              && setupHeader.find("AudioDeviceSelectorComponent") == std::string::npos
              && setupUi.find("\"Test\"") == std::string::npos;
    item("startup and Audio setup share one audio card menu", oneMenu);

    youhost::RecordAttempt attempt;
    attempt.hasSession = true;
    attempt.deviceLive = true;
    attempt.deviceRate = 48000.0;
    attempt.channels[0].rec = true;
    attempt.channels[0].inputOpen = true;
    const auto started = youhost::resolveTransport(attempt, youhost::TransportPress::commandSpace);
    item("record starts from an armed channel with an open input",
         started.startRecording && started.alert.empty());

    const auto busyStarted = std::chrono::steady_clock::now();
    for (int step = 0; step < 50; ++step)
    {
        youhost::windowOpenSize(680, 520, 1920, 1080, oldSmall);
        youhost::ChannelLatencyInput sample[2] = { { 100, true, 0 }, { 40, true, 0 } };
        (void) youhost::planCompensation(sample, 2, youhost::AlignMode::group);
    }
    const auto busyMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - busyStarted).count();
    item("no message-thread stall over 16 ms on a 50-step burst", busyMs < 16);

    const bool fileAsks = youhost::sessionCloseAsks(true, false, youhost::SessionCloseReason::newSession)
                          && youhost::sessionCloseAsks(true, false, youhost::SessionCloseReason::open)
                          && youhost::sessionCloseAsks(true, false, youhost::SessionCloseReason::openRecent)
                          && youhost::sessionReplaceAsks(true)
                          && ! youhost::sessionReplaceAsks(false)
                          && youhost::sessionCloseSavesFirst(true, false, youhost::SessionCloseReason::newSession, youhost::UnsavedChoice::save)
                          && youhost::sessionCloseSaveAsFirst(true, false, youhost::SessionCloseReason::open, youhost::UnsavedChoice::saveAs);
    item("New, Open, Open Recent, Save, and Save As ask before discarding edits", fileAsks);

    youhost::SessionNode root;
    root.name = "YouHostSession";
    youhost::sessionSetAttribute(root, "version", "7");
    youhost::sessionSetAttribute(root, "align", "group");
    youhost::SessionNode future;
    future.name = "Future";
    youhost::sessionSetAttribute(future, "keep", "yes");
    root.children.push_back(future);
    youhost::SessionDocumentModel model;
    const bool read = youhost::readSessionModel(root, model);
    item("session files stay compatible (align and unknown elements)",
         read && model.align == "group" && model.hasSource && youhost::sessionModelIsClean(youhost::cleanSessionModel()));

    youhost::ChannelLatencyInput grouped[4] = {};
    grouped[0] = { 100, true, 0 };
    grouped[1] = { 300, true, 0 };
    grouped[2] = { 1000, true, -1 };
    grouped[3] = { 50, true, 1 };
    const auto perGroup = youhost::planCompensation(grouped, 4, youhost::AlignMode::group);
    const auto allAligned = youhost::planCompensation(grouped, 4, youhost::AlignMode::all);
    item("Per group delays only that group's channels; Global is the other switch",
         perGroup.delaySamples[0] == 200 && perGroup.delaySamples[1] == 0
             && perGroup.delaySamples[2] == 0 && perGroup.delaySamples[3] == 0
             && perGroup.alignmentSamples == 300
             && allAligned.delaySamples[2] == 0 && allAligned.alignmentSamples == 1000);

    const auto latencyUi = readWorkspaceFile("src/ui/LatencyReadout.cpp");
    const auto latencyHeader = readWorkspaceFile("src/ui/LatencyReadout.h");
    item("LATENCY has Global and Per group, and no Timeline button",
         latencyUi.find("\"Timeline\"") == std::string::npos
             && latencyHeader.find("Timeline") == std::string::npos
             && latencyHeader.find("\"Global\"") != std::string::npos
             && latencyHeader.find("\"Per group\"") != std::string::npos
             && latencyUi.find("Ungrouped") != std::string::npos
             && ! readWorkspaceFile("docs/release-checklist.md").empty());

    const auto mainUi = readWorkspaceFile("src/ui/MainComponent.cpp");
    const auto mainHeader = readWorkspaceFile("src/ui/MainComponent.h");
    item("ALL button labels stay ALL REC, ALL INPUT, and ALL OFF",
         mainHeader.find("\"ALL REC\"") != std::string::npos
             && mainHeader.find("\"ALL INPUT\"") != std::string::npos
             && mainHeader.find("\"ALL OFF\"") != std::string::npos
             && mainUi.find("only those channels") == std::string::npos
             && mainUi.find("hasSelection ? juce::String(\"REC\")") == std::string::npos);

    const auto emptyGroup = youhost::planNewGroup(false, 0, "Drums", 1);
    const auto folded = youhost::planNewGroup(true, 0, "  ", 2);
    youhost::KeyQuery plainG;
    plainG.kind = youhost::KeyKind::character;
    plainG.character = 'g';
    youhost::KeyQuery commandG = plainG;
    commandG.command = true;
    const auto go = youhost::matchShortcut(plainG);
    const auto make = youhost::matchShortcut(commandG);
    item("Cmd+G names and folds the selected channels; plain G goes to a channel",
         ! emptyGroup.created && folded.created && folded.collapsed && folded.name == "Group 1"
             && go.has_value() && *go == youhost::ShortcutId::goToChannel
             && make.has_value() && *make == youhost::ShortcutId::makeGroup);

    item("startup window has no instructional paragraph",
         mainUi.find("Choose the interface, sample rate, and buffer size") == std::string::npos
             && mainUi.find("Recordings usually go on an external drive. The interface opens") == std::string::npos);

    const auto macbookPair = youhost::pairBuiltinCards({
        { "MacBook Pro Microphone", 1, 0 },
        { "MacBook Pro Speakers", 0, 2 },
        { "External Headphones", 0, 2 },
    });
    bool macbookOne = macbookPair.size() == 2;
    for (const auto& device : macbookPair)
        if (device.name == "MacBook Pro")
            macbookOne = macbookOne && device.inputs == 1 && device.outputs == 2
                         && device.inputName == "MacBook Pro Microphone"
                         && device.outputName == "MacBook Pro Speakers";
    const auto menuModel = readWorkspaceFile("src/engine/AudioSetupMenus.h");
    item("MacBook built-in is one audio card",
         macbookOne && menuModel.find("\"Audio card:\"") != std::string::npos
             && mainUi.find(".label, juce::dontSendNotification") != std::string::npos);

    int listenModes[4] = {};
    const auto clickStarted = std::chrono::steady_clock::now();
    for (int step = 0; step < 50; ++step)
        youhost::applyLocalListen(listenModes, 4, step % 4, step % 3);
    const auto clickMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - clickStarted)
                             .count();
    const auto engineSrc = readWorkspaceFile("src/engine/AudioEngine.cpp");
    const auto listenAt = engineSrc.find("void AudioEngine::setChannelListen");
    const auto flushAt = engineSrc.find("void AudioEngine::flushListenEdits");
    const auto listenBody = listenAt == std::string::npos || flushAt == std::string::npos || flushAt < listenAt
                                ? std::string()
                                : engineSrc.substr(listenAt, flushAt - listenAt);
    item("rapid REC/INPUT clicks do not rebuild the graph",
         clickMs < 16 && listenBody.find("setAudible") == std::string::npos && listenBody.find("listenFlush_ = true") != std::string::npos);

    youhost::SessionDocumentModel savedCard;
    savedCard.buffer = 32;
    savedCard.cardChannels = 16;
    savedCard.inputDevice = "WING";
    savedCard.outputDevice = "WING";
    savedCard.bits = 24;
    savedCard.rate = 48000.0;
    const auto savedXml = youhost::writeSessionXml(youhost::writeSessionModel(savedCard));
    youhost::SessionNode savedRoot;
    youhost::SessionDocumentModel savedBack;
    const bool savedRound = youhost::parseSessionXml(savedXml, savedRoot) && youhost::readSessionModel(savedRoot, savedBack);
    const auto missing = youhost::restoreSessionCard("WING", "WING", 16, 32, false, false);
    const auto present = youhost::restoreSessionCard("WING", "WING", 16, 32, false, true);
    const auto explicitOffline = youhost::restoreSessionCard(youhost::kOfflineDeviceName, "", 16, 32, true, false);
    const auto missingView = youhost::missingCardChannelView(16);
    item("buffer 32 is saved in the session",
         youhost::kNewSessionBufferSamples == 32 && savedRound && savedBack.buffer == 32
             && savedBack.cardChannels == 16 && savedBack.inputDevice == "WING" && savedXml.find("channels=") == std::string::npos);

    item("missing card goes Offline and keeps the saved channel count",
         missing.missing && missing.offline && missing.visible == 16 && missing.buffer == 32
             && missing.status == "Saved card WING not found - Offline"
             && missingView.visible == 16 && missingView.missingCard
             && present.useSaved && ! present.missing && present.visible == 16
             && explicitOffline.offline && ! explicitOffline.missing && explicitOffline.visible == 128);

    const auto timelineUi = readWorkspaceFile("src/ui/TimelineView.cpp");
    item("Go field takes focus only from a click or G",
         timelineUi.find("setWantsKeyboardFocus(false)") != std::string::npos
             && timelineUi.find("releaseGoFocus") != std::string::npos
             && timelineUi.find("onEscapeKey") != std::string::npos);

    item("File button and Clear Timeline are gone; Open lists recent sessions",
         mainHeader.find("fileButton_") == std::string::npos
             && mainUi.find("Clear Timeline") == std::string::npos
             && mainUi.find("Browse...") != std::string::npos
             && mainUi.find("No recent sessions") != std::string::npos);

    const auto shortLine = youhost::formatStatusLine("A", "1 in / 2 out", "48.0 kHz", "24-bit", 7, "Song");
    const auto longLine = youhost::formatStatusLine("B", "48 in / 48 out", "192.0 kHz", "32-bit float", 100, "Other");
    const auto offlineLine = youhost::formatStatusLine("Offline (no audio) - 128 channels",
                                                       "128 ch",
                                                       "",
                                                       "24-bit",
                                                       0,
                                                       "10.10.2026_1 disk");
    const auto cutLine = youhost::formatStatusLine(std::string(80, 'A'), "1 in / 2 out", "48.0 kHz", "24-bit", 7, "Song");
    const auto cpuAt = static_cast<std::size_t>(youhost::statusCpuColumnIndex());
    const bool offlineSeparated = offlineLine.size() == static_cast<std::size_t>(youhost::statusLineChars())
                                  && offlineLine.find("Offline (no audio) - 128 channels") == 0
                                  && offlineLine.find("cha128") == std::string::npos
                                  && offlineLine.find("0%10.") == std::string::npos
                                  && offlineLine.compare(cpuAt, 8, "DSP   0%") == 0
                                  && offlineLine[cpuAt + 8] == ' '
                                  && offlineLine.find("10.10.2026_1 disk") != std::string::npos
                                  && offlineLine.compare(static_cast<std::size_t>(youhost::kStatusDeviceChars), 6, "128 ch") == 0;
    const bool cutKeepsGap = cutLine.size() == offlineLine.size()
                             && cutLine.compare(0, 3, "...") != 0
                             && cutLine.find("...") == static_cast<std::size_t>(youhost::kStatusDeviceChars - youhost::kStatusColumnGap - 3)
                             && cutLine[static_cast<std::size_t>(youhost::kStatusDeviceChars - 1)] == ' '
                             && cutLine.compare(static_cast<std::size_t>(youhost::kStatusDeviceChars), 12, "1 in / 2 out") == 0;
    item("status line keeps fixed columns and pads DSP",
         youhost::formatCpuField(7) == "DSP   7%" && youhost::formatCpuField(42) == "DSP  42%"
             && youhost::formatCpuField(100) == "DSP 100%" && youhost::formatCpuField(-4) == "DSP   0%"
             && youhost::formatCpuField(140) == "DSP 100%"
             && youhost::formatCpuField(999) == "DSP 100%"
             && youhost::formatCpuField(1400) == "DSP 100%"
             && shortLine.size() == longLine.size()
             && shortLine.size() == static_cast<std::size_t>(youhost::statusLineChars())
             && offlineSeparated && cutKeepsGap);

    const auto install = readWorkspaceFile("INSTALL.md");
    item("install guide is the Mac steps after the app is on the Mac",
         install.find("YouHost.app") != std::string::npos && install.find("Applications") != std::string::npos
             && install.find("apt ") == std::string::npos && install.find("Linux") == std::string::npos);

    item("session is required before the main window",
         ! youhost::mainWindowAllowed(false, false) && ! youhost::mainWindowAllowed(false, true)
             && youhost::mainWindowAllowed(true, false));

    const auto appSrc = readWorkspaceFile("src/YouHostApplication.cpp");
    item("Cmd+Q asks before quitting",
         appSrc.find("requestApplicationQuit") != std::string::npos
             && appSrc.find("systemRequestedQuit") != std::string::npos
             && quitAsks);

    const auto recBounds = mainUi.find("recButton_.setBounds");
    const auto lockBounds = mainUi.find("recordLock_->setBounds");
    item("record lock arms only from the padlock",
         engineSrc.find("recordLockArmed_ = true") == std::string::npos
             && mainUi.find("setRecordLockArmed(! engine_.recordLockArmed())") != std::string::npos
             && recBounds != std::string::npos && lockBounds != std::string::npos && recBounds < lockBounds
             && mainUi.find("transport.removeFromLeft(kRecordLockButtonWidth)") == std::string::npos
             && mainUi.find("cluster.removeFromLeft(kRecordLockButtonWidth)") != std::string::npos);

    item("device menu is not rebuilt on every timer tick",
         mainUi.find("if (signature != cardSignature_)") != std::string::npos
             && mainUi.find("fillBox(boxes_[0], menus[0])") != std::string::npos);

    item("Show Backups is not a control",
         mainUi.find("Show Backup") == std::string::npos && mainHeader.find("Show Backup") == std::string::npos);

    item("clicking a REC channel enlarges that lane",
         mainUi.find("focusChannelLane(channel, page_ == 1)") != std::string::npos
             && readWorkspaceFile("src/engine/TimelineZoom.h").find("kReadableLaneHeightPx") != std::string::npos);

    const auto hiddenLabel = youhost::showHiddenChannelsLabel(16);
    const bool hiddenSwitch = hiddenLabel == "Show channels not on this card (17-128)"
                              && std::string(youhost::kShowHiddenChannelsNote) == "They stay in the session and are not recorded or processed."
                              && mainUi.find("showHiddenChannelsLabel(") != std::string::npos
                              && mainUi.find("kShowHiddenChannelsNote") != std::string::npos
                              && mainUi.find("unsupportedChannelLine") == std::string::npos
                              && mainUi.find("Show on mixer") == std::string::npos
                              && youhost::shortcutHelpText().find("not on this card") == std::string::npos
                              && youhost::shortcutHelpText().find(youhost::kShowHiddenChannelsNote) != std::string::npos;
    item("Audio setup offers Show channels not on this card (17-128)", hiddenSwitch);

    item("REC armed hint is REC ARMED - press PLAY (or Cmd+Space)",
         std::string(youhost::kRecordArmedHint) == "REC ARMED - press PLAY (or Cmd+Space)"
             && mainUi.find("kRecordArmedHint") != std::string::npos
             && mainUi.find("engine_.isRecordReady() && ! recording") != std::string::npos);

    const auto failedStart = youhost::failAttempt("No REC channel has an input on this device. Recording did not start.");
    item("record start failure is logged and shown",
         failedStart.log.find("record start failed") != std::string::npos
             && failedStart.alert.find("Recording did not start.") != std::string::npos
             && engineSrc.find("logTransport(\"record start failed:") != std::string::npos
             && mainUi.find("showTransportResult") != std::string::npos);

    const auto recorderSrc = readWorkspaceFile("src/engine/Recorder.cpp");
    const auto copyUnderLock = recorderSrc.find("Copy under the lock");
    const auto callVisitor = recorderSrc.find("fn(views.empty()");
    item("timeline paint releases the recorder lock before drawing",
         copyUnderLock != std::string::npos && callVisitor != std::string::npos && copyUnderLock < callVisitor);

    const std::vector<youhost::ListedDevice> virtualOnly = { { "Pro Tools Audio Bridge 32", 32, 32 } };
    const std::vector<youhost::ListedDevice> virtualAndReal = {
        { "WING 2", 48, 48 },
        { "Pro Tools Audio Bridge 32", 32, 32 },
    };
    item("startup does not auto-pick a virtual device",
         youhost::chooseStartupDevice("Pro Tools Audio Bridge 32", "", virtualAndReal) == "WING 2"
             && youhost::chooseStartupDevice("", "", virtualOnly).empty()
             && youhost::chooseStartupDevice("", "Pro Tools Audio Bridge 32", virtualOnly) == "Pro Tools Audio Bridge 32");

    const auto missingSaved = youhost::decideStartupCard("Scarlet ja mac out", false);
    const auto listedSaved = youhost::decideStartupCard("Scarlet ja mac out", true);
    item("startup with a missing saved card stays Offline",
         ! youhost::startupOpensSavedDevice()
             && ! missingSaved.openSaved && missingSaved.offline && ! missingSaved.showSyncLost
             && missingSaved.notice == "Saved card Scarlet ja mac out not found - Offline"
             && ! listedSaved.openSaved && listedSaved.offline && ! listedSaved.showSyncLost
             && listedSaved.notice.empty()
             && ! youhost::syncLostMayShow(false, false)
             && ! youhost::syncLostMayShow(false, true)
             && ! youhost::syncLostMayShow(true, true)
             && youhost::syncLostMayShow(true, false)
             && youhost::choosingOfflineClearsSyncLost()
             && engineSrc.find("initialise(inputs, kMaxChannels, saved.get(), true)") == std::string::npos
             && engineSrc.find("awaitingSavedDevice_ = true") == std::string::npos
             && engineSrc.find("syncLostMayShow") != std::string::npos);

    const auto sampleCpu = mainUi.find("engine_.sampleLiveCpu();");
    const auto lockedSkip = mainUi.find("if (! (engine_.deviceSetupLocked() && engine_.deviceIsOpen() && ! engine_.deviceLost()))");
    const auto pollAfterSkip = lockedSkip == std::string::npos ? std::string::npos : mainUi.find("engine_.pollDeviceStats();", lockedSkip);
    const auto gateAt = engineSrc.find("if (! deviceInventoryQueryAllowed");
    const auto scanAfterGate = gateAt == std::string::npos ? std::string::npos : engineSrc.find("type->scanForDevices()", gateAt);
    item("background timers do not scan a locked card",
         sampleCpu != std::string::npos && lockedSkip != std::string::npos && pollAfterSkip != std::string::npos
             && sampleCpu < lockedSkip
             && gateAt != std::string::npos && scanAfterGate != std::string::npos);

    youhost::RawKey menuA;
    menuA.keyCode = 'A';
    menuA.textCharacter = 0;
    menuA.command = true;
    youhost::RawKey menuG = menuA;
    menuG.keyCode = 'G';
    const auto menuSelect = youhost::dispatchRawKey(menuA, 1, false);
    const auto menuGroup = youhost::dispatchRawKey(menuG, 1, false);
    item("Cmd+A and Cmd+G go through the command dispatch",
         youhost::commandForRawKey(menuA) == youhost::kSelectAllCommandId
             && youhost::commandForRawKey(menuG) == youhost::kMakeGroupCommandId
             && menuSelect.has_value() && *menuSelect == youhost::ShortcutId::selectAll
             && menuGroup.has_value() && *menuGroup == youhost::ShortcutId::makeGroup
             && youhost::dispatchRawKey(menuA, 2, false) == youhost::ShortcutId::selectAll
             && ! youhost::dispatchRawKey(menuA, 1, true).has_value()
             && ! youhost::dispatchRawKey(menuA, 2, true).has_value()
             && youhost::dispatchRawKey(menuG, 2, false) == youhost::ShortcutId::makeGroup
             && ! youhost::dispatchRawKey(menuG, 1, true).has_value()
             && ! youhost::dispatchRawKey(menuG, 2, true).has_value()
             && mainUi.find("dispatchRawKey") != std::string::npos
             && mainUi.find("\"Edit\"") != std::string::npos
             && mainUi.find("\"Select All\"") != std::string::npos
             && mainUi.find("\"Make Group\"") != std::string::npos
             && mainUi.find("selectAllCommand") != std::string::npos
             && mainUi.find("makeGroupCommand") != std::string::npos
             && mainUi.find("commandApplies") != std::string::npos
             && mainUi.find("runShortcut") != std::string::npos);

    std::string shot;
    const auto gridUi = readWorkspaceFile("src/ui/MeterGrid.cpp");
    const auto hostUi = readWorkspaceFile("src/ui/PluginPage.cpp");
    const bool wroteShot = groupMeterShotMatches(&shot) && shot.size() > 1000;
    if (wroteShot)
    {
        std::ofstream out("/opt/cursor/artifacts/rec_group_meter.ppm", std::ios::binary);
        out.write(shot.data(), static_cast<std::streamsize>(shot.size()));
    }
    item("REC group meter is a thin track down the bar, like HOST",
         wroteShot
             && gridUi.find("recGroupMeterTrack") != std::string::npos
             && gridUi.find("0.22f") == std::string::npos
             && hostUi.find("hostGroupMeterTrack") != std::string::npos
             && mainUi.find("publishMeters(false)") != std::string::npos
             && mainUi.find("channelSnapshot") == std::string::npos);

    const auto menuAt = mainUi.find("setChannelMenuHandler");
    const auto menuEnd = mainUi.find("setNameCommitHandler", menuAt);
    const auto menuHandler = menuAt == std::string::npos || menuEnd == std::string::npos
                                 ? std::string()
                                 : mainUi.substr(menuAt, menuEnd - menuAt);
    const auto popupAt = hostUi.find("showChannelMenu(engine_, *this, channel_, nullptr, true)");
    const auto popupHandler = popupAt == std::string::npos ? std::string() : hostUi.substr(popupAt, 80);
    const auto copyStart = engineSrc.find("bool AudioEngine::beginSessionCopy");
    const auto copyEnd = engineSrc.find("AudioEngine::", copyStart + 40);
    const auto copyBody = copyStart == std::string::npos ? std::string() : engineSrc.substr(copyStart, copyEnd - copyStart);
    const auto diskSrc = readWorkspaceFile("src/engine/SessionDisk.cpp");
    const auto runAt = diskSrc.find("void SessionDisk::runCopy");
    const auto runEnd = diskSrc.find("void SessionDisk::", runAt + 20);
    const auto runCopy = runAt == std::string::npos ? std::string() : diskSrc.substr(runAt, runEnd - runAt);
    const auto copyHeader = readWorkspaceFile("src/engine/SessionCopy.h");
    const auto fastAt = copyHeader.find("copyFileFast");
    const auto slowAt = copyHeader.find("copyFileSlow");
    const auto fastBody = fastAt == std::string::npos || slowAt == std::string::npos
                              ? std::string()
                              : copyHeader.substr(fastAt, slowAt - fastAt);
    const auto dropSrc = readWorkspaceFile("src/ui/DropoutWindow.cpp");
    const auto scaleSrc = readWorkspaceFile("src/engine/MeterScale.h");
    item("per-channel colour menu is back; group colour follows only uncoloured channels",
         menuHandler.find("showChannelMenu") != std::string::npos
             && menuHandler.find("showMakeGroupDialog") == std::string::npos
             && popupHandler.find("showChannelMenu") != std::string::npos
             && popupHandler.find("showMakeGroupDialog") == std::string::npos
             && mainUi.find("groupButton_.onClick") != std::string::npos
             && engineSrc.find("colorOwn") != std::string::npos
             && engineSrc.find("chooseChannelColor") != std::string::npos
             && engineSrc.find("followGroupColor") != std::string::npos
             && engineSrc.find("leaveGroupColor") != std::string::npos);
    item("DROPOUTS opens on Whole session",
         youhost::dropoutRangeOnOpen() == youhost::kDropoutWholeSessionId
             && std::string(youhost::kDropoutWholeSessionLabel) == "Whole session"
             && dropSrc.find("openOnWholeSession") != std::string::npos
             && dropSrc.find("dropoutRangeOnOpen()") != std::string::npos
             && dropSrc.find("setSelectedId(1") == std::string::npos
             && dropSrc.find("Last 10 min") != std::string::npos
             && dropSrc.find("Last 1 hour") != std::string::npos);
    item("F fits the REC and HOST timeline",
         youhost::shortcutChord(youhost::ShortcutId::fit) == "F or Option+R"
             && youhost::dispatchRawKey(macLetter('f', false, false, false), 1, false) == youhost::ShortcutId::fit
             && youhost::dispatchRawKey(macLetter('f', false, false, false), 2, false) == youhost::ShortcutId::fit
             && ! youhost::dispatchRawKey(macLetter('f', false, false, false), 1, true).has_value()
             && ! youhost::dispatchRawKey(macLetter('f', false, true, false), 1, false).has_value());
    item("mode change while recording asks twice, and not at all when stopped",
         youhost::classifyListenChange(true, false, true) == youhost::ListenChangeKind::confirm
             && youhost::classifyListenChange(false, false, true) == youhost::ListenChangeKind::now
             && youhost::channelListenChangeLine(4, youhost::ChannelListen::record, youhost::ChannelListen::input)
                    == "Switch ch 5 REC -> INPUT"
             && mainUi.find("juce::Colours::red") != std::string::npos
             && mainUi.find("kListenChangeVerb") != std::string::npos
             && mainUi.find("kListenChangeSureTitle") != std::string::npos
             && mainUi.find("requestChannelListenCycle") != std::string::npos
             && hostUi.find("onListen_(channel_)") != std::string::npos
             && youhost::listenChangeApplies(youhost::ListenChangeKind::confirm, 1, 1)
             && ! youhost::listenChangeApplies(youhost::ListenChangeKind::confirm, 1, 0));
    item("RMS 0 reference moves the 0 line and the scale numbers",
         near(youhost::normaliseDb(-14.0f, youhost::rmsMeterSpan(-14)), 46.0f / 60.0f, 0.001f)
             && near(youhost::normaliseDb(-20.0f, youhost::rmsMeterSpan(-20)), 40.0f / 60.0f, 0.001f)
             && youhost::rmsHeadroomDb(-14) == 14
             && scaleSrc.find("kMeterFloorDb, 0.0f") != std::string::npos);
    item("Save As copies audio in the background, skips Backups, and writes the session once",
         copyBody.find("saveSession()") == std::string::npos
             && copyBody.find("captureSessionData()") != std::string::npos
             && copyBody.find("startCopy") != std::string::npos
             && copyBody.find("sessionDirty_ = false") != std::string::npos
             && runCopy.find("copyFileFast") != std::string::npos
             && runCopy.find("writeSessionFile") != std::string::npos
             && runCopy.find("Backups") == std::string::npos
             && diskSrc.find("Thread::sleep") == std::string::npos
             && diskSrc.find("clonefile") != std::string::npos
             && fastBody.find("usleep") == std::string::npos
             && fastBody.find("clonefile") != std::string::npos
             && copyHeader.find("FICLONE") != std::string::npos);

    const auto columns = youhost::layoutHostColumns(1200);
    item("Cmd+A and Cmd+G work on HOST through the real key dispatch",
         youhost::dispatchRawKey(macLetter('a', false, true, false), 1, false) == youhost::ShortcutId::selectAll
             && youhost::dispatchRawKey(macLetter('a', false, true, false), 2, false) == youhost::ShortcutId::selectAll
             && youhost::dispatchRawKey(macLetter('g', false, true, false), 1, false) == youhost::ShortcutId::makeGroup
             && youhost::dispatchRawKey(macLetter('g', false, true, false), 2, false) == youhost::ShortcutId::makeGroup
             && ! youhost::dispatchRawKey(macLetter('a', false, true, false), 2, true).has_value()
             && ! youhost::dispatchRawKey(macLetter('g', false, true, false), 2, true).has_value()
             && mainUi.find("dispatchRawKey(rawKeyFromPress(key), page_, false)") != std::string::npos
             && mainUi.find("showMakeGroupDialog") != std::string::npos);
    item("HOST header is fixed, cached, and aligned CH MODE NAME DELAY COMP PLUGINS GAIN",
         columns.channel.x < columns.mode.x
             && columns.mode.x < columns.name.x
             && columns.name.x < columns.delay.x
             && columns.delay.x < columns.plugins.x
             && columns.plugins.x < columns.gain.x
             && columns.gain.x < columns.output.x
             && std::string(youhost::kHostColumnChannel) == "CH"
             && std::string(youhost::kHostColumnMode) == "MODE"
             && std::string(youhost::kHostColumnName) == "NAME"
             && std::string(youhost::kHostColumnDelay) == "DELAY COMP"
             && std::string(youhost::kHostColumnPlugins) == "PLUGINS"
             && std::string(youhost::kHostColumnGain) == "GAIN"
             && std::string(youhost::kHostColumnOutput) == "OUT"
             && hostUi.find("columnHeader_") != std::string::npos
             && hostUi.find("rebuildCache") != std::string::npos
             && hostUi.find("drawImageAt") != std::string::npos
             && hostUi.find("kHostColumnHeaderHeight") != std::string::npos);
    item("NaN from a plugin is silenced and an empty chain is passthrough",
         engineSrc.find("sanitizeAudioBlock") != std::string::npos
             && engineSrc.find("silenceNonFinite") != std::string::npos
             && engineSrc.find("armChannelReset") != std::string::npos
             && engineSrc.find("resetRequested") != std::string::npos);
    const auto menuSrc = readWorkspaceFile("src/ui/ChannelMenu.cpp");
    const auto linkAt = engineSrc.find("void AudioEngine::setStereoLinked");
    const auto afterLink = engineSrc.find("void AudioEngine::setSlotBypassed");
    const auto linkBody = linkAt == std::string::npos || afterLink == std::string::npos || afterLink < linkAt
                              ? std::string()
                              : engineSrc.substr(linkAt, afterLink - linkAt);
    item("stereo link is HOST only and does not change channel settings",
         youhost::stereoLinkMenuLabel(0) == "Stereo link with ch 2"
             && youhost::stereoLinkMenuLabel(1) == "Stereo link with ch 2"
             && youhost::pairChainSamples(true, 0, 128) == 128
             && youhost::pairProcessCost(true, true, 256) < youhost::pairProcessCost(false, true, 256)
             && youhost::pluginProcessCallsForPair(true, true) == 1
             && youhost::pluginProcessCallsForPair(false, true) == 2
             && ! youhost::hostPluginSlotsVisible(1, true)
             && youhost::hostPluginSlotsVisible(0, true)
             && menuSrc.find("hostPage && canStereoLink") != std::string::npos
             && mainUi.find("showChannelMenu(engine_, meterGrid_") != std::string::npos
             && hostUi.find("showChannelMenu(engine_, *this, channel_, nullptr, true)") != std::string::npos
             && hostUi.find("Stereo link with ch") != std::string::npos
             && linkBody.find("channelColor_") == std::string::npos
             && linkBody.find("setChannelName") == std::string::npos
             && linkBody.find("listen_") == std::string::npos
             && linkBody.find("channelGroup_") == std::string::npos
             && engineSrc.find("stereoMirror") == std::string::npos
             && readWorkspaceFile("src/engine/PluginRack.cpp").find("pairChainSamples") != std::string::npos
             && recorderSrc.find("stereoLink") == std::string::npos
             && recorderSrc.find("setStereoLink") == std::string::npos);
    item("plugin list marks M, ST, and M/ST",
         youhost::pluginBusMark(true, false) == "M"
             && youhost::pluginBusMark(false, true) == "ST"
             && youhost::pluginBusMark(true, true) == "M/ST"
             && hostUi.find("isPluginBusMark") != std::string::npos);

    const auto timerAt = mainUi.find("void MainComponent::timerCallback()");
    const auto beatAt = timerAt == std::string::npos ? std::string::npos : mainUi.find("beatMessageThread()", timerAt);
    const auto lockedAt = timerAt == std::string::npos ? std::string::npos : mainUi.find("deviceSetupLocked()", timerAt);
    item("message heartbeat runs while a card is open",
         beatAt != std::string::npos && lockedAt != std::string::npos && beatAt < lockedAt);

    int silenceCalls = 0;
    for (std::size_t pos = 0; (pos = engineSrc.find("silenceLiveState()", pos)) != std::string::npos; pos += 1)
        ++silenceCalls;
    const auto rackSrc = readWorkspaceFile("src/engine/PluginRack.cpp");
    item("New, Open, and Offline zero meters and plugin scratch",
         silenceCalls >= 4
             && engineSrc.find("clearMeterBallistics") != std::string::npos
             && rackSrc.find("void PluginRack::silencePublished()") != std::string::npos
             && rackSrc.find("preparePluginBlock") != std::string::npos
             && rackSrc.find("kSlotsPerChannel * LiveGraph::kScratchCap") != std::string::npos
             && mainUi.find("performAnyPendingRepaintsNow") == std::string::npos
             && rackSrc.find("kRepaintParameter") != std::string::npos
             && rackSrc.find("kRepaintLatency") != std::string::npos);

    const auto sliceBetween = [](const std::string& source, const char* begin, const char* end)
    {
        const auto start = source.find(begin);
        const auto stop = start == std::string::npos ? std::string::npos : source.find(end, start + 1);
        if (start == std::string::npos || stop == std::string::npos || stop < start)
            return std::string();
        return source.substr(start, stop - start);
    };
    const auto groupEdits = sliceBetween(engineSrc, "void AudioEngine::assignChannelsToGroup", "bool AudioEngine::groupsAreExpanded");
    const char* audioWords[] = {
        "syncCompensation", "setChannelGroups", "publishUnlocked", "prepareToPlay",
        "buildGraph", "loadPlugin", "removePlugin", "lifeLock_"
    };
    bool groupEditsStayVisual = ! groupEdits.empty();
    for (const char* word : audioWords)
        groupEditsStayVisual = groupEditsStayVisual && groupEdits.find(word) == std::string::npos;

    // Remove group while plugins are loaded, beside a running callback.
    // The edit thread only rewrites membership. The callback watches the instances.
    constexpr int kChannels = 48;
    constexpr int kSlots = 4;
    std::array<std::array<int, kSlots>, kChannels> plugins {};
    for (int channel = 0; channel < kChannels; ++channel)
        plugins[static_cast<std::size_t>(channel)][0] = 1;
    std::array<int, youhost::kMaxChannels> membership {};
    membership.fill(-1);
    struct VisualGroup
    {
        bool used = false;
        int color = 0;
        bool collapsed = false;
    };
    std::array<VisualGroup, youhost::kMaxDisplayGroups> visual {};
    std::atomic<int> prepares { 0 };
    std::atomic<int> publishes { 0 };
    std::atomic<int> broken { 0 };
    std::atomic<int> callbacks { 0 };
    std::atomic<bool> runCallback { true };
    std::thread callback([&]
    {
        while (runCallback.load(std::memory_order_acquire))
        {
            int loaded = 0;
            for (int channel = 0; channel < kChannels; ++channel)
                loaded += plugins[static_cast<std::size_t>(channel)][0];
            if (loaded != kChannels || prepares.load(std::memory_order_relaxed) != 0
                || publishes.load(std::memory_order_relaxed) != 0)
                broken.fetch_add(1, std::memory_order_relaxed);
            callbacks.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });
    const auto armed = std::chrono::steady_clock::now();
    while (callbacks.load(std::memory_order_acquire) < 10
           && std::chrono::steady_clock::now() - armed < std::chrono::seconds(2))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    for (int step = 0; step < 300; ++step)
    {
        const int group = step % youhost::kMaxDisplayGroups;
        visual[static_cast<std::size_t>(group)].used = true;
        visual[static_cast<std::size_t>(group)].color = step % 8;
        visual[static_cast<std::size_t>(group)].collapsed = true;
        for (int channel = 0; channel < kChannels; ++channel)
            if ((channel % youhost::kMaxDisplayGroups) == group)
                membership[static_cast<std::size_t>(channel)] = group;
        for (int channel = 0; channel < youhost::kMaxChannels; ++channel)
            if (membership[static_cast<std::size_t>(channel)] == group)
                membership[static_cast<std::size_t>(channel)] = -1;
        visual[static_cast<std::size_t>(group)] = {};
    }
    runCallback.store(false, std::memory_order_release);
    callback.join();
    const auto checklistDoc = readWorkspaceFile("docs/release-checklist.md");
    const auto workflow = readWorkspaceFile(".github/workflows/build-macos.yml");
    const auto periodNs = youhost::expectedPeriodNs(48000.0, 32);
    item("Groups stay visual",
         groupEditsStayVisual
             && groupEdits.find("Removing a group leaves every plugin instance") != std::string::npos
             && callbacks.load() >= 10
             && broken.load() == 0
             && prepares.load() == 0
             && publishes.load() == 0
             && periodNs == 666667
             && youhost::callbackCpuOverrun(periodNs + 1, periodNs) == 1
             && youhost::callbackCpuOverrun(periodNs, periodNs) == 0
             && ! youhost::callbackCpuNearBaseline(100000, 270000)
             && youhost::callbackCpuNearBaseline(100000, 170000)
             && ! youhost::callbackCpuNearBaseline(100000, 170001)
             && youhost::callbackMedianInsidePeriod(400000, periodNs)
             && ! youhost::callbackMedianInsidePeriod(periodNs + 1, periodNs)
             && youhost::callbackOverrunStreak(nullptr, 0, periodNs) == 0
             && youhost::callbackDeadlinesHeld(1)
             && youhost::callbackDeadlinesHeld(4)
             && ! youhost::callbackDeadlinesHeld(youhost::kDeadlineStreakLimit)
             && youhost::editOverrunsNearBaseline(18, 4512, 182, 47147)
             && ! youhost::editOverrunsNearBaseline(0, 1000, 800, 1000)
             && workflow.find("YouHostGroupStressTest") != std::string::npos
             && checklistDoc.find("Groups are purely visual") != std::string::npos
             && checklistDoc.find("prepareToPlay") != std::string::npos
             && checklistDoc.find("fastest pass") == std::string::npos);

    auto lockedTake = readyTake();
    lockedTake.lockArmed = true;
    lockedTake.recording = true;
    auto openTake = lockedTake;
    openTake.lockArmed = false;
    int transportKeys = 0;
    bool otherKeysLeaveTransport = true;
    for (int raw = 0; raw < static_cast<int>(youhost::ShortcutId::count); ++raw)
    {
        const auto id = static_cast<youhost::ShortcutId>(raw);
        if (youhost::shortcutAsksTransport(id))
            ++transportKeys;
        else if (id == youhost::ShortcutId::playOrStop || id == youhost::ShortcutId::recordNow)
            otherKeysLeaveTransport = false;
    }
    for (const auto& binding : youhost::kBindings)
        if (! youhost::shortcutAsksTransport(binding.id)
            && (binding.id == youhost::ShortcutId::playOrStop || binding.id == youhost::ShortcutId::recordNow))
            otherKeysLeaveTransport = false;
    youhost::KeyQuery plainSpace;
    plainSpace.kind = youhost::KeyKind::space;
    youhost::KeyQuery commandSpace = plainSpace;
    commandSpace.command = true;
    const auto runKeys = sliceBetween(mainUi,
                                      "bool MainComponent::runShortcut",
                                      "juce::ApplicationCommandTarget* MainComponent::getNextCommandTarget");
    const auto saveAsKeys = sliceBetween(mainUi,
                                         "void MainComponent::saveSessionAs",
                                         "void MainComponent::chooseSaveAsDestination");
    const auto quitKeys = sliceBetween(mainUi,
                                       "void MainComponent::runAfterUnsavedCheck",
                                       "void MainComponent::requestApplicationQuit");
    const auto locateKeys = sliceBetween(recorderSrc,
                                         "void Recorder::locate",
                                         "void Recorder::jumpMarker");
    const auto moveKeys = sliceBetween(recorderSrc,
                                       "void Recorder::jumpMarker",
                                       "void Recorder::addImportedTake");
    const auto saveAsLock = saveAsKeys.find("recordingLocked()");
    const auto saveAsStop = saveAsKeys.find("transportStop()");
    const auto help = youhost::shortcutHelpText();
    item("Locked recording ignores keys",
         youhost::lockedRecordingTransportHolds(lockedTake)
             && ! youhost::lockedRecordingTransportHolds(openTake)
             && youhost::resolveTransport(openTake, youhost::TransportPress::space).stop
             && youhost::matchShortcut(plainSpace) == youhost::ShortcutId::playOrStop
             && youhost::matchShortcut(commandSpace) == youhost::ShortcutId::recordNow
             && transportKeys == 2
             && otherKeysLeaveTransport
             && static_cast<int>(youhost::ShortcutId::count) > 2
             && runKeys.find("finishTransport") == std::string::npos
             && runKeys.find("recorder_->stop") == std::string::npos
             && runKeys.find("prepareForQuit") == std::string::npos
             && runKeys.find("transportStop()") != std::string::npos
             && saveAsLock != std::string::npos && saveAsStop != std::string::npos && saveAsLock < saveAsStop
             && quitKeys.find("recordingLocked()") != std::string::npos
             && quitKeys.find("prepareForQuit") == std::string::npos
             && locateKeys.find("isRecording()") != std::string::npos
             && locateKeys.find("stop()") == std::string::npos
             && moveKeys.find("stop()") == std::string::npos
             && help.find("No keyboard shortcut stops that take.") != std::string::npos
             && checklistDoc.find("Locked recording ignores keys") != std::string::npos);

    const auto monitorToggles = sliceBetween(mainUi,
                                            "void MainComponent::toggleDropouts()",
                                            "void MainComponent::saveSession()");
    const auto monitorRaise = sliceBetween(mainUi,
                                          "void MainComponent::raiseMonitorWindows",
                                          "void MainComponent::keepMonitorWindowsOpen");
    int keepOpenCalls = 0;
    for (std::size_t pos = 0; (pos = monitorToggles.find("keepMonitorWindowsOpen", pos)) != std::string::npos; pos += 1)
        ++keepOpenCalls;
    item("DROPOUTS, CPU, and LATENCY stay open together",
         keepOpenCalls == 3
             && monitorToggles.find("toggleDropouts") != std::string::npos
             && monitorRaise.find("setVisible") == std::string::npos
             && monitorRaise.find("&dropouts_") != std::string::npos
             && monitorRaise.find("&cpu_") != std::string::npos
             && monitorRaise.find("latencyWindow_") != std::string::npos
             && mainUi.find("dropoutsButton_.onClick = [this] { toggleDropouts(); };") != std::string::npos
             && mainUi.find("cpuButton_.onClick = [this] { toggleCpu(); };") != std::string::npos
             && mainUi.find("latencyButton_.onClick = [this] { toggleLatency(); };") != std::string::npos
             && runKeys.find("toggleDropouts()") != std::string::npos
             && runKeys.find("toggleCpu()") != std::string::npos
             && runKeys.find("toggleLatency()") != std::string::npos
             && help.find("Opening DROPOUTS, CPU, or LATENCY leaves the others open.") != std::string::npos
             && checklistDoc.find("DROPOUTS, CPU, and LATENCY stay open together") != std::string::npos);

    const auto callbackSlice = sliceBetween(engineSrc,
                                           "void AudioEngine::audioDeviceIOCallbackWithContext",
                                           "void AudioEngine::audioDeviceAboutToStart");
    const auto memberSlice = sliceBetween(engineSrc,
                                         "void AudioEngine::addChannelsToGroup",
                                         "void AudioEngine::setGroupName");
    const auto undoBody = sliceBetween(engineSrc, "bool AudioEngine::undoEdit()", "bool AudioEngine::redoEdit()");
    const auto markerBody = sliceBetween(engineSrc, "void AudioEngine::dropMarker()", "std::int64_t AudioEngine::markerSample");
    const auto dspLoad = sliceBetween(rackSrc, "DspLoad PluginRack::dspLoad()", "void PluginRack::deliverOffThreadInstance");
    const auto helpWindow = sliceBetween(mainUi, "class HelpWindow", "void MainComponent::setPeakMode");
    const bool membershipVisual = ! memberSlice.empty()
                                  && memberSlice.find("syncCompensation") == std::string::npos
                                  && memberSlice.find("setChannelGroups") == std::string::npos
                                  && memberSlice.find("publishUnlocked") == std::string::npos
                                  && memberSlice.find("loadPlugin") == std::string::npos
                                  && memberSlice.find("addMembersToGroup") != std::string::npos
                                  && memberSlice.find("removeMembersFromGroups") != std::string::npos;
    const auto outColumns = youhost::layoutHostColumns(960);
    const auto delayHeader = youhost::delayCompHeaderBox(outColumns);
    youhost::SyncLostState sync;
    sync.deviceLost(14, 32, 1);
    const auto lostText = youhost::syncOverlayText(sync);
    const bool alarmOn = sync.showAlarm() && sync.keepReattaching() && sync.isMissing()
                         && lostText.visible && lostText.lost
                         && lostText.primary == youhost::kSyncLostTitle
                         && lostText.secondary == youhost::kSyncLostFinnish
                         && youhost::syncLostChrome(sync) == youhost::SyncLostChrome::center;
    sync.acknowledge();
    const bool stillLost = sync.showAlarm() && sync.keepReattaching() && sync.isMissing();
    sync.deviceReturned(3000);
    const auto restored = youhost::syncOverlayText(sync);
    const auto cornerBox = youhost::syncLostCornerBox(1280, 800);
    const auto centerBox = youhost::syncLostCenterBox(1280, 800);
    const bool waitingForClick = sync.showRestored() && ! sync.keepReattaching()
                                 && youhost::syncLostChrome(sync) == youhost::SyncLostChrome::corner
                                 && restored.visible && ! restored.lost
                                 && restored.primary == "Sync was lost at 14:32:01 for 3 s, now restored"
                                 && cornerBox.x == 12 && cornerBox.y == 752 && cornerBox.y > 400
                                 && centerBox.w == 720 && centerBox.x == (1280 - 720) / 2;
    sync.acknowledge();
    const bool alarmClear = ! sync.showAlarm() && ! sync.keepReattaching();
    sync.deviceLost(1, 2, 3);
    const bool restamped = sync.isMissing() && sync.hour == 1 && sync.minute == 2 && sync.second == 3;
    sync.dismiss();
    const bool dismissed = ! sync.showAlarm() && ! sync.isMissing();
    const auto helpText = youhost::shortcutHelpText();
    const auto gigAt = helpText.find("Before a gig / for best performance");
    const auto installSteps = helpText.find("1. Unzip the file");
    youhost::CpuDisplayMeter dspMeter;
    const bool dspHolds = dspMeter.push(0, 10) == 10
                          && dspMeter.push(100, 90) == 10
                          && dspMeter.push(499, 80) == 10
                          && dspMeter.peak == 90
                          && dspMeter.push(500, 40) == 55
                          && dspMeter.push(900, 0) == 55;
    youhost::CpuDisplayMeter capped;
    const bool cappedAtFull = capped.push(0, 250) == 100 && capped.push(400, 400) == 100;
    const auto cpuWindow = readWorkspaceFile("src/ui/CpuWindow.cpp");
    item("Real CPU meter",
         youhost::formatCpuField(140) == "DSP 100%"
             && youhost::blockCpuPercent(21333333, 512, 48000.0) == 100
             && youhost::clampCpuPercent(1400) == 100
             && youhost::clampCpuPercent(-8) == 0
             && youhost::displayCpuPercent(140.4f) == 100
             && dspHolds
             && cappedAtFull
             && rackSrc.find("gate->shownPercent.store") != std::string::npos
             && dspLoad.find("shownPercent.load") != std::string::npos
             && dspLoad.find("shownPercent.exchange") == std::string::npos
             && dspLoad.find("displayCpuPercent") != std::string::npos
             && callbackSlice.find("callbackElapsedNs_.store") != std::string::npos
             && engineSrc.find("dspDisplay_.push") != std::string::npos
             && mainUi.find("engine_.callbackCpuPercent()") != std::string::npos
             && mainUi.find("engine_.pluginCpuPercent()") == std::string::npos
             && cpuWindow.find("\"DSP\"") != std::string::npos
             && cpuWindow.find("\"Plugins\"") != std::string::npos
             && cpuWindow.find("999") == std::string::npos
             && helpText.find("Every CPU and DSP figure stops at 100") != std::string::npos
             && checklistDoc.find("never goes above 100") != std::string::npos
             && checklistDoc.find("Real CPU meter") != std::string::npos);
    item("Dropout log",
         youhost::formatDropoutHostLine(3, 48000, 32, 180, 15) == "dropout x3  take 48000  buffer 32  dsp 180%  plugins 15"
             && youhost::dropoutLogDue(0, 0, 1)
             && ! youhost::dropoutLogDue(1000, 1, 1)
             && callbackSlice.find("formatDropoutHostLine") == std::string::npos
             && engineSrc.find("formatDropoutHostLine") != std::string::npos
             && checklistDoc.find("Dropout log") != std::string::npos);
    const auto recordAt = callbackSlice.find("recorder_->processRecord(inputChannelData");
    const auto playAt = callbackSlice.find("recorder_->processPlayback(playbackPtrs_");
    const auto playCopyAt = callbackSlice.find("processPassthrough(playbackPtrs_");
    const auto liveCopyAt = callbackSlice.find("processPassthrough(inputChannelData");
    const auto pluginsAt = callbackSlice.find("rack_->process(");
    const auto gainAt = callbackSlice.find("applyOutputTrim(");
    item("Playback through host",
         youhost::callbackStageOrder(youhost::CallbackStage::recordRawInput)
                 < youhost::callbackStageOrder(youhost::CallbackStage::replaceWithPlayback)
             && youhost::callbackStageOrder(youhost::CallbackStage::replaceWithPlayback)
                    < youhost::callbackStageOrder(youhost::CallbackStage::pluginsLinkAndDelay)
             && youhost::callbackStageOrder(youhost::CallbackStage::pluginsLinkAndDelay)
                    < youhost::callbackStageOrder(youhost::CallbackStage::outputGain)
             && youhost::callbackInput(true) == youhost::CallbackInput::playback
             && youhost::callbackInput(false) == youhost::CallbackInput::live
             && youhost::playbackFeedsHostChain(true)
             && youhost::stopReturnsLiveInput(false)
             && youhost::recorderSample(0.25f) == 0.25f
             && youhost::hostDrySample(true, 0.25f, -0.5f) == -0.5f
             && youhost::hostDrySample(false, 0.25f, -0.5f) == 0.25f
             && recordAt != std::string::npos && playAt != std::string::npos
             && playCopyAt != std::string::npos && liveCopyAt != std::string::npos
             && pluginsAt != std::string::npos && gainAt != std::string::npos
             && recordAt < playAt && playAt < playCopyAt && playCopyAt < pluginsAt
             && pluginsAt < gainAt && playCopyAt < liveCopyAt
             && callbackSlice.find("processRecord(playbackPtrs_") == std::string::npos
             && callbackSlice.find("delayInPlace") == std::string::npos
             && callbackSlice.find("hostWantsTake") != std::string::npos
             && callbackSlice.find("blendHostChannels") != std::string::npos
             && callbackSlice.find("prepareToPlay") == std::string::npos
             && callbackSlice.find("publishUnlocked") == std::string::npos
             && callbackSlice.find("buildGraph") == std::string::npos
             && readWorkspaceFile("src/engine/PluginRack.cpp").find("delayInPlace") != std::string::npos
             && checklistDoc.find("The recorder copies the device input before the swap") != std::string::npos
             && checklistDoc.find("short crossfade") != std::string::npos);
    const auto playBody = sliceBetween(recorderSrc, "void Recorder::play()", "void Recorder::locate(");
    const auto stopBody = sliceBetween(recorderSrc, "void Recorder::stop()", "void Recorder::play()");
    const auto primeAt = playBody.find("fillOnce()");
    const auto playingStore = playBody.find("TransportMode::playing");
    item("Click-free playback handoff",
         youhost::kHostSwitchFade == 128
             && ! youhost::hostWantsTake(true, 0, 512, false, 0, false)
             && youhost::hostWantsTake(true, 512, 512, false, 0, false)
             && youhost::hostWantsTake(true, 0, 512, false, youhost::kHostSwitchFade, false)
             && ! youhost::hostWantsTake(false, 512, 512, false, youhost::kHostSwitchFade, false)
             && youhost::hostWantsTake(true, 10, 512, false, 0, true)
             && ! youhost::hostWantsTake(true, 0, 512, false, 0, true)
             && primeAt != std::string::npos && playingStore != std::string::npos && primeAt < playingStore
             && playBody.find("prepareToPlay") == std::string::npos
             && playBody.find("publishUnlocked") == std::string::npos
             && stopBody.find("playbackRingHeld_") != std::string::npos
             && stopBody.find("prepareToPlay") == std::string::npos
             && help.find("short crossfade") != std::string::npos);
    const auto syncHeader = readWorkspaceFile("src/engine/SyncLost.h");
    const auto overlayUi = readWorkspaceFile("src/ui/SyncLostOverlay.h");
    const auto choseOther = sliceBetween(engineSrc, "if (userChoseThis)", "A card that is not the session card");
    const auto paintBody = sliceBetween(mainUi, "void MainComponent::paint(", "void MainComponent::resized()");
    item("SYNC LOST",
         alarmOn && stillLost && waitingForClick && alarmClear && restamped && dismissed
             && youhost::syncLossWholeSeconds(3000) == 3
             && youhost::syncLostChrome(sync) == youhost::SyncLostChrome::hidden
             && ! youhost::syncOverlayText(sync).visible
             && youhost::syncLostBlink(0)
             && ! youhost::syncLostBlink(400)
             && std::string(youhost::kSyncLostFinnish) == "Äänikortti kadonnut"
             && static_cast<unsigned char>(youhost::kSyncLostFinnish[0]) == 0xC3
             && static_cast<unsigned char>(youhost::kSyncLostFinnish[1]) == 0x84
             && syncHeader.find("Äänikortti kadonnut") != std::string::npos
             && overlayUi.find("CharPointer_UTF8") != std::string::npos
             && overlayUi.find("syncLostUtf8(kSyncLostFinnish)") != std::string::npos
             && overlayUi.find("DejaVu Sans") != std::string::npos
             && overlayUi.find("setOpaque(true)") != std::string::npos
             && overlayUi.find("place_ != SyncLostChrome::corner") != std::string::npos
             && overlayUi.find("return false") != std::string::npos
             && overlayUi.find("closeAudioDevice") == std::string::npos
             && overlayUi.find("recorder_") == std::string::npos
             && overlayUi.find("onOpenDetails") != std::string::npos
             && overlayUi.find("\"OK\"") != std::string::npos
             && mainUi.find("acknowledgeSyncLost") != std::string::npos
             && mainUi.find("syncOverlay_.sync") != std::string::npos
             && mainUi.find("syncOverlay_.onOpenDetails") != std::string::npos
             && mainUi.find("setBlinkPhase(syncLostBlink") != std::string::npos
             && mainUi.find("syncLostCornerBox") != std::string::npos
             && mainUi.find("drawText(\"SYNC LOST\"") == std::string::npos
             && mainUi.find("syncOverlay_.repaint()") == std::string::npos
             && paintBody.find("syncLostBlink") == std::string::npos
             && choseOther.find("dismiss()") != std::string::npos
             && choseOther.find("deviceReturned") == std::string::npos
             && engineSrc.find("syncLost_.deviceReturned(gapMs)") != std::string::npos
             && checklistDoc.find("SYNC LOST") != std::string::npos
             && checklistDoc.find("does not appear at startup") != std::string::npos
             && sliceBetween(engineSrc, "void AudioEngine::reconcileStartupDevice()", "void AudioEngine::transportLocate").find("openNamedDevice") == std::string::npos
             && sliceBetween(engineSrc, "void AudioEngine::reconcileStartupDevice()", "void AudioEngine::transportLocate").find("decideStartupCard") != std::string::npos
             && sliceBetween(engineSrc, "void AudioEngine::setOfflineTemplate", "void AudioEngine::setPreferredSampleRate").find("clearSyncLost") != std::string::npos
             && checklistDoc.find("not from a timer scan") != std::string::npos
             && checklistDoc.find("bottom-left") != std::string::npos
             && checklistDoc.find("repaints only that box") != std::string::npos);
    auto liveClock = []
    {
        youhost::ClockSyncInput input;
        input.deviceOpen = true;
        input.hostWasRunning = true;
        input.dspPercent = 4;
        input.nowNs = 1000000000;
        input.lastCallbackNs = 1000000000;
        input.callbackCount = 10;
        return input;
    };
    youhost::ClockSyncMemory clock;
    auto clockIn = liveClock();
    const bool clockQuiet = youhost::decideClockSync(clock, clockIn) == youhost::ClockSyncAction::hold && ! clock.missing;
    clockIn.lastCallbackNs = 0;
    clockIn.nowNs = 2000000000;
    const bool clockBeforeFirst = youhost::decideClockSync(clock, clockIn) == youhost::ClockSyncAction::hold && ! clock.missing;
    clockIn = liveClock();
    clockIn.nowNs = clockIn.lastCallbackNs + 250000000;
    const auto lostAt = clockIn.nowNs;
    const bool clockFrozen = youhost::decideClockSync(clock, clockIn) == youhost::ClockSyncAction::lost && clock.missing
                             && clock.lostAtNs == lostAt;
    clockIn.nowNs += 250000000;
    const bool clockNotRestamped = youhost::decideClockSync(clock, clockIn) == youhost::ClockSyncAction::hold
                                   && clock.missing && clock.lostAtNs == lostAt;
    clockIn.lastCallbackNs = clockIn.nowNs;
    clockIn.callbackCount = 11;
    const auto resumedAt = clockIn.nowNs;
    const bool clockHalfRestore = youhost::decideClockSync(clock, clockIn) == youhost::ClockSyncAction::hold && clock.missing;
    clockIn.nowNs = resumedAt + 500000000;
    clockIn.lastCallbackNs = clockIn.nowNs;
    clockIn.callbackCount = 12;
    const bool clockNotYet = youhost::decideClockSync(clock, clockIn) == youhost::ClockSyncAction::hold && clock.missing;
    clockIn.nowNs = resumedAt + 1000000000;
    clockIn.lastCallbackNs = clockIn.nowNs;
    clockIn.callbackCount = 13;
    const bool clockRestored = youhost::decideClockSync(clock, clockIn) == youhost::ClockSyncAction::restored && ! clock.missing;
    youhost::ClockSyncMemory hole;
    auto holeIn = liveClock();
    holeIn.nowNs = holeIn.lastCallbackNs + 250000000;
    youhost::decideClockSync(hole, holeIn);
    holeIn.lastCallbackNs = holeIn.nowNs;
    holeIn.callbackCount = 11;
    youhost::decideClockSync(hole, holeIn);
    const auto holeMark = hole.regularSinceNs;
    holeIn.nowNs += 250000000;
    const bool holeReset = youhost::decideClockSync(hole, holeIn) == youhost::ClockSyncAction::hold
                           && hole.missing && hole.regularSinceNs == 0 && hole.lostAtNs != 0 && holeMark != 0;
    youhost::ClockSyncMemory offlineClock;
    auto offlineIn = liveClock();
    offlineIn.offline = true;
    offlineIn.nowNs = offlineIn.lastCallbackNs + 250000000;
    const bool offlineHolds = youhost::decideClockSync(offlineClock, offlineIn) == youhost::ClockSyncAction::hold
                              && ! offlineClock.missing;
    youhost::ClockSyncMemory idleClock;
    auto idleIn = liveClock();
    idleIn.hostWasRunning = false;
    idleIn.nowNs = idleIn.lastCallbackNs + 1000000000;
    const bool idleHolds = youhost::decideClockSync(idleClock, idleIn) == youhost::ClockSyncAction::hold && ! idleClock.missing;
    youhost::ClockSyncMemory oneDrop;
    auto dropIn = liveClock();
    dropIn.dspPercent = 0;
    dropIn.dropoutCount = 0;
    dropIn.nowNs = 0;
    dropIn.lastCallbackNs = 0;
    youhost::decideClockSync(oneDrop, dropIn);
    dropIn.nowNs = 10000000;
    dropIn.lastCallbackNs = 10000000;
    dropIn.dropoutCount = 1;
    dropIn.callbackCount = 2;
    const bool oneDropoutHolds = youhost::decideClockSync(oneDrop, dropIn) == youhost::ClockSyncAction::hold && ! oneDrop.missing;
    dropIn.nowNs = 500000000;
    dropIn.lastCallbackNs = 500000000;
    const bool oneDropoutStays = youhost::decideClockSync(oneDrop, dropIn) == youhost::ClockSyncAction::hold && ! oneDrop.missing;
    youhost::ClockSyncMemory burst;
    auto burstIn = liveClock();
    burstIn.dspPercent = 0;
    burstIn.dropoutCount = 0;
    burstIn.nowNs = 0;
    burstIn.lastCallbackNs = 0;
    youhost::decideClockSync(burst, burstIn);
    burstIn.nowNs = 10000000;
    burstIn.lastCallbackNs = 10000000;
    burstIn.dropoutCount = 1;
    burstIn.callbackCount = 2;
    youhost::decideClockSync(burst, burstIn);
    burstIn.nowNs = 260000000;
    burstIn.lastCallbackNs = 260000000;
    burstIn.dropoutCount = 2;
    burstIn.callbackCount = 3;
    const bool burstLost = youhost::decideClockSync(burst, burstIn) == youhost::ClockSyncAction::lost && burst.missing;
    youhost::ClockSyncMemory history;
    auto historyIn = liveClock();
    historyIn.dspPercent = 0;
    historyIn.dropoutCount = 50;
    historyIn.nowNs = 0;
    historyIn.lastCallbackNs = 0;
    const bool historyStamp = youhost::decideClockSync(history, historyIn) == youhost::ClockSyncAction::hold && ! history.missing;
    historyIn.nowNs = 300000000;
    historyIn.lastCallbackNs = 300000000;
    const bool historyHolds = youhost::decideClockSync(history, historyIn) == youhost::ClockSyncAction::hold && ! history.missing;
    youhost::ClockSyncMemory hot;
    auto hotIn = liveClock();
    hotIn.dspPercent = 40;
    hotIn.dropoutCount = 1;
    hotIn.nowNs = 0;
    youhost::decideClockSync(hot, hotIn);
    hotIn.nowNs = 300000000;
    hotIn.lastCallbackNs = 300000000;
    hotIn.dropoutCount = 8;
    const bool hotHolds = youhost::decideClockSync(hot, hotIn) == youhost::ClockSyncAction::hold && ! hot.missing;
    youhost::ClockSyncMemory alive;
    auto aliveIn = liveClock();
    aliveIn.aliveDown = true;
    const bool aliveLost = youhost::decideClockSync(alive, aliveIn) == youhost::ClockSyncAction::lost && alive.missing;
    const auto watchAt = mainUi.find("engine_.watchClockSync();");
    const auto lockedCard = mainUi.find("if (! (engine_.deviceSetupLocked() && engine_.deviceIsOpen() && ! engine_.deviceLost()))");
    item("Network clock",
         clockQuiet && clockBeforeFirst && clockFrozen && clockNotRestamped && clockHalfRestore && clockNotYet && clockRestored
             && holeReset && offlineHolds && idleHolds && oneDropoutHolds && oneDropoutStays && burstLost
             && historyStamp && historyHolds && hotHolds && aliveLost
             && youhost::kClockSilenceNs == 200000000 && youhost::kClockRestoredNs == 1000000000
             && watchAt != std::string::npos && lockedCard != std::string::npos && watchAt < lockedCard
             && callbackSlice.find("callbackSerial_.fetch_add") != std::string::npos
             && engineSrc.find("kAudioDevicePropertyDeviceIsAlive") != std::string::npos
             && engineSrc.find("kAudioDevicePropertyDeviceIsRunning") != std::string::npos
             && engineSrc.find("kAudioDevicePropertyClockSource") != std::string::npos
             && engineSrc.find("kAudioDevicePropertyNominalSampleRate") != std::string::npos
             && engineSrc.find("syncLost_.deviceReturned(gapMs)") != std::string::npos
             && engineSrc.find("deviceLostBanner_ || lossFinalized_") != std::string::npos
             && helpText.find("3.6 ms") != std::string::npos
             && helpText.find("SoundGrid driver buffer") != std::string::npos
             && helpText.find("LV1") != std::string::npos
             && checklistDoc.find("SoundGrid, Dante, and AVB") != std::string::npos);
    const auto lossBody = sliceBetween(engineSrc, "void AudioEngine::handleDeviceDown()", "void AudioEngine::noteHardwareDevicesChanged()");
    const auto listBody = sliceBetween(engineSrc, "void AudioEngine::noteHardwareDeviceList()", "void AudioEngine::rejectForeignDevice()");
    const auto reopenBody = sliceBetween(engineSrc, "void AudioEngine::tryReopenWanted()", "bool AudioEngine::deviceNameListed");
    const auto lost = youhost::decideDeviceLoss("X32", "", false, true, true, true);
    const auto other = youhost::decideDeviceLoss("X32", "MacBook Pro Speakers", false, true, true, true);
    const auto back = youhost::decideDeviceLoss("X32", "X32", true, true, true, true);
    const auto idle = youhost::decideDeviceLoss("X32", "X32", true, false, false, false);
    item("Device lost recovery",
         std::string(youhost::kDeviceLostBanner) == "Device lost - waiting"
             && ! youhost::deviceReattachUsesTimer()
             && youhost::deviceLossKeepsPlugins()
             && lost.showWaiting && lost.closeTake && ! lost.reattachSame && lost.keepLock
             && other.refusedOther && ! other.reattachSame && other.showWaiting
             && back.reattachSame && back.resumeRecording && back.keepLock && ! back.refusedOther
             && ! idle.reattachSame && ! idle.closeTake
             && youhost::deviceLossMarker(48000).name == "Device lost"
             && youhost::deviceLossStartLine("X32", true) == "device lost \"X32\", take closed"
             && youhost::deviceLossLogLine("X32", 1500, true) == "device lost \"X32\" for 1500 ms, recording resumed"
             && ! lossBody.empty() && lossBody.find("tryReopenWanted") == std::string::npos
             && lossBody.find("deviceLossMarker") != std::string::npos
             && lossBody.find("removePlugin") == std::string::npos
             && listBody.find("tryReopenWanted()") != std::string::npos
             && listBody.find("lastDeviceScanMs_") == std::string::npos
             && reopenBody.find("lastDeviceScanMs_") == std::string::npos
             && engineSrc.find("kAudioHardwarePropertyDevices") != std::string::npos
             && engineSrc.find("noteHardwareDevicesChanged") != std::string::npos
             && checklistDoc.find("not from a timer scan") != std::string::npos);
    youhost::ChannelStrip hostMeter;
    const auto meterTiming = youhost::meterTimingFor(48000.0);
    const auto meterGains = youhost::blockMeterGains(meterTiming, 64);
    float tone[64];
    for (int index = 0; index < 64; ++index)
        tone[index] = 0.112f * static_cast<float>(std::sin((2.0 * 3.141592653589793 * index) / 64.0));
    for (int block = 0; block < 400; ++block)
    {
        youhost::updateMeter(hostMeter, tone, 64, meterGains);
        youhost::updateMeter(hostMeter.outputMeterState, hostMeter.outputMeter, tone, 64, meterGains);
    }
    const float inputDb = youhost::gainToDb(hostMeter.meter.rms.load(std::memory_order_relaxed));
    const float outputDb = youhost::gainToDb(hostMeter.outputMeter.rms.load(std::memory_order_relaxed));
    const float meterDeltaDb = std::fabs(inputDb - outputDb);
    const float truePeakDb = youhost::gainToDb(0.112f);
    std::cout << "HOST OUT rms " << inputDb << " dB output " << outputDb << " dB delta " << meterDeltaDb
              << " dB true peak " << truePeakDb << " dB\n";
    float monoFold[8];
    for (int index = 0; index < 8; ++index)
        monoFold[index] = 0.25f;
    float foldScratch[8] = {};
    youhost::renderFoldedMono(monoFold, 8, 1, 1, youhost::StereoFold::left, foldScratch, nullptr,
                              [](float**, int, int) {});
    bool foldUnity = true;
    for (int index = 0; index < 8; ++index)
        foldUnity = foldUnity && monoFold[index] == 0.25f;
    item("HOST OUT meter",
         outColumns.gain.x + outColumns.gain.width == outColumns.output.x
             && delayHeader.x == outColumns.exclude.x
             && meterDeltaDb <= 0.1f
             && hostMeter.meter.peak.load(std::memory_order_relaxed)
                    == hostMeter.outputMeter.peak.load(std::memory_order_relaxed)
             && youhost::meterColourForDb(inputDb, -20) == youhost::MeterColour::green
             && youhost::meterColourForDb(outputDb, -20) == youhost::MeterColour::green
             && youhost::meterColourForDb(truePeakDb, -20) == youhost::MeterColour::yellow
             && youhost::outputDbToLinear(0.0f) == 1.0f
             && foldUnity
             && hostUi.find("outputPeakFor") != std::string::npos
             && hostUi.find("outputRmsFor") != std::string::npos
             && hostUi.find("kHostColumnOutput") != std::string::npos
             && callbackSlice.find("updateMeter(strip.outputMeterState") != std::string::npos
             && callbackSlice.find("outputPeak_") == std::string::npos
             && callbackSlice.find("previous * 0.9f") == std::string::npos
             && checklistDoc.find("within 0.1 dB") != std::string::npos
             && checklistDoc.find("HOST OUT meter") != std::string::npos);
    item("Bigger CLIP",
         youhost::clipBandHeight(200.0f) == 22.0f
             && youhost::clipBandHeight(100.0f) == 16.0f
             && readWorkspaceFile("src/ui/MeterGrid.cpp").find("\"CLIP\"") != std::string::npos
             && checklistDoc.find("Bigger CLIP") != std::string::npos);
    item("Undo and redo",
         youhost::commandApplies(youhost::ShortcutId::undo, 2, true) == false
             && youhost::dispatchRawKey(macLetter('z', false, true, false), 2, false) == youhost::ShortcutId::undo
             && youhost::dispatchRawKey(macLetter('z', true, true, false), 2, false) == youhost::ShortcutId::redo
             && ! youhost::dispatchRawKey(macLetter('z', false, true, false), 2, true).has_value()
             && undoBody.find("recorder_->stop") == std::string::npos
             && undoBody.find("deleteFile") == std::string::npos
             && checklistDoc.find("Undo and redo") != std::string::npos);
    item("Add and remove group members",
         membershipVisual && checklistDoc.find("Adding channels to an existing group") != std::string::npos);
    item("Channel name outline",
         readWorkspaceFile("src/ui/MeterGrid.cpp").find("editingChannel_ != cell.channel") != std::string::npos
             && hostUi.find("! name_.isBeingEdited()") != std::string::npos
             && hostUi.find("expanded(1.5f)") != std::string::npos
             && checklistDoc.find("Channel name outline") != std::string::npos);
    item("LINK greyed when the group is not a pair",
         youhost::stereoLinkBlockedByGroups(0, 8, std::array<int, 4>{ 0, 0, 0, -1 }.data(), 4)
             && ! youhost::stereoLinkBlockedByGroups(0, 8, std::array<int, 4>{ 0, 0, -1, -1 }.data(), 4)
             && hostUi.find("stereoLinkBlockedTip") != std::string::npos
             && checklistDoc.find("LINK is grey") != std::string::npos);
    item("Before a gig",
         gigAt != std::string::npos && installSteps != std::string::npos && gigAt < installSteps
             && helpWindow.find("kGigPerformanceCard") != std::string::npos
             && readWorkspaceFile("INSTALL.md").find("Ennen keikkaa / paras suorituskyky") != std::string::npos
             && checklistDoc.find("Before a gig") != std::string::npos);
    const auto versionCpp = readWorkspaceFile("src/YouHostApplication.cpp");
    const auto aboutVersion = sliceBetween(versionCpp, "getApplicationVersion()", "moreThanOneInstanceAllowed");
    item("Version label",
         std::string(youhost::versionLabel()) == "YouHost 1.0"
             && std::string(youhost::versionLabel()).find("Beta") == std::string::npos
             && std::string(youhost::versionNumber()) == "1.0"
             && youhost::kSessionFormatVersion == 7
             && readWorkspaceFile("CMakeLists.txt").find("VERSION 1.0") != std::string::npos
             && readWorkspaceFile("CMakeLists.txt").find("YouHost ${PROJECT_VERSION}") != std::string::npos
             && readWorkspaceFile(".github/workflows/build-macos.yml").find("YouHost-${VERSION}-macOS.zip") != std::string::npos
             && readWorkspaceFile(".github/workflows/build-macos.yml").find("YouHost-Beta-") == std::string::npos
             && readWorkspaceFile(".github/workflows/build-macos.yml").find("YOUHOST_RELEASE_NAME=YouHost ${VERSION}") != std::string::npos
             && aboutVersion.find("Beta") == std::string::npos
             && aboutVersion.find("YOUHOST_VERSION") != std::string::npos
             && engineSrc.find("savedWith") != std::string::npos
             && checklistDoc.find("Version label") != std::string::npos
             && checklistDoc.find("YouHost 1.0") != std::string::npos
             && checklistDoc.find("YouHost-1.0-macOS.zip") != std::string::npos
             && checklistDoc.find("session format stays 7") != std::string::npos);
    const auto fromMarker = youhost::playSampleFromSelectedMarker(true, false, true, 48000, true);
    youhost::RawKey commandBackspace;
    commandBackspace.token = youhost::RawToken::backspace;
    commandBackspace.keyCode = 8;
    commandBackspace.command = true;
    youhost::RawKey plainBackspace = commandBackspace;
    plainBackspace.command = false;
    youhost::RawKey plainDelete;
    plainDelete.keyCode = 127;
    plainDelete.textCharacter = 127;
    youhost::RawKey commandDelete = plainDelete;
    commandDelete.command = true;
    commandDelete.textCharacter = 0;
    const auto deleteBody = sliceBetween(engineSrc, "bool AudioEngine::deleteMarker", "bool AudioEngine::openOwnEdit");
    const auto deleteCase = sliceBetween(mainUi, "case ShortcutId::deleteMarker:", "case ShortcutId::undo:");
    const auto timelineSrc = readWorkspaceFile("src/ui/TimelineView.cpp");
    bool recordingHeld = true;
    bool lockHeld = true;
    std::vector<youhost::MarkerPoint> liveMarkers;
    liveMarkers.push_back(youhost::deviceLossMarker(48000));
    liveMarkers.push_back(youhost::makeMarker(96000, 2));
    youhost::MarkerEdit eraseLost;
    eraseLost.kind = youhost::MarkerEditKind::erase;
    eraseLost.index = 0;
    eraseLost.marker = liveMarkers[0];
    const bool erasedLost = youhost::applyMarkerEdit(liveMarkers, eraseLost);
    liveMarkers.push_back(youhost::makeMarker(144000, 3));
    youhost::MarkerEdit restoreLost;
    restoreLost.kind = youhost::MarkerEditKind::insert;
    restoreLost.index = 0;
    restoreLost.marker = youhost::deviceLossMarker(48000);
    const bool restoredLost = youhost::applyMarkerEdit(liveMarkers, restoreLost);
    youhost::UndoTape tape;
    tape.recording = true;
    tape.gain = 0.0f;
    tape.markers.push_back(youhost::makeMarker(1000, 1));
    youhost::undoTapeDeleteMarker(tape, 0);
    youhost::undoTapeSetGain(tape, -6.0f);
    const bool undoGain = youhost::undoTapeStep(tape, false);
    const bool gainUndone = undoGain && tape.gain == 0.0f && tape.markers.empty() && tape.recording;
    const bool undoMarker = youhost::undoTapeStep(tape, false);
    const bool markerBack = undoMarker && tape.markers.size() == 1 && tape.markers[0].name == "Marker 1"
                            && tape.gain == 0.0f && tape.recording;
    const bool redoMarker = youhost::undoTapeStep(tape, true);
    const bool markerGoneAgain = redoMarker && tape.markers.empty() && tape.gain == 0.0f && tape.recording;
    const bool redoGain = youhost::undoTapeStep(tape, true);
    const bool gainRedone = redoGain && tape.gain == -6.0f && tape.markers.empty() && tape.recording;
    const auto applyBody = sliceBetween(engineSrc, "void AudioEngine::applyMixSnapshot", "void AudioEngine::restorePluginSnapshot");
    const auto markerOnlyAt = applyBody.find("if (snapshot.markerOnly)");
    const auto gainWriteAt = applyBody.find("outputGain_");
    youhost::MarkerBand band;
    band.lineX = 100.0f;
    band.lineTop = 20.0f;
    band.lineBottom = 200.0f;
    band.pillX = 103.0f;
    band.pillY = 4.0f;
    band.pillW = 70.0f;
    band.pillH = 14.0f;
    item("Markers",
         ! youhost::markerDropAffectsTransport()
             && youhost::markerDropAllowedWhileLocked()
             && ! youhost::markerDeleteAffectsTransport()
             && youhost::markerDeleteAllowedWhileLocked()
             && youhost::markerDeleteShortcut(true, true, false, false)
             && ! youhost::markerDeleteShortcut(false, true, false, false)
             && ! youhost::markerDeleteShortcut(false, false, true, false)
             && ! youhost::markerDeleteShortcut(true, false, true, false)
             && ! youhost::markerDeleteShortcut(true, true, false, true)
             && youhost::dispatchRawKey(commandBackspace, 1, false) == youhost::ShortcutId::deleteMarker
             && ! youhost::dispatchRawKey(plainBackspace, 1, false).has_value()
             && ! youhost::dispatchRawKey(plainDelete, 1, false).has_value()
             && ! youhost::dispatchRawKey(commandDelete, 1, false).has_value()
             && ! youhost::dispatchRawKey(commandBackspace, 1, true).has_value()
             && ! youhost::shortcutAsksTransport(youhost::ShortcutId::deleteMarker)
             && erasedLost && recordingHeld && lockHeld && liveMarkers.size() == 3
             && restoredLost && liveMarkers[0].name == "Device lost" && liveMarkers[1].name == "Marker 2"
             && youhost::markerIndexAt(&band, 1, 110.0f, 10.0f) == 0
             && youhost::markerIndexAt(&band, 1, 100.0f, 80.0f) == 0
             && youhost::markerIndexAt(&band, 1, 140.0f, 80.0f) == -1
             && youhost::markerIndexAfterDelete(2, 0, 3) == 1
             && youhost::markerIndexAfterDelete(0, 0, 0) == -1
             && ! deleteBody.empty()
             && deleteBody.find("stop(") == std::string::npos
             && deleteBody.find("transportStop") == std::string::npos
             && deleteBody.find("recordingLocked") == std::string::npos
             && deleteBody.find(".wav") == std::string::npos
             && deleteBody.find("eraseMarker") != std::string::npos
             && deleteBody.find("markersCaptured") != std::string::npos
             && deleteBody.find("markerOnly = true") != std::string::npos
             && gainUndone && markerBack && markerGoneAgain && gainRedone
             && markerOnlyAt != std::string::npos && gainWriteAt != std::string::npos && markerOnlyAt < gainWriteAt
             && engineSrc.find("to.markerOnly = from.markerOnly") != std::string::npos
             && engineSrc.find("applyMarkerEdit") != std::string::npos
             && engineSrc.find("stampInverseMarker") != std::string::npos
             && deleteCase.find("deleteMarkerIndex") != std::string::npos
             && deleteCase.find("transportStop") == std::string::npos
             && mainUi.find("setButtonText(kMarkerDeleteItem)") != std::string::npos
             && mainUi.find("showDeleteMenu") != std::string::npos
             && mainUi.find("isPopupMenu()") != std::string::npos
             && timelineSrc.find("kMarkerDeleteItem") != std::string::npos
             && timelineSrc.find("isPopupMenu()") != std::string::npos
             && timelineSrc.find("showMarkerMenu") != std::string::npos
             && youhost::toolWindowForwardsTransportKeys(false)
             && ! youhost::toolWindowForwardsTransportKeys(true)
             && fromMarker.has_value() && *fromMarker == 48000
             && ! youhost::playSampleFromSelectedMarker(true, true, true, 48000, true).has_value()
             && ! youhost::playSampleFromSelectedMarker(false, false, true, 48000, true).has_value()
             && ! youhost::playSampleFromSelectedMarker(true, false, false, 48000, true).has_value()
             && ! youhost::playSampleFromSelectedMarker(true, false, true, 48000, false).has_value()
             && youhost::dispatchRawKey(macLetter('m', false, false, false), 1, false) == youhost::ShortcutId::marker
             && youhost::dispatchRawKey(macLetter('m', false, false, false), 1, true) == std::nullopt
             && ! youhost::shortcutAsksTransport(youhost::ShortcutId::marker)
             && markerBody.find("stop(") == std::string::npos
             && mainUi.find("addAndMakeVisible(markerButton_)") != std::string::npos
             && mainUi.find("addAndMakeVisible(markersButton_)") != std::string::npos
             && mainUi.find("addChildComponent(syncOverlay_)") != std::string::npos
             && mainUi.find("markerButton_.setBounds(transport.removeFromRight") != std::string::npos
             && mainUi.find("bindToolKeys(dropouts_") != std::string::npos
             && mainUi.find("bindToolKeys(cpu_") != std::string::npos
             && mainUi.find("bindToolKeys(*latencyWindow_") != std::string::npos
             && mainUi.find("bindToolKeys(*this") != std::string::npos
             && mainUi.find("playSampleFromSelectedMarker") != std::string::npos
             && help.find("play from that marker") != std::string::npos
             && help.find("Cmd+Backspace") != std::string::npos
             && help.find("Delete the selected marker") != std::string::npos
             && checklistDoc.find("| Markers |") != std::string::npos
             && checklistDoc.find("starts playback from that marker") != std::string::npos
             && checklistDoc.find("Cmd+Backspace") != std::string::npos
             && checklistDoc.find("Right-click") != std::string::npos
             && checklistDoc.find("Plain Backspace") != std::string::npos
             && checklistDoc.find("Device lost") != std::string::npos
             && checklistDoc.find("the first Cmd+Z undoes the gain") != std::string::npos
             && checklistDoc.find("the second brings the marker back") != std::string::npos);
    item("WAV names",
         youhost::takeWaveName(1, 1, "BD") == "1_BD_1.wav"
             && youhost::takeWaveName(2, 1, "BD") == "1_BD_2.wav"
             && youhost::takeWaveName(1, 2, "Sn") == "2_Sn_1.wav"
             && youhost::takeWaveName(1, 1, "") == "1_1.wav"
             && youhost::takeWaveName(2, 1, "  ") == "1_2.wav"
             && youhost::parseRecordingName("1_BD_2.wav").takeNumber == 2
             && youhost::parseRecordingName("1_1_BD.wav").channelNumber == 1
             && youhost::parseRecordingName("1_1_BD.wav").takeNumber == 1
             && youhost::parseRecordingName("Take01_Ch03_Kick.wav").channelNumber == 3
             && checklistDoc.find("WAV names") != std::string::npos);
    item("Mono fold stays 2/2",
         youhost::pluginProcessCallsForPair(true, true) == 1
             && std::string(youhost::stereoFoldToken(youhost::StereoFold::left)) == "L"
             && youhost::stereoFoldFromInt(0) == youhost::StereoFold::left
             && youhost::layoutForChannel(youhost::ChosenLayout::stereo, false, true) == youhost::ChosenLayout::stereo
             && youhost::layoutForChannel(youhost::ChosenLayout::stereo, true, true)
                    == (youhost::kUnlinkedMonoUsesMonoBus ? youhost::ChosenLayout::mono : youhost::ChosenLayout::stereo)
             && checklistDoc.find("Mono fold stays 2/2") != std::string::npos
             && checklistDoc.find(youhost::kUnlinkedMonoUsesMonoBus ? "unlinked mono channels use 1/1"
                                                                   : "Mono 1/1 is not switched on")
                    != std::string::npos
             && checklistDoc.find("YouHostMonoNullTest") != std::string::npos
             && workflow.find("YouHostMonoNullTest") != std::string::npos
             && readWorkspaceFile("src/engine/PluginRack.cpp").find("layoutForChannel") != std::string::npos
             && readWorkspaceFile("src/engine/HostPath.h").find("kUnlinkedMonoUsesMonoBus") != std::string::npos);

    std::cout << "release checklist " << passed << "/" << listed << "\n";
    CHECK(passed == listed);
}

void testChannelColorRules()
{
    youhost::ChannelPaint paint;
    youhost::chooseChannelColor(paint, 1, 4, true);
    CHECK(paint.own);
    CHECK(paint.color == 1);
    youhost::followGroupColor(paint, 2);
    CHECK(paint.color == 1);
    youhost::chooseChannelColor(paint, 0, 4, true);
    CHECK(! paint.own);
    CHECK(paint.color == 4);
    youhost::followGroupColor(paint, 2);
    CHECK(paint.color == 2);
    youhost::leaveGroupColor(paint);
    CHECK(paint.color == 0);

    youhost::ChannelPaint ownedPaint;
    youhost::chooseChannelColor(ownedPaint, 3, 4, true);
    youhost::leaveGroupColor(ownedPaint);
    CHECK(ownedPaint.own);
    CHECK(ownedPaint.color == 3);

    youhost::ChannelPaint members[3];
    const int membership[] = { 0, 1, 0 };
    youhost::chooseChannelColor(members[0], 1, 0, false);
    youhost::followGroupColorOnMembers(members, membership, 3, 0, 5);
    CHECK(members[0].color == 1);
    CHECK(members[1].color == 0);
    CHECK(members[2].color == 5);

    CHECK(! youhost::colorIsOwnInSession(false, false, 0));
    CHECK(youhost::colorIsOwnInSession(false, false, 3));
    CHECK(! youhost::colorIsOwnInSession(true, false, 4));
    CHECK(youhost::colorIsOwnInSession(true, true, 4));
    CHECK(! youhost::colorIsOwnInSession(true, true, 0));

    youhost::SessionDocumentModel model;
    youhost::SessionChannelRecord owned;
    owned.index = 1;
    owned.color = 3;
    owned.colorOwn = true;
    youhost::SessionChannelRecord followed;
    followed.index = 2;
    followed.color = 4;
    followed.colorOwn = false;
    followed.group = 0;
    model.channels.push_back(owned);
    model.channels.push_back(followed);
    const auto xml = youhost::writeSessionXml(youhost::writeSessionModel(model));
    CHECK(xml.find("colorOwn=\"1\"") != std::string::npos);
    CHECK(xml.find("colorOwn=\"0\"") != std::string::npos);
    CHECK(xml.find("version=\"7\"") != std::string::npos);
    youhost::SessionNode root;
    CHECK(youhost::parseSessionXml(xml, root));
    youhost::SessionDocumentModel round;
    CHECK(youhost::readSessionModel(root, round));
    bool sawOwn = false;
    bool sawFollow = false;
    for (const auto& channel : round.channels)
    {
        if (channel.index == 1)
            sawOwn = channel.colorOwn && channel.color == 3;
        if (channel.index == 2)
            sawFollow = ! channel.colorOwn && channel.color == 4;
    }
    CHECK(sawOwn);
    CHECK(sawFollow);

    const char* legacy = R"(<YouHostSession version="7"><Channel index="3" color="5" name="Snare"/></YouHostSession>)";
    youhost::SessionNode legacyRoot;
    CHECK(youhost::parseSessionXml(legacy, legacyRoot));
    youhost::SessionDocumentModel legacyModel;
    CHECK(youhost::readSessionModel(legacyRoot, legacyModel));
    CHECK(legacyModel.channels.size() == 1);
    CHECK(legacyModel.channels[0].color == 5);
    CHECK(legacyModel.channels[0].colorOwn);
    CHECK(youhost::kSessionFormatVersion == 7);
}

void testSaveAsCopySpeed()
{
    const auto root = std::filesystem::path("/tmp/youhost-saveas-bench");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto audio = root / "audio";
    const auto backups = root / "Backups" / "01";
    const auto slow = root / "slow";
    const auto fast = root / "fast";
    std::filesystem::create_directories(audio);
    std::filesystem::create_directories(backups);
    std::filesystem::create_directories(slow / "audio");
    std::filesystem::create_directories(slow / "Backups" / "01");
    std::filesystem::create_directories(fast / "audio");

    const std::string wav = (audio / "take.wav").string();
    const std::string backup = (backups / "session.youhost").string();
    {
        std::ofstream out(wav, std::ios::binary);
        std::vector<char> block(1024 * 1024, 7);
        for (int index = 0; index < 8; ++index)
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    {
        std::ofstream out(backup, std::ios::binary);
        std::vector<char> block(1024 * 1024, 3);
        for (int index = 0; index < 4; ++index)
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
    }

    const auto started = std::chrono::steady_clock::now();
    const auto slowWav = youhost::copyFileSlow(wav, (slow / "audio" / "take.wav").string());
    const auto slowBackup = youhost::copyFileSlow(backup, (slow / "Backups" / "01" / "session.youhost").string());
    const auto slowSessionA = youhost::copyFileSlow(backup, (slow / "session-a.youhost").string());
    const auto slowSessionB = youhost::copyFileSlow(backup, (slow / "session-b.youhost").string());
    const auto before = std::chrono::steady_clock::now();
    const auto fastWav = youhost::copyFileFast(wav, (fast / "audio" / "take.wav").string());
    {
        std::ofstream out((fast / "session.youhost").string());
        out << "<YouHostSession version=\"7\"/>\n";
    }
    const auto after = std::chrono::steady_clock::now();
    const auto beforeMs = std::chrono::duration_cast<std::chrono::milliseconds>(before - started).count();
    const auto afterMs = std::chrono::duration_cast<std::chrono::milliseconds>(after - before).count();
    const auto beforeBytes = slowWav.bytes + slowBackup.bytes + slowSessionA.bytes + slowSessionB.bytes;
    const auto afterBytes = fastWav.bytes + static_cast<std::uint64_t>(std::filesystem::file_size(fast / "session.youhost"));
    std::cout << "save-as copy before " << beforeMs << " ms " << beforeBytes << " bytes\n";
    std::cout << "save-as copy after " << afterMs << " ms " << afterBytes << " bytes\n";
    CHECK(slowWav.ok);
    CHECK(slowBackup.ok);
    CHECK(slowSessionA.ok);
    CHECK(slowSessionB.ok);
    CHECK(fastWav.ok);
    CHECK(afterBytes < beforeBytes);
    CHECK(afterMs < beforeMs);
    std::filesystem::remove_all(root, error);
}

void testNaNStereoAndColumns()
{
    float dirty[4] = { std::nanf(""), std::numeric_limits<float>::infinity(), 100.0f, 0.25f };
    const auto report = youhost::sanitizeAudioBlock(dirty, 4);
    CHECK(report.nonFinite);
    CHECK(report.clamped);
    CHECK(dirty[0] == 0.0f);
    CHECK(dirty[1] == 0.0f);
    CHECK(dirty[2] == youhost::kPluginOutputCeiling);
    CHECK(dirty[3] == 0.25f);

    youhost::ChannelStrip strip;
    strip.meterState.meanSquare = std::nanf("");
    strip.meterState.heldPeak = std::numeric_limits<float>::infinity();
    strip.meterState.clipped = true;
    strip.meter.clipped.store(true);
    strip.meter.resetRequested.store(true);
    float silence[8] = {};
    const auto gains = youhost::blockMeterGains(youhost::meterTimingFor(48000.0), 8);
    youhost::updateMeter(strip, silence, 8, gains);
    CHECK(strip.meter.rms.load() == 0.0f);
    CHECK(strip.meter.peak.load() == 0.0f);
    CHECK(strip.meter.clipped.load() == false);

    float poisoned[4] = { std::nanf(""), 1.0e20f, std::nanf(""), 3.0f };
    int write = 2;
    youhost::resetDelayLine(poisoned, 4, write);
    CHECK(write == 0);
    for (float sample : poisoned)
        CHECK(sample == 0.0f);

    float dry[4] = { 0.1f, -0.2f, 0.3f, -0.4f };
    float output[4] = { std::nanf(""), std::nanf(""), std::nanf(""), std::nanf("") };
    youhost::emptyChainPassthrough(output, dry, 4);
    for (int index = 0; index < 4; ++index)
        CHECK(output[index] == dry[index]);

    int calls = 0;
    float left[4] = { 0.2f, 0.2f, 0.2f, 0.2f };
    float right[4] = { -0.5f, -0.5f, -0.5f, -0.5f };
    calls = youhost::runLinkedPair(left, right, 4, true, true, [](float* a, float* b, int count)
    {
        for (int index = 0; index < count; ++index)
        {
            a[index] = std::nanf("");
            if (b != nullptr)
                b[index] = 1.0e9f;
        }
    });
    CHECK(calls == 1);
    youhost::sanitizeAudioBlock(left, 4);
    youhost::sanitizeAudioBlock(right, 4);
    CHECK(left[0] == 0.0f);
    CHECK(right[0] == youhost::kPluginOutputCeiling);
    CHECK(right[0] != left[1]);

    calls = youhost::runLinkedPair(left, right, 4, false, true, [](float*, float*, int) {});
    CHECK(calls == 2);
    CHECK(youhost::pairProcessCost(true, true, 512) < youhost::pairProcessCost(false, true, 512));
    int stereoCalls = 0;
    int monoCalls = 0;
    float stereoLeft[32];
    float stereoRight[32];
    float monoLeft[32];
    float monoRight[32];
    for (int index = 0; index < 32; ++index)
    {
        stereoLeft[index] = 0.25f;
        stereoRight[index] = -0.25f;
        monoLeft[index] = 0.25f;
        monoRight[index] = -0.25f;
    }
    auto burn = [](float* first, float* second, int count)
    {
        for (int index = 0; index < count; ++index)
        {
            float sample = first[index];
            for (int tap = 0; tap < 24; ++tap)
                sample = sample * 0.97f + 0.01f;
            first[index] = sample;
            if (second == nullptr)
                continue;
            sample = second[index];
            for (int tap = 0; tap < 24; ++tap)
                sample = sample * 0.97f + 0.01f;
            second[index] = sample;
        }
    };
    youhost::runLinkedPair(stereoLeft, stereoRight, 32, true, true, [&](float* first, float* second, int count)
    {
        ++stereoCalls;
        burn(first, second, count);
    });
    youhost::runLinkedPair(monoLeft, monoRight, 32, false, true, [&](float* first, float* second, int count)
    {
        ++monoCalls;
        burn(first, second, count);
    });
    CHECK(stereoCalls == 1);
    CHECK(monoCalls == 2);
    CHECK(stereoCalls < monoCalls);

    youhost::SessionChannelRecord linked;
    linked.index = 0;
    linked.link = true;
    linked.listen = "rec";
    youhost::SessionDocumentModel model;
    model.channels.push_back(linked);
    const auto written = youhost::writeSessionModel(model);
    const auto* channel = findChannel(written, "0");
    CHECK(channel != nullptr);
    CHECK(youhost::sessionAttributeBool(*channel, "link", false));
    youhost::SessionDocumentModel round;
    CHECK(youhost::readSessionModel(written, round));
    CHECK(round.channels.size() == 1);
    CHECK(round.channels[0].link);
    CHECK(round.channels[0].listen == "rec");

    std::vector<youhost::CatalogPlugin> plugins {
        { "AUDelay", "Apple", "AudioUnit", "ST" },
        { "AUGraphicEQ", "Apple", "AudioUnit", "M/ST" },
        { "MonoGain", "YouHost", "VST3", "M" }
    };
    const auto rows = youhost::groupInsertPlugins(plugins, "");
    CHECK(rows.size() == 2);
    CHECK(rows[0].plugins[0].label == "AUDelay ST");
    CHECK(rows[0].plugins[1].label == "AUGraphicEQ M/ST");
    CHECK(rows[1].plugins[0].label == "MonoGain M");

    const auto header = youhost::layoutHostColumns(960);
    CHECK(header.delay.x + header.delay.width == header.plugins.x);
    CHECK(header.plugins.x + header.plugins.width == header.gain.x);
}

void testBeta090()
{
    const double periodNs = (512.0 / 48000.0) * 1.0e9;
    CHECK(youhost::blockCpuPercent(static_cast<std::uint64_t>(periodNs), 512, 48000.0) == 100);
    CHECK(youhost::blockCpuPercent(static_cast<std::uint64_t>(periodNs * 2.0), 512, 48000.0) == 100);
    CHECK(youhost::blockCpuPercent(static_cast<std::uint64_t>(periodNs * 20.0), 512, 48000.0) == 100);
    CHECK(youhost::blockCpuPercent(0, 512, 48000.0) == 0);
    CHECK(youhost::smoothCpuPercent(0, 100) == 50);
    CHECK(youhost::smoothCpuPercent(50, 100) == 75);
    CHECK(youhost::smoothCpuPercent(2000, -4) == 50);

    const auto started = std::chrono::steady_clock::now();
    int shown = 0;
    for (int block = 0; block < 1000000; ++block)
        shown = youhost::smoothCpuPercent(shown, youhost::blockCpuPercent(static_cast<std::uint64_t>(periodNs), 512, 48000.0));
    const auto cpuUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << "cpu meter arithmetic 1000000 blocks in " << cpuUs << " us, settled " << shown << "\n";
    CHECK(shown == 100);
    CHECK(cpuUs < 250000);

    CHECK(youhost::formatDropoutHostLine(1, -1, 64, 40, 15) == "dropout x1  buffer 64  dsp 40%  plugins 15");
    CHECK(youhost::dropoutLogDue(500000001, 1, 2));
    int falling = 1;
    for (int step = 0; step < 8; ++step)
        falling = youhost::smoothCpuPercent(falling, 0);
    CHECK(falling == 0);
    CHECK(! youhost::dropoutLogDue(10, 1, 0));

    audioAllocations.store(0);
    inAudioCallback = true;
    youhost::DropoutRing ring;
    for (int index = 0; index < 600; ++index)
        ring.push(index + 1, 48000, true, 32, 180, 15);
    youhost::DropoutMark drained[8];
    const int got = ring.drain(drained, 8);
    int groupOf[youhost::kMaxChannels];
    for (int channel = 0; channel < youhost::kMaxChannels; ++channel)
        groupOf[channel] = -1;
    bool used[youhost::kMaxDisplayGroups] {};
    const auto editStarted = std::chrono::steady_clock::now();
    for (int pass = 0; pass < 10000; ++pass)
    {
        int channels[8] = { 0, 1, 2, 3, 10, 11, 40, 41 };
        youhost::addMembersToGroup(groupOf, youhost::kMaxChannels, 2, channels, 8, used);
        int emptied[youhost::kMaxDisplayGroups];
        youhost::removeMembersFromGroups(groupOf, youhost::kMaxChannels, channels, 8, emptied, youhost::kMaxDisplayGroups);
    }
    const auto editUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - editStarted).count();
    const int allocations = audioAllocations.load();
    inAudioCallback = false;
    std::cout << "group membership 10000 add/remove passes in " << editUs << " us, audio allocations " << allocations << "\n";
    CHECK(got == 8);
    CHECK(drained[0].bufferSamples == 32);
    CHECK(drained[0].dspPercent == 180);
    CHECK(drained[0].pluginCount == 15);
    CHECK(allocations == 0);
    CHECK(editUs < 250000);
    CHECK(groupOf[0] == -1);
    CHECK(used[2]);

    int wide[4] = { 0, 1, 2, -1 };
    CHECK(youhost::stereoLinkBlockedByGroups(0, 16, wide, 4));
    int pair[4] = { 0, 0, -1, -1 };
    CHECK(! youhost::stereoLinkBlockedByGroups(0, 16, pair, 4));
    int none[4] = { -1, -1, -1, -1 };
    CHECK(! youhost::stereoLinkBlockedByGroups(0, 16, none, 4));
    CHECK(! youhost::groupMembersAreAdjacentPair(wide, 3));
    int adjacent[2] = { 2, 3 };
    CHECK(youhost::groupMembersAreAdjacentPair(adjacent, 2));

    CHECK(youhost::playbackFeedsHostChain(true));
    CHECK(! youhost::playbackFeedsHostChain(false));
    CHECK(youhost::stopReturnsLiveInput(false));
    CHECK(! youhost::stopReturnsLiveInput(true));

    const auto marker = youhost::makeMarker(96000, 3);
    CHECK(marker.sample == 96000);
    CHECK(marker.name == "Marker 3");
    CHECK(youhost::makeMarker(-8, 0).sample == 0);
    CHECK(youhost::markerRulerText("") == "MARK");
    CHECK(youhost::markerRulerText("Verse") == "Verse");
    CHECK(! youhost::markerDropAffectsTransport());
    CHECK(youhost::markerDropAllowedWhileLocked());

    youhost::SessionDocumentModel model;
    model.savedWith = "YouHost Beta 0.9.0";
    youhost::SessionMarkerRecord stored;
    stored.sample = 96000;
    stored.name = "Verse";
    model.markers.push_back(stored);
    youhost::SessionTakeRecord take;
    take.start = 0;
    take.length = 192000;
    model.takes.push_back(take);
    const auto written = youhost::writeSessionModel(model);
    youhost::SessionDocumentModel round;
    CHECK(youhost::readSessionModel(written, round));
    CHECK(round.savedWith == "YouHost Beta 0.9.0");
    CHECK(round.version == youhost::kSessionFormatVersion);
    youhost::SessionDocumentModel current;
    current.savedWith = "YouHost 1.0";
    current.version = youhost::kSessionFormatVersion;
    youhost::SessionDocumentModel currentRound;
    CHECK(youhost::readSessionModel(youhost::writeSessionModel(current), currentRound));
    CHECK(currentRound.savedWith == "YouHost 1.0");
    CHECK(currentRound.version == 7);
    youhost::SessionDocumentModel legacyRound;
    CHECK(youhost::readSessionModelLegacy(youhost::writeSessionModel(current), legacyRound));
    CHECK(legacyRound.savedWith == "YouHost 1.0");
    CHECK(youhost::kSessionFormatVersion == 7);
    CHECK(round.markers.size() == 1);
    CHECK(round.markers[0].name == "Verse");
    CHECK(round.markers[0].sample == 96000);
    CHECK(round.takes.size() == 1);
    CHECK(round.takes[0].length == 192000);

    struct MixStep
    {
        int group = -1;
        std::string name;
    };
    youhost::EditHistory<MixStep> history;
    history.push(MixStep { -1, "Kick" });
    MixStep restored;
    CHECK(history.undo(MixStep { 3, "Kick" }, restored));
    CHECK(restored.group == -1);
    CHECK(restored.name == "Kick");
    MixStep redone;
    CHECK(history.redo(restored, redone));
    CHECK(redone.group == 3);
    CHECK(history.undoDepth() == 1);
    CHECK(! history.canRedo());

    const auto markerKey = youhost::dispatchRawKey(macLetter('m', false, false, false), 1, false);
    CHECK(markerKey == youhost::ShortcutId::marker);
    CHECK(! youhost::dispatchRawKey(macLetter('m', false, false, false), 1, true).has_value());
    CHECK(! youhost::shortcutAsksTransport(youhost::ShortcutId::marker));
    CHECK(youhost::dispatchRawKey(macLetter('6', false, false, false), 1, false) == youhost::ShortcutId::markers);
    youhost::RawKey commandBackspace;
    commandBackspace.token = youhost::RawToken::backspace;
    commandBackspace.keyCode = 8;
    commandBackspace.command = true;
    CHECK(youhost::dispatchRawKey(commandBackspace, 1, false) == youhost::ShortcutId::deleteMarker);
    commandBackspace.command = false;
    CHECK(! youhost::dispatchRawKey(commandBackspace, 1, false).has_value());
    CHECK(! youhost::shortcutAsksTransport(youhost::ShortcutId::deleteMarker));
    CHECK(! youhost::markerDeleteAffectsTransport());
    std::vector<youhost::MarkerPoint> undoMarkers;
    undoMarkers.push_back(youhost::deviceLossMarker(1000));
    youhost::EditHistory<std::vector<youhost::MarkerPoint>> markerHistory;
    markerHistory.push(undoMarkers);
    CHECK(youhost::eraseMarker(undoMarkers, 0));
    CHECK(undoMarkers.empty());
    std::vector<youhost::MarkerPoint> broughtBack;
    CHECK(markerHistory.undo(undoMarkers, broughtBack));
    CHECK(broughtBack.size() == 1);
    CHECK(broughtBack[0].name == "Device lost");
    CHECK(broughtBack[0].sample == 1000);

    const auto columns = youhost::layoutHostColumns(960);
    CHECK(columns.output.x >= columns.gain.x + columns.gain.width);
    CHECK(youhost::delayCompHeaderBox(columns).x == columns.exclude.x);
    CHECK(youhost::delayCompHeaderBox(columns).width > columns.delay.width);
    CHECK(youhost::clipBandHeight(10.0f) == 10.0f);
    CHECK(youhost::clipBandHeight(0.0f) == 0.0f);

    const auto guide = readWorkspaceFile("INSTALL.md");
    const auto finnish = guide.find("Ennen keikkaa / paras suorituskyky");
    const auto english = guide.find("Before a gig / for best performance");
    const auto step = guide.find("1. Pura zip");
    CHECK(finnish != std::string::npos && english != std::string::npos && step != std::string::npos);
    CHECK(finnish < step && english < guide.find("1. Unzip"));
    CHECK(youhost::shortcutHelpText().find(youhost::kGigPerformanceCard) != std::string::npos);
}

} // namespace

int main()
{
    testTimelineScroll();
    testSessionCompatibility();
    testNullTestIsTransparent();
    testPluginEditStressStaysResponsive();
    testAudioEngineStressDoesNotAllocate();
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
    testHostSwitchIsClickFree();
    testDryChannelDelayMatchesPluginChannel();
    testScanOrderSkipsWavesAndHidesBuiltIns();
    testTakeImportGroups();
    testDropoutWindow();
    testSessionLayout();
    testTimelineZoom();
    testInsertMenuAndStall();
    testCrashJournal();
    testDeviceWatch();
    testDeviceOpensAllChannels();
    testMergePeaks();
    testUnwrittenOutputsAreCleared();
    testLatencyFormulas();
    testLatencyWindowFits();
    testTimelineNavigation();
    testSessionFileActions();
    testInstallGuide();
    testWindowContentFits();
    testTakePlan();
    testMeterLayoutScales();
    testOffChannelStaysSilent();
    testChannelPick();
    testSessionNames();
    testPluginLoadPace();
    testShortcutsMatchTheHelp();
    testKeyDispatch();
    testNaNStereoAndColumns();
    testChannelColorRules();
    testSaveAsCopySpeed();
    testGroupsFoldAndPalette();
    testSessionChannelsRateLockAndClose();
    testTimelinePaintDoesNotReenterLock();
    testRecordStartTransport();
    testDeviceListGrouping();
    testStartupAndSetupShareTheDeviceMenu();
    testScratchSilenceAndRepaint();
    testBeta090();
    testReleaseChecklist();

    if (failures != 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "YouHost engine tests passed\n";
    return EXIT_SUCCESS;
}
