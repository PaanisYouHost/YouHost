#pragma once

#include <algorithm>
#include <vector>

namespace youhost
{

enum class ChannelPick
{
    replace,
    range,
    toggle
};

struct ChannelSelection
{
    std::vector<int> channels;
    int anchor = 0;
};

// Click replaces the selection. Shift-click fills the inclusive range from the
// anchor. Cmd/Ctrl-click adds or removes that one channel and moves the anchor.
inline ChannelSelection pickChannels(ChannelSelection current, int channel, int channelCount, ChannelPick pick)
{
    if (channel < 0 || channel >= channelCount)
        return current;

    if (pick == ChannelPick::replace)
    {
        current.channels.clear();
        current.channels.push_back(channel);
        current.anchor = channel;
        return current;
    }

    if (pick == ChannelPick::toggle)
    {
        const auto found = std::find(current.channels.begin(), current.channels.end(), channel);
        if (found == current.channels.end())
            current.channels.push_back(channel);
        else
            current.channels.erase(found);
        std::sort(current.channels.begin(), current.channels.end());
        current.anchor = channel;
        return current;
    }

    const int first = std::min(current.anchor, channel);
    const int last = std::max(current.anchor, channel);
    current.channels.clear();
    for (int index = first; index <= last && index < channelCount; ++index)
        if (index >= 0)
            current.channels.push_back(index);
    return current;
}

} // namespace youhost
