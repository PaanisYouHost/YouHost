#include "engine/ChannelEnable.h"
#include "engine/ChannelSelect.h"
#include "engine/SessionNames.h"
#include "engine/Shortcuts.h"
#include "engine/ChannelListen.h"
#include "engine/SessionChannels.h"
#include "engine/RecordLock.h"
#include "engine/OutputGain.h"
#include "engine/SignalPath.h"
#include "engine/WaveformScale.h"
#include "engine/DisplayLayout.h"
#include "engine/DropoutDetect.h"
#include "engine/DropoutLog.h"
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
#include "engine/InsertMenu.h"
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
    CHECK(card.modes.top + card.modes.height <= card.note.top);
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
    CHECK(kept.width == 480);
    CHECK(kept.height == 280);
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
    CHECK(youhost::shortcutHelpText().find("A smaller window scrolls that card") != std::string::npos);
    const auto legacyState = youhost::parseWindowState("8 8 900 700 fullscreen");
    CHECK(legacyState.valid);
    CHECK(! legacyState.hasFit);
    CHECK(youhost::keepRememberedWindow(legacyState, card.contentWidth, card.contentHeight));
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

    const int anchored = youhost::laneScrollKeepingAnchor(32, 0, 16, 8, 3);
    CHECK(anchored <= 3);
    CHECK(anchored + 8 > 3);
    CHECK(youhost::laneUnderPointer(50.0f, 0.0f, 160.0f, 8, 4) == 6);

    const std::vector<std::vector<int>> lanes = { { 0, 1 }, { 5 } };
    CHECK(youhost::laneIndexContaining(lanes, 5) == 1);
    CHECK(youhost::laneIndexContaining(lanes, 3) == -1);
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

    const auto plan = youhost::planSessionCopy({ "session.youhost" }, { "Kick.wav", "Snare.wav" }, { "01" });
    CHECK(plan.size() == 4);
    CHECK(plan[0].relativePath == "session.youhost");
    CHECK(plan[1].relativePath == "audio/Kick.wav");
    CHECK(plan[2].relativePath == "audio/Snare.wav");
    CHECK(plan[3].relativePath == "Backups/01/session.youhost");
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
        "Artifacts",
        "YouHost-macOS-universal",
        "YouHost.app",
        "Applications",
        "Open Anyway",
        "Avaa silti",
        "xattr -dr com.apple.quarantine /Applications/YouHost.app",
        "SCAN",
        "~/Library/Application Support/Ambient Audio/YouHost",
        "youhost.log",
        "crash-journal.txt",
        "32-64",
        "Do not scan plugins during a show.",
        "Älä skannaa plugineja keikan aikana.",
        "ASENNUSOHJE - INSTALL.txt",
    };
    for (const char* phrase : required)
        CHECK(guide.find(phrase) != std::string::npos);

    CHECK(workflow.find("pack/ASENNUSOHJE - INSTALL.txt") != std::string::npos);
    CHECK(workflow.find("cp \"INSTALL.md\" \"pack/ASENNUSOHJE - INSTALL.txt\"") != std::string::npos);
    CHECK(workflow.find("path: pack") != std::string::npos);

    const auto help = youhost::shortcutHelpText();
    CHECK(help.find("INSTALL.md") != std::string::npos);
    CHECK(help.find("ASENNUSOHJE - INSTALL.txt") != std::string::npos);
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
    CHECK(youhost::setupWindowHeight() >= 480);
    CHECK(youhost::startupWindowHeight() >= 640);
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
    expect(youhost::ShortcutId::goToChannel, 'g', youhost::KeyKind::character, false, false, false);

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
    CHECK(help.find("Option-drag") != std::string::npos);
    CHECK(help.find("REC means the audio passes through the plugins and is recorded") != std::string::npos);
    CHECK(help.find("INPUT means the audio passes through the plugins to the output and is not recorded") != std::string::npos);
    CHECK(help.find("OFF cuts the channel fully: no audio, no plugins, and no recording") != std::string::npos);
    CHECK(help.find("INSTALL.md") != std::string::npos);
    CHECK(help.find("ASENNUSOHJE - INSTALL.txt") != std::string::npos);
    CHECK(help.find("Open Anyway") != std::string::npos);
    CHECK(help.find("xattr -dr com.apple.quarantine /Applications/YouHost.app") != std::string::npos);
    CHECK(help.find("32-64") != std::string::npos);
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
    CHECK(youhost::lanesVisible(40, 0, 70.0f) == 5);
    CHECK(youhost::lanesVisible(4, 0, 200.0f) == 4);
    CHECK(youhost::lanesVisible(40, 100, 400.0f) == 1);
    CHECK(youhost::lanesShownForVerticalStep(128, 0) == 128);

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
    CHECK(! youhost::globalListenNeedsConfirm(true, youhost::ChannelListen::record, recordingModes, 2));
    CHECK(! youhost::globalListenNeedsConfirm(false, youhost::ChannelListen::off, recordingModes, 2));
    const int quietModes[] = { static_cast<int>(youhost::ChannelListen::input) };
    CHECK(! youhost::globalListenNeedsConfirm(true, youhost::ChannelListen::off, quietModes, 1));
    CHECK(! youhost::globalListenNeedsConfirm(true, youhost::ChannelListen::off, nullptr, 0));

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
    CHECK(picked.size() == 2);
    CHECK(picked[0] == 1);
    CHECK(picked[1] == 16);
    const auto all = youhost::channelsForGlobalListen(nullptr, 0, 4);
    CHECK(all.size() == 4);
    CHECK(all[0] == 0);
    CHECK(all[3] == 3);

    const auto help = youhost::shortcutHelpText();
    CHECK(help.find("Offline (no audio) - 128 channels") != std::string::npos);
    CHECK(help.find("not on this card") != std::string::npos);
    CHECK(help.find("Show on mixer") != std::string::npos);
    CHECK(help.find("ALL REC") != std::string::npos);
    CHECK(help.find("Leaving REC while recording asks first") != std::string::npos);
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
    CHECK(offlineStart.startRecording);
    CHECK(offlineStart.files.size() == static_cast<std::size_t>(youhost::kMaxChannels));
    CHECK(youhost::writeMonoWav((dir / offlineStart.files.front()).string(), 48000, samples, 4));
    CHECK(youhost::writeMonoWav((dir / offlineStart.files.back()).string(), 48000, samples, 4));
    CHECK(wavFileHasSamples((dir / "1_1.wav").string()));
    CHECK(wavFileHasSamples((dir / "128_1.wav").string()));

    auto allOff = offline;
    for (auto& channel : allOff.channels)
        channel.rec = false;
    const auto off = youhost::resolveTransport(allOff, youhost::TransportPress::space);
    CHECK(! off.startRecording);
    CHECK(off.alert.find("Every channel is OFF") != std::string::npos);

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
    CHECK(afterNew.files.size() == static_cast<std::size_t>(youhost::kMaxChannels));

    auto noSession = card;
    noSession.hasSession = false;
    const auto folder = youhost::resolveTransport(noSession, youhost::TransportPress::play);
    CHECK(folder.alert.find("no folder") != std::string::npos);

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
    testGroupsFoldAndPalette();
    testSessionChannelsRateLockAndClose();
    testTimelinePaintDoesNotReenterLock();
    testRecordStartTransport();
    testDeviceListGrouping();
    testStartupAndSetupShareTheDeviceMenu();

    if (failures != 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "YouHost engine tests passed\n";
    return EXIT_SUCCESS;
}
