#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

struct DropoutMark
{
    std::int64_t steadyNs = 0;
    std::int64_t timelineSample = -1;
    bool recording = false;
    int bufferSamples = 0;
    int dspPercent = 0;
    int pluginCount = 0;
    // Slowest plugin on the callback that produced this mark. Frozen here so a
    // later short callback cannot replace it when the line is formatted.
    int slowChannel = -1;
    int slowSlot = -1;
    int slowNs = 0;
    char slowName[40] {};
};

struct CpuSample
{
    std::int64_t steadyNs = 0;
    float load = 0.0f;
};

// One producer (the audio thread) and one consumer (the message thread).
// Push never allocates. A burst that overruns the ring keeps the newest marks.
class DropoutRing
{
public:
    static constexpr int kCapacity = 512;

    void push(std::int64_t steadyNs,
              std::int64_t timelineSample,
              bool recording,
              int bufferSamples = 0,
              int dspPercent = 0,
              int pluginCount = 0,
              int slowChannel = -1,
              int slowSlot = -1,
              int slowNs = 0,
              const char* slowName = nullptr) noexcept
    {
        const auto index = write_.load(std::memory_order_relaxed);
        auto& slot = slots_[static_cast<std::size_t>(index % kCapacity)];
        slot.steadyNs = steadyNs;
        slot.timelineSample = timelineSample;
        slot.recording = recording ? 1 : 0;
        slot.bufferSamples = bufferSamples;
        slot.dspPercent = dspPercent;
        slot.pluginCount = pluginCount;
        slot.slowChannel = slowChannel;
        slot.slowSlot = slowSlot;
        slot.slowNs = slowNs;
        slot.slowName[0] = '\0';
        if (slowName != nullptr)
        {
            int name = 0;
            for (; name < static_cast<int>(sizeof(slot.slowName)) - 1 && slowName[name] != '\0'; ++name)
                slot.slowName[name] = slowName[name];
            slot.slowName[name] = '\0';
        }
        write_.store(index + 1, std::memory_order_release);
    }

    int drain(DropoutMark* destination, int maxCount)
    {
        if (destination == nullptr || maxCount <= 0)
            return 0;

        const auto published = write_.load(std::memory_order_acquire);
        if (published - read_ > static_cast<std::uint32_t>(kCapacity))
            read_ = published - static_cast<std::uint32_t>(kCapacity);

        int count = 0;
        while (read_ != published && count < maxCount)
        {
            const auto& slot = slots_[static_cast<std::size_t>(read_ % kCapacity)];
            destination[count].steadyNs = slot.steadyNs;
            destination[count].timelineSample = slot.timelineSample;
            destination[count].recording = slot.recording != 0;
            destination[count].bufferSamples = slot.bufferSamples;
            destination[count].dspPercent = slot.dspPercent;
            destination[count].pluginCount = slot.pluginCount;
            destination[count].slowChannel = slot.slowChannel;
            destination[count].slowSlot = slot.slowSlot;
            destination[count].slowNs = slot.slowNs;
            for (int name = 0; name < static_cast<int>(sizeof(destination[count].slowName)); ++name)
            {
                destination[count].slowName[name] = slot.slowName[name];
                if (slot.slowName[name] == '\0')
                    break;
            }
            ++count;
            ++read_;
        }
        return count;
    }

    void discardPending() noexcept
    {
        read_ = write_.load(std::memory_order_acquire);
    }

private:
    struct Slot
    {
        std::int64_t steadyNs = 0;
        std::int64_t timelineSample = -1;
        int recording = 0;
        int bufferSamples = 0;
        int dspPercent = 0;
        int pluginCount = 0;
        int slowChannel = -1;
        int slowSlot = -1;
        int slowNs = 0;
        char slowName[40] {};
    };

    Slot slots_[kCapacity] {};
    std::atomic<std::uint32_t> write_ { 0 };
    std::uint32_t read_ = 0;
};

// The DROPOUTS range menu. Whole session is what the window shows each time it opens.
inline constexpr int kDropoutRangeTenMinutes = 1;
inline constexpr int kDropoutRangeOneHour = 2;
inline constexpr int kDropoutWholeSessionId = 3;
inline constexpr const char* kDropoutWholeSessionLabel = "Whole session";

