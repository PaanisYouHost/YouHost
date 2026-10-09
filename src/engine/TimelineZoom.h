#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace youhost
{

// Safety cap. The real limit is the span: zoom out always reaches the whole session,
// and zoom in stops at kMinTimelineZoomSamples.
inline constexpr int kMaxTimelineZoomStep = 40;
inline constexpr std::int64_t kMinTimelineZoomSamples = 2048;
inline constexpr float kLaneLabelMinPx = 12.0f;

inline int maxZoomStepForSpan(std::int64_t span) noexcept
{
    if (span < 1)
        span = 1;
    int step = 0;
    std::int64_t visible = span;
    while (visible > kMinTimelineZoomSamples && step < kMaxTimelineZoomStep)
    {
        visible /= 2;
        ++step;
    }
    return step;
}

inline int clampZoomStep(int step, std::int64_t span) noexcept
{
    if (step < 0)
        return 0;
    const int maxStep = maxZoomStepForSpan(span);
    if (step > maxStep)
        return maxStep;
    return step;
}

// Step 0 is the fitted session. Empty sessions show one minute. A recorded session
// runs from 0 to the end of the last take, plus a small margin so the end is visible.
inline std::int64_t fitSpanSamples(std::int64_t contentEnd, double sampleRate) noexcept
{
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    if (contentEnd < 0)
        contentEnd = 0;
    if (contentEnd == 0)
        return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(rate * 60.0)));

    const auto halfSecond = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(rate * 0.5)));
    const auto twoPercent = contentEnd / 50;
    const auto margin = std::max(halfSecond, twoPercent);
    return contentEnd + margin;
}

// Step 0 shows the whole span. Each step halves it, down to a short detail view.
inline std::int64_t zoomVisibleSamples(std::int64_t span, int step) noexcept
{
    if (span < 1)
        span = 1;
    step = clampZoomStep(step, span);
    std::int64_t visible = span;
    for (int index = 0; index < step; ++index)
    {
        if (visible <= kMinTimelineZoomSamples)
            break;
        visible /= 2;
    }
    const auto floor = std::min(span, kMinTimelineZoomSamples);
    if (visible < floor)
        visible = floor;
    return std::max<std::int64_t>(visible, 1);
}

// Vertical zoom. Step 0 fits every lane. Each step shows fewer, taller lanes.
inline int lanesShownForVerticalStep(int totalLanes, int step) noexcept
{
    if (totalLanes < 1)
        totalLanes = 1;
    if (step < 0)
        step = 0;
    int shown = totalLanes;
    while (step > 0 && shown > 1)
    {
        shown = std::max(1, (shown + 1) / 2);
        --step;
    }
    return shown;
}

inline int maxVerticalZoomStep(int totalLanes) noexcept
{
    if (totalLanes < 1)
        return 0;
    int step = 0;
    int shown = totalLanes;
    while (shown > 1 && step < 16)
    {
        shown = std::max(1, (shown + 1) / 2);
        ++step;
    }
    return step;
}

inline bool laneNumberVisible(float heightPx) noexcept
{
    return heightPx >= kLaneLabelMinPx;
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

// While playing or recording, keep the cursor near 75% of the view once it gets
// there. Before that, the take start stays on screen.
inline constexpr double kTransportAnchor = 0.75;

inline std::int64_t anchorPlayhead(std::int64_t span, std::int64_t visible, std::int64_t playhead) noexcept
{
    if (span < 1)
        span = 1;
    if (visible < 1)
        visible = 1;
    if (playhead < 0)
        playhead = 0;
    if (visible >= span)
        return 0;

    const auto maxStart = span - visible;
    auto start = playhead - static_cast<std::int64_t>(std::llround(kTransportAnchor * static_cast<double>(visible)));
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
