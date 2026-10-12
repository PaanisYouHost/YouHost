// Remove group used to call syncCompensation(). That took the rack lock and
// published a new graph (delay lines, feed rings, scratch) while plugins were
// loaded. The device callback then ran long on every buffer and the CPU meter
// stayed high until the process restarted. Membership edits no longer publish.
//
// This runs the real AudioEngine callback on a realtime thread at 48 kHz / 32
// samples with 16 Apple Audio Units. The message thread creates, edits,
// recolours, and removes groups, including Remove group on channels that have
// plugins, for more than 30 s.
//
// A deadline miss is the callback thread's own CPU time past the buffer period.
// Wall time is recorded too. One deschedule does not fail the run. A callback
// that is late on every buffer moves the median past the period, and a meter
// that stays near 270% of the quiet reading fails callbackCpuNearBaseline.

#include "AppSettings.h"
#include "engine/AudioEngine.h"
#include "engine/DropoutDetect.h"

#include <CoreFoundation/CoreFoundation.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr double kRate = 48000.0;
constexpr int kBlock = 32;
constexpr int kChannels = 16;
constexpr int kWarmupMs = 3000;
constexpr int kBaselineMs = 3000;
constexpr int kEditMs = 32000;
constexpr int kSettleMs = 2000;
constexpr int kAfterMs = 3000;
constexpr std::uint64_t kPublishBudget = 400;

enum Phase
{
    Warmup = 0,
    Baseline = 1,
    Edit = 2,
    Settle = 3,
    After = 4,
    Stop = 5
};

struct Unit
{
    const char* name;
    const char* identifier;
};

constexpr Unit kDelay { "AUDelay", "AudioUnit:Effects/aufx,dely,appl" };
constexpr Unit kBand { "AUBandpass", "AudioUnit:Effects/aufx,bpas,appl" };
constexpr Unit kEq { "AUGraphicEQ", "AudioUnit:Effects/aufx,greq,appl" };
constexpr Unit kDist { "AUDistortion", "AudioUnit:Effects/aufx,dist,appl" };

Unit unitFor(int channel)
{
    if (channel < 10)
        return kDelay;
    if (channel < 13)
        return kBand;
    if (channel < 15)
        return kEq;
    return kDist;
}

struct Sample
{
    std::int64_t cpuNs = 0;
    std::int64_t wallNs = 0;
};

int fail(const std::string& message)
{
    std::cerr << "group stress failed: " << message << "\n";
    return 1;
}

std::int64_t threadCpuNs()
{
    timespec ts {};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return -1;
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + static_cast<std::int64_t>(ts.tv_nsec);
}

std::int64_t wallNs()
{
    timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
        return -1;
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + static_cast<std::int64_t>(ts.tv_nsec);
}

std::uint64_t nsToTicks(std::int64_t ns)
{
    mach_timebase_info_data_t info {};
    mach_timebase_info(&info);
    if (info.numer == 0)
        return 0;
    return static_cast<std::uint64_t>(ns) * static_cast<std::uint64_t>(info.denom) / static_cast<std::uint64_t>(info.numer);
}

int setRealtime(std::int64_t periodNs)
{
    (void) pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const auto ticks = nsToTicks(periodNs);
    if (ticks == 0 || ticks > 0xffffffffu)
        return -1;
    thread_time_constraint_policy_data_t policy {};
    policy.period = static_cast<std::uint32_t>(ticks);
    policy.computation = policy.period;
    policy.constraint = policy.period;
    policy.preemptible = 1;
    const auto port = pthread_mach_thread_np(pthread_self());
    return static_cast<int>(thread_policy_set(port,
                                              THREAD_TIME_CONSTRAINT_POLICY,
                                              reinterpret_cast<thread_policy_t>(&policy),
                                              THREAD_TIME_CONSTRAINT_POLICY_COUNT));
}

void pumpFor(youhost::AudioEngine& engine, int milliseconds)
{
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(milliseconds);
    while (juce::Time::getMillisecondCounter() < end)
    {
        engine.beatMessageThread();
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    }
}