inline int dropoutRangeOnOpen() noexcept
{
    return kDropoutWholeSessionId;
}

inline int countMarksInWindow(const DropoutMark* marks, int count, std::int64_t nowNs, std::int64_t windowNs)
{
    if (marks == nullptr || count <= 0)
        return 0;

    const std::int64_t start = windowNs > 0 ? nowNs - windowNs : 0;
    int inside = 0;
    for (int index = 0; index < count; ++index)
    {
        const auto stamp = marks[index].steadyNs;
        if (stamp <= 0 || stamp > nowNs)
            continue;
        if (windowNs > 0 && stamp < start)
            continue;
        ++inside;
    }
    return inside;
}

// The dropout line names the slowest plugin in the coalesced batch. A later
// 16 us callback must not replace a 700 us overrun that is already in the batch.
inline const DropoutMark* slowestInDropoutBatch(const DropoutMark* marks, int count) noexcept
{
    const DropoutMark* best = nullptr;
    if (marks == nullptr || count <= 0)
        return nullptr;
    for (int index = 0; index < count; ++index)
    {
        const auto& mark = marks[index];
        if (mark.slowName[0] == '\0' || mark.slowChannel < 0 || mark.slowSlot < 0)
            continue;
        if (best == nullptr || mark.slowNs > best->slowNs)
            best = &mark;
    }
    return best;
}

inline std::int64_t latestMarkNs(const DropoutMark* marks, int count)
{
    std::int64_t latest = 0;
    if (marks == nullptr)
        return 0;
    for (int index = 0; index < count; ++index)
        if (marks[index].steadyNs > latest)
            latest = marks[index].steadyNs;
    return latest;
}

inline bool windowIsStable(const DropoutMark* marks, int count, std::int64_t nowNs, std::int64_t windowNs)
{
    return countMarksInWindow(marks, count, nowNs, windowNs) == 0;
}

// youhost.log line. The ISO8601 prefix is added by appendHostLog.
// "dropout x3  take 48000  buffer 32  dsp 180%  plugins 15"
inline std::string formatDropoutHostLine(int count, std::int64_t timelineSample, int bufferSamples, int dspPercent, int pluginCount)
{
    if (count < 1)
        count = 1;
    std::string line = "dropout x" + std::to_string(count);
    if (timelineSample >= 0)
        line += "  take " + std::to_string(timelineSample);
    line += "  buffer " + std::to_string(bufferSamples);
    line += "  dsp " + std::to_string(dspPercent) + "%";
    line += "  plugins " + std::to_string(pluginCount);
    return line;
}

// Added after formatDropoutHostLine. Names the plugin that took the longest
// on the callback that dropped out. channel and slot are 1-based, as in the log.
inline std::string formatDropoutSlowest(int channel, int slot, std::string_view name, int microseconds)
{
    if (name.empty() || channel < 1 || slot < 1 || microseconds < 0)
        return {};
    std::string safe;
    safe.reserve(name.size());
    for (char raw : name)
    {
        const auto character = static_cast<unsigned char>(raw);
        if (character < 32 || character == '\t')
            safe.push_back(' ');
        else
            safe.push_back(static_cast<char>(character));
    }
    if (safe.empty())
        return {};
    return "  slowest ch " + std::to_string(channel) + " slot " + std::to_string(slot) + " " + safe
           + " " + std::to_string(microseconds) + "us";
}

// First event logs immediately. Later events inside 500 ms stay in one line.
inline bool dropoutLogDue(std::int64_t nowNs, std::int64_t lastLogNs, int pending) noexcept
{
    if (pending <= 0)
        return false;
    if (lastLogNs <= 0)
        return true;
    return nowNs - lastLogNs >= 500000000;
}

struct DropoutSnapshot
{
    std::int64_t nowNs = 0;
    std::int64_t originNs = 0;
    int total = 0;
    std::int64_t lastSteadyNs = 0;
    std::vector<DropoutMark> marks;
    std::vector<CpuSample> cpu;
};

} // namespace youhost
