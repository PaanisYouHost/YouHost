#pragma once

#include "ScanJobs.h"

#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

// REC records and plays through the plugins. HOST plays through and is not
// recorded. OFF is silent and is not recorded. The stored name stays "input"
// so a session written before the label change still opens.
enum class ChannelListen
{
    off = 0,
    input = 1,
    record = 2
};

inline ChannelListen channelListenFromName(std::string_view name) noexcept
{
    if (equalsFold(name, "input") || equalsFold(name, "host"))
        return ChannelListen::input;
    if (equalsFold(name, "off") || equalsFold(name, "0") || equalsFold(name, "false"))
        return ChannelListen::off;
    return ChannelListen::record;
}

inline const char* channelListenName(ChannelListen mode) noexcept
{
    switch (mode)
    {
        case ChannelListen::off: return "off";
        case ChannelListen::input: return "input";
        case ChannelListen::record: return "rec";
    }
    return "rec";
}

inline const char* channelListenLabel(ChannelListen mode) noexcept
{
    switch (mode)
    {
        case ChannelListen::off: return "OFF";
        case ChannelListen::input: return "HOST";
        case ChannelListen::record: return "REC";
    }
    return "REC";
}

inline bool channelListenAudible(ChannelListen mode) noexcept
{
    return mode != ChannelListen::off;
}

inline bool channelListenRecords(ChannelListen mode) noexcept
{
    return mode == ChannelListen::record;
}

inline ChannelListen cycleChannelListen(ChannelListen mode) noexcept
{
    switch (mode)
    {
        case ChannelListen::record: return ChannelListen::input;
        case ChannelListen::input: return ChannelListen::off;
        case ChannelListen::off: return ChannelListen::record;
    }
    return ChannelListen::record;
}

// The word painted red on the first warning. The second dialog title follows.
inline constexpr const char* kListenChangeVerb = "Switch";
inline constexpr const char* kListenChangeSureTitle = "Are you sure?";
inline constexpr const char* kListenChangeDetail = "This changes channels in the running recording.";

enum class ListenChangeKind
{
    now,
    blocked,
    unchanged,
    confirm
};

// Any real mode difference while recording waits for two OK presses.
// The same modes, a stopped transport, and the record lock do not open a dialog.
inline ListenChangeKind classifyListenChange(bool recording, bool locked, bool anyDifference) noexcept
{
    if (locked)
        return ListenChangeKind::blocked;
    if (! anyDifference)
        return ListenChangeKind::unchanged;
    if (! recording)
        return ListenChangeKind::now;
    return ListenChangeKind::confirm;
}

// `now` applies with no dialog. `confirm` applies only after both results are OK (1).
inline bool listenChangeApplies(ListenChangeKind kind, int firstOk, int secondOk) noexcept
{
    if (kind == ListenChangeKind::now)
        return true;
    if (kind == ListenChangeKind::confirm)
        return firstOk == 1 && secondOk == 1;
    return false;
}

// Channel numbers on the strip are 1-based. Example: "Switch ch 5 REC -> HOST".
inline std::string channelListenChangeLine(int channel, ChannelListen from, ChannelListen to)
{
    return std::string(kListenChangeVerb) + " ch " + std::to_string(channel + 1) + " "
           + channelListenLabel(from) + " -> " + channelListenLabel(to);
}

inline std::string globalListenChangeLine(ChannelListen next)
{
    return std::string(kListenChangeVerb) + " every visible channel to " + channelListenLabel(next);
}

// True when a global REC, HOST, or OFF would change any visible channel during a take.
inline bool globalListenNeedsConfirm(bool recording, ChannelListen next, const int* targetModes, int targetCount) noexcept
{
    if (! recording || targetModes == nullptr || targetCount <= 0)
        return false;
    for (int index = 0; index < targetCount; ++index)
        if (targetModes[index] != static_cast<int>(next))
            return true;
    return false;
}

// ALL REC, ALL HOST, and ALL OFF set every visible channel. A selection does
// not narrow them. REC page and HOST page both use this list.
inline std::vector<int> channelsForGlobalListen(const int* selected, int selectedCount, int visible)
{
    (void) selected;
    (void) selectedCount;
    std::vector<int> channels;
    if (visible < 0)
        visible = 0;
    channels.reserve(static_cast<std::size_t>(visible));
    for (int channel = 0; channel < visible; ++channel)
        channels.push_back(channel);
    return channels;
}

// A channel-button click only writes the mode. It does not rebuild a graph.
inline void applyLocalListen(int* modes, int count, int channel, int mode) noexcept
{
    if (modes == nullptr || channel < 0 || channel >= count)
        return;
    modes[channel] = mode;
}

// Page 1 is REC and page 2 is HOST. Both return the same visible channels.
inline std::vector<int> channelsForPageListen(int page, const int* selected, int selectedCount, int visible)
{
    if (page != 1 && page != 2)
        return {};
    return channelsForGlobalListen(selected, selectedCount, visible);
}

} // namespace youhost
