#pragma once

#include "TakePlan.h"

#include <algorithm>
#include <cstdint>
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

} // namespace youhost
