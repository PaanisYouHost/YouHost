#pragma once

#include "DisplayLayout.h"

namespace youhost
{

// Membership only. Collapsed, the name, and the audio graph stay as they are.
inline void addMembersToGroup(int* groupOf, int channelCount, int group, const int* channels, int count, bool* used) noexcept
{
    if (groupOf == nullptr || channels == nullptr || group < 0 || group >= kMaxDisplayGroups)
        return;
    if (used != nullptr)
        used[group] = true;
    const int limit = channelCount < kMaxChannels ? channelCount : kMaxChannels;
    for (int index = 0; index < count; ++index)
    {
        const int channel = channels[index];
        if (channel < 0 || channel >= limit)
            continue;
        groupOf[channel] = group;
    }
}

// Ungroups the listed channels. A group with nobody left is reported so the
// caller can clear its used flag and name. Returns how many groups emptied.
inline int removeMembersFromGroups(int* groupOf, int channelCount, const int* channels, int count, int* emptied, int emptiedCapacity) noexcept
{
    if (groupOf == nullptr || channels == nullptr)
        return 0;
    bool touched[kMaxDisplayGroups] {};
    const int limit = channelCount < kMaxChannels ? channelCount : kMaxChannels;
    for (int index = 0; index < count; ++index)
    {
        const int channel = channels[index];
        if (channel < 0 || channel >= limit)
            continue;
        const int group = groupOf[channel];
        if (group < 0 || group >= kMaxDisplayGroups)
            continue;
        touched[group] = true;
        groupOf[channel] = -1;
    }

    int emptiedCount = 0;
    for (int group = 0; group < kMaxDisplayGroups; ++group)
    {
        if (! touched[group])
            continue;
        bool any = false;
        for (int channel = 0; channel < limit; ++channel)
            any = any || groupOf[channel] == group;
        if (any)
            continue;
        if (emptied != nullptr && emptiedCount < emptiedCapacity)
            emptied[emptiedCount] = group;
        ++emptiedCount;
    }
    return emptiedCount;
}

} // namespace youhost
