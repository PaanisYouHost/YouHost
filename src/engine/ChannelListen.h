#pragma once

#include "ScanJobs.h"

#include <string_view>

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

} // namespace youhost
