#pragma once

#include "ScanJobs.h"

#include <string_view>
#include <vector>

namespace youhost
{

// REC records and plays through the plugins. INPUT plays through and is not
// recorded. OFF is silent and is not recorded.
enum class ChannelListen
{
    off = 0,
    input = 1,
    record = 2
};

inline ChannelListen channelListenFromName(std::string_view name) noexcept
{
    if (equalsFold(name, "input"))
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
        case ChannelListen::input: return "INPUT";
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

// True when a global INPUT or OFF would move a channel off REC during a take.
// REC itself, and any change while the transport is stopped, applies at once.
inline bool globalListenNeedsConfirm(bool recording, ChannelListen next, const int* targetModes, int targetCount) noexcept
{
    if (! recording || next == ChannelListen::record || targetModes == nullptr || targetCount <= 0)
        return false;
    for (int index = 0; index < targetCount; ++index)
        if (targetModes[index] == static_cast<int>(ChannelListen::record))
            return true;
    return false;
}

// ALL REC, ALL INPUT, and ALL OFF set every visible channel. A selection does
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
