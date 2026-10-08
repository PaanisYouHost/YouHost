#include "TimelineView.h"
#include "Theme.h"

namespace youhost
{

void TimelineView::setTransport(const TransportView& view)
{
    view_ = view;
    repaint();
}

void TimelineView::setLocateHandler(std::function<void(std::int64_t)> handler)
{
    onLocate_ = std::move(handler);
}

std::int64_t TimelineView::spanSamples() const
{
    const double rate = view_.sampleRate > 0.0 ? view_.sampleRate : 48000.0;
    const auto minimum = static_cast<std::int64_t>(rate * 30.0);
    return std::max(view_.length, minimum);
}

std::int64_t TimelineView::sampleAt(float x) const
{
    const float width = std::max(1.0f, static_cast<float>(getWidth()));
    const float clamped = juce::jlimit(0.0f, width, x);
    const auto sample = static_cast<std::int64_t>((static_cast<double>(clamped) / static_cast<double>(width))
                                                  * static_cast<double>(spanSamples()));
    return clampTimeline(sample, view_.length);
}

void TimelineView::locateAt(float x)
{
    if (onLocate_ != nullptr)
        onLocate_(sampleAt(x));
}

void TimelineView::mouseDown(const juce::MouseEvent& event)
{
    locateAt(event.position.x);
}

void TimelineView::mouseDrag(const juce::MouseEvent& event)
{
    locateAt(event.position.x);
}

void TimelineView::paint(juce::Graphics& graphics)
{
    auto bounds = getLocalBounds().toFloat();
    graphics.setColour(theme::panel);
    graphics.fillRoundedRectangle(bounds, 8.0f);

    auto inner = bounds.reduced(8.0f, 6.0f);
    auto header = inner.removeFromTop(14.0f);
    graphics.setColour(theme::dim);
    graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
    graphics.drawText("0:00:00", header.removeFromLeft(70.0f), juce::Justification::centredLeft, false);
    graphics.drawText(timecodeText(spanSamples(), view_.sampleRate > 0.0 ? view_.sampleRate : 48000.0),
                      header,
                      juce::Justification::centredRight,
                      false);

    const float width = std::max(1.0f, inner.getWidth());
    const auto span = spanSamples();
    const auto sampleToX = [inner, width, span](std::int64_t sample)
    {
        const double ratio = span <= 0 ? 0.0 : static_cast<double>(sample) / static_cast<double>(span);
        return inner.getX() + static_cast<float>(ratio) * width;
    };

    auto drawTake = [&](const TakeDraw& take, juce::Colour fill)
    {
        if (take.span.length <= 0 && take.span.start <= 0 && take.peaks.empty())
            return;
        const float x1 = sampleToX(take.span.start);
        const float x2 = std::max(x1 + 2.0f, sampleToX(take.span.start + std::max<std::int64_t>(take.span.length, 1)));
        auto block = juce::Rectangle<float>(x1, inner.getY(), x2 - x1, inner.getHeight());
        graphics.setColour(fill);
        graphics.fillRoundedRectangle(block, 3.0f);

        if (! take.peaks.empty() && block.getWidth() > 2.0f)
        {
            graphics.setColour(theme::green.withAlpha(0.9f));
            const float mid = block.getCentreY();
            const float half = block.getHeight() * 0.45f;
            for (int pixel = 0; pixel < static_cast<int>(block.getWidth()); ++pixel)
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

    const float playX = sampleToX(view_.position);
    graphics.setColour(view_.mode == TransportMode::recording ? theme::red : theme::text);
    graphics.drawLine(playX, inner.getY(), playX, inner.getBottom(), 2.0f);
}

} // namespace youhost
