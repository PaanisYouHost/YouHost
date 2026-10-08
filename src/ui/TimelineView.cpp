#include "TimelineView.h"
#include "Theme.h"

namespace youhost
{

namespace
{

void quietButton(juce::Button& button)
{
    button.setMouseClickGrabsKeyboardFocus(false);
    button.setWantsKeyboardFocus(false);
}

} // namespace

TimelineView::TimelineView()
{
    addAndMakeVisible(scroll_);
    addAndMakeVisible(zoomOutButton_);
    addAndMakeVisible(zoomInButton_);
    scroll_.addListener(this);
    scroll_.setAutoHide(false);
    quietButton(zoomOutButton_);
    quietButton(zoomInButton_);
    zoomOutButton_.setTooltip("Zoom out (R).");
    zoomInButton_.setTooltip("Zoom in (T).");
    zoomOutButton_.onClick = [this] { zoomOut(); };
    zoomInButton_.onClick = [this] { zoomIn(); };
}

TimelineView::~TimelineView()
{
    scroll_.removeListener(this);
}

void TimelineView::setTransport(const TransportView& view)
{
    const bool moving = view.mode == TransportMode::playing || view.mode == TransportMode::recording;
    view_ = view;
    if (moving && zoomStep_ > 0)
        viewStart_ = followPlayhead(fullSpan(), viewStart_, visibleSamples(), view_.position);
    const auto span = fullSpan();
    const auto visible = visibleSamples();
    const auto maxStart = std::max<std::int64_t>(0, span - visible);
    if (viewStart_ < 0)
        viewStart_ = 0;
    if (viewStart_ > maxStart)
        viewStart_ = maxStart;
    syncScroll();
    repaint();
}

void TimelineView::setLocateHandler(std::function<void(std::int64_t)> handler)
{
    onLocate_ = std::move(handler);
}

void TimelineView::zoomIn()
{
    zoomBy(1);
}

void TimelineView::zoomOut()
{
    zoomBy(-1);
}

void TimelineView::zoomBy(int delta)
{
    const auto span = fullSpan();
    const auto oldVisible = visibleSamples();
    const int next = clampZoomStep(zoomStep_ + delta);
    const auto newVisible = zoomVisibleSamples(span, next);
    viewStart_ = viewStartKeepingPlayhead(span, viewStart_, oldVisible, newVisible, view_.position);
    zoomStep_ = next;
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
    updatingScroll_ = false;
}

void TimelineView::scrollBarMoved(juce::ScrollBar*, double newRangeStart)
{
    if (updatingScroll_)
        return;
    viewStart_ = static_cast<std::int64_t>(newRangeStart);
    repaint();
}

std::int64_t TimelineView::fullSpan() const
{
    const double rate = view_.sampleRate > 0.0 ? view_.sampleRate : 48000.0;
    const auto minimum = static_cast<std::int64_t>(rate * 30.0);
    return std::max(view_.length, minimum);
}

std::int64_t TimelineView::visibleSamples() const
{
    return zoomVisibleSamples(fullSpan(), zoomStep_);
}

juce::Rectangle<float> TimelineView::waveformArea() const
{
    auto bounds = getLocalBounds().toFloat().reduced(8.0f, 6.0f);
    bounds.removeFromBottom(22.0f);
    bounds.removeFromTop(14.0f);
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
    auto bottom = getLocalBounds().removeFromBottom(24).reduced(8, 2);
    zoomInButton_.setBounds(bottom.removeFromRight(28));
    bottom.removeFromRight(4);
    zoomOutButton_.setBounds(bottom.removeFromRight(28));
    bottom.removeFromRight(6);
    scroll_.setBounds(bottom);
}

void TimelineView::mouseDown(const juce::MouseEvent& event)
{
    if (event.position.y >= static_cast<float>(getHeight() - 24))
        return;
    locateAt(event.position.x);
}

void TimelineView::mouseDrag(const juce::MouseEvent& event)
{
    if (event.position.y >= static_cast<float>(getHeight() - 24))
        return;
    locateAt(event.position.x);
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

    auto drawTake = [&](const TakeDraw& take, juce::Colour fill)
    {
        if (take.span.length <= 0 && take.span.start <= 0 && take.peaks.empty())
            return;
        const float x1 = sampleToX(take.span.start);
        const float x2 = std::max(x1 + 2.0f, sampleToX(take.span.start + std::max<std::int64_t>(take.span.length, 1)));
        if (x2 < inner.getX() || x1 > inner.getRight())
            return;
        auto block = juce::Rectangle<float>(x1, inner.getY(), x2 - x1, inner.getHeight());
        graphics.setColour(fill);
        graphics.fillRoundedRectangle(block, 3.0f);

        if (! take.peaks.empty() && block.getWidth() > 2.0f)
        {
            graphics.setColour(theme::green.withAlpha(0.9f));
            const float mid = block.getCentreY();
            const float half = block.getHeight() * 0.45f;
            const int left = std::max(0, static_cast<int>(std::floor(inner.getX() - block.getX())));
            const int right = std::min(static_cast<int>(block.getWidth()),
                                       static_cast<int>(std::ceil(inner.getRight() - block.getX())));
            for (int pixel = left; pixel < right; ++pixel)
            {
                const auto first = static_cast<std::size_t>((static_cast<float>(pixel) / block.getWidth()) * static_cast<float>(take.peaks.size()));
                const auto last = static_cast<std::size_t>((static_cast<float>(pixel + 1) / block.getWidth()) * static_cast<float>(take.peaks.size()));
                float low = 0.0f;
                float high = 0.0f;
                for (std::size_t index = first; index < std::max(first + 1, last) && index < take.peaks.size(); ++index)
                {
                    low = std::min(low, take.peaks[index].low);
                    high = std::max(high, take.peaks[index].high);
                }
                const float y1 = mid - juce::jlimit(-1.0f, 1.0f, high) * half;
                const float y2 = mid - juce::jlimit(-1.0f, 1.0f, low) * half;
                graphics.drawVerticalLine(static_cast<int>(block.getX()) + pixel, std::min(y1, y2), std::max(y1, y2));
            }
        }

        graphics.setColour(theme::text.withAlpha(0.8f));
        graphics.drawVerticalLine(juce::roundToInt(x1), inner.getY(), inner.getBottom());
        graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
        graphics.drawText(juce::String(take.number),
                          juce::Rectangle<float>(x1 + 3.0f, inner.getY(), 24.0f, 14.0f),
                          juce::Justification::centredLeft,
                          false);
    };

    int index = 0;
    for (const auto& take : view_.takes)
        drawTake(take, index++ % 2 == 0 ? juce::Colour(0xff243044) : juce::Colour(0xff2a3142));
    if (view_.liveValid)
        drawTake(view_.live, juce::Colour(0xff4a2430));

    if (view_.position >= viewStart_ && view_.position <= viewStart_ + visible)
    {
        const float playX = sampleToX(view_.position);
        graphics.setColour(view_.mode == TransportMode::recording ? theme::red : theme::text);
        graphics.drawLine(playX, inner.getY(), playX, inner.getBottom(), 2.0f);
    }

    graphics.restoreState();
}

} // namespace youhost
