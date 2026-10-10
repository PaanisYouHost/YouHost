#pragma once

#include "DisplayLayout.h"
#include "HostLimits.h"
#include "TakePlan.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace youhost
{

struct TimelineRegion
{
    std::int64_t start = 0;
    std::int64_t length = 0;
    std::vector<WavePeak> peaks;
};

struct TimelineLane
{
    int number = 0;
    int color = 0;
    bool group = false;
    std::string title;
    std::vector<TimelineRegion> regions;
};

struct TimelinePicture
{
    double sampleRate = 0.0;
    std::int64_t position = 0;
    std::int64_t length = 0;
    int mode = 0;
    std::vector<TimelineLane> lanes;
};

// Pointers are valid only for the duration of the visit that produced them.
struct TimelineRegionView
{
    int number = 0;
    std::int64_t start = 0;
    std::int64_t length = 0;
    const std::vector<WavePeak>* peaks = nullptr;
};

struct TakeMark
{
    int number = 0;
    std::int64_t start = 0;
};

inline void rememberTakeMark(std::vector<TakeMark>& marks, int number, std::int64_t start)
{
    if (number < 1)
        return;
    for (const auto& mark : marks)
        if (mark.number == number)
            return;
    marks.push_back(TakeMark { number, std::max<std::int64_t>(0, start) });
}

inline float timelineSampleToX(float origin, float width, std::int64_t viewStart, std::int64_t visible, std::int64_t sample) noexcept
{
    if (visible < 1)
        visible = 1;
    if (width < 1.0f)
        width = 1.0f;
    const double ratio = static_cast<double>(sample - viewStart) / static_cast<double>(visible);
    return origin + static_cast<float>(ratio) * width;
}

// Pixel span of one take inside the visible view. A short take stays short.
inline void timelineRegionPixels(float origin,
                                 float width,
                                 std::int64_t viewStart,
                                 std::int64_t visible,
                                 std::int64_t start,
                                 std::int64_t length,
                                 float& x1,
                                 float& x2) noexcept
{
    if (length < 1)
        length = 1;
    x1 = timelineSampleToX(origin, width, viewStart, visible, start);
    x2 = timelineSampleToX(origin, width, viewStart, visible, start + length);
    if (x2 < x1 + 1.0f)
        x2 = x1 + 1.0f;
}

struct TimelineLaneView
{
    int number = 0;
    int color = 0;
    bool group = false;
    std::string title;
    std::vector<int> members;
    std::vector<TimelineRegionView> regions;
};

// Fold a group's member envelopes into one min/max trace. Empty inputs stay empty.
inline std::vector<WavePeak> mergePeakLayers(const std::vector<const std::vector<WavePeak>*>& layers)
{
    std::size_t count = 0;
    for (const auto* layer : layers)
        if (layer != nullptr)
            count = std::max(count, layer->size());

    std::vector<WavePeak> merged(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        float low = 0.0f;
        float high = 0.0f;
        bool any = false;
        for (const auto* layer : layers)
        {
            if (layer == nullptr || index >= layer->size())
                continue;
            low = any ? std::min(low, (*layer)[index].low) : (*layer)[index].low;
            high = any ? std::max(high, (*layer)[index].high) : (*layer)[index].high;
            any = true;
        }
        merged[index] = { low, high };
    }
    return merged;
}

// JUCE-free picture of one recorded take. Peak pointers stay valid for the visit.
struct TimelineTakeSource
{
    int number = 0;
    std::int64_t start = 0;
    std::int64_t length = 0;
    std::array<const std::vector<WavePeak>*, kMaxChannels> peaks {};
    std::array<bool, kMaxChannels> recorded {};
};

struct TimelineChannelInfo
{
    std::string name;
    int color = 0;
    int group = -1;
};

struct TimelineGroupInfo
{
    bool collapsed = false;
    int color = 0;
    std::string name;
};

// Builds the lanes a timeline paint draws. Names come from the caller so the
// paint never locks the recorder again to read them.
inline void buildTimelineLanes(const TimelineTakeSource* takes,
                                int count,
                                const TimelineTakeSource* live,
                                const TimelineChannelInfo* channels,
                                int shown,
                                const TimelineGroupInfo* groups,
                                int groupCount,
                                const std::function<void(const std::vector<TimelineLaneView>&)>& fn)
{
    if (fn == nullptr)
        return;
    if (shown < 0)
        shown = 0;
    if (shown > kMaxChannels)
        shown = kMaxChannels;
    if (count < 0)
        count = 0;

    struct LaneDesc
    {
        bool group = false;
        int number = 0;
        int color = 0;
        std::string title;
        std::vector<int> members;
    };

    std::vector<LaneDesc> descriptions;
    std::array<bool, kMaxDisplayGroups> groupDone {};
    for (int channel = 0; channel < shown; ++channel)
    {
        const int group = channels != nullptr ? channels[channel].group : -1;
        const bool collapsed = group >= 0 && group < groupCount && groups != nullptr && groups[group].collapsed;
        if (collapsed)
        {
            if (groupDone[static_cast<std::size_t>(group)])
                continue;
            groupDone[static_cast<std::size_t>(group)] = true;
            LaneDesc description;
            description.group = true;
            description.color = groups[group].color;
            description.title = groups[group].name.empty() ? "Group " + std::to_string(group + 1) : groups[group].name;
            for (int member = 0; member < shown; ++member)
                if (channels != nullptr && channels[member].group == group)
                    description.members.push_back(member);
            descriptions.push_back(std::move(description));
            continue;
        }

        LaneDesc description;
        description.number = channel + 1;
        description.color = channels != nullptr ? channels[channel].color : 0;
        description.title = channels != nullptr ? channels[channel].name : std::string();
        description.members.push_back(channel);
        descriptions.push_back(std::move(description));
    }

    std::vector<std::vector<WavePeak>> ownedMerges;
    ownedMerges.reserve(descriptions.size() * static_cast<std::size_t>(count + 1));
    std::vector<TimelineLaneView> lanes;
    lanes.reserve(descriptions.size());

    const auto addRegion = [&](TimelineLaneView& lane, const LaneDesc& description, const TimelineTakeSource& take)
    {
        bool any = false;
        for (const int member : description.members)
        {
            const auto index = static_cast<std::size_t>(member);
            const auto* peaks = take.peaks[index];
            if (take.recorded[index] || (peaks != nullptr && ! peaks->empty()))
                any = true;
        }
        if (! any)
            return;

        TimelineRegionView region;
        region.number = take.number;
        region.start = take.start;
        region.length = take.length;
        if (description.members.size() == 1)
        {
            region.peaks = take.peaks[static_cast<std::size_t>(description.members.front())];
        }
        else
        {
            std::vector<const std::vector<WavePeak>*> layers;
            for (const int member : description.members)
                if (take.peaks[static_cast<std::size_t>(member)] != nullptr)
                    layers.push_back(take.peaks[static_cast<std::size_t>(member)]);
            ownedMerges.push_back(mergePeakLayers(layers));
            region.peaks = &ownedMerges.back();
        }
        lane.regions.push_back(region);
    };

    for (const auto& description : descriptions)
    {
        TimelineLaneView lane;
        lane.number = description.number;
        lane.color = description.color;
        lane.group = description.group;
        lane.title = description.title;
        lane.members = description.members;
        for (int index = 0; index < count; ++index)
            addRegion(lane, description, takes[index]);
        if (live != nullptr)
            addRegion(lane, description, *live);
        lanes.push_back(std::move(lane));
    }
    fn(lanes);
}

} // namespace youhost
