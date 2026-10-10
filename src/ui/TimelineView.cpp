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
    addAndMakeVisible(gotoBox_);
    addAndMakeVisible(waveOutButton_);
    addAndMakeVisible(waveInButton_);
    scroll_.addListener(this);
    laneScrollBar_.addListener(this);
    scroll_.setAutoHide(false);
    laneScrollBar_.setAutoHide(false);
    scroll_.setColour(juce::ScrollBar::thumbColourId, theme::panelEdge);
    scroll_.setColour(juce::ScrollBar::trackColourId, theme::background);
    laneScrollBar_.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xffc5ccd8));
    laneScrollBar_.setColour(juce::ScrollBar::trackColourId, juce::Colour(0xff2a3140));
    scroll_.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xffc5ccd8));
    scroll_.setColour(juce::ScrollBar::trackColourId, juce::Colour(0xff2a3140));
    for (auto* button : { &zoomOutButton_, &zoomInButton_, &verticalOutButton_, &verticalInButton_, &fitButton_,
                          &waveOutButton_, &waveInButton_ })
        quietButton(*button);
    zoomOutButton_.setTooltip("Zoom out (R). Keeps going until the whole session fits.");
    zoomInButton_.setTooltip("Zoom in (T).");
    verticalOutButton_.setTooltip("Shorter lanes (Cmd+]).");
    verticalInButton_.setTooltip("Taller lanes (Cmd+[).");
    fitButton_.setTooltip("Fit every take across the width and every lane down the timeline (Option+R).");
    fitButton_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2f6f4e));
    fitButton_.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    gotoBox_.setTextToShowWhenEmpty("Go", theme::fainter);
    gotoBox_.setInputRestrictions(3, "0123456789");
    gotoBox_.setJustification(juce::Justification::centred);
    gotoBox_.setTooltip("Go to channel. Type the channel number and press Return. G focuses this field.");
    gotoBox_.onReturnKey = [this]
    {
        const int number = parseGoToChannel(gotoBox_.getText().toStdString());
        if (number > 0)
            scrollToChannel(number - 1);
        gotoBox_.setText({}, juce::dontSendNotification);
    };
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
    if (pendingView_)
    {
        pendingView_ = false;
        zoomStep_ = std::clamp(pendingViewState_.zoom, 0, kMaxTimelineZoomStep);
        verticalStep_ = std::max(0, pendingViewState_.vertical);
        viewStart_ = std::max<std::int64_t>(0, pendingViewState_.scroll);
        laneScroll_ = std::max(0, pendingViewState_.laneScroll);
        holdTimeScroll_ = viewStart_ > 0;
    }
    const auto span = fullSpan();
    const auto visible = visibleSamples();
    if (moving && visible < span)
    {
        if (holdTimeScroll_ && (view_.position < viewStart_ || view_.position >= viewStart_ + visible))
            holdTimeScroll_ = false;
        if (! holdTimeScroll_)
            viewStart_ = anchorPlayhead(span, visible, view_.position);
    }
    const auto maxStart = std::max<std::int64_t>(0, span - visible);
    if (viewStart_ < 0)
        viewStart_ = 0;
    if (viewStart_ > maxStart)
        viewStart_ = maxStart;
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
    holdTimeScroll_ = false;
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
    holdTimeScroll_ = false;
    zoomStep_ = next;
    syncScroll();
    repaint();
}

void TimelineView::setSelectedChannel(std::function<int()> channel)
{
    selectedChannel_ = std::move(channel);
}

void TimelineView::rememberLanes(const std::vector<TimelineLaneView>& lanes)
{
    knownLanes_ = static_cast<int>(lanes.size());
    laneMembers_.resize(lanes.size());
    for (std::size_t index = 0; index < lanes.size(); ++index)
        laneMembers_[index] = lanes[index].members;
}

void TimelineView::scrollToChannel(int channel)
{
    if (channel < 0 || laneProvider_ == nullptr)
        return;
    int found = -1;
    laneProvider_([this, channel, &found](const std::vector<TimelineLaneView>& lanes)
    {
        rememberLanes(lanes);
        found = laneIndexContaining(laneMembers_, channel);
    });
    if (found < 0)
        return;
    laneScroll_ = laneScrollToReveal(std::max(1, laneCount()), std::max(1, lanesShown()), laneScroll_, found);
    syncScroll();
    repaint();
}

void TimelineView::focusChannelJump()
{
    gotoBox_.grabKeyboardFocus();
    gotoBox_.selectAll();
}

int TimelineView::zoomAnchorLane() const
{
    const auto position = getMouseXYRelative().toFloat();
    const auto area = waveformArea();
    const int shown = std::max(1, lanesShown());
    if (area.contains(position))
        return laneUnderPointer(position.y, area.getY(), area.getHeight(), shown, laneScroll_);
    if (selectedChannel_ != nullptr)
    {
        const int lane = laneIndexContaining(laneMembers_, selectedChannel_());
        if (lane >= 0)
            return lane;
    }
    return laneScroll_;
}

