#pragma once

#include <algorithm>
#include <cmath>

namespace youhost
{

struct MeterLayout
{
    int columns = 1;
    int rows = 1;
    float cellWidth = 0.0f;
    float cellHeight = 0.0f;
    float originX = 0.0f;
    float contentWidth = 0.0f;
};

// One horizontal row. Strips narrow to fit, down to a width that can still
// show a REC label, then contentWidth grows past the viewport for scrolling.
inline MeterLayout layoutMeters(int channels, float width, float height)
{
    constexpr float minCellWidth = 36.0f;
    constexpr float maxCellWidth = 68.0f;
    constexpr float maxCellHeight = 320.0f;

    MeterLayout layout;
    if (channels <= 0 || width < 1.0f || height < 1.0f)
        return layout;

    const float naturalWidth = width / static_cast<float>(channels);
    layout.columns = channels;
    layout.rows = 1;
    layout.cellWidth = std::clamp(naturalWidth, minCellWidth, maxCellWidth);
    layout.cellHeight = std::min(maxCellHeight, height);
    layout.contentWidth = layout.cellWidth * static_cast<float>(channels);
    layout.originX = std::max(0.0f, (width - layout.contentWidth) * 0.5f);
    return layout;
}

} // namespace youhost
