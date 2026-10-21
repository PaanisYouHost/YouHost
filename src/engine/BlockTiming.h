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
    int slowestChannel = -1;
    int slowestSlot = -1;
    int slowestNs = 0;
    char slowestName[40] {};
};

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
