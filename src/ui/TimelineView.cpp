#include "TimelineView.h"
#include "Theme.h"
#include "X32Look.h"
#include "engine/SignalPath.h"
#include "engine/WaveformScale.h"

#include <algorithm>

namespace youhost
{

namespace
{

void quietButton(juce::Button& button)
{
    button.setMouseClickGrabsKeyboardFocus(false);
    button.setWantsKeyboardFocus(false);
}

juce::Colour laneWaveColour(int colorIndex)
{
    const auto hue = x32Hue(colorIndex);
    if (hue.isTransparent())
        return theme::green;
    return hue;
}

juce::Colour laneWashColour(int colorIndex)
{
    const auto hue = x32Hue(colorIndex);
    if (hue.isTransparent())
        return juce::Colour(0xff243044);
    return hue.withAlpha(0.45f);
}

} // namespace

TimelineView::TimelineView()
{
    addAndMakeVisible(scroll_);
    addAndMakeVisible(laneScrollBar_);
    addAndMakeVisible(zoomOutButton_);
    addAndMakeVisible(zoomInButton_);
    addAndMakeVisible(verticalOutButton_);
    addAndMakeVisible(verticalInButton_);
    addAndMakeVisible(fitButton_);
    addAndMakeVisible(waveOutButton_);
    addAndMakeVisible(waveInButton_);
    scroll_.addListener(this);
    laneScrollBar_.addListener(this);
    scroll_.setAutoHide(false);
    laneScrollBar_.setAutoHide(true);
    for (auto* button : { &zoomOutButton_, &zoomInButton_, &verticalOutButton_, &verticalInButton_, &fitButton_,
                          &waveOutButton_, &waveInButton_ })
        quietButton(*button);
    zoomOutButton_.setTooltip("Zoom out (R). Keeps going until the whole session fits.");
    zoomInButton_.setTooltip("Zoom in (T).");
    verticalOutButton_.setTooltip("Shorter lanes (Cmd+]).");
    verticalInButton_.setTooltip("Taller lanes (Cmd+[).");
    fitButton_.setTooltip("Fit every take across the width and every lane down the height (Option+R).");
    waveOutButton_.setTooltip("Shorter waveform. Display only. Cmd or Option plus the wheel does this too.");
    waveInButton_.setTooltip("Taller waveform. Display only. A full-scale peak still fills the lane at the default.");
    zoomOutButton_.onClick = [this] { zoomOut(); };
    zoomInButton_.onClick = [this] { zoomIn(); };
    verticalOutButton_.onClick = [this] { verticalZoomOut(); };
    verticalInButton_.onClick = [this] { verticalZoomIn(); };
    fitButton_.onClick = [this] { fitAll(); };
    waveOutButton_.onClick = [this] { nudgeWaveformGain(-1); };
    waveInButton_.onClick = [this] { nudgeWaveformGain(1); };
}

TimelineView::~TimelineView()
{
    scroll_.removeListener(this);
    laneScrollBar_.removeListener(this);
}

void TimelineView::setTransport(const TransportView& view)
{
    const bool moving = view.mode == TransportMode::playing || view.mode == TransportMode::recording;
    view_ = view;
    const auto span = fullSpan();
    if (moving && visibleSamples() < span)
        viewStart_ = anchorPlayhead(span, visibleSamples(), view_.position);
    const auto visible = visibleSamples();
    const auto maxStart = std::max<std::int64_t>(0, span - visible);
    if (viewStart_ < 0)
        viewStart_ = 0;
    if (viewStart_ > maxStart)
        viewStart_ = maxStart;
    if (zoomStep_ == 0)
        viewStart_ = 0;
    syncScroll();
    repaint();
}

void TimelineView::setLaneProvider(LaneProvider provider)
{
    laneProvider_ = std::move(provider);
}

void TimelineView::setLocateHandler(std::function<void(std::int64_t)> handler)
{
    onLocate_ = std::move(handler);
}

void TimelineView::setHeightHandler(std::function<void(int)> handler)
{
    onHeight_ = std::move(handler);
}

void TimelineView::zoomIn()
{
    zoomBy(1);
}

void TimelineView::zoomOut()
{
    zoomBy(-1);
}

void TimelineView::fitAll()
{
    zoomStep_ = 0;
    verticalStep_ = 0;
    laneScroll_ = 0;
    viewStart_ = 0;
    syncScroll();
    repaint();
}

void TimelineView::verticalZoomIn()
{
    verticalZoomBy(1);
}

void TimelineView::verticalZoomOut()
{
    verticalZoomBy(-1);
}

void TimelineView::setWaveformGain(float gain)
{
    const float clamped = clampWaveformGain(gain);
    if (sameFloatBits(clamped, waveformGain_))
        return;
    waveformGain_ = clamped;
    repaint();
}

void TimelineView::setWaveformGainHandler(std::function<void(float)> handler)
{
    onWaveformGain_ = std::move(handler);
}

void TimelineView::nudgeWaveformGain(int direction)
{
    waveformGain_ = stepWaveformGain(waveformGain_, direction);
    if (onWaveformGain_ != nullptr)
        onWaveformGain_(waveformGain_);
    repaint();
}

std::int64_t TimelineView::zoomAnchorSample() const
{
    const auto position = getMouseXYRelative().toFloat();
    if (waveformArea().contains(position))
        return sampleAt(position.x);
    return view_.position;
}

void TimelineView::zoomBy(int delta)
{
    const auto span = fullSpan();
    const auto oldVisible = visibleSamples();
    const int next = clampZoomStep(zoomStep_ + delta, span);
    const auto newVisible = zoomVisibleSamples(span, next);
    if (next == 0)
        viewStart_ = 0;
    else
        viewStart_ = viewStartKeepingPlayhead(span, viewStart_, oldVisible, newVisible, zoomAnchorSample());
    zoomStep_ = next;
    syncScroll();
    repaint();
}

void TimelineView::verticalZoomBy(int delta)
{
    const int maxStep = maxVerticalZoomStep(laneCount());
    verticalStep_ = std::clamp(verticalStep_ + delta, 0, maxStep);
    clampLaneScroll();
    syncScroll();
    repaint();
}

void TimelineView::syncScroll()
{
    if (updatingScroll_)
        return;
    updatingScroll_ = true;
    const auto span = std::max<std::int64_t>(1, fullSpan());
    const auto visible = visibleSamples();
    scroll_.setRangeLimits(0.0, static_cast<double>(span), juce::dontSendNotification);
    scroll_.setCurrentRange(static_cast<double>(viewStart_), static_cast<double>(visible), juce::dontSendNotification);
    scroll_.setVisible(zoomStep_ > 0 && visible < span);

    const int total = std::max(1, laneCount());
    const int shown = std::max(1, lanesShown());
    laneScrollBar_.setRangeLimits(0.0, static_cast<double>(total), juce::dontSendNotification);
    laneScrollBar_.setCurrentRange(static_cast<double>(laneScroll_), static_cast<double>(shown), juce::dontSendNotification);
    laneScrollBar_.setVisible(shown < total);
    updatingScroll_ = false;
}

void TimelineView::scrollBarMoved(juce::ScrollBar* bar, double newRangeStart)
{
    if (updatingScroll_)
        return;
    if (bar == &laneScrollBar_)
        laneScroll_ = static_cast<int>(newRangeStart);
    else
        viewStart_ = static_cast<std::int64_t>(newRangeStart);
    repaint();
}

std::int64_t TimelineView::fullSpan() const
{
    return fitSpanSamples(view_.length, view_.sampleRate);
}

std::int64_t TimelineView::visibleSamples() const
{
    return zoomVisibleSamples(fullSpan(), zoomStep_);
}

int TimelineView::laneCount() const
{
    return knownLanes_;
}

int TimelineView::lanesShown() const
{
    return lanesShownForVerticalStep(std::max(1, laneCount()), verticalStep_);
}

void TimelineView::clampLaneScroll()
{
    const int maxStart = std::max(0, laneCount() - lanesShown());
    if (laneScroll_ < 0)
        laneScroll_ = 0;
    if (laneScroll_ > maxStart)
        laneScroll_ = maxStart;
}

juce::Rectangle<float> TimelineView::waveformArea() const
{
    auto bounds = getLocalBounds().toFloat().reduced(8.0f, 6.0f);
    bounds.removeFromBottom(28.0f);
    bounds.removeFromTop(14.0f);
    if (laneScrollBar_.isVisible())
        bounds.removeFromRight(12.0f);
    return bounds;
}

std::int64_t TimelineView::sampleAt(float x) const
{
    const auto area = waveformArea();
    const float width = std::max(1.0f, area.getWidth());
    const float clamped = juce::jlimit(0.0f, width, x - area.getX());
    const auto visible = std::max<std::int64_t>(1, visibleSamples());
    const auto sample = viewStart_ + static_cast<std::int64_t>((static_cast<double>(clamped) / static_cast<double>(width))
                                                               * static_cast<double>(visible));
    return clampTimeline(sample, view_.length);
}

void TimelineView::locateAt(float x)
{
    if (onLocate_ != nullptr)
        onLocate_(sampleAt(x));
}

void TimelineView::resized()
{
    auto bottom = getLocalBounds().removeFromBottom(28).reduced(8, 4);
    zoomInButton_.setBounds(bottom.removeFromRight(26));
    bottom.removeFromRight(3);
    zoomOutButton_.setBounds(bottom.removeFromRight(26));
    bottom.removeFromRight(3);
    verticalInButton_.setBounds(bottom.removeFromRight(28));
    bottom.removeFromRight(3);
    verticalOutButton_.setBounds(bottom.removeFromRight(28));
    bottom.removeFromRight(3);
    waveInButton_.setBounds(bottom.removeFromRight(32));
    bottom.removeFromRight(3);
    waveOutButton_.setBounds(bottom.removeFromRight(32));
    bottom.removeFromRight(3);
    fitButton_.setBounds(bottom.removeFromRight(40));
    bottom.removeFromRight(6);
    if (laneCount() > lanesShown())
        laneScrollBar_.setBounds(getLocalBounds().reduced(8, 6).removeFromRight(12).withTrimmedTop(14).withTrimmedBottom(28));
    scroll_.setBounds(bottom);
    clampLaneScroll();
}

void TimelineView::mouseDown(const juce::MouseEvent& event)
{
    if (event.position.y >= static_cast<float>(getHeight() - 8))
    {
        draggingHeight_ = true;
        dragStartY_ = event.getScreenY();
        dragStartHeight_ = getHeight();
        return;
    }
    if (event.position.y >= static_cast<float>(getHeight() - 28))
        return;
    locateAt(event.position.x);
}

void TimelineView::mouseDrag(const juce::MouseEvent& event)
{
    if (draggingHeight_)
    {
        if (onHeight_ != nullptr)
            onHeight_(dragStartHeight_ + (event.getScreenY() - dragStartY_));
        return;
    }
    if (event.position.y >= static_cast<float>(getHeight() - 28))
        return;
    locateAt(event.position.x);
}

void TimelineView::mouseUp(const juce::MouseEvent&)
{
    draggingHeight_ = false;
}

void TimelineView::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (event.mods.isAltDown() || event.mods.isCommandDown())
    {
        gainWheel_ += wheel.deltaY;
        if (gainWheel_ >= 0.45f)
        {
            gainWheel_ = 0.0f;
            nudgeWaveformGain(1);
        }
        else if (gainWheel_ <= -0.45f)
        {
            gainWheel_ = 0.0f;
            nudgeWaveformGain(-1);
        }
        return;
    }

