#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace youhost
{

// View only. Groups do not change recording, routing, or the audio callback.
inline constexpr int kMaxDisplayGroups = 10;
inline constexpr float kMinStripWidth = 36.0f;
inline constexpr float kMaxStripWidth = 68.0f;
inline constexpr float kGroupHeaderWidth = 108.0f;

enum class StripKind
{
    channel,
    groupHeader
};

struct StripItem
{
    StripKind kind = StripKind::channel;
    int channel = -1;
    int group = -1;
};

struct BridgeMetrics
{
    float channelWidth = 0.0f;
    float headerWidth = 0.0f;
    float contentWidth = 0.0f;
    float originX = 0.0f;
};

// One row. Strips shrink to fit the viewport, down to kMinStripWidth, and the
// content width grows past the viewport so the caller can scroll.
inline BridgeMetrics layoutBridge(int channelCells, int headerCells, float viewportWidth)
{
    BridgeMetrics metrics;
    if (channelCells < 0)
        channelCells = 0;
    if (headerCells < 0)
        headerCells = 0;

    metrics.headerWidth = headerCells > 0 ? kGroupHeaderWidth : 0.0f;
    const float headers = static_cast<float>(headerCells) * metrics.headerWidth;
    if (channelCells == 0)
    {
        metrics.contentWidth = headers;
        metrics.originX = std::max(0.0f, (viewportWidth - metrics.contentWidth) * 0.5f);
        return metrics;
    }

    const float room = std::max(0.0f, viewportWidth - headers);
    const float natural = room / static_cast<float>(channelCells);
    metrics.channelWidth = std::clamp(natural, kMinStripWidth, kMaxStripWidth);
    metrics.contentWidth = headers + metrics.channelWidth * static_cast<float>(channelCells);
    metrics.originX = std::max(0.0f, (viewportWidth - metrics.contentWidth) * 0.5f);
    return metrics;
}

// Group bars sit at the first member. Expanded members follow that bar, in
// channel order. A collapsed group keeps the bar and hides its channels.
// Ungrouped channels stay in place. Returns the number of items written,
// which can be larger than outCapacity when the buffer is short.
inline int layoutChannelStrips(int channelCount,
                               const int* groupOf,
                               const bool* collapsed,
                               StripItem* out,
                               int outCapacity)
{
    if (channelCount < 0)
        channelCount = 0;
    if (channelCount > 128)
        channelCount = 128;

    bool emitted[kMaxDisplayGroups] = {};
    int written = 0;
    const auto push = [&](StripItem item)
    {
        if (out != nullptr && written < outCapacity)
            out[written] = item;
        ++written;
    };

    for (int channel = 0; channel < channelCount; ++channel)
    {
        const int group = groupOf != nullptr ? groupOf[static_cast<std::size_t>(channel)] : -1;
        if (group < 0 || group >= kMaxDisplayGroups)
        {
            push({ StripKind::channel, channel, -1 });
            continue;
        }
        if (emitted[group])
            continue;

        emitted[group] = true;
        push({ StripKind::groupHeader, -1, group });
        const bool fold = collapsed != nullptr && collapsed[group];
        if (fold)
            continue;

        for (int member = 0; member < channelCount; ++member)
        {
            if (groupOf[static_cast<std::size_t>(member)] == group)
                push({ StripKind::channel, member, group });
        }
    }

    return written;
}

// Next or previous channel strip in display order. Group headers are skipped,
// and channels hidden by a folded group are not in the list. -1 at either end.
inline int adjacentVisibleChannel(const StripItem* strips, int count, int current, int direction) noexcept
{
    if (strips == nullptr || count <= 0 || direction == 0 || current < 0)
        return -1;

    int found = -1;
    for (int index = 0; index < count; ++index)
    {
        if (strips[index].kind == StripKind::channel && strips[index].channel == current)
        {
            found = index;
            break;
        }
    }
    if (found < 0)
        return -1;

    const int step = direction > 0 ? 1 : -1;
    for (int index = found + step; index >= 0 && index < count; index += step)
    {
        if (strips[index].kind == StripKind::channel && strips[index].channel >= 0)
            return strips[index].channel;
    }
    return -1;
}

// Channel strips currently drawn. Group headers and folded members are omitted.
inline int shownChannelNumbers(const StripItem* strips, int count, int* out, int capacity) noexcept
{
    if (strips == nullptr || count <= 0 || out == nullptr || capacity <= 0)
        return 0;
    int written = 0;
    for (int index = 0; index < count && written < capacity; ++index)
    {
        if (strips[index].kind != StripKind::channel || strips[index].channel < 0)
            continue;
        out[written++] = strips[index].channel;
    }
    return written;
}

// Fixed fold mark for a group bar. Collapsed is ▸, open is ▾, then the channel count.
inline std::string groupFoldLabel(bool collapsed, int channelCount)
{
    if (channelCount < 0)
        channelCount = 0;
    return std::string(collapsed ? "\u25B8 " : "\u25BE ") + std::to_string(channelCount) + " ch";
}

// First group index with no member. -1 when every slot is taken.
inline int firstUnusedGroup(const int* membership, int channels, int groupCount) noexcept
{
    if (groupCount <= 0)
        return -1;
    if (groupCount > kMaxDisplayGroups)
        groupCount = kMaxDisplayGroups;
    bool used[kMaxDisplayGroups] = {};
    if (membership != nullptr)
    {
        for (int channel = 0; channel < channels; ++channel)
        {
            const int group = membership[channel];
            if (group >= 0 && group < groupCount)
                used[group] = true;
        }
    }
    for (int group = 0; group < groupCount; ++group)
        if (! used[group])
            return group;
    return -1;
}

struct PlannedGroup
{
    bool created = false;
    int group = -1;
    std::string name;
    int color = 0;
    bool collapsed = false;
};

inline std::string trimmedGroupName(std::string_view text)
{
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t'))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        --end;
    std::string name(text.substr(begin, end - begin));
    if (name.size() > 40)
        name.resize(40);
    return name;
}

// Enter or OK. A new group always starts folded. Esc does not call this.
inline PlannedGroup planNewGroup(bool hasSelection, int freeSlot, std::string_view typedName, int color)
{
    PlannedGroup plan;
    if (! hasSelection || freeSlot < 0 || freeSlot >= kMaxDisplayGroups)
        return plan;
    plan.created = true;
    plan.group = freeSlot;
    plan.color = color < 0 ? 0 : color;
    plan.name = trimmedGroupName(typedName);
    if (plan.name.empty())
        plan.name = "Group " + std::to_string(freeSlot + 1);
    plan.collapsed = true;
    return plan;
}

} // namespace youhost
