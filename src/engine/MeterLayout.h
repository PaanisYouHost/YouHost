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
};

// One horizontal meter bridge when the bars fit, otherwise wrapped rows.
// Cells stay narrow so 32 channels read as a desk and 128 still fit.
inline MeterLayout layoutMeters(int channels, float width, float height)
{
    constexpr float minCellWidth = 16.0f;
    constexpr float maxCellWidth = 42.0f;
    constexpr float maxCellHeight = 260.0f;

    MeterLayout layout;
    if (channels <= 0 || width < 1.0f || height < 1.0f)
        return layout;

    int columns = channels;
    if (static_cast<float>(channels) * minCellWidth > width)
        columns = std::max(1, static_cast<int>(std::floor(width / minCellWidth)));

    const int rows = (channels + columns - 1) / columns;
    const float naturalWidth = width / static_cast<float>(columns);
    layout.columns = columns;
    layout.rows = rows;
    layout.cellWidth = std::min(maxCellWidth, naturalWidth);
    layout.cellHeight = std::min(maxCellHeight, height / static_cast<float>(rows));
    const float usedWidth = layout.cellWidth * static_cast<float>(columns);
    layout.originX = std::max(0.0f, (width - usedWidth) * 0.5f);
    return layout;
}

} // namespace youhost