void TimelineView::verticalZoomBy(int delta)
{
    const int total = std::max(1, laneCount());
    const int oldShown = std::max(1, lanesShown());
    const int oldScroll = laneScroll_;
    const int anchor = zoomAnchorLane();
    const int maxStep = maxVerticalZoomStep(total);
    verticalStep_ = std::clamp(verticalStep_ + delta, 0, maxStep);
    const int newShown = std::max(1, lanesShown());
    laneScroll_ = laneScrollKeepingAnchor(total, oldScroll, oldShown, newShown, anchor);
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
    const auto timeBar = timeBarRange(viewStart_, visible, span);
    scroll_.setRangeLimits(0.0, timeBar.limit, juce::dontSendNotification);
    scroll_.setCurrentRange(timeBar.start, timeBar.size, juce::dontSendNotification);
    scroll_.setSingleStepSize(std::max(1.0, timeBar.size / 20.0));
    scroll_.setVisible(true);

    const int total = std::max(1, laneCount());
    const int shown = std::max(1, lanesShown());
    const auto laneBar = laneBarRange(laneScroll_, shown, total);
    laneScroll_ = static_cast<int>(laneBar.start);
    laneScrollBar_.setRangeLimits(0.0, laneBar.limit, juce::dontSendNotification);
    laneScrollBar_.setCurrentRange(laneBar.start, laneBar.size, juce::dontSendNotification);
    laneScrollBar_.setSingleStepSize(1.0);
    laneScrollBar_.setVisible(true);
    updatingScroll_ = false;
}

void TimelineView::scrollBarMoved(juce::ScrollBar* bar, double newRangeStart)
{
    if (updatingScroll_)
        return;
    if (bar == &laneScrollBar_)
        laneScroll_ = static_cast<int>(std::llround(newRangeStart));
    else
    {
        viewStart_ = static_cast<std::int64_t>(std::llround(newRangeStart));
        holdTimeScroll_ = true;
    }
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
    return lanesVisible(std::max(1, laneCount()), verticalStep_, waveformArea().getHeight());
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
    bounds.removeFromBottom(32.0f);
    bounds.removeFromTop(static_cast<float>(kTimelineRulerHeightPx));
    bounds.removeFromRight(18.0f);
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
    auto bottom = getLocalBounds().removeFromBottom(32).reduced(8, 4);
    fitButton_.setBounds(bottom.removeFromLeft(64));
    bottom.removeFromLeft(4);
    gotoBox_.setBounds(bottom.removeFromLeft(46));
    bottom.removeFromLeft(6);
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
    bottom.removeFromRight(4);
    scroll_.setBounds(bottom);
    auto lanes = getLocalBounds().reduced(8, 6);
    lanes.removeFromBottom(32);
    lanes.removeFromTop(kTimelineRulerHeightPx);
    laneScrollBar_.setBounds(lanes.removeFromRight(18));
    clampLaneScroll();
    syncScroll();
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
    if (event.position.y >= static_cast<float>(getHeight() - 32))
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
    if (event.position.y >= static_cast<float>(getHeight() - 32))
        return;
    locateAt(event.position.x);
}

void TimelineView::mouseUp(const juce::MouseEvent&)
{
    draggingHeight_ = false;
}

void TimelineView::setViewState(const SessionTimelineState& state)
{
    pendingViewState_ = state;
    pendingView_ = true;
    zoomStep_ = std::clamp(state.zoom, 0, kMaxTimelineZoomStep);
    verticalStep_ = std::max(0, state.vertical);
    viewStart_ = std::max<std::int64_t>(0, state.scroll);
    laneScroll_ = std::max(0, state.laneScroll);
    holdTimeScroll_ = viewStart_ > 0;
    const auto maxStart = std::max<std::int64_t>(0, fullSpan() - visibleSamples());
    if (viewStart_ > maxStart)
        viewStart_ = maxStart;
    clampLaneScroll();
    syncScroll();
    repaint();
}

SessionTimelineState TimelineView::viewState() const
{
    SessionTimelineState state;
    state.zoom = zoomStep_;
    state.vertical = verticalStep_;
    state.scroll = viewStart_;
    state.laneScroll = laneScroll_;
    return state;
}

void TimelineView::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    const auto axis = timelineScrollAxis(wheel.deltaX,
                                         wheel.deltaY,
                                         event.mods.isShiftDown(),
                                         event.mods.isAltDown() || event.mods.isCommandDown());
    if (axis == TimelineScrollAxis::gain)
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

    if (axis == TimelineScrollAxis::time)
    {
        const float delta = timelineTimeDelta(wheel.deltaX, wheel.deltaY, event.mods.isShiftDown());
        viewStart_ = applyTimeWheel(timeWheel_, delta, viewStart_, fullSpan(), visibleSamples());
        holdTimeScroll_ = true;
        syncScroll();
        repaint();
        return;
    }

    const int total = std::max(1, laneCount());
    const int shown = std::max(1, lanesShown());
    if (shown >= total)
        return;
    laneScroll_ = applyLaneWheel(laneWheel_, wheel.deltaY, laneScroll_, total, shown);
    syncScroll();
    repaint();
}

