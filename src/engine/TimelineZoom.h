#pragma once

#include <algorithm>
#include <cstdint>

namespace youhost
{

inline constexpr int kMaxTimelineZoomStep = 10;

inline int clampZoomStep(int step) noexcept
{
    if (step < 0)
        return 0;
    if (step > kMaxTimelineZoomStep)
        return kMaxTimelineZoomStep;
    return step;
}

// Step 0 shows the whole span. Each step halves it, Pro Tools style.
inline std::int64_t zoomVisibleSamples(std::int64_t span, int step) noexcept
{
    if (span < 1)
        span = 1;
    step = clampZoomStep(step);
    std::int64_t visible = span;
    for (int index = 0; index < step; ++index)
    {
        if (visible <= 1)
            return 1;
        visible /= 2;
    }
    return std::max<std::int64_t>(visible, 1);
}

// Keep the playhead at the same fraction of the view, then clamp so it stays on screen.
inline std::int64_t viewStartKeepingPlayhead(std::int64_t span,
                                            std::int64_t oldStart,
                                            std::int64_t oldVisible,
                                            std::int64_t newVisible,
                                            std::int64_t playhead) noexcept
{
    if (span < 1)
        span = 1;
    if (playhead < 0)
        playhead = 0;
    if (playhead > span)
        playhead = span;
    if (newVisible < 1)
        newVisible = 1;
    if (newVisible >= span)
        return 0;

    double fraction = 0.5;
    if (oldVisible > 0)
        fraction = static_cast<double>(playhead - oldStart) / static_cast<double>(oldVisible);
    if (fraction < 0.0)
        fraction = 0.0;
    if (fraction > 1.0)
        fraction = 1.0;

    const auto maxStart = span - newVisible;
    auto start = playhead - static_cast<std::int64_t>(std::llround(fraction * static_cast<double>(newVisible)));
    if (start < 0)
        start = 0;
    if (start > maxStart)
        start = maxStart;
    if (playhead < start)
        start = playhead;
    if (playhead >= start + newVisible)
        start = playhead - newVisible + 1;
    if (start < 0)
        start = 0;
    if (start > maxStart)
        start = maxStart;
    return start;
}

// During playback, scroll only when the playhead leaves the comfortable middle of the view.
inline std::int64_t followPlayhead(std::int64_t span,
                                  std::int64_t start,
                                  std::int64_t visible,
                                  std::int64_t playhead) noexcept
{
    if (span < 1)
        span = 1;
    if (visible < 1)
        visible = 1;
    if (visible >= span)
        return 0;

    const auto margin = std::max<std::int64_t>(1, visible / 8);
    if (playhead >= start + margin && playhead < start + visible - margin)
        return start;

    const auto maxStart = span - visible;
    auto next = playhead - visible / 3;
    if (next < 0)
        next = 0;
    if (next > maxStart)
        next = maxStart;
    return next;
}

} // namespace youhost
