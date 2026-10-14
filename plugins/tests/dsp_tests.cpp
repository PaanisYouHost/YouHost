#include "dsp/PseCore.h"
#include "dsp/YouMixCore.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
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

void fill(std::vector<float>& buffer, float value)
{
    for (float& sample : buffer)
        sample = value;
}

void processSlots(youmix::YouMixCore& core, std::vector<std::vector<float>>& buffers, int samples, double rate)
{
    for (int slot = 0; slot < static_cast<int>(buffers.size()); ++slot)
    {
        float* data = buffers[static_cast<std::size_t>(slot)].data();
        float* channels[] = { data };
        core.process(slot, channels, 1, samples, rate);
    }
}

void settle(youmix::YouMixCore& core, int slots, float level, int blocks)
{
    std::vector<std::vector<float>> buffers(static_cast<std::size_t>(slots), std::vector<float>(64, level));
    for (int block = 0; block < blocks; ++block)
    {
        for (auto& buffer : buffers)
            fill(buffer, level);
        processSlots(core, buffers, 64, 48000.0);
    }
}

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

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main()
{
    CHECK(youmix::inputMeterFill(0.0f) == 0.0f);
    CHECK(youmix::inputMeterFill(1.0f) == 1.0f);
    CHECK(youmix::reductionMeterFill(0.0f) == 0.0f);
    CHECK(youmix::reductionMeterFill(40.0f) == 1.0f);
    CHECK(youmix::reductionMeterFill(20.0f) > youmix::reductionMeterFill(0.0f));
    CHECK(youmix::meterColourIndex(-40.0f) == 0);
    CHECK(youmix::meterColourIndex(-10.0f) == 1);
    CHECK(youmix::meterColourIndex(0.0f) == 2);

    youmix::YouMixCore mix;
    CHECK(mix.claimSlot() == 0);
    CHECK(mix.claimSlot() == 1);
    CHECK(mix.latencySamples() == 0);
    mix.setGroup(0, 1);
    mix.setGroup(1, 1);
    mix.setPinkDb(0, -160.0f);
    settle(mix, 2, 0.5f, 800);
    const float left = mix.gainFor(0);
    const float right = mix.gainFor(1);
    std::cout << "share " << left << " " << right << " sum " << (left + right) << "\n";
    CHECK(near(left + right, 1.0f, 0.05f));
    CHECK(near(left, 0.5f, 0.08f));
    CHECK(near(right, 0.5f, 0.08f));

    youmix::YouMixCore solo;
    CHECK(solo.claimSlot() == 0);
    solo.setPinkDb(0, -160.0f);
    settle(solo, 1, 0.5f, 400);
    CHECK(solo.gainFor(0) > 0.9f);

    youmix::YouMixCore grouped;
    CHECK(grouped.claimSlot() == 0);
    CHECK(grouped.claimSlot() == 1);
    grouped.setGroup(0, 1);
    grouped.setGroup(1, 2);
    grouped.setPinkDb(0, 0.0f);
    grouped.setPinkDb(1, -160.0f);
    settle(grouped, 2, 0.5f, 800);
    CHECK(grouped.gainFor(0) > 0.35f);
    CHECK(grouped.gainFor(0) < 0.65f);
    CHECK(grouped.gainFor(1) > 0.9f);

    youmix::YouMixCore quiet;
    CHECK(quiet.claimSlot() == 0);
    quiet.setPinkDb(0, -12.0f);
    settle(quiet, 1, 0.05f, 800);
    CHECK(quiet.gainFor(0) > 0.7f);
    std::vector<float> silence(128, 0.0f);
    float* silent[] = { silence.data() };
    quiet.process(0, silent, 1, 128, 48000.0);
    bool stillSilent = true;
    for (float sample : silence)
        stillSilent = stillSilent && sample == 0.0f;
    CHECK(stillSilent);

    youmix::YouMixCore pinkSilent;
    CHECK(pinkSilent.claimSlot() == 0);
    pinkSilent.setPinkDb(0, 0.0f);
    std::vector<float> pinkBuffer(64, 0.0f);
    for (int block = 0; block < 20; ++block)
    {
        fill(pinkBuffer, 0.0f);
        float* pinkChannels[] = { pinkBuffer.data() };
        pinkSilent.process(0, pinkChannels, 1, 64, 48000.0);
    }
    bool pinkStayedOut = true;
    for (float sample : pinkBuffer)
        pinkStayedOut = pinkStayedOut && sample == 0.0f;
    CHECK(pinkStayedOut);

    youmix::YouMixCore settledBypass;
    CHECK(settledBypass.claimSlot() == 0);
    settledBypass.setPinkDb(0, 0.0f);
    settle(settledBypass, 1, 0.05f, 400);
    CHECK(settledBypass.gainFor(0) < 0.65f);
    CHECK(settledBypass.gainFor(0) > 0.35f);
    settledBypass.setBypass(true);
    std::vector<float> returning(64, 0.05f);
    for (int block = 0; block < 800; ++block)
    {
        fill(returning, 0.05f);
        float* returningChannels[] = { returning.data() };
        settledBypass.process(0, returningChannels, 1, 64, 48000.0);
    }
    CHECK(settledBypass.gainFor(0) == 1.0f);
    std::vector<float> settledAfter(96, 0.25f);
    auto settledCopy = settledAfter;
    float* settledChannels[] = { settledAfter.data() };
    settledBypass.process(0, settledChannels, 1, 96, 48000.0);
    CHECK(settledAfter == settledCopy);

    youmix::YouMixCore bypassed;
    CHECK(bypassed.claimSlot() == 0);
    bypassed.setBypass(true);
    std::vector<float> pattern(256);
    for (int index = 0; index < 256; ++index)
        pattern[static_cast<std::size_t>(index)] = ((index * 17) % 50) * 0.01f - 0.2f;
    auto copy = pattern;
    float* patternChannels[] = { pattern.data() };
    bypassed.process(0, patternChannels, 1, 256, 48000.0);
    CHECK(pattern == copy);

    std::vector<float> impulse(32, 0.0f);
    impulse[0] = 0.5f;
    youmix::YouMixCore latency;
    CHECK(latency.claimSlot() == 0);
    float* impulseChannels[] = { impulse.data() };
    latency.process(0, impulseChannels, 1, 32, 48000.0);
    CHECK(impulse[0] == 0.5f);
    CHECK(latency.latencySamples() == 0);

    youmix::YouMixCore smooth;
    CHECK(smooth.claimSlot() == 0);
    CHECK(smooth.claimSlot() == 1);
    smooth.setGroup(0, 1);
    smooth.setGroup(1, 1);
    smooth.setPinkDb(0, -160.0f);
    settle(smooth, 2, 0.5f, 400);
    float previous = smooth.gainFor(0);
    float biggestStep = 0.0f;
    inAudioCallback = true;
    audioAllocations.store(0);
    for (int index = 0; index < 2000; ++index)
    {
        if (index == 400)
            smooth.setPinkDb(0, -6.0f);
        float sampleA = 0.5f;
        float sampleB = 0.5f;
        float* a[] = { &sampleA };
        float* b[] = { &sampleB };
        smooth.process(0, a, 1, 1, 48000.0);
        smooth.process(1, b, 1, 1, 48000.0);
        const float step = std::fabs(smooth.gainFor(0) - previous);
        if (step > biggestStep)
            biggestStep = step;
        previous = smooth.gainFor(0);
        CHECK(sampleA <= 0.5001f);
        CHECK(sampleA > 0.0f);
    }
    const int allocations = audioAllocations.load();
    inAudioCallback = false;
    std::cout << "largest gain step " << biggestStep << " allocations " << allocations << "\n";
    CHECK(biggestStep < 0.02f);
    CHECK(allocations == 0);

    youmix::YouMixCore limited;
    int claimed = 0;
    for (int slot = 0; slot < youmix::kYouMixSlots; ++slot)
    {
        if (limited.claimSlot() >= 0)
            ++claimed;
    }
    CHECK(claimed == youmix::kYouMixSlots);
    CHECK(limited.claimSlot() == -1);
    limited.releaseSlot(3);
    CHECK(limited.claimSlot(3) == 3);

    youmix::YouMixCore voicePreset;
    voicePreset.applyPreset(1);
    const auto voice = voicePreset.settings();
    CHECK(voice.preset == 1);
    CHECK(near(voice.attackMs, 5.0f, 0.01f));
    CHECK(near(voice.releaseMs, 70.0f, 0.01f));
    CHECK(near(voice.thresholdDb, -45.0f, 0.01f));
    CHECK(near(voice.pinkDb[0], -32.0f, 0.01f));
    voicePreset.applyPreset(2);
    const auto singing = voicePreset.settings();
    CHECK(singing.preset == 2);
    CHECK(near(singing.attackMs, 10.0f, 0.01f));
    CHECK(near(singing.releaseMs, 400.0f, 0.01f));
    CHECK(near(singing.thresholdDb, -74.0f, 0.01f));
    CHECK(near(singing.pinkDb[3], -58.0f, 0.01f));
    voicePreset.setThresholdDb(-40.0f);
    CHECK(voicePreset.settings().preset == 0);

    youmix::YouMixCore quietSong;
    quietSong.applyPreset(2);
    CHECK(quietSong.claimSlot() == 0);
    settle(quietSong, 1, 0.001f, 800);
    CHECK(quietSong.gainFor(0) > 0.9f);

    youmix::YouMixCore spread;
    CHECK(spread.claimSlot() == 0);
    CHECK(spread.claimSlot() == 1);
    spread.setGroup(0, 1);
    spread.setGroup(1, 1);
    CHECK(spread.settings().preset == 1);
    spread.setPinkBypass(0, true);
    spread.setPinkDb(0, 0.0f, false);
    CHECK(spread.settings().preset == 1);
    std::vector<float> spreadLoud(64, 0.5f);
    std::vector<float> spreadSoft(64, 0.02f);
    for (int block = 0; block < 500; ++block)
    {
        fill(spreadLoud, 0.5f);
        fill(spreadSoft, 0.02f);
        float* loudChannels[] = { spreadLoud.data() };
        float* softChannels[] = { spreadSoft.data() };
        spread.process(0, loudChannels, 1, 64, 48000.0);
        spread.process(1, softChannels, 1, 64, 48000.0);
    }
    CHECK(near(spread.gainFor(0), 0.5f, 0.12f));
    CHECK(near(spread.gainFor(1), 0.5f, 0.12f));
    spread.setWeightDb(1, 12.0f);
    CHECK(spread.settings().preset == 1);
    for (int block = 0; block < 800; ++block)
    {
        fill(spreadLoud, 0.5f);
        fill(spreadSoft, 0.02f);
        float* loudChannels[] = { spreadLoud.data() };
        float* softChannels[] = { spreadSoft.data() };
        spread.process(0, loudChannels, 1, 64, 48000.0);
        spread.process(1, softChannels, 1, 64, 48000.0);
    }
    CHECK(spread.gainFor(1) > 0.75f);
    CHECK(spread.gainFor(0) < 0.30f);

    youmix::YouMixCore pinkOff;
    CHECK(pinkOff.claimSlot() == 0);
    pinkOff.setPinkDb(0, 0.0f);
    pinkOff.setPinkBypass(0, true);
    settle(pinkOff, 1, 0.5f, 400);
    CHECK(pinkOff.gainFor(0) > 0.9f);

    youmix::YouMixCore pinkClick;
    CHECK(pinkClick.claimSlot() == 0);
    pinkClick.setPinkDb(0, 0.0f);
    settle(pinkClick, 1, 0.5f, 400);
    CHECK(pinkClick.gainFor(0) > 0.35f);
    CHECK(pinkClick.gainFor(0) < 0.65f);
    pinkClick.setPinkBypass(0, true);
    float pinkPrevious = pinkClick.gainFor(0);
    float pinkStep = 0.0f;
    for (int index = 0; index < 2500; ++index)
    {
        float sample = 0.5f;
        float* channels[] = { &sample };
        pinkClick.process(0, channels, 1, 1, 48000.0);
        const float step = std::fabs(pinkClick.gainFor(0) - pinkPrevious);
        if (step > pinkStep)
            pinkStep = step;
        pinkPrevious = pinkClick.gainFor(0);
    }
    CHECK(pinkStep < 0.02f);
    CHECK(pinkClick.gainFor(0) > 0.9f);

    youmix::YouMixCore below;
    CHECK(below.claimSlot() == 0);
    below.setThresholdDb(-20.0f);
    below.setPinkDb(0, -60.0f);
    settle(below, 1, 0.01f, 800);
    CHECK(below.gainFor(0) < 0.05f);

    youmix::PseCore gate;
    CHECK(gate.latencySamples() == 0);
    std::vector<float> loud(64, 0.5f);
    for (int block = 0; block < 200; ++block)
    {
        fill(loud, 0.5f);
        float* channels[] = { loud.data() };
        gate.process(channels, 1, 64, 48000.0);
    }
    CHECK(gate.gainFor() > 0.9f);

    youmix::PseCore partial;
    partial.setThresholdDb(-40.0f);
    partial.setKneeDb(12.0f);
    partial.setRangeDb(18.0f);
    partial.setHangMs(0.0f);
    partial.setReleaseMs(40.0f);
    const float partialLevel = std::pow(10.0f, -43.0f / 20.0f);
    std::vector<float> partialBuffer(64, partialLevel);
    for (int block = 0; block < 400; ++block)
    {
        fill(partialBuffer, partialLevel);
        float* channels[] = { partialBuffer.data() };
        partial.process(channels, 1, 64, 48000.0);
    }
    std::cout << "pse partial gain " << partial.gainFor() << "\n";
    CHECK(partial.gainFor() > 0.45f);
    CHECK(partial.gainFor() < 0.9f);

    youmix::PseCore closed;
    closed.setRangeDb(18.0f);
    closed.setThresholdDb(-40.0f);
    closed.setHangMs(0.0f);
    closed.setReleaseMs(40.0f);
    std::vector<float> soft(64, 0.001f);
    for (int block = 0; block < 400; ++block)
    {
        fill(soft, 0.001f);
        float* channels[] = { soft.data() };
        closed.process(channels, 1, 64, 48000.0);
    }
    std::cout << "pse closed gain " << closed.gainFor() << "\n";
    CHECK(near(closed.gainFor(), youmix::decibelsToGain(-18.0f), 0.03f));
    CHECK(closed.gainFor() > 0.05f);

    youmix::PseCore chatter;
    chatter.setThresholdDb(-40.0f);
    chatter.setKneeDb(12.0f);
    chatter.setRangeDb(18.0f);
    chatter.setHangMs(0.0f);
    chatter.setAttackMs(12.0f);
    chatter.setReleaseMs(30.0f);
    const float chatterLow = std::pow(10.0f, -41.0f / 20.0f);
    const float chatterHigh = std::pow(10.0f, -39.0f / 20.0f);
    float chatterMin = 2.0f;
    float chatterMax = 0.0f;
    std::vector<float> chatterBuffer(64, chatterLow);
    for (int block = 0; block < 600; ++block)
    {
        const float level = (block % 2) == 0 ? chatterLow : chatterHigh;
        fill(chatterBuffer, level);
        float* channels[] = { chatterBuffer.data() };
        chatter.process(channels, 1, 64, 48000.0);
        if (block > 200)
        {
            chatterMin = std::fmin(chatterMin, chatter.gainFor());
            chatterMax = std::fmax(chatterMax, chatter.gainFor());
        }
    }
    std::cout << "pse chatter " << chatterMin << " " << chatterMax << "\n";
    CHECK(chatterMax - chatterMin < 0.2f);
    CHECK(chatterMin > 0.4f);
    CHECK(chatterMax < 0.95f);

    youmix::PseBus bus;
    youmix::PseCore primary(&bus);
    youmix::PseCore bleed(&bus);
    primary.setThresholdDb(-36.0f);
    bleed.setThresholdDb(-36.0f);
    primary.setRangeDb(18.0f);
    bleed.setRangeDb(18.0f);
    primary.setHangMs(0.0f);
    bleed.setHangMs(0.0f);
    primary.setReleaseMs(40.0f);
    bleed.setReleaseMs(40.0f);
    primary.setKneeDb(12.0f);
    bleed.setKneeDb(12.0f);
    std::vector<float> primaryBuffer(64, 0.5f);
    std::vector<float> bleedBuffer(64, 0.01f);
    for (int block = 0; block < 500; ++block)
    {
        fill(primaryBuffer, 0.5f);
        fill(bleedBuffer, 0.01f);
        float* primaryChannels[] = { primaryBuffer.data() };
        float* bleedChannels[] = { bleedBuffer.data() };
        primary.process(primaryChannels, 1, 64, 48000.0);
        bleed.process(bleedChannels, 1, 64, 48000.0);
    }
    std::cout << "pse relative " << primary.gainFor() << " " << bleed.gainFor() << "\n";
    CHECK(primary.gainFor() > 0.8f);
    CHECK(bleed.gainFor() < 0.4f);

    youmix::PseCore moved;
    moved.setHangMs(400.0f);
    moved.setReleaseMs(20.0f);
    moved.setThresholdDb(-20.0f);
    moved.setRangeDb(18.0f);
    moved.setKneeDb(8.0f);
    std::vector<float> movedBuffer(64, 0.5f);
    for (int block = 0; block < 200; ++block)
    {
        fill(movedBuffer, 0.5f);
        float* channels[] = { movedBuffer.data() };
        moved.process(channels, 1, 64, 48000.0);
    }
    CHECK(moved.gainFor() > 0.9f);
    std::vector<float> movedQuiet(10000, 0.001f);
    float* movedQuietChannels[] = { movedQuiet.data() };
    moved.process(movedQuietChannels, 1, 10000, 48000.0);
    std::cout << "pse hang gain " << moved.gainFor() << "\n";
    CHECK(moved.gainFor() < 0.95f);
    CHECK(moved.gainFor() > 0.05f);

    youmix::PseCore floorTrack;
    floorTrack.setThresholdDb(-70.0f);
    floorTrack.setRangeDb(18.0f);
    floorTrack.setKneeDb(8.0f);
    floorTrack.setHangMs(0.0f);
    floorTrack.setReleaseMs(30.0f);
    std::vector<float> room(64, 0.1f);
    for (int block = 0; block < 12000; ++block)
    {
        fill(room, 0.1f);
        float* channels[] = { room.data() };
        floorTrack.process(channels, 1, 64, 48000.0);
    }
    const float underLevel = std::pow(10.0f, -32.0f / 20.0f);
    std::vector<float> under(64, underLevel);
    for (int block = 0; block < 400; ++block)
    {
        fill(under, underLevel);
        float* channels[] = { under.data() };
        floorTrack.process(channels, 1, 64, 48000.0);
    }
    std::cout << "pse floor gain " << floorTrack.gainFor() << "\n";
    CHECK(floorTrack.gainFor() < 0.7f);

    youmix::PseCore pseSettled;
    pseSettled.setRangeDb(18.0f);
    pseSettled.setThresholdDb(-40.0f);
    pseSettled.setHangMs(0.0f);
    pseSettled.setReleaseMs(20.0f);
    std::vector<float> pseSoft(64, 0.001f);
    for (int block = 0; block < 400; ++block)
    {
        fill(pseSoft, 0.001f);
        float* pseSoftChannels[] = { pseSoft.data() };
        pseSettled.process(pseSoftChannels, 1, 64, 48000.0);
    }
    CHECK(pseSettled.gainFor() < 0.5f);
    pseSettled.setBypass(true);
    for (int block = 0; block < 800; ++block)
    {
        fill(pseSoft, 0.001f);
        float* pseSoftChannels[] = { pseSoft.data() };
        pseSettled.process(pseSoftChannels, 1, 64, 48000.0);
    }
    CHECK(pseSettled.gainFor() == 1.0f);
    std::vector<float> pseAfter(64, 0.2f);
    auto pseAfterCopy = pseAfter;
    float* pseAfterChannels[] = { pseAfter.data() };
    pseSettled.process(pseAfterChannels, 1, 64, 48000.0);
    CHECK(pseAfter == pseAfterCopy);

    youmix::PseCore pseBypass;
    pseBypass.setBypass(true);
    std::vector<float> psePattern(128);
    for (int index = 0; index < 128; ++index)
        psePattern[static_cast<std::size_t>(index)] = ((index * 13) % 40) * 0.02f - 0.3f;
    auto pseCopy = psePattern;
    float* pseChannels[] = { psePattern.data() };
    pseBypass.process(pseChannels, 1, 128, 48000.0);
    CHECK(psePattern == pseCopy);

    std::vector<float> pseImpulse(16, 0.0f);
    pseImpulse[0] = 0.25f;
    youmix::PseCore pseLatency;
    float* pseImpulseChannels[] = { pseImpulse.data() };
    pseLatency.process(pseImpulseChannels, 1, 16, 48000.0);
    CHECK(pseImpulse[0] == 0.25f);

    youmix::PseCore pseSmooth;
    float previousGain = 1.0f;
    float pseStep = 0.0f;
    inAudioCallback = true;
    audioAllocations.store(0);
    for (int index = 0; index < 1500; ++index)
    {
        if (index == 200)
            pseSmooth.setThresholdDb(-6.0f);
        float sample = 0.2f;
        float* channels[] = { &sample };
        pseSmooth.process(channels, 1, 1, 48000.0);
        const float step = std::fabs(pseSmooth.gainFor() - previousGain);
        if (step > pseStep)
            pseStep = step;
        previousGain = pseSmooth.gainFor();
        CHECK(sample <= 0.2001f);
        CHECK(sample == sample);
    }
    const int pseAllocations = audioAllocations.load();
    inAudioCallback = false;
    std::cout << "pse largest gain step " << pseStep << " allocations " << pseAllocations << "\n";
    CHECK(pseStep < 0.05f);
    CHECK(pseAllocations == 0);

    youmix::YouMixCore labels;
    const int named = labels.claimSlot();
    labels.setHostName(named, "Lead Vox");
    labels.setName(named, "typed");
    CHECK(std::string(labels.hostName(named)) == "Lead Vox");
    CHECK(std::string(labels.settings().name[named]) == "typed");
    labels.setHostName(named, nullptr);
    CHECK(labels.hostName(named)[0] == 0);
    CHECK(std::string(labels.settings().name[named]) == "typed");
    labels.setName(named, "3 Vocal");
    CHECK(std::string(labels.settings().name[named]) == "3 Vocal");
    char longName[80];
    for (int index = 0; index < 79; ++index)
        longName[index] = 'A';
    longName[79] = 0;
    labels.setName(named, longName);
    CHECK(std::strlen(labels.settings().name[named]) == 31);
    labels.setTrackColour(named, 0xff2266ccu);
    CHECK(labels.trackColour(named) == 0xff2266ccu);
    labels.setHostName(named, "Lead Vox");
    labels.releaseSlot(named);
    CHECK(labels.trackColour(named) == 0);
    CHECK(labels.hostName(named)[0] == 0);
    CHECK(labels.settings().name[named][0] == 0);

    if (failures != 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "YouMix and PSE dsp tests passed\n";
    return EXIT_SUCCESS;
}
