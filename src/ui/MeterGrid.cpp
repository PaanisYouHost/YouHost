#include "MeterGrid.h"
#include "Theme.h"
#include "engine/MeterScale.h"

#include <cmath>
#include <vector>

namespace youhost
{
namespace
{

constexpr float kScaleGutter = 42.0f;

juce::Colour colourForLevel(float linearGain)
{
    const float db = gainToDb(linearGain);
    if (db >= -6.0f)
        return theme::red;
    if (db >= -18.0f)
        return theme::amber;
    return theme::green;
}

struct BarParts
{
    juce::Rectangle<float> bar;
    juce::Rectangle<float> clip;
    juce::Rectangle<float> number;
};

BarParts splitCell(juce::Rectangle<float> cell)
{
    const float numberHeight = juce::jlimit(10.0f, 14.0f, cell.getHeight() * 0.18f);
    auto body = cell.reduced(3.0f, 2.0f);
    auto number = body.removeFromBottom(numberHeight);
    body.removeFromBottom(2.0f);
    const auto clip = body.removeFromTop(juce::jmin(7.0f, body.getHeight() * 0.08f));
    body.removeFromTop(2.0f);
    return { body, clip, number };
}

juce::String tickText(int label)
{
    if (label > 0)
        return "+" + juce::String(label);
    return juce::String(label);
}

void drawScale(juce::Graphics& graphics, juce::Rectangle<float> bar, bool peak, int referenceDb)
{
    if (bar.getHeight() < 8.0f)
        return;

    MeterTick ticks[9];
    const int count = peak ? peakTicks(ticks, 9) : rmsTicks(referenceDb, ticks, 9);
    const MeterSpan span = peak ? peakMeterSpan() : rmsMeterSpan(referenceDb);

    // Keep 0 and the two ends, then fill the rest wherever the labels still fit.
    std::vector<int> order;
    order.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
        if (ticks[index].label == 0)
            order.push_back(index);
    if (count > 0)
        order.push_back(0);
    if (count > 1)
        order.push_back(count - 1);
    for (int index = 0; index < count; ++index)
        order.push_back(index);

    struct Placed
    {
        float y = 0.0f;
        int label = 0;
    };
    std::vector<Placed> placed;
    placed.reserve(static_cast<std::size_t>(count));
    // -3 and -6 are 3 dB apart. Keep both while the bar is tall enough to separate them.
    constexpr float minSeparation = 9.0f;

    for (int index : order)
    {
        const float y = bar.getBottom() - normaliseDb(ticks[index].dbFs, span) * bar.getHeight();
        bool crowded = false;
        for (const auto& existing : placed)
        {
            if (std::fabs(existing.y - y) < minSeparation)
            {
                crowded = true;
                break;
            }
        }
        if (crowded)
            continue;
        placed.push_back({ y, ticks[index].label });
    }

    graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
    for (const auto& tick : placed)
    {
        graphics.setColour(theme::panelEdge);
        graphics.fillRect(bar.getX() - 5.0f, tick.y, 4.0f, 1.0f);

        graphics.setColour(tick.label == 0 ? theme::text : theme::dim);
        const auto area = juce::Rectangle<float>(0.0f, tick.y - 6.0f, bar.getX() - 6.0f, 12.0f);
        graphics.drawText(tickText(tick.label), area, juce::Justification::centredRight, false);
    }
}

} // namespace

MeterGrid::MeterGrid()
{
    setOpaque(false);
}

void MeterGrid::setReadings(std::vector<MeterReading> readings, bool showPeak, int rmsReferenceDb)
{
    readings_ = std::move(readings);
    showPeak_ = showPeak;
    rmsReferenceDb_ = normaliseRmsReferenceDb(rmsReferenceDb);
    repaint();
}

void MeterGrid::setClearHandler(std::function<void(int channel)> handler)
{
    onClearClip_ = std::move(handler);
}

MeterLayout MeterGrid::layoutFor(int count) const
{
    if (count <= 0)
        return {};

    const float bridgeWidth = std::max(1.0f, static_cast<float>(getWidth()) - kScaleGutter);
    auto layout = layoutMeters(count, bridgeWidth, static_cast<float>(getHeight()));
    layout.originX += kScaleGutter;
    return layout;
}

int MeterGrid::channelAt(juce::Point<float> position) const
{
    const int count = static_cast<int>(readings_.size());
    if (count <= 0 || layout_.cellWidth <= 0.0f || layout_.cellHeight <= 0.0f)
        return -1;

    const float localX = position.x - layout_.originX;
    const float localY = position.y;
    if (localX < 0.0f || localY < 0.0f)
        return -1;

    const int column = static_cast<int>(localX / layout_.cellWidth);
    const int row = static_cast<int>(localY / layout_.cellHeight);
    if (column < 0 || column >= layout_.columns || row < 0 || row >= layout_.rows)
        return -1;

    const int index = row * layout_.columns + column;
    if (index < 0 || index >= count)
        return -1;
    return index;
}

void MeterGrid::paint(juce::Graphics& graphics)
{
    const int count = static_cast<int>(readings_.size());
    if (count <= 0)
    {
        graphics.setColour(theme::dim);
        graphics.setFont(juce::Font(juce::FontOptions(15.0f)));
        graphics.drawFittedText("No input channels open. Choose a device below and enable its inputs.",
                                getLocalBounds().reduced(8),
                                juce::Justification::centred,
                                3);
        return;
    }

    layout_ = layoutFor(count);
    const float fontSize = juce::jlimit(8.0f, 12.0f, layout_.cellWidth * 0.42f);
    const MeterSpan span = showPeak_ ? peakMeterSpan() : rmsMeterSpan(rmsReferenceDb_);

    for (int row = 0; row < layout_.rows; ++row)
    {
        const auto probe = juce::Rectangle<float>(layout_.originX,
                                                 static_cast<float>(row) * layout_.cellHeight,
                                                 layout_.cellWidth,
                                                 layout_.cellHeight);
        drawScale(graphics, splitCell(probe).bar, showPeak_, rmsReferenceDb_);
    }

    for (int index = 0; index < count; ++index)
    {
        const int column = index % layout_.columns;
        const int row = index / layout_.columns;
        const auto cell = juce::Rectangle<float>(layout_.originX + static_cast<float>(column) * layout_.cellWidth,
                                                 static_cast<float>(row) * layout_.cellHeight,
                                                 layout_.cellWidth,
                                                 layout_.cellHeight);
        const auto parts = splitCell(cell);
        const auto& reading = readings_[static_cast<std::size_t>(index)];
        const float level = showPeak_ ? reading.peak : reading.rms;

        graphics.setColour(theme::meterTrack);
        graphics.fillRoundedRectangle(parts.bar, 2.0f);

        if (reading.hasInput && level > 0.0f && parts.bar.getHeight() > 1.0f)
        {
            const float filled = normaliseDb(gainToDb(level), span) * parts.bar.getHeight();
            auto levelArea = parts.bar.withTop(parts.bar.getBottom() - filled);
            graphics.setColour(colourForLevel(level));
            graphics.fillRoundedRectangle(levelArea, 2.0f);
        }

        graphics.setColour(reading.clipped ? theme::red : theme::panelEdge);
        graphics.fillRoundedRectangle(parts.clip.reduced(juce::jmax(0.0f, (parts.clip.getWidth() - 8.0f) * 0.5f), 0.0f), 1.5f);

        graphics.setColour(reading.hasInput ? theme::dim : theme::fainter);
        graphics.setFont(juce::Font(juce::FontOptions(fontSize)));
        graphics.drawText(juce::String(index + 1), parts.number, juce::Justification::centred, false);
    }
}

void MeterGrid::mouseDown(const juce::MouseEvent& event)
{
    layout_ = layoutFor(static_cast<int>(readings_.size()));
    const int channel = channelAt(event.position);
    if (channel >= 0 && onClearClip_ != nullptr)
        onClearClip_(channel);
}

} // namespace youhost
