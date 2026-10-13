#include "dsp/PseCore.h"
#include "dsp/YouMixCore.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
    CHECK(grouped.gainFor(0) < 0.45f);
    CHECK(grouped.gainFor(1) > 0.9f);

    youmix::YouMixCore quiet;
    CHECK(quiet.claimSlot() == 0);
    quiet.setPinkDb(0, -12.0f);
    settle(quiet, 1, 0.05f, 800);
    CHECK(quiet.gainFor(0) < 0.2f);
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
    CHECK(settledBypass.gainFor(0) < 0.5f);
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

    youmix::PseCore gate;
    CHECK(gate.latencySamples() == 0);
    std::vector<float> loud(64, 0.5f);
    for (int block = 0; block < 200; ++block)
    {
        fill(loud, 0.5f);
        float* channels[] = { loud.data() };
        gate.process(channels, 1, 64, 48000.0);
    }
    CHECK(gate.isOpen());
    CHECK(gate.gainFor() > 0.9f);

    youmix::PseCore closed;
    closed.setRangeDb(18.0f);
    closed.setThresholdDb(-40.0f);
    closed.setHoldMs(0.0f);
    closed.setReleaseMs(40.0f);
    std::vector<float> soft(64, 0.001f);
    for (int block = 0; block < 400; ++block)
    {
        fill(soft, 0.001f);
        float* channels[] = { soft.data() };
        closed.process(channels, 1, 64, 48000.0);
    }
    std::cout << "pse closed gain " << closed.gainFor() << "\n";
    CHECK(! closed.isOpen());
    CHECK(near(closed.gainFor(), youmix::decibelsToGain(-18.0f), 0.03f));

    youmix::PseCore pseSettled;
    pseSettled.setRangeDb(18.0f);
    pseSettled.setThresholdDb(-40.0f);
    pseSettled.setHoldMs(0.0f);
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

    if (failures != 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "YouMix and PSE dsp tests passed\n";
    return EXIT_SUCCESS;
}
