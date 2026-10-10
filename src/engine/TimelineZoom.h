#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

// Safety cap. The real limit is the span: zoom out always reaches the whole session,
// and zoom in stops at kMinTimelineZoomSamples.
inline constexpr int kMaxTimelineZoomStep = 40;
inline constexpr std::int64_t kMinTimelineZoomSamples = 2048;
inline constexpr float kLaneLabelMinPx = 12.0f;
inline constexpr float kReadableLaneHeightPx = 72.0f;
inline constexpr int kTimelineRulerHeightPx = 18;
inline constexpr float kTimelineTimecodeColumn = 70.0f;

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
// Step 0 wants every lane, but a lane never draws shorter than this, so a long
// session scrolls instead of collapsing into hairlines.
inline constexpr float kMinLaneHeightPx = 14.0f;

inline int lanesVisible(int totalLanes, int verticalStep, float areaHeightPx) noexcept
{
    if (totalLanes < 1)
        totalLanes = 1;
    const int wanted = lanesShownForVerticalStep(totalLanes, verticalStep);
    if (! (areaHeightPx >= kMinLaneHeightPx))
        return wanted;
    const int fitted = std::max(1, static_cast<int>(areaHeightPx / kMinLaneHeightPx));
    return std::max(1, std::min(wanted, fitted));
}

inline int clampLaneIndex(int scroll, int totalLanes, int shown) noexcept
{
    const int maxStart = std::max(0, totalLanes - std::max(1, shown));
    if (scroll < 0)
        return 0;
    if (scroll > maxStart)
        return maxStart;
    return scroll;
}

// One wheel notch is about 1.0. Positive deltaY (scroll up) moves toward lane 0.
inline int applyLaneWheel(float& accumulator, float deltaY, int scroll, int total, int shown) noexcept
{
    accumulator += deltaY;
    const int steps = static_cast<int>(std::trunc(accumulator));
    accumulator -= static_cast<float>(steps);
    return clampLaneIndex(scroll - steps, total, shown);
}

// Positive delta scrolls toward earlier time. One notch moves about 12% of the view.
inline std::int64_t applyTimeWheel(double& accumulator,
                                   float delta,
                                   std::int64_t start,
                                   std::int64_t span,
                                   std::int64_t visible) noexcept
{
    if (span < 1)
        span = 1;
    if (visible < 1)
        visible = 1;
    if (visible > span)
        visible = span;
    const auto maxStart = span - visible;
    accumulator += static_cast<double>(-delta) * static_cast<double>(visible) * 0.12;
    const auto nudge = static_cast<std::int64_t>(accumulator);
    accumulator -= static_cast<double>(nudge);
    auto next = start + nudge;
    if (next < 0)
        next = 0;
    if (next > maxStart)
        next = maxStart;
    return next;
}

enum class TimelineScrollAxis
{
    lanes,
    time,
    gain
};

inline TimelineScrollAxis timelineScrollAxis(float deltaX, float deltaY, bool shift, bool altOrCommand) noexcept
{
    if (altOrCommand)
        return TimelineScrollAxis::gain;
    if (shift)
        return TimelineScrollAxis::time;
    if (std::fabs(deltaX) > std::fabs(deltaY) && deltaX != 0.0f)
        return TimelineScrollAxis::time;
    return TimelineScrollAxis::lanes;
}

inline float timelineTimeDelta(float deltaX, float deltaY, bool shift) noexcept
{
    if (shift)
        return std::fabs(deltaX) > std::fabs(deltaY) ? deltaX : deltaY;
    return deltaX;
}

struct TimelineBarRange
{
    double limit = 1.0;
    double start = 0.0;
    double size = 1.0;
};

inline TimelineBarRange timeBarRange(std::int64_t start, std::int64_t visible, std::int64_t span) noexcept
{
    if (span < 1)
        span = 1;
    if (visible < 1)
        visible = 1;
    if (visible > span)
        visible = span;
    if (start < 0)
        start = 0;
    const auto maxStart = span - visible;
    if (start > maxStart)
        start = maxStart;
    return { static_cast<double>(span), static_cast<double>(start), static_cast<double>(visible) };
}