void TimelineView::paint(juce::Graphics& graphics)
{
    auto bounds = getLocalBounds().toFloat();
    graphics.setColour(theme::panel);
    graphics.fillRoundedRectangle(bounds, 8.0f);

    auto inner = waveformArea();
    auto header = getLocalBounds().toFloat().reduced(8.0f, 6.0f).removeFromTop(static_cast<float>(kTimelineRulerHeightPx));
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
    struct CornerTag
    {
        TimelineLabelRect box;
        juce::String text;
    };
    std::vector<CornerTag> cornerTags;

    const auto paintLanes = [&](const std::vector<TimelineLaneView>& lanes)
    {
    const int total = static_cast<int>(lanes.size());
    rememberLanes(lanes);
    const int shown = std::max(1, lanesVisible(std::max(1, total), verticalStep_, inner.getHeight()));
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
            CornerTag tag;
            tag.box = laneCornerLabelRect(row.getX(), row.getY(), row.getWidth(), row.getHeight());
            tag.text = juce::String::fromUTF8(laneCornerLabel(lane.number, lane.title, lane.group).c_str());
            cornerTags.push_back(std::move(tag));
        }
    }
    };

    if (laneProvider_ != nullptr)
        laneProvider_(paintLanes);
    else
        paintLanes({});
    syncScroll();

    std::sort(takeMarks.begin(), takeMarks.end(), [](const TakeMark& left, const TakeMark& right)
    {
        if (left.start != right.start)
            return left.start < right.start;
        return left.number < right.number;
    });
    for (std::size_t markIndex = 0; markIndex < takeMarks.size(); ++markIndex)
    {
        const float markX = sampleToX(takeMarks[markIndex].start);
        if (markX < inner.getX() - 1.0f || markX > inner.getRight() + 1.0f)
            continue;
        graphics.setColour(theme::amber);
        graphics.drawLine(markX, inner.getY(), markX, inner.getBottom(), 2.0f);
    }

    const auto playhead = view_.position;
    if (playhead >= viewStart_ && playhead <= viewStart_ + visible)
    {
        const float playX = sampleToX(playhead);
        graphics.setColour(view_.mode == TransportMode::recording ? theme::red : theme::text);
        graphics.drawLine(playX, inner.getY(), playX, inner.getBottom(), 2.0f);
    }

    for (const auto& tag : cornerTags)
    {
        if (tag.box.width < 8.0f || tag.box.height < 8.0f)
            continue;
        auto pill = juce::Rectangle<float>(tag.box.x, tag.box.y, tag.box.width, tag.box.height);
        graphics.setColour(theme::panel);
        graphics.fillRoundedRectangle(pill, 3.0f);
        graphics.setColour(theme::text);
        graphics.setFont(juce::Font(juce::FontOptions(std::min(12.0f, tag.box.height - 1.0f))));
        graphics.drawText(tag.text, pill.reduced(4.0f, 0.0f), juce::Justification::centredLeft, true);
    }

    graphics.restoreState();

    graphics.setFont(juce::Font(juce::FontOptions(12.0f).withStyle("Bold")));
    for (std::size_t markIndex = 0; markIndex < takeMarks.size(); ++markIndex)
    {
        const float markX = sampleToX(takeMarks[markIndex].start);
        if (markX < inner.getX() - 1.0f || markX > inner.getRight() + 1.0f)
            continue;
        float nextX = inner.getRight();
        if (markIndex + 1 < takeMarks.size())
            nextX = sampleToX(takeMarks[markIndex + 1].start);
        const float room = nextX - markX;
        const auto placed = takeRulerLabelRect(inner.getX(), header.getY(), inner.getWidth(), header.getHeight(), markX, nextX);
        if (placed.width < 8.0f || placed.height < 8.0f)
            continue;
        auto pill = juce::Rectangle<float>(placed.x, placed.y, placed.width, placed.height);
        graphics.setColour(theme::amber);
        graphics.fillRoundedRectangle(pill, 3.0f);
        graphics.setColour(juce::Colour(0xff141414));
        const auto label = juce::String::fromUTF8(takeRulerLabelText(takeMarks[markIndex].number, room).c_str());
        graphics.drawText(label, pill, juce::Justification::centred, true);
    }

    graphics.setColour(theme::panelEdge);
    graphics.fillRect(0, getHeight() - 4, getWidth(), 4);
}

} // namespace youhost