    if (event.mods.isShiftDown())
    {
        const auto span = fullSpan();
        const auto visible = visibleSamples();
        timeWheel_ += static_cast<double>(-wheel.deltaY) * static_cast<double>(visible) * 0.045;
        const auto nudge = static_cast<std::int64_t>(timeWheel_);
        timeWheel_ -= static_cast<double>(nudge);
        const auto maxStart = std::max<std::int64_t>(0, span - visible);
        viewStart_ = std::clamp(viewStart_ + nudge, static_cast<std::int64_t>(0), maxStart);
        syncScroll();
        repaint();
        return;
    }

    if (laneCount() > lanesShown())
    {
        laneWheel_ += wheel.deltaY * 0.85f;
        const int steps = static_cast<int>(std::trunc(laneWheel_));
        laneWheel_ -= static_cast<float>(steps);
        laneScroll_ -= steps;
        clampLaneScroll();
        syncScroll();
        repaint();
    }
}

void TimelineView::paint(juce::Graphics& graphics)
{
    auto bounds = getLocalBounds().toFloat();
    graphics.setColour(theme::panel);
    graphics.fillRoundedRectangle(bounds, 8.0f);

    auto inner = waveformArea();
    auto header = getLocalBounds().toFloat().reduced(8.0f, 6.0f).removeFromTop(14.0f);
    const double rate = view_.sampleRate > 0.0 ? view_.sampleRate : 48000.0;
    const auto visible = std::max<std::int64_t>(1, visibleSamples());
    graphics.setColour(theme::dim);
    graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
    graphics.drawText(timecodeText(viewStart_, rate), header.removeFromLeft(70.0f), juce::Justification::centredLeft, false);
    graphics.drawText(timecodeText(viewStart_ + visible, rate), header, juce::Justification::centredRight, false);

    const float width = std::max(1.0f, inner.getWidth());
    const auto sampleToX = [inner, width, visible, start = viewStart_](std::int64_t sample)
    {
        const double ratio = static_cast<double>(sample - start) / static_cast<double>(visible);
        return inner.getX() + static_cast<float>(ratio) * width;
    };

    graphics.saveState();
    graphics.reduceClipRegion(inner.toNearestInt());
    std::vector<TakeMark> takeMarks;

    const auto paintLanes = [&](const std::vector<TimelineLaneView>& lanes)
    {
    const int total = static_cast<int>(lanes.size());
    knownLanes_ = total;
    const int shown = std::max(1, lanesShownForVerticalStep(std::max(1, total), verticalStep_));
    const float laneHeight = inner.getHeight() / static_cast<float>(shown);
    const bool showNumbers = laneNumberVisible(laneHeight);
    const int maxStart = std::max(0, total - shown);
    const int first = std::clamp(laneScroll_, 0, maxStart);

    auto drawRegion = [&](const TimelineRegionView& region, juce::Rectangle<float> lane, juce::Colour wave)
    {
        if (region.length <= 0 && (region.peaks == nullptr || region.peaks->empty()))
            return;
        const float x1 = sampleToX(region.start);
        const float x2 = std::max(x1 + 1.0f, sampleToX(region.start + std::max<std::int64_t>(region.length, 1)));
        if (x2 < inner.getX() || x1 > inner.getRight())
            return;
        auto block = juce::Rectangle<float>(x1, lane.getY(), x2 - x1, lane.getHeight()).getIntersection(lane);
        if (block.isEmpty())
            return;
        const auto* peaks = region.peaks;
        if (peaks != nullptr && ! peaks->empty() && block.getWidth() > 1.0f && block.getHeight() >= 2.0f)
        {
            graphics.setColour(wave);
            const float mid = block.getCentreY();
            const float half = std::max(0.5f, block.getHeight() * 0.45f);
            const int left = std::max(0, static_cast<int>(std::floor(block.getX())));
            const int right = std::min(static_cast<int>(std::ceil(block.getRight())), static_cast<int>(inner.getRight()));
            for (int pixel = left; pixel < right; ++pixel)
            {
                const float local = static_cast<float>(pixel) - block.getX();
                const auto index = static_cast<std::size_t>((local / std::max(1.0f, block.getWidth()))
                                                            * static_cast<float>(peaks->size()));
                const auto last = static_cast<std::size_t>(((local + 1.0f) / std::max(1.0f, block.getWidth()))
                                                           * static_cast<float>(peaks->size()));
                float low = 0.0f;
                float high = 0.0f;
                for (std::size_t peak = index; peak < std::max(index + 1, last) && peak < peaks->size(); ++peak)
                {
                    low = std::min(low, (*peaks)[peak].low);
                    high = std::max(high, (*peaks)[peak].high);
                }
                const float shownHigh = waveformDisplaySigned(high, waveformGain_);
                const float shownLow = waveformDisplaySigned(low, waveformGain_);
                const float y1 = mid - shownHigh * half;
                const float y2 = mid - shownLow * half;
                graphics.drawVerticalLine(pixel, std::min(y1, y2), std::max(y1, y2) + 0.5f);
            }
        }
        else
        {
            graphics.setColour(wave);
            graphics.fillRect(block.withSizeKeepingCentre(block.getWidth(), std::max(1.0f, block.getHeight())));
        }
    };

    if (total == 0)
    {
        graphics.setColour(theme::dim);
        graphics.drawText("No recorded tracks yet", inner, juce::Justification::centred, false);
    }

    for (int index = 0; index < shown; ++index)
    {
        const int laneIndex = first + index;
        if (laneIndex < 0 || laneIndex >= total)
            break;
        const auto& lane = lanes[static_cast<std::size_t>(laneIndex)];
        auto row = juce::Rectangle<float>(inner.getX(), inner.getY() + laneHeight * static_cast<float>(index),
                                          inner.getWidth(), std::max(1.0f, laneHeight - 1.0f));
        const auto wave = laneWaveColour(lane.color);
        graphics.setColour(theme::background);
        graphics.fillRect(row);
        for (const auto& region : lane.regions)
        {
            rememberTakeMark(takeMarks, region.number, region.start);
            if (region.length <= 0 && (region.peaks == nullptr || region.peaks->empty()))
                continue;
            float x1 = 0.0f;
            float x2 = 0.0f;
            timelineRegionPixels(inner.getX(), width, viewStart_, visible, region.start,
                                 std::max<std::int64_t>(region.length, 1), x1, x2);
            if (x2 < inner.getX() || x1 > inner.getRight())
                continue;
            auto block = juce::Rectangle<float>(x1, row.getY(), x2 - x1, row.getHeight()).getIntersection(row);
            if (block.isEmpty())
                continue;
            graphics.setColour(laneWashColour(lane.color));
            graphics.fillRect(block);
            drawRegion(region, row, wave);
        }
        if (showNumbers)
        {
            graphics.setColour(theme::text);
            graphics.setFont(juce::Font(juce::FontOptions(std::min(12.0f, laneHeight - 1.0f))));
            const auto label = lane.title.empty() ? juce::String(lane.number) : juce::String::fromUTF8(lane.title.c_str());
            graphics.drawText(label, row.reduced(3.0f, 0.0f), juce::Justification::centredLeft, false);
        }
    }
    };

    if (laneProvider_ != nullptr)
        laneProvider_(paintLanes);
    else
        paintLanes({});

    std::sort(takeMarks.begin(), takeMarks.end(), [](const TakeMark& left, const TakeMark& right)
    {
        if (left.start != right.start)
            return left.start < right.start;
        return left.number < right.number;
    });
    graphics.setFont(juce::Font(juce::FontOptions(12.0f).withStyle("Bold")));
    for (std::size_t markIndex = 0; markIndex < takeMarks.size(); ++markIndex)
    {
        const float markX = sampleToX(takeMarks[markIndex].start);
        if (markX < inner.getX() - 1.0f || markX > inner.getRight() + 1.0f)
            continue;
        graphics.setColour(theme::amber);
        graphics.drawLine(markX, inner.getY(), markX, inner.getBottom(), 2.0f);
        float nextX = inner.getRight();
        if (markIndex + 1 < takeMarks.size())
            nextX = sampleToX(takeMarks[markIndex + 1].start);
        const float room = nextX - markX;
        const auto label = room < 58.0f ? juce::String(takeMarks[markIndex].number)
                                        : "TAKE " + juce::String(takeMarks[markIndex].number);
        auto pill = juce::Rectangle<float>(markX + 3.0f, inner.getY() + 1.0f,
                                           std::min(78.0f, std::max(18.0f, room - 6.0f)), 16.0f);
        graphics.setColour(theme::amber);
        graphics.fillRoundedRectangle(pill, 3.0f);
        graphics.setColour(juce::Colour(0xff141414));
        graphics.drawText(label, pill, juce::Justification::centred, true);
    }

    const auto playhead = view_.position;
    if (playhead >= viewStart_ && playhead <= viewStart_ + visible)
    {
        const float playX = sampleToX(playhead);
        graphics.setColour(view_.mode == TransportMode::recording ? theme::red : theme::text);
        graphics.drawLine(playX, inner.getY(), playX, inner.getBottom(), 2.0f);
    }

    graphics.restoreState();

    graphics.setColour(theme::panelEdge);
    graphics.fillRect(0, getHeight() - 4, getWidth(), 4);
}

} // namespace youhost
