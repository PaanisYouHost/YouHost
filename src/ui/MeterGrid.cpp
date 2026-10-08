#include "MeterGrid.h"
#include "Theme.h"
#include "engine/MeterScale.h"

#include <cmath>
#include <vector>

namespace youhost
{
namespace
{

constexpr float kScaleGutter = 44.0f;

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
    juce::Rectangle<float> slotRow;
    juce::Rectangle<float> number;
};

BarParts splitCell(juce::Rectangle<float> cell, bool recordRow)
{
    const float numberHeight = juce::jlimit(10.0f, 14.0f, cell.getHeight() * 0.12f);
    const float slotHeight = recordRow ? juce::jlimit(14.0f, 18.0f, cell.getHeight() * 0.16f)
                                       : juce::jlimit(9.0f, 12.0f, cell.getHeight() * 0.1f);
    auto body = cell.reduced(3.0f, 2.0f);
    auto number = body.removeFromBottom(numberHeight);
    body.removeFromBottom(1.0f);
    auto slotRow = body.removeFromBottom(slotHeight);
    body.removeFromBottom(2.0f);
    const auto clip = body.removeFromTop(juce::jmin(7.0f, body.getHeight() * 0.08f));
    body.removeFromTop(2.0f);
    return { body, clip, slotRow, number };
}

juce::String tickText(int label)
{
    if (label > 0)
        return "+" + juce::String(label);
    return juce::String(label);
}

struct PlacedTick
{
    float y = 0.0f;
    int label = 0;
};

std::vector<PlacedTick> placeTicks(juce::Rectangle<float> bar, bool peak, int referenceDb)
{
    std::vector<PlacedTick> placed;
    if (bar.getHeight() < 8.0f)
        return placed;

    MeterTick ticks[9];
    const int count = peak ? peakTicks(ticks, 9) : rmsTicks(referenceDb, ticks, 9);
    const MeterSpan span = peak ? peakMeterSpan() : rmsMeterSpan(referenceDb);

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
    return placed;
}

void drawScaleLabels(juce::Graphics& graphics, const std::vector<PlacedTick>& ticks, float barLeft, float barRight)
{
    graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
    for (const auto& tick : ticks)
    {
        graphics.setColour(tick.label == 0 ? theme::text : theme::dim);
        const auto left = juce::Rectangle<float>(barLeft - kScaleGutter, tick.y - 6.0f, kScaleGutter - 6.0f, 12.0f);
        const auto right = juce::Rectangle<float>(barRight + 6.0f, tick.y - 6.0f, kScaleGutter - 8.0f, 12.0f);
        graphics.drawText(tickText(tick.label), left, juce::Justification::centredRight, false);
        graphics.drawText(tickText(tick.label), right, juce::Justification::centredLeft, false);
    }
}

void drawScaleLines(juce::Graphics& graphics, const std::vector<PlacedTick>& ticks, float barLeft, float barRight)
{
    const float width = juce::jmax(0.0f, barRight - barLeft);
    for (const auto& tick : ticks)
    {
        if (tick.label == 0)
            continue;
        graphics.setColour(juce::Colour(0x66c5d0e0));
        graphics.fillRect(barLeft, tick.y - 0.5f, width, 1.0f);
    }
    for (const auto& tick : ticks)
    {
        if (tick.label != 0)
            continue;
        graphics.setColour(theme::text);
        graphics.fillRect(barLeft, tick.y - 1.0f, width, 2.0f);
    }
}

void drawSlotChips(juce::Graphics& graphics, juce::Rectangle<float> row, const std::array<SlotMark, kSlotsPerChannel>& slots)
{
    if (row.getWidth() < 4.0f || row.getHeight() < 4.0f)
        return;

    const float gap = 1.0f;
    const float chipWidth = (row.getWidth() - gap * static_cast<float>(kSlotsPerChannel - 1)) / static_cast<float>(kSlotsPerChannel);
    for (int slot = 0; slot < kSlotsPerChannel; ++slot)
    {
        const auto chip = juce::Rectangle<float>(row.getX() + static_cast<float>(slot) * (chipWidth + gap),
                                                 row.getY(),
                                                 chipWidth,
                                                 row.getHeight())
                              .reduced(0.0f, 1.0f);
        const auto& mark = slots[static_cast<std::size_t>(slot)];
        if (mark.loading)
            graphics.setColour(theme::amber);
        else if (! mark.occupied)
            graphics.setColour(theme::panelEdge);
        else if (mark.bypassed)
            graphics.setColour(theme::fainter);
        else
            graphics.setColour(theme::green);

        graphics.fillRoundedRectangle(chip, 1.5f);
        if (mark.selected)
        {
            graphics.setColour(theme::text);
            graphics.drawRoundedRectangle(chip, 1.5f, 1.0f);
        }
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

void MeterGrid::setRecordMode(bool enabled)
{
    showRecord_ = enabled;
    repaint();
}

void MeterGrid::setClearHandler(std::function<void(int channel)> handler)
{
    onClearClip_ = std::move(handler);
}

void MeterGrid::setSlotHandler(std::function<void(int channel, int slot)> handler)
{
    onSlot_ = std::move(handler);
}

void MeterGrid::setRecordHandler(std::function<void(int channel)> handler)
{
    onRecord_ = std::move(handler);
}

MeterLayout MeterGrid::layoutFor(int count) const
{
    if (count <= 0)
        return {};

    const float bridgeWidth = std::max(1.0f, static_cast<float>(getWidth()) - kScaleGutter * 2.0f);
    auto layout = layoutMeters(count, bridgeWidth, static_cast<float>(getHeight()));
    layout.originX += kScaleGutter;
    return layout;
}

MeterHit MeterGrid::meterAt(juce::Point<float> position) const
{
    MeterHit hit;
    const int count = static_cast<int>(readings_.size());
    if (count <= 0 || layout_.cellWidth <= 0.0f || layout_.cellHeight <= 0.0f)
        return hit;

    const float localX = position.x - layout_.originX;
    const float localY = position.y;
    if (localX < 0.0f || localY < 0.0f)
        return hit;

    const int column = static_cast<int>(localX / layout_.cellWidth);
    const int row = static_cast<int>(localY / layout_.cellHeight);
    if (column < 0 || column >= layout_.columns || row < 0 || row >= layout_.rows)
        return hit;

    const int index = row * layout_.columns + column;
    if (index < 0 || index >= count)
        return hit;

    const auto cell = juce::Rectangle<float>(layout_.originX + static_cast<float>(column) * layout_.cellWidth,
                                             static_cast<float>(row) * layout_.cellHeight,
                                             layout_.cellWidth,
                                             layout_.cellHeight);
    const auto parts = splitCell(cell, showRecord_);
    hit.channel = index;
    if (parts.clip.contains(position))
    {
        hit.clip = true;
        return hit;
    }

    if (showRecord_ && parts.slotRow.contains(position))
    {
        hit.record = true;
        return hit;
    }

    if (parts.slotRow.contains(position))
    {
        const float width = parts.slotRow.getWidth() / static_cast<float>(kSlotsPerChannel);
        if (width > 0.0f)
        {
            const int slot = juce::jlimit(0, kSlotsPerChannel - 1, static_cast<int>((position.x - parts.slotRow.getX()) / width));
            hit.slot = slot;
        }
    }
    return hit;
}

void MeterGrid::paint(juce::Graphics& graphics)
{
    const int count = static_cast<int>(readings_.size());
    if (count <= 0)
    {
        graphics.setColour(theme::dim);
        graphics.setFont(juce::Font(juce::FontOptions(15.0f)));
        graphics.drawFittedText("No input channels open. Open Audio setup and enable the inputs.",
                                getLocalBounds().reduced(8),
                                juce::Justification::centred,
                                3);
        return;
    }

    layout_ = layoutFor(count);
    const float fontSize = juce::jlimit(8.0f, 12.0f, layout_.cellWidth * 0.42f);
    const MeterSpan span = showPeak_ ? peakMeterSpan() : rmsMeterSpan(rmsReferenceDb_);

    for (int index = 0; index < count; ++index)
    {
        const int column = index % layout_.columns;
        const int row = index / layout_.columns;
        const auto cell = juce::Rectangle<float>(layout_.originX + static_cast<float>(column) * layout_.cellWidth,
                                                 static_cast<float>(row) * layout_.cellHeight,
                                                 layout_.cellWidth,
                                                 layout_.cellHeight);
        const auto parts = splitCell(cell, showRecord_);
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

        if (showRecord_)
        {
            const auto button = parts.slotRow.withSizeKeepingCentre(std::min(parts.slotRow.getWidth(), 14.0f),
                                                                    std::min(parts.slotRow.getHeight(), 14.0f));
            graphics.setColour(reading.recordLive ? theme::red : reading.recordArmed ? juce::Colour(0xff8d2430) : theme::panelEdge);
            graphics.fillEllipse(button);
            if (reading.recordArmed)
            {
                graphics.setColour(reading.recordLive ? theme::text : theme::red);
                graphics.drawEllipse(button, 1.0f);
            }
        }
        else
        {
            drawSlotChips(graphics, parts.slotRow, reading.slots);
        }

        graphics.setColour(reading.hasInput ? theme::dim : theme::fainter);
        graphics.setFont(juce::Font(juce::FontOptions(fontSize)));
        graphics.drawText(juce::String(index + 1), parts.number, juce::Justification::centred, false);
    }

    for (int row = 0; row < layout_.rows; ++row)
    {
        const int rowStart = row * layout_.columns;
        const int rowCount = std::min(layout_.columns, count - rowStart);
        if (rowCount <= 0)
            continue;

        const auto first = juce::Rectangle<float>(layout_.originX,
                                                  static_cast<float>(row) * layout_.cellHeight,
                                                  layout_.cellWidth,
                                                  layout_.cellHeight);
        const auto last = juce::Rectangle<float>(layout_.originX + static_cast<float>(rowCount - 1) * layout_.cellWidth,
                                                 static_cast<float>(row) * layout_.cellHeight,
                                                 layout_.cellWidth,
                                                 layout_.cellHeight);
        const auto firstBar = splitCell(first, showRecord_).bar;
        const auto lastBar = splitCell(last, showRecord_).bar;
        const auto ticks = placeTicks(firstBar, showPeak_, rmsReferenceDb_);
        drawScaleLines(graphics, ticks, firstBar.getX(), lastBar.getRight());
        drawScaleLabels(graphics, ticks, firstBar.getX(), lastBar.getRight());
    }
}

void MeterGrid::mouseDown(const juce::MouseEvent& event)
{
    layout_ = layoutFor(static_cast<int>(readings_.size()));
    const auto hit = meterAt(event.position);
    if (hit.channel < 0)
        return;
    if (hit.record && onRecord_ != nullptr)
        onRecord_(hit.channel);
    else if (hit.slot >= 0 && onSlot_ != nullptr)
        onSlot_(hit.channel, hit.slot);
    else if (hit.clip && onClearClip_ != nullptr)
        onClearClip_(hit.channel);
}

} // namespace youhost