// Keep the lane under the pointer (or the selected lane) on screen while the
// lane height changes. The anchor stays at the same fraction of the view.
inline int laneScrollKeepingAnchor(int total, int oldScroll, int oldShown, int newShown, int anchorLane) noexcept
{
    if (total < 1)
        total = 1;
    if (oldShown < 1)
        oldShown = 1;
    if (newShown < 1)
        newShown = 1;
    if (anchorLane < 0)
        anchorLane = oldScroll;
    if (anchorLane >= total)
        anchorLane = total - 1;
    if (newShown >= total)
        return 0;

    int offset = anchorLane - oldScroll;
    if (offset < 0)
        offset = 0;
    if (offset >= oldShown)
        offset = oldShown - 1;

    int newOffset = offset;
    if (oldShown > 1 && newShown > 1)
        newOffset = static_cast<int>((static_cast<long long>(offset) * (newShown - 1)) / (oldShown - 1));
    if (newOffset < 0)
        newOffset = 0;
    if (newOffset >= newShown)
        newOffset = newShown - 1;
    return clampLaneIndex(anchorLane - newOffset, total, newShown);
}

struct LaneFocus
{
    int verticalStep = 0;
    int laneScroll = 0;
};

// Clicking a channel on REC makes that lane tall enough to read and puts it at the top.
inline LaneFocus focusReadableLane(int totalLanes, int lane, float areaHeightPx) noexcept
{
    LaneFocus focus;
    if (totalLanes < 1)
        totalLanes = 1;
    if (lane < 0)
        lane = 0;
    if (lane >= totalLanes)
        lane = totalLanes - 1;

    const int maxStep = maxVerticalZoomStep(totalLanes);
    int step = 0;
    for (; step < maxStep; ++step)
    {
        const int shown = std::max(1, lanesVisible(totalLanes, step, areaHeightPx));
        if (areaHeightPx / static_cast<float>(shown) >= kReadableLaneHeightPx)
            break;
    }
    focus.verticalStep = step;
    const int shown = std::max(1, lanesVisible(totalLanes, focus.verticalStep, areaHeightPx));
    focus.laneScroll = clampLaneIndex(lane, totalLanes, shown);
    return focus;
}

// FIT puts every lane back. Step 0 is the fitted view.
inline LaneFocus fitAllLaneFocus() noexcept
{
    return {};
}

// Scroll just enough that `lane` is fully inside the visible range.
inline int laneScrollToReveal(int total, int shown, int scroll, int lane) noexcept
{
    if (total < 1)
        total = 1;
    if (shown < 1)
        shown = 1;
    if (lane < 0)
        lane = 0;
    if (lane >= total)
        lane = total - 1;
    scroll = clampLaneIndex(scroll, total, shown);
    if (lane < scroll)
        return clampLaneIndex(lane, total, shown);
    if (lane >= scroll + shown)
        return clampLaneIndex(lane - shown + 1, total, shown);
    return scroll;
}

inline int laneUnderPointer(float y, float areaY, float areaHeight, int shown, int scroll) noexcept
{
    if (shown < 1)
        shown = 1;
    if (! (areaHeight > 0.0f))
        return std::max(0, scroll);
    const float laneHeight = areaHeight / static_cast<float>(shown);
    int index = static_cast<int>((y - areaY) / laneHeight);
    if (index < 0)
        index = 0;
    if (index >= shown)
        index = shown - 1;
    return scroll + index;
}

inline int laneIndexContaining(const std::vector<std::vector<int>>& lanes, int channel) noexcept
{
    for (int index = 0; index < static_cast<int>(lanes.size()); ++index)
        for (const int member : lanes[static_cast<std::size_t>(index)])
            if (member == channel)
                return index;
    return -1;
}

