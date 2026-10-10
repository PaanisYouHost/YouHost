#pragma once

#include <cstdint>
#include <cstring>

namespace youhost
{

// The padlock is as tall as the transport row. Play and Record sit inside that row with a vertical inset.
inline constexpr int kRecordLockRowHeight = 44;
inline constexpr int kRecordLockButtonWidth = 64;
inline constexpr int kTransportControlInsetY = 4;
inline constexpr std::uint32_t kRecordLockUnlocked = 0xff6f9e96u;
inline constexpr std::uint32_t kRecordLockLocked = 0xffff2430u;

inline int recordLockControlHeight() noexcept
{
    return kRecordLockRowHeight;
}

inline int transportControlHeight() noexcept
{
    return kRecordLockRowHeight - kTransportControlInsetY * 2;
}

// Armed any time. It engages only while a take is recording.
inline bool recordLockEngaged(bool armed, bool recording) noexcept
{
    return armed && recording;
}

enum class RecordDisrupt
{
    stop,
    space,
    commandSpace,
    recToggle,
    newSession,
    open,
    openRecent,
    clearTimeline,
    importRecordings,
    changeDevice,
    changeRate,
    changeBuffer,
    offlineSwitch,
    channelListen,
    globalListen,
    quit,
    hostEdit,
    save
};

enum class RecordGuard
{
    allow,
    blockWhileRecording,
    blockWhileLocked,
    unlockBeforeQuit
};

inline RecordGuard guardRecordAction(RecordDisrupt action, bool recording, bool lockArmed) noexcept
{
    const bool locked = recordLockEngaged(lockArmed, recording);
    const bool stops = action == RecordDisrupt::stop
                       || action == RecordDisrupt::space
                       || action == RecordDisrupt::commandSpace
                       || action == RecordDisrupt::recToggle;
    const bool device = action == RecordDisrupt::changeDevice
                        || action == RecordDisrupt::changeRate
                        || action == RecordDisrupt::changeBuffer
                        || action == RecordDisrupt::offlineSwitch;
    const bool listen = action == RecordDisrupt::channelListen
                        || action == RecordDisrupt::globalListen;
    const bool closes = action == RecordDisrupt::newSession
                        || action == RecordDisrupt::open
                        || action == RecordDisrupt::openRecent
                        || action == RecordDisrupt::clearTimeline
                        || action == RecordDisrupt::importRecordings
                        || action == RecordDisrupt::quit;

    if (action == RecordDisrupt::hostEdit || action == RecordDisrupt::save)
        return RecordGuard::allow;

    if (locked)
    {
        if (action == RecordDisrupt::quit)
            return RecordGuard::unlockBeforeQuit;
        if (stops || device || listen || closes)
            return RecordGuard::blockWhileLocked;
        return RecordGuard::allow;
    }

    if (recording && (device || closes))
        return RecordGuard::blockWhileRecording;

    return RecordGuard::allow;
}

inline bool recordActionAllowed(RecordDisrupt action, bool recording, bool lockArmed) noexcept
{
    return guardRecordAction(action, recording, lockArmed) == RecordGuard::allow;
}

// The record tap copies raw input. A host edit does not shorten that copy.
inline int copyRawRecordBlock(const float* input, float* recorded, int numSamples) noexcept
{
    if (input == nullptr || recorded == nullptr || numSamples <= 0)
        return 0;
    std::memcpy(recorded, input, sizeof(float) * static_cast<std::size_t>(numSamples));
    return numSamples;
}

} // namespace youhost