std::int64_t medianOf(std::vector<std::int64_t> values)
{
    if (values.empty())
        return -1;
    const auto mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    if ((values.size() % 2) == 1)
        return values[mid];
    const auto upper = values[mid];
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid - 1), values.end());
    return (values[mid - 1] + upper) / 2;
}

std::int64_t columnMedian(const std::vector<Sample>& samples, bool cpu)
{
    std::vector<std::int64_t> values;
    values.reserve(samples.size());
    for (const auto& sample : samples)
        values.push_back(cpu ? sample.cpuNs : sample.wallNs);
    return medianOf(std::move(values));
}

int countCpuOverruns(const std::vector<Sample>& samples, std::int64_t periodNs)
{
    int count = 0;
    for (const auto& sample : samples)
        count += youhost::callbackCpuOverrun(sample.cpuNs, periodNs);
    return count;
}

int countWallOverruns(const std::vector<Sample>& samples, std::int64_t periodNs)
{
    int count = 0;
    for (const auto& sample : samples)
        if (sample.wallNs > periodNs)
            ++count;
    return count;
}

std::int64_t maxColumn(const std::vector<Sample>& samples, bool cpu)
{
    std::int64_t peak = 0;
    for (const auto& sample : samples)
        peak = std::max(peak, cpu ? sample.cpuNs : sample.wallNs);
    return peak;
}

bool pushSample(std::vector<Sample>& dest, Sample sample, std::atomic<int>& overflow)
{
    if (dest.size() == dest.capacity())
    {
        overflow.store(1, std::memory_order_relaxed);
        return false;
    }
    dest.push_back(sample);
    return true;
}