// Top-left lane tag: "12 Kick", or the group name. It is drawn in the visible
// lane, so horizontal scrolling does not move it. Take names stay in the ruler.
inline std::string laneCornerLabel(int number, const std::string& name, bool group)
{
    if (group)
        return name.empty() ? "Group" : name;
    if (number < 1)
        return name;
    if (name.empty() || name == std::to_string(number))
        return std::to_string(number);
    return std::to_string(number) + "  " + name;
}

struct TimelineLabelRect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

inline bool timelineLabelsOverlap(TimelineLabelRect left, TimelineLabelRect right) noexcept
{
    if (left.width <= 0.0f || left.height <= 0.0f || right.width <= 0.0f || right.height <= 0.0f)
        return false;
    return left.x < right.x + right.width && right.x < left.x + left.width
           && left.y < right.y + right.height && right.y < left.y + left.height;
}

inline bool timelineLabelInside(TimelineLabelRect box, float x, float y, float width, float height) noexcept
{
    return box.width > 0.0f && box.height > 0.0f
           && box.x >= x - 0.01f && box.y >= y - 0.01f
           && box.x + box.width <= x + width + 0.01f
           && box.y + box.height <= y + height + 0.01f;
}

// "TAKE 1", or just the number when the next take is close. The box stays in
// the ruler, clear of the clocks at each end, and never enters a lane.
inline std::string takeRulerLabelText(int number, float room)
{
    if (number < 1)
        number = 1;
    if (room < 58.0f)
        return std::to_string(number);
    return "TAKE " + std::to_string(number);
}

inline TimelineLabelRect takeRulerLabelRect(float rulerX,
                                            float rulerY,
                                            float rulerWidth,
                                            float rulerHeight,
                                            float markX,
                                            float nextX) noexcept
{
    TimelineLabelRect box;
    if (! (rulerWidth > 0.0f) || ! (rulerHeight > 0.0f))
        return box;
    const float room = nextX - markX;
    float width = std::min(78.0f, std::max(18.0f, room - 6.0f));
    const float height = std::min(14.0f, std::max(8.0f, rulerHeight - 4.0f));
    const float minX = rulerX + kTimelineTimecodeColumn;
    const float maxRight = rulerX + rulerWidth - kTimelineTimecodeColumn;
    if (maxRight - minX < 18.0f)
        return box;
    if (width > maxRight - minX)
        width = maxRight - minX;
    float x = markX + 3.0f;
    if (x < minX)
        x = minX;
    if (x + width > maxRight)
        x = maxRight - width;
    box.x = x;
    box.y = rulerY + (rulerHeight - height) * 0.5f;
    box.width = width;
    box.height = height;
    return box;
}

// Pinned to the top-left of one lane. Horizontal scrolling does not move it.
inline TimelineLabelRect laneCornerLabelRect(float laneX, float laneY, float laneWidth, float laneHeight) noexcept
{
    TimelineLabelRect box;
    if (! (laneWidth > 4.0f) || ! (laneHeight > 4.0f))
        return box;
    box.height = std::min(16.0f, laneHeight);
    box.height = std::min(box.height, std::max(10.0f, laneHeight - 2.0f));
    box.width = std::min(200.0f, std::max(0.0f, laneWidth - 4.0f));
    box.x = laneX + 2.0f;
    box.y = laneY + 1.0f;
    if (box.y + box.height > laneY + laneHeight)
        box.y = laneY;
    return box;
}

// The Go field. A positive result is the 1-based channel number.
inline int parseGoToChannel(std::string_view text) noexcept
{
    int value = 0;
    bool any = false;
    for (const char character : text)
    {
        if (character == ' ')
            continue;
        if (character < '0' || character > '9')
            return 0;
        any = true;
        value = value * 10 + (character - '0');
        if (value > 999)
            return 0;
    }
    return any ? value : 0;
}

inline TimelineBarRange laneBarRange(int scroll, int shown, int total) noexcept
{
    if (total < 1)
        total = 1;
    if (shown < 1)
        shown = 1;
    if (shown > total)
        shown = total;
    scroll = clampLaneIndex(scroll, total, shown);
    return { static_cast<double>(total), static_cast<double>(scroll), static_cast<double>(shown) };
}

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
