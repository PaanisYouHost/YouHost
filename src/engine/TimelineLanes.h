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
    std::int64_t start = 0;
    std::int64_t length = 0;
    const std::vector<WavePeak>* peaks = nullptr;
};

struct TimelineLaneView
{
    int number = 0;
    int color = 0;
    bool group = false;
    std::string title;
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
