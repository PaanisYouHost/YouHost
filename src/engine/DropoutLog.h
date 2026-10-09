#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace youhost
{

struct DropoutMark
{
    std::int64_t steadyNs = 0;
    std::int64_t timelineSample = -1;
    bool recording = false;
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

    void push(std::int64_t steadyNs, std::int64_t timelineSample, bool recording) noexcept
    {
        const auto index = write_.load(std::memory_order_relaxed);
        auto& slot = slots_[static_cast<std::size_t>(index % kCapacity)];
        slot.steadyNs = steadyNs;
        slot.timelineSample = timelineSample;
        slot.recording = recording ? 1 : 0;
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
    };

    Slot slots_[kCapacity] {};
    std::atomic<std::uint32_t> write_ { 0 };
    std::uint32_t read_ = 0;
};

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
