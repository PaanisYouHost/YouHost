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

// Cmd+A and A select the channels that are on screen. Folded group members are
// not in that list. The anchor is the first visible channel.
inline ChannelSelection selectAllChannels(const int* channels, int count)
{
    ChannelSelection selection;
    if (channels == nullptr || count <= 0)
        return selection;
    for (int index = 0; index < count; ++index)
    {
        const int channel = channels[index];
        if (channel < 0)
            continue;
        if (std::find(selection.channels.begin(), selection.channels.end(), channel) != selection.channels.end())
            continue;
        selection.channels.push_back(channel);
    }
    if (! selection.channels.empty())
        selection.anchor = selection.channels.front();
    return selection;
}

// Select-all is the REC page only. A text field keeps the key for typing.
inline bool selectAllShortcutApplies(int page, bool textFieldFocused) noexcept
{
    return page == 1 && ! textFieldFocused;
}

// Cmd+G matches the Group button and "Make group from selection". REC page
// only, and a text field keeps the key.
inline bool makeGroupShortcutApplies(int page, bool textFieldFocused) noexcept
{
    return page == 1 && ! textFieldFocused;
}

} // namespace youhost
