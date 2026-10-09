#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <pthread.h>

namespace youhost
{

// Phases are plain integers so the audio thread can store one without taking a lock.
inline constexpr int kPhaseIdle = 0;
inline constexpr int kPhaseAudio = 1;
inline constexpr int kPhasePlugins = 2;
inline constexpr int kPhaseMessage = 3;
inline constexpr int kPhaseLoad = 4;
inline constexpr int kPhasePrepare = 5;
inline constexpr int kPhaseEditor = 6;
inline constexpr int kPhaseCloseEditor = 7;
inline constexpr int kPhasePublish = 8;
inline constexpr int kPhaseCapture = 9;

inline constexpr std::int64_t kStallLimitNs = 2000000000LL;

inline const char* stallPhaseName(int phase) noexcept
{
    switch (phase)
    {
        case kPhaseAudio: return "audio";
        case kPhasePlugins: return "plugins";
        case kPhaseMessage: return "message";
        case kPhaseLoad: return "load";
        case kPhasePrepare: return "prepare";
        case kPhaseEditor: return "editor";
        case kPhaseCloseEditor: return "close-editor";
        case kPhasePublish: return "publish";
        case kPhaseCapture: return "capture";
        default: return "idle";
    }
}

// A beat of 0 means that thread has not started. A running thread is stalled
// once its last beat is older than the limit.
inline bool beatIsStale(std::int64_t nowNs, std::int64_t beatNs, std::int64_t limitNs) noexcept
{
    if (beatNs <= 0 || limitNs <= 0 || nowNs <= beatNs)
        return false;
    return nowNs - beatNs > limitNs;
}

struct StallClock
{
    std::atomic<std::int64_t> messageNs { 0 };
    std::atomic<std::int64_t> audioNs { 0 };
    std::atomic<int> messagePhase { kPhaseIdle };
    std::atomic<int> audioPhase { kPhaseIdle };
    std::atomic<int> audioLive { 0 };
    std::atomic<std::uintptr_t> messageThread { 0 };
    std::atomic<std::uintptr_t> audioThread { 0 };
    std::atomic<int> activePlugins { 0 };
    std::atomic<std::uint32_t> nameSeq { 0 };
    char names[160] {};

    static StallClock& get() noexcept
    {
        static StallClock clock;
        return clock;
    }

    void noteThread(std::atomic<std::uintptr_t>& slot) noexcept
    {
        if (slot.load(std::memory_order_relaxed) != 0)
            return;
        const pthread_t self = pthread_self();
        std::uintptr_t bits = 0;
        static_assert(sizeof(pthread_t) <= sizeof(std::uintptr_t), "pthread_t does not fit");
        std::memcpy(&bits, &self, sizeof(self));
        std::uintptr_t expected = 0;
        slot.compare_exchange_strong(expected, bits, std::memory_order_release, std::memory_order_relaxed);
    }

    void setNames(const char* text) noexcept
    {
        nameSeq.fetch_add(1, std::memory_order_release);
        if (text == nullptr)
            text = "";
        std::size_t length = 0;
        while (text[length] != '\0' && length + 1 < sizeof(names))
            ++length;
        std::memcpy(names, text, length);
        names[length] = '\0';
        nameSeq.fetch_add(1, std::memory_order_release);
    }

    // Returns false if the writer was mid-update.
    bool copyNames(char* dest, std::size_t destSize) const noexcept
    {
        if (dest == nullptr || destSize == 0)
            return false;
        dest[0] = '\0';
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const auto first = nameSeq.load(std::memory_order_acquire);
            if ((first & 1u) != 0u)
                continue;
            std::size_t length = 0;
            while (length + 1 < destSize && length < sizeof(names) && names[length] != '\0')
                ++length;
            std::memcpy(dest, names, length);
            dest[length] = '\0';
            if (nameSeq.load(std::memory_order_acquire) == first)
                return true;
        }
        dest[0] = '\0';
        return false;
    }
};

struct PhaseScope
{
    int previous = kPhaseIdle;

    explicit PhaseScope(int phase) noexcept
        : previous(StallClock::get().messagePhase.exchange(phase, std::memory_order_relaxed))
    {
    }

    ~PhaseScope()
    {
        StallClock::get().messagePhase.store(previous, std::memory_order_relaxed);
    }

    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;
};

} // namespace youhost
