#pragma once

#include <algorithm>
#include <cstddef>

namespace youhost
{

// View only. Groups do not change recording, routing, or the audio callback.
inline constexpr int kMaxDisplayGroups = 10;
inline constexpr float kMinStripWidth = 36.0f;
inline constexpr float kMaxStripWidth = 68.0f;
inline constexpr float kGroupHeaderWidth = 88.0f;

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

} // namespace youhost