int run()
{
    const auto periodNs = youhost::expectedPeriodNs(kRate, kBlock);
    if (periodNs != 666667)
        return fail("buffer period is " + std::to_string(periodNs) + " ns, expected 666667");

    youhost::AppSettings settings;
    youhost::AudioEngine engine(settings);
    engine.beatMessageThread();
    engine.prepareStandaloneCallback(kRate, kBlock, kChannels);

    int band = 0;
    int eq = 0;
    for (int channel = 0; channel < kChannels; ++channel)
    {
        const auto unit = unitFor(channel);
        if (unit.identifier == kBand.identifier)
            ++band;
        if (unit.identifier == kEq.identifier)
            ++eq;
        juce::PluginDescription description;
        description.name = unit.name;
        description.pluginFormatName = "AudioUnit";
        description.fileOrIdentifier = unit.identifier;
        description.manufacturerName = "Apple";
        engine.loadPlugin(channel, 0, description, false);
        std::cout << "load ch " << (channel + 1) << " " << unit.name << " " << unit.identifier << "\n";
    }
    if (band < 1 || eq < 1 || kChannels < 16)
        return fail("the rig needs 16 Apple AUs, including AUBandpass and AUGraphicEQ");

    const auto loadDeadline = juce::Time::getMillisecondCounter() + 120000u;
    std::string loadError;
    while (juce::Time::getMillisecondCounter() < loadDeadline)
    {
        engine.beatMessageThread();
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
        int ready = 0;
        loadError.clear();
        for (int channel = 0; channel < kChannels; ++channel)
        {
            const auto snap = engine.channelSnapshot(channel);
            const auto& slot = snap.slots[0];
            if (slot.occupied)
            {
                ++ready;
                continue;
            }
            if (slot.error.isNotEmpty())
                loadError += "ch " + std::to_string(channel + 1) + " " + slot.error.toStdString() + "\n";
        }
        if (! loadError.empty())
            return fail(loadError);
        if (ready == kChannels)
            break;
    }

    int ready = 0;
    for (int channel = 0; channel < kChannels; ++channel)
    {
        const auto snap = engine.channelSnapshot(channel);
        if (snap.slots[0].occupied)
        {
            ++ready;
            std::cout << "ready ch " << (channel + 1) << " " << snap.slots[0].name.toStdString()
                      << " " << snap.slots[0].inputChannels << "/" << snap.slots[0].outputChannels << "\n";
        }
        else
        {
            loadError += "ch " + std::to_string(channel + 1) + " did not load\n";
        }
    }
    if (ready != kChannels)
        return fail(loadError.empty() ? "plugins did not load" : loadError);

    std::vector<float> tone(static_cast<std::size_t>(kBlock), 0.0f);
    std::vector<float> outputStorage(static_cast<std::size_t>(kChannels * kBlock), 0.0f);
    std::array<const float*, kChannels> inputs {};
    std::array<float*, kChannels> outputs {};
    for (int channel = 0; channel < kChannels; ++channel)
    {
        inputs[static_cast<std::size_t>(channel)] = tone.data();
        outputs[static_cast<std::size_t>(channel)] = outputStorage.data() + static_cast<std::size_t>(channel * kBlock);
    }

    std::vector<Sample> baseline;
    std::vector<Sample> during;
    std::vector<Sample> after;
    baseline.reserve(20000);
    during.reserve(200000);
    after.reserve(20000);
    std::atomic<int> overflow { 0 };
    std::atomic<int> phase { Warmup };
    std::atomic<int> clockFailed { 0 };
    std::atomic<int> realtimeResult { -1 };

    double phaseAngle = 0.0;
    const double phaseStep = 2.0 * 3.14159265358979323846 * 1000.0 / kRate;

    std::thread audio([&]
    {
        realtimeResult.store(setRealtime(periodNs), std::memory_order_relaxed);
        const auto periodTicks = nsToTicks(periodNs);
        auto nextTick = mach_absolute_time();
        while (phase.load(std::memory_order_acquire) != Stop)
        {
            for (int index = 0; index < kBlock; ++index)
            {
                tone[static_cast<std::size_t>(index)] = 0.1f * static_cast<float>(std::sin(phaseAngle));
                phaseAngle += phaseStep;
            }
            const auto mode = phase.load(std::memory_order_acquire);
            const auto cpu0 = threadCpuNs();
            const auto wall0 = wallNs();
            engine.processStandaloneCallback(inputs.data(), kChannels, outputs.data(), kChannels, kBlock);
            const auto cpu1 = threadCpuNs();
            const auto wall1 = wallNs();
            if (cpu0 < 0 || wall0 < 0 || cpu1 < cpu0 || wall1 < wall0)
            {
                clockFailed.store(1, std::memory_order_relaxed);
                break;
            }
            Sample sample { cpu1 - cpu0, wall1 - wall0 };
            if (mode == Baseline)
                pushSample(baseline, sample, overflow);
            else if (mode == Edit)
                pushSample(during, sample, overflow);
            else if (mode == After)
                pushSample(after, sample, overflow);

            if (periodTicks == 0)
                continue;
            nextTick += periodTicks;
            const auto nowTick = mach_absolute_time();
            if (nowTick < nextTick)
                mach_wait_until(nextTick);
            else
                nextTick = nowTick;
        }
    });

    pumpFor(engine, kWarmupMs);
    phase.store(Baseline, std::memory_order_release);
    pumpFor(engine, kBaselineMs);

    const auto serialBefore = engine.graphPublishSerial();
    phase.store(Edit, std::memory_order_release);
    const auto editStart = std::chrono::steady_clock::now();
    int removes = 0;
    std::vector<int> members;
    std::vector<int> one;
    members.reserve(static_cast<std::size_t>(kChannels));
    one.reserve(1);
    int step = 0;
    while (std::chrono::steady_clock::now() - editStart < std::chrono::milliseconds(kEditMs))
    {
        const int group = step % 8;
        members.clear();
        for (int channel = 0; channel < kChannels; ++channel)
            if ((channel % 8) == group)
                members.push_back(channel);
        if (members.empty())
            members.push_back(step % kChannels);
        engine.assignChannelsToGroup(members, group);
        engine.setGroupName(group, "G" + juce::String(step % 1000));
        engine.setGroupColor(group, step % 8);
        engine.addChannelsToGroup(members, group);
        one.clear();
        one.push_back(members.front());
        engine.removeChannelsFromGroup(one);
        engine.clearGroup(group);
        ++removes;
        ++step;
        engine.beatMessageThread();
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
    }
    const auto editMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - editStart).count();
    const auto serialAfter = engine.graphPublishSerial();

    phase.store(Settle, std::memory_order_release);
    pumpFor(engine, kSettleMs);
    phase.store(After, std::memory_order_release);
    pumpFor(engine, kAfterMs);
    phase.store(Stop, std::memory_order_release);
    audio.join();

    int stillLoaded = 0;
    for (int channel = 0; channel < kChannels; ++channel)
        if (engine.channelSnapshot(channel).slots[0].occupied)
            ++stillLoaded;

    const auto baseCpu = columnMedian(baseline, true);
    const auto baseWall = columnMedian(baseline, false);
    const auto editCpu = columnMedian(during, true);
    const auto editWall = columnMedian(during, false);
    const auto afterCpu = columnMedian(after, true);
    const auto afterWall = columnMedian(after, false);
    const int cpuOverruns = countCpuOverruns(baseline, periodNs) + countCpuOverruns(during, periodNs) + countCpuOverruns(after, periodNs);
    const int wallOverruns = countWallOverruns(baseline, periodNs) + countWallOverruns(during, periodNs) + countWallOverruns(after, periodNs);
    const auto publishes = serialAfter - serialBefore;

    std::cout << "group stress channels " << kChannels
              << " periodNs " << periodNs
              << " realtime " << realtimeResult.load()
              << " removes " << removes
              << " editMs " << editMs
              << " callbacks baseline " << baseline.size()
              << " edit " << during.size()
              << " after " << after.size()
              << "\n";
    std::cout << "deadline overruns (callback CPU time > period) " << cpuOverruns << "\n";
    std::cout << "wall samples over period (preemptions, not the deadline count) " << wallOverruns << "\n";
    std::cout << "median CPU ns baseline " << baseCpu << " edit " << editCpu << " after " << afterCpu << "\n";
    std::cout << "median wall ns baseline " << baseWall << " edit " << editWall << " after " << afterWall << "\n";
    std::cout << "max CPU ns baseline " << maxColumn(baseline, true)
              << " edit " << maxColumn(during, true)
              << " after " << maxColumn(after, true) << "\n";
    std::cout << "cpu overruns baseline " << countCpuOverruns(baseline, periodNs)
              << " edit " << countCpuOverruns(during, periodNs)
              << " after " << countCpuOverruns(after, periodNs) << "\n";
    std::cout << "graph publishes during edits " << publishes << "\n";
    std::cout << "plugins still loaded " << stillLoaded << "\n";

    if (clockFailed.load() != 0)
        return fail("thread CPU clock failed");
    if (overflow.load() != 0)
        return fail("callback sample buffer filled; the run was not fully counted");
    if (realtimeResult.load() != 0)
        return fail("realtime thread policy was not set, kern " + std::to_string(realtimeResult.load()));
    if (editMs < 30000)
        return fail("edit window was " + std::to_string(editMs) + " ms");
    if (removes < 100)
        return fail("Remove group ran " + std::to_string(removes) + " times");
    if (baseline.size() < 1000 || during.size() < 10000 || after.size() < 1000)
        return fail("not enough paced callbacks");
    if (stillLoaded != kChannels)
        return fail("plugins were dropped, " + std::to_string(stillLoaded) + " left");
    if (publishes > kPublishBudget)
        return fail("group edits published the graph " + std::to_string(publishes) + " times");
    if (cpuOverruns != 0)
        return fail(std::to_string(cpuOverruns) + " callback CPU overruns");
    if (! youhost::callbackMedianInsidePeriod(baseWall, periodNs)
        || ! youhost::callbackMedianInsidePeriod(editWall, periodNs)
        || ! youhost::callbackMedianInsidePeriod(afterWall, periodNs))
        return fail("median wall time left the buffer period");
    if (! youhost::callbackMedianInsidePeriod(baseCpu, periodNs)
        || ! youhost::callbackMedianInsidePeriod(editCpu, periodNs)
        || ! youhost::callbackMedianInsidePeriod(afterCpu, periodNs))
        return fail("median callback CPU time left the buffer period");
    if (! youhost::callbackCpuNearBaseline(baseCpu, afterCpu) || ! youhost::callbackCpuNearBaseline(baseWall, afterWall))
        return fail("CPU did not return to the quiet baseline");

    std::cout << "group stress passed, deadline overruns 0, CPU back to baseline\n";
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
