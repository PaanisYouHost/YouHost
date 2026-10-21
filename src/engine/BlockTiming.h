#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace youhost
{

// One audio block, written by the audio thread into a fixed ring.
// The message thread formats it. No lock and no allocation on the audio thread.
struct BlockTimingRecord
{
    int samples = 0;
    int prepared = 0;
    int reprepare = 0;
    int vst3Inline = 0;
    int waitNs = 0;
    int recordNs = 0;
    int passNs = 0;
    int pluginNs = 0;
    int meterNs = 0;
    // finishedNs - startedNs. record+pass+plugins+meters is the same span.
    int wallNs = 0;
    // Audio thread CPU time over that span. wall - cpu is time the thread was off CPU.
    int cpuNs = 0;
    int periodNs = 0;
    // How far this callback's start was past the buffer period. Not inside wallNs.
    int lateNs = 0;
    // processBlock only, for the slowest plugin. slowestNs includes the scratch copy.
    int blockNs = 0;
    int slowestChannel = -1;
    int slowestSlot = -1;
    int slowestNs = 0;
    char slowestName[40] {};
};

inline int timingPartsNs(const BlockTimingRecord& block) noexcept
{
    return block.recordNs + block.passNs + block.pluginNs + block.meterNs;
}

// Time the audio thread was not running during the callback. A deschedule
// inflates wall without more plugin work. cpuNs == 0 means it was not measured.
inline int timingOffCpuNs(int wallNs, int cpuNs) noexcept
{
    if (wallNs <= 0 || cpuNs <= 0 || wallNs <= cpuNs)
        return 0;
    return wallNs - cpuNs;
}

// The callback that used the most wall time. The highest plugin time can be a
// different block, and that mismatch is how a 350 us plugin was read against
// a 106% dropout from another callback.
inline int worstCallbackIndex(const BlockTimingRecord* blocks, int count) noexcept
{
    if (blocks == nullptr || count <= 0)
        return 0;
    int worst = 0;
    for (int index = 1; index < count; ++index)
    {
        if (blocks[index].wallNs > blocks[worst].wallNs)
            worst = index;
        else if (blocks[index].wallNs == blocks[worst].wallNs
                 && blocks[index].pluginNs > blocks[worst].pluginNs)
            worst = index;
    }
    return worst;
}

// wall is the callback span. parts is record+pass+plugins+meters and matches wall.
// cpu is the audio thread's own run time. off is wall-cpu (the thread was not
// on CPU). late is the start past the buffer period and is not inside wall.
// block is processBlock. wrap is the scratch copy and sanitize inside the
// plugin timer (slowest - block).
inline std::string formatCallbackSplit(const BlockTimingRecord& block)
{
    const auto us = [](int ns)
    {
        if (ns < 0)
            ns = 0;
        return std::to_string(ns / 1000);
    };
    std::string line = " wall " + us(block.wallNs) + "us";
    line += " parts " + us(timingPartsNs(block)) + "us";
    if (block.cpuNs > 0)
    {
        line += " cpu " + us(block.cpuNs) + "us";
        const int off = timingOffCpuNs(block.wallNs, block.cpuNs);
        if (off > 0)
            line += " off " + us(off) + "us";
    }
    if (block.periodNs > 0)
        line += " period " + us(block.periodNs) + "us";
    if (block.lateNs > 0)
        line += " late " + us(block.lateNs) + "us";
    if (block.blockNs > 0 || block.slowestNs > 0)
    {
        line += " block " + us(block.blockNs) + "us";
        if (block.slowestNs > block.blockNs)
            line += " wrap " + us(block.slowestNs - block.blockNs) + "us";
    }
    return line;
}

inline std::string formatBlockTimingLine(const BlockTimingRecord& block)
{
    const auto us = [](int ns)
    {
        if (ns < 0)
            ns = 0;
        return std::to_string(ns / 1000);
    };
    std::string line = "timing samples " + std::to_string(block.samples);
    line += " prepared " + std::to_string(block.prepared);
    line += " reprepare " + std::to_string(block.reprepare);
    line += " vst3-inline " + std::to_string(block.vst3Inline);
    line += " wait " + us(block.waitNs) + "us";
    line += " record " + us(block.recordNs) + "us";
    line += " pass " + us(block.passNs) + "us";
    line += " plugins " + us(block.pluginNs) + "us";
    line += " meters " + us(block.meterNs) + "us";
    if (block.slowestName[0] == '\0' || block.slowestChannel < 0 || block.slowestSlot < 0)
    {
        line += " slowest none";
        line += formatCallbackSplit(block);
        return line;
    }
    line += " slowest ch " + std::to_string(block.slowestChannel + 1);
    line += " slot " + std::to_string(block.slowestSlot + 1);
    line += " ";
    for (int index = 0; block.slowestName[index] != '\0' && index < static_cast<int>(sizeof(block.slowestName)); ++index)
    {
        const auto character = static_cast<unsigned char>(block.slowestName[index]);
        line.push_back(character < 32 || character == '\t' ? ' ' : static_cast<char>(character));
    }
    line += " " + us(block.slowestNs) + "us";
    // Host time in the plugin stage that is outside the plugin's own timer.
    if (block.pluginNs > block.slowestNs)
        line += " outside " + us(block.pluginNs - block.slowestNs) + "us";
    line += formatCallbackSplit(block);
    return line;
}

// The audio thread publishes the slowest plugin. The message thread copies it
// out for the dropout line. Odd sequence means a write is in progress.
struct SlowestPublish
{
    std::atomic<std::uint32_t> seq { 0 };
    int channel = -1;
    int slot = -1;
    int nanoseconds = 0;
    char name[40] {};

    void publish(int pluginChannel, int pluginSlot, int ns, const char* pluginName) noexcept
    {
        seq.fetch_add(1, std::memory_order_release);
        std::atomic_signal_fence(std::memory_order_release);
        channel = pluginChannel;
        slot = pluginSlot;
        nanoseconds = ns;
        name[0] = '\0';
        if (pluginName != nullptr)
        {
            int index = 0;
            for (; index < static_cast<int>(sizeof(name)) - 1 && pluginName[index] != '\0'; ++index)
                name[index] = pluginName[index];
            name[index] = '\0';
        }
        std::atomic_signal_fence(std::memory_order_release);
        seq.fetch_add(1, std::memory_order_release);
    }

    bool read(int& pluginChannel, int& pluginSlot, int& ns, char* pluginName, int cap) const noexcept
    {
        if (pluginName == nullptr || cap < 2)
            return false;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const auto first = seq.load(std::memory_order_acquire);
            if ((first & 1u) != 0u)
                continue;
            std::atomic_signal_fence(std::memory_order_acquire);
            pluginChannel = channel;
            pluginSlot = slot;
            ns = nanoseconds;
            int index = 0;
            for (; index < cap - 1 && index < static_cast<int>(sizeof(name)) && name[index] != '\0'; ++index)
                pluginName[index] = name[index];
            pluginName[index] = '\0';
            std::atomic_signal_fence(std::memory_order_acquire);
            const auto second = seq.load(std::memory_order_acquire);
            if (first == second && (second & 1u) == 0u)
                return pluginName[0] != '\0' && pluginChannel >= 0 && pluginSlot >= 0;
        }
        return false;
    }
};

// One producer (the audio thread) and one consumer (the message thread).
class BlockTimingRing
{
public:
    static constexpr int kCapacity = 32;

    void push(const BlockTimingRecord& record) noexcept
    {
        const auto index = write_.load(std::memory_order_relaxed);
        slots_[static_cast<std::size_t>(index % kCapacity)] = record;
        write_.store(index + 1, std::memory_order_release);
    }

    int drain(BlockTimingRecord* destination, int maxCount) noexcept
    {
        if (destination == nullptr || maxCount <= 0)
            return 0;
        const auto published = write_.load(std::memory_order_acquire);
        if (published - read_ > static_cast<std::uint32_t>(kCapacity))
            read_ = published - static_cast<std::uint32_t>(kCapacity);
        int count = 0;
        while (read_ != published && count < maxCount)
        {
            destination[count] = slots_[static_cast<std::size_t>(read_ % kCapacity)];
            ++count;
            ++read_;
        }
        return count;
    }

private:
    BlockTimingRecord slots_[kCapacity] {};
    std::atomic<std::uint32_t> write_ { 0 };
    std::uint32_t read_ = 0;
};

} // namespace youhost
