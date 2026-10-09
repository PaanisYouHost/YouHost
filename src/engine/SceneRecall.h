#pragma once

#include "ChannelListen.h"
#include "HostLimits.h"

#include <cstdint>
#include <cstring>

namespace youhost
{

// Decisions for scene recall. The audio callback never calls these.
// Identical plugins take a new state. Anything else is a load or an unload,
// which the rack already prepares off the audio thread.

enum class SlotRecall
{
    leave,
    applyState,
    load,
    unload
};

inline SlotRecall decideSlotRecall(bool currentOccupied,
                                   const char* currentId,
                                   bool wantedOccupied,
                                   const char* wantedId) noexcept
{
    if (! currentOccupied && ! wantedOccupied)
        return SlotRecall::leave;
    if (currentOccupied && ! wantedOccupied)
        return SlotRecall::unload;
    if (! currentOccupied && wantedOccupied)
        return SlotRecall::load;
    if (currentId == nullptr || wantedId == nullptr || currentId[0] == '\0' || wantedId[0] == '\0')
        return SlotRecall::load;
    if (std::strcmp(currentId, wantedId) == 0)
        return SlotRecall::applyState;
    return SlotRecall::load;
}

inline bool recallSkipsChannel(bool safe) noexcept
{
    return safe;
}

// A scene channel that was not stored means REC, 0 dB, and empty slots.
inline bool sceneChannelIsDefault(ChannelListen listen, float outputDb, bool anyPlugin) noexcept
{
    if (listen != ChannelListen::record)
        return false;
    const float delta = outputDb < 0.0f ? -outputDb : outputDb;
    if (delta > 0.01f)
        return false;
    return ! anyPlugin;
}

inline std::uint64_t hashSceneIdentity(const char* text) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    if (text == nullptr)
        return hash;
    for (const auto* cursor = reinterpret_cast<const unsigned char*>(text); *cursor != 0; ++cursor)
    {
        hash ^= static_cast<std::uint64_t>(*cursor);
        hash *= 1099511628211ull;
    }
    return hash;
}

// status is the first MIDI byte. Program Change is 0xC0 plus the channel (0-15).
// listenChannel is 1-16. program is set only when this returns true.
inline bool takeProgramChange(int status, int data1, int listenChannel, int& program) noexcept
{
    if (listenChannel < 1 || listenChannel > 16)
        return false;
    if ((status & 0xF0) != 0xC0)
        return false;
    if ((status & 0x0F) != (listenChannel - 1))
        return false;
    if (data1 < 0 || data1 > 127)
        return false;
    program = data1;
    return true;
}

// remotes[i] is -1 when the scene has no program, otherwise 0-127. First match wins.
inline int sceneForProgram(const int* remotes, int count, int program) noexcept
{
    if (remotes == nullptr || program < 0 || program > 127)
        return -1;
    for (int index = 0; index < count; ++index)
        if (remotes[index] == program)
            return index;
    return -1;
}

inline int clampRemoteProgram(int value) noexcept
{
    if (value < 0)
        return -1;
    if (value > 127)
        return 127;
    return value;
}

inline int clampMidiChannel(int value) noexcept
{
    if (value < 1)
        return 1;
    if (value > 16)
        return 16;
    return value;
}

} // namespace youhost
