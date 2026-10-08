#include "MeterGrid.h"
#include "Theme.h"
#include "engine/HostLimits.h"

#include <cmath>

namespace youhost
{
namespace
{

float gainToDb(float linearGain)
{
    if (linearGain <= 0.0000001f)
        return kMeterFloorDb;
    const float db = 20.0f * std::log10(linearGain);
    return db < kMeterFloorDb ? kMeterFloorDb : db;
}

float meterNormal(float linearGain)
{
    const float db = gainToDb(linearGain);
    const float normalised = (db - kMeterFloorDb) / (0.0f - kMeterFloorDb);
    if (normalised < 0.0f)
        return 0.0f;
    if (normalised > 1.0f)
        return 1.0f;
    return normalised;
}

juce::Colour colourForLevel(float linearGain)
{
    const float db = gainToDb(linearGain);
    if (db >= -6.0f)
        return theme::red;
    if (db >= -18.0f)
        return theme::amber;
    return theme::green;
}

} // namespace

MeterGrid::MeterGrid()
{
    setOpaque(false);
}

void MeterGrid::setReadings(std::vector<MeterReading> readings, bool showPeak)
{
    readings_ = std::move(readings);
    showPeak_ = showPeak;
    repaint();
}

void MeterGrid::setClearHandler(std::function<void(int channel)> handler)
{
    onClearClip_ = std::move(handler);
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

    layout_ = layoutMeters(count, static_cast<float>(getWidth()), static_cast<float>(getHeight()));
    const float numberHeight = juce::jlimit(10.0f, 14.0f, layout_.cellHeight * 0.18f);
    const float fontSize = juce::jlimit(8.0f, 12.0f, layout_.cellWidth * 0.42f);

    for (int index = 0; index < count; ++index)
    {
        const int column = index % layout_.columns;
        const int row = index / layout_.columns;
        const auto cell = juce::Rectangle<float>(layout_.originX + static_cast<float>(column) * layout_.cellWidth,
                                                 static_cast<float>(row) * layout_.cellHeight,
                                                 layout_.cellWidth,
                                                 layout_.cellHeight);

        const auto& reading = readings_[static_cast<std::size_t>(index)];
        const float level = showPeak_ ? reading.peak : reading.rms;
        auto barArea = cell.reduced(3.0f, 2.0f);
        auto numberArea = barArea.removeFromBottom(numberHeight);
        barArea.removeFromBottom(2.0f);
        const auto clipArea = barArea.removeFromTop(juce::jmin(7.0f, barArea.getHeight() * 0.08f));
        barArea.removeFromTop(2.0f);

        graphics.setColour(theme::meterTrack);
        graphics.fillRoundedRectangle(barArea, 2.0f);

        if (reading.hasInput && level > 0.0f && barArea.getHeight() > 1.0f)
        {
            const float filled = meterNormal(level) * barArea.getHeight();
            auto levelArea = barArea.withTop(barArea.getBottom() - filled);
            graphics.setColour(colourForLevel(level));
            graphics.fillRoundedRectangle(levelArea, 2.0f);
        }

        graphics.setColour(reading.clipped ? theme::red : theme::panelEdge);
        graphics.fillRoundedRectangle(clipArea.reduced(juce::jmax(0.0f, (clipArea.getWidth() - 8.0f) * 0.5f), 0.0f), 1.5f);

        graphics.setColour(reading.hasInput ? theme::dim : theme::fainter);
        graphics.setFont(juce::Font(juce::FontOptions(fontSize)));
        graphics.drawText(juce::String(index + 1), numberArea, juce::Justification::centred, false);
    }
}

void MeterGrid::mouseDown(const juce::MouseEvent& event)
{
    layout_ = layoutMeters(static_cast<int>(readings_.size()),
                           static_cast<float>(getWidth()),
                           static_cast<float>(getHeight()));
    const int channel = channelAt(event.position);
    if (channel >= 0 && onClearClip_ != nullptr)
        onClearClip_(channel);
}

} // namespace youhost
