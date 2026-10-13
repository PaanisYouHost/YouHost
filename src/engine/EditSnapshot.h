#pragma once

#include "ChannelListen.h"
#include "DisplayLayout.h"
#include "HostLimits.h"
#include "SessionDocument.h"
#include "SignalPath.h"

#include <array>
#include <cstdint>
#include <string>

namespace youhost
{

// Message-thread mix state. Takes and WAV files are not in here.
struct MixSnapshot
{
    struct GroupSnap
    {
        bool used = false;
        bool collapsed = false;
        int color = 0;
        std::string name;
    };

    std::array<std::string, kMaxChannels> names {};
    std::array<int, kMaxChannels> color {};
    std::array<char, kMaxChannels> colorOwn {};
    std::array<int, kMaxChannels> group {};
    std::array<int, kMaxChannels> listen {};
    std::array<float, kMaxChannels> outputDb {};
    std::array<char, kMaxChannels> linked {};
    std::array<char, kMaxChannels> exclude {};
    std::array<GroupSnap, kMaxDisplayGroups> groups {};
    int align = 0;
    bool bypassAll = false;
    bool hasPlugins = false;
    SessionData plugins;
    // Set only for a marker delete. Undo inserts the marker. Redo erases it.
    // markerOnly means this step does not restore mix fields, so a later gain
    // edit stays its own step above it.
    bool markersCaptured = false;
    bool markerOnly = false;
    bool markerErase = false;
    int markerIndex = -1;
    std::int64_t markerSample = 0;
    std::string markerName;
};

inline bool mixFieldsEqual(const MixSnapshot& left, const MixSnapshot& right) noexcept
{
    if (left.align != right.align || left.bypassAll != right.bypassAll || left.hasPlugins != right.hasPlugins)
        return false;
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        const auto index = static_cast<std::size_t>(channel);
        if (left.names[index] != right.names[index] || left.color[index] != right.color[index]
            || left.colorOwn[index] != right.colorOwn[index] || left.group[index] != right.group[index]
            || left.listen[index] != right.listen[index] || ! sameFloatBits(left.outputDb[index], right.outputDb[index])
            || left.linked[index] != right.linked[index] || left.exclude[index] != right.exclude[index])
            return false;
    }
    for (int group = 0; group < kMaxDisplayGroups; ++group)
    {
        const auto index = static_cast<std::size_t>(group);
        if (left.groups[index].used != right.groups[index].used
            || left.groups[index].collapsed != right.groups[index].collapsed
            || left.groups[index].color != right.groups[index].color
            || left.groups[index].name != right.groups[index].name)
            return false;
    }
    if (left.markersCaptured != right.markersCaptured || left.markerOnly != right.markerOnly
        || left.markerErase != right.markerErase || left.markerIndex != right.markerIndex
        || left.markerSample != right.markerSample || left.markerName != right.markerName)
        return false;
    return true;
}

} // namespace youhost
