#include "MeterGrid.h"
#include "Theme.h"
#include "X32Look.h"
#include "engine/MeterScale.h"

#include <cmath>
#include <vector>

namespace youhost
{
namespace
{

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
    juce::Rectangle<float> button;
    juce::Rectangle<float> number;
};

BarParts splitCell(juce::Rectangle<float> cell)
{
    const float numberHeight = juce::jlimit(12.0f, 16.0f, cell.getHeight() * 0.1f);
    const float buttonHeight = juce::jlimit(16.0f, 22.0f, cell.getHeight() * 0.12f);
    auto body = cell.reduced(2.0f, 2.0f);
    auto number = body.removeFromBottom(numberHeight);
    body.removeFromBottom(2.0f);
    auto button = body.removeFromBottom(buttonHeight);
    body.removeFromBottom(2.0f);
    const auto clip = body.removeFromTop(juce::jmin(7.0f, body.getHeight() * 0.08f));
    body.removeFromTop(2.0f);
    return { body, clip, button, number };
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

juce::String channelButtonText(const MeterReading& reading)
{
    return reading.recordArmed ? "REC" : "OFF";
}

} // namespace

void MeterScaleRail::setScale(bool peak, int referenceDb, bool alignRight)
{
    peak_ = peak;
    referenceDb_ = normaliseRmsReferenceDb(referenceDb);
    alignRight_ = alignRight;
    repaint();
}

void MeterScaleRail::paint(juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat();
    const auto parts = splitCell(area);
    const auto ticks = placeTicks(parts.bar, peak_, referenceDb_);
    graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
    for (const auto& tick : ticks)
    {
        graphics.setColour(tick.label == 0 ? theme::text : theme::dim);
        graphics.drawText(tickText(tick.label),
                          juce::Rectangle<float>(2.0f, tick.y - 6.0f, area.getWidth() - 4.0f, 12.0f),
                          alignRight_ ? juce::Justification::centredLeft : juce::Justification::centredRight,
                          false);
    }
}

MeterGrid::MeterGrid()
{
    setOpaque(false);
}

void MeterGrid::setCells(std::vector<BridgeCell> cells, bool showPeak, int rmsReferenceDb)
{
    cells_ = std::move(cells);
    showPeak_ = showPeak;
    rmsReferenceDb_ = normaliseRmsReferenceDb(rmsReferenceDb);
    repaint();
}

void MeterGrid::setFitWidth(int viewportWidth)
{
    fitWidth_ = std::max(0, viewportWidth);
}

int MeterGrid::preferredWidth(int viewportWidth) const
{
    const auto metrics = metricsFor(std::max(1, viewportWidth));
    return std::max(viewportWidth, static_cast<int>(std::ceil(metrics.contentWidth)));
}

void MeterGrid::setClearHandler(std::function<void(int channel)> handler)
{
    onClearClip_ = std::move(handler);
}

void MeterGrid::setRecordHandler(std::function<void(int channel)> handler)
{
    onRecord_ = std::move(handler);
}

void MeterGrid::setChannelMenuHandler(std::function<void(int channel)> handler)
{
    onChannelMenu_ = std::move(handler);
}

void MeterGrid::setGroupToggleHandler(std::function<void(int group)> handler)
{
    onGroupToggle_ = std::move(handler);
}

void MeterGrid::setGroupMenuHandler(std::function<void(int group)> handler)
{
    onGroupMenu_ = std::move(handler);
}

void MeterGrid::setSelectHandler(std::function<void(int channel, bool extend)> handler)
{
    onSelect_ = std::move(handler);
}

BridgeMetrics MeterGrid::metricsFor(int viewportWidth) const
{
    int channels = 0;
    int headers = 0;
    for (const auto& cell : cells_)
    {
        if (cell.header)
            ++headers;
        else
            ++channels;
    }
    return layoutBridge(channels, headers, static_cast<float>(std::max(1, viewportWidth)));
}

MeterHit MeterGrid::hitAt(juce::Point<float> position) const
{
    MeterHit hit;
    float x = metrics_.originX;
    for (const auto& cell : cells_)
    {
        const float width = cell.header ? metrics_.headerWidth : metrics_.channelWidth;
        const auto bounds = juce::Rectangle<float>(x, 0.0f, width, static_cast<float>(getHeight()));
        x += width;
        if (! bounds.contains(position))
            continue;

        if (cell.header)
        {
            hit.header = true;
            hit.group = cell.group;
            return hit;
        }

        hit.channel = cell.channel;
        const auto parts = splitCell(bounds);
        if (parts.clip.contains(position))
            hit.clip = true;
        else if (parts.button.contains(position))
            hit.record = true;
        return hit;
    }
    return hit;
}

void MeterGrid::paint(juce::Graphics& graphics)
{
    const int fit = fitWidth_ > 0 ? fitWidth_ : getWidth();
    metrics_ = metricsFor(fit);
    if (metrics_.contentWidth > static_cast<float>(fit))
        metrics_.originX = 0.0f;

    if (cells_.empty())
    {
        graphics.setColour(theme::dim);
        graphics.setFont(juce::Font(juce::FontOptions(15.0f)));
        graphics.drawFittedText("No input channels are open. Open Audio setup and enable the inputs.",
                                getLocalBounds().reduced(8),
                                juce::Justification::centred,
                                3);
        return;
    }

    const MeterSpan span = showPeak_ ? peakMeterSpan() : rmsMeterSpan(rmsReferenceDb_);
    float x = metrics_.originX;
    juce::Rectangle<float> firstBar;
    juce::Rectangle<float> lastBar;
    bool haveBar = false;

    for (const auto& cell : cells_)
    {
        const float width = cell.header ? metrics_.headerWidth : metrics_.channelWidth;
        const auto bounds = juce::Rectangle<float>(x, 0.0f, width, static_cast<float>(getHeight()));
        x += width;

        if (cell.header)
        {
            graphics.setColour(x32Fill(cell.color));
            graphics.fillRoundedRectangle(bounds.reduced(1.0f), 4.0f);
            if (kX32Colours[normaliseX32Colour(cell.color)].inverted)
            {
                graphics.setColour(x32Ink(cell.color));
                graphics.fillRect(bounds.getX() + 2.0f, bounds.getY() + 2.0f, 3.0f, bounds.getHeight() - 4.0f);
            }
            auto body = bounds.reduced(4.0f, 8.0f);
            graphics.setColour(x32Ink(cell.color));
            graphics.setFont(juce::Font(juce::FontOptions(12.0f).withStyle("Bold")));
            graphics.drawFittedText(cell.title, body.removeFromTop(32.0f).toNearestInt(), juce::Justification::centred, 2);

            const float level = showPeak_ ? cell.reading.peak : cell.reading.rms;
            auto meter = body.removeFromTop(std::min(80.0f, body.getHeight() * 0.45f)).reduced(10.0f, 4.0f);
            graphics.setColour(theme::meterTrack);
            graphics.fillRoundedRectangle(meter, 2.0f);
            if (level > 0.0f && meter.getHeight() > 1.0f)
            {
                const float filled = normaliseDb(gainToDb(level), span) * meter.getHeight();
                auto levelArea = meter.withTop(meter.getBottom() - filled);
                graphics.setColour(colourForLevel(level));
                graphics.fillRoundedRectangle(levelArea, 2.0f);
            }

            juce::String state = "OFF";
            if (cell.memberCount > 0 && cell.membersOn == cell.memberCount)
                state = "REC";
            else if (cell.membersOn > 0)
                state = juce::String(cell.membersOn) + " on";
            if (cell.reading.clipped)
                state << "  CLIP";
            if (cell.anyPlugin)
                state << "  FX";
            if (cell.collapsed)
                state << "  folded";

            graphics.setColour(x32Ink(cell.color));
            graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
            graphics.drawFittedText(state, body.toNearestInt(), juce::Justification::centred, 3);
            continue;
        }

        graphics.setColour(x32Fill(cell.color));
        graphics.fillRect(bounds.getX(), bounds.getY(), bounds.getWidth(), 4.0f);
        if (kX32Colours[normaliseX32Colour(cell.color)].inverted)
        {
            graphics.setColour(x32Ink(cell.color));
            graphics.fillRect(bounds.getX(), bounds.getY(), 3.0f, bounds.getHeight());
        }

        const auto parts = splitCell(bounds);
        if (! haveBar)
        {
            firstBar = parts.bar;
            haveBar = true;
        }
        lastBar = parts.bar;

        const bool on = cell.reading.recordArmed;
        const float level = showPeak_ ? cell.reading.peak : cell.reading.rms;
        graphics.setColour(on ? theme::meterTrack : theme::panelEdge.withAlpha(0.45f));
        graphics.fillRoundedRectangle(parts.bar, 2.0f);
        if (on && cell.reading.hasInput && level > 0.0f && parts.bar.getHeight() > 1.0f)
        {
            const float filled = normaliseDb(gainToDb(level), span) * parts.bar.getHeight();
            auto levelArea = parts.bar.withTop(parts.bar.getBottom() - filled);
            graphics.setColour(colourForLevel(level));
            graphics.fillRoundedRectangle(levelArea, 2.0f);
        }

        graphics.setColour(! on ? theme::panelEdge : cell.reading.clipped ? theme::red : theme::panelEdge);
        graphics.fillRoundedRectangle(parts.clip.reduced(juce::jmax(0.0f, (parts.clip.getWidth() - 8.0f) * 0.5f), 0.0f), 1.5f);

        auto button = parts.button.reduced(1.0f, 0.0f);
        graphics.setColour(on ? (cell.reading.recordLive ? theme::red : juce::Colour(0xff8d2430)) : theme::button);
        graphics.fillRoundedRectangle(button, 3.0f);
        graphics.setColour(on ? juce::Colours::white : theme::fainter);
        const float fontSize = juce::jlimit(8.0f, 11.0f, button.getWidth() * 0.34f);
        graphics.setFont(juce::Font(juce::FontOptions(fontSize).withStyle("Bold")));
        graphics.drawText(channelButtonText(cell.reading), button, juce::Justification::centred, false);

        graphics.setColour(on ? theme::text : theme::fainter);
        graphics.setFont(juce::Font(juce::FontOptions(juce::jlimit(8.0f, 12.0f, width * 0.28f))));
        auto number = cell.title.isNotEmpty() ? cell.title : juce::String(cell.channel + 1);
        graphics.drawText(number, parts.number, juce::Justification::centred, true);

        if (cell.selected)
        {
            graphics.setColour(theme::text);
            graphics.drawRoundedRectangle(bounds.reduced(1.0f), 3.0f, 1.5f);
        }
    }

    if (haveBar)
    {
        const auto ticks = placeTicks(firstBar, showPeak_, rmsReferenceDb_);
        drawScaleLines(graphics, ticks, firstBar.getX(), lastBar.getRight());
    }
}

void MeterGrid::mouseDown(const juce::MouseEvent& event)
{
    const int fit = fitWidth_ > 0 ? fitWidth_ : getWidth();
    metrics_ = metricsFor(fit);
    if (metrics_.contentWidth > static_cast<float>(fit))
        metrics_.originX = 0.0f;

    const auto hit = hitAt(event.position);
    if (event.mods.isPopupMenu())
    {
        if (hit.header && onGroupMenu_ != nullptr)
            onGroupMenu_(hit.group);
        else if (hit.channel >= 0 && onChannelMenu_ != nullptr)
            onChannelMenu_(hit.channel);
        return;
    }

    if (event.mods.isShiftDown() && hit.channel >= 0 && onSelect_ != nullptr)
    {
        onSelect_(hit.channel, true);
        return;
    }

    if (hit.header && onGroupToggle_ != nullptr)
        onGroupToggle_(hit.group);
    else if (hit.record && onRecord_ != nullptr)
        onRecord_(hit.channel);
    else if (hit.clip && onClearClip_ != nullptr)
        onClearClip_(hit.channel);
    else if (hit.channel >= 0 && onSelect_ != nullptr)
        onSelect_(hit.channel, false);
}

} // namespace youhost
