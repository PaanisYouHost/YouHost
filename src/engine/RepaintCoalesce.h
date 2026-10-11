#pragma once

#include <atomic>
#include <cstdint>

namespace youhost
{

// Why a slot asked to be drawn. Parameter and latency callbacks must not
// paint the window themselves. They record a note. The message thread takes
// at most one slot paint per 33 ms (30 Hz) and never a full-window paint.
inline constexpr int kRepaintParameter = 1;
inline constexpr int kRepaintLatency = 2;
inline constexpr int kRepaintMove = 3;

inline const char* repaintTriggerName(int trigger) noexcept
{
    if (trigger == kRepaintParameter)
        return "parameter";
    if (trigger == kRepaintLatency)
        return "latency";
    if (trigger == kRepaintMove)
        return "move";
    return "slot";
}

class RepaintCoalescer
{
public:
    void note(int channel, int slot, int trigger) noexcept
    {
        const std::uint64_t packed = (static_cast<std::uint64_t>(channel + 1) & 0xffffu)
                                     | ((static_cast<std::uint64_t>(slot + 1) & 0xffffu) << 16)
                                     | ((static_cast<std::uint64_t>(trigger) & 0xffu) << 32);
        pending_.store(packed, std::memory_order_release);
        notes_.fetch_add(1, std::memory_order_relaxed);
    }

    // False when nothing is waiting, or the last paint was under 33 ms ago.
    // A true result is one slot. wantsFullWindow() stays false.
    bool take(std::int64_t nowMs, int& channel, int& slot, int& trigger) noexcept
    {
        const auto last = lastPaintMs_.load(std::memory_order_relaxed);
        if (last != 0 && nowMs >= last && nowMs - last < 33)
            return false;
        const auto packed = pending_.exchange(0, std::memory_order_acq_rel);
        if (packed == 0)
            return false;
        channel = static_cast<int>(packed & 0xffffu) - 1;
        slot = static_cast<int>((packed >> 16) & 0xffffu) - 1;
        trigger = static_cast<int>((packed >> 32) & 0xffu);
        lastPaintMs_.store(nowMs, std::memory_order_relaxed);
        paints_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    int notes() const noexcept { return static_cast<int>(notes_.load(std::memory_order_relaxed)); }
    int paints() const noexcept { return static_cast<int>(paints_.load(std::memory_order_relaxed)); }
    bool wantsFullWindow() const noexcept { return false; }

private:
    std::atomic<std::uint64_t> pending_ { 0 };
    std::atomic<std::uint32_t> notes_ { 0 };
    std::atomic<std::uint32_t> paints_ { 0 };
    std::atomic<std::int64_t> lastPaintMs_ { 0 };
};

} // namespace youhost
