#include "MeterGrid.h"
#include "MeterColours.h"
#include "Theme.h"
#include "X32Look.h"
#include "engine/ChannelListen.h"
#include "engine/MeterScale.h"

#include <cmath>
#include <vector>

namespace youhost
{
namespace
{

struct BarParts
{
    juce::Rectangle<float> bar;
    juce::Rectangle<float> clip;
    juce::Rectangle<float> button;
    juce::Rectangle<float> name;
    juce::Rectangle<float> number;
};

BarParts splitCell(juce::Rectangle<float> cell)
{
    const float numberHeight = juce::jlimit(16.0f, 22.0f, cell.getHeight() * 0.08f);
    const float nameHeight = juce::jlimit(22.0f, 32.0f, cell.getHeight() * 0.09f);
    const float buttonHeight = juce::jlimit(16.0f, 22.0f, cell.getHeight() * 0.1f);
    auto body = cell.reduced(2.0f, 2.0f);
    auto number = body.removeFromBottom(numberHeight);
    body.removeFromBottom(1.0f);
    auto name = body.removeFromBottom(nameHeight);
    body.removeFromBottom(2.0f);
    auto button = body.removeFromBottom(buttonHeight);
    body.removeFromBottom(2.0f);
    const auto clip = body.removeFromTop(juce::jmin(7.0f, body.getHeight() * 0.08f));
    body.removeFromTop(2.0f);
    return { body, clip, button, name, number };
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
    const int raw = reading.listen;
    const auto listen = raw == 0 ? ChannelListen::off : raw == 1 ? ChannelListen::input : ChannelListen::record;
    return channelListenLabel(listen);
}

ChannelListen listenOf(const MeterReading& reading)
{
    if (reading.listen == 0)
        return ChannelListen::off;
    if (reading.listen == 1)
        return ChannelListen::input;
    return ChannelListen::record;
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
        const float textX = alignRight_ ? 0.0f : 1.0f;
        graphics.drawText(tickText(tick.label),
                          juce::Rectangle<float>(textX, tick.y - 7.0f, area.getWidth() - 1.0f, 14.0f),
                          alignRight_ ? juce::Justification::centredLeft : juce::Justification::centredRight,
                          false);
    }
}

struct MeterGrid::NameKeys : juce::KeyListener
{
    explicit NameKeys(MeterGrid& owner)
        : grid(owner)
    {
    }

    bool keyPressed(const juce::KeyPress& key, juce::Component*) override
    {
        return grid.handleNameKey(key);
    }

    MeterGrid& grid;
};

MeterGrid::MeterGrid()
{
    setOpaque(false);
}

MeterGrid::~MeterGrid() = default;

void MeterGrid::setCells(std::vector<BridgeCell> cells, bool showPeak, int rmsReferenceDb)
{
    cells_ = std::move(cells);
    showPeak_ = showPeak;
    rmsReferenceDb_ = normaliseRmsReferenceDb(rmsReferenceDb);
    placeNameEditor(false);
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

int MeterGrid::naturalContentWidth(int viewportWidth) const
{
    const auto metrics = metricsFor(std::max(1, viewportWidth));
    return std::max(1, static_cast<int>(std::ceil(metrics.contentWidth)));
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

void MeterGrid::setGroupRenameHandler(std::function<void(int group)> handler)
{
    onGroupRename_ = std::move(handler);
}

void MeterGrid::setSelectHandler(std::function<void(int channel, bool extend)> handler)
{
    onSelect_ = std::move(handler);
}

void MeterGrid::setNameCommitHandler(std::function<void(int channel, juce::String name)> handler)
{
    onNameCommit_ = std::move(handler);
}

void MeterGrid::setNameStepHandler(std::function<int(int channel, int direction)> handler)
{
    onNameStep_ = std::move(handler);
}

void MeterGrid::refreshMetrics()
{
    const int fit = fitWidth_ > 0 ? fitWidth_ : getWidth();
    metrics_ = metricsFor(fit);
    if (metrics_.contentWidth > static_cast<float>(fit))
        metrics_.originX = 0.0f;
}

void MeterGrid::ensureEditor()
{
    if (editor_ != nullptr)
        return;
    editor_ = std::make_unique<juce::TextEditor>();
    editor_->setSelectAllWhenFocused(true);
    editor_->setFont(juce::Font(juce::FontOptions(11.0f)));
    editor_->setJustification(juce::Justification::centred);
    editor_->setColour(juce::TextEditor::backgroundColourId, theme::panel);
    editor_->setColour(juce::TextEditor::textColourId, theme::text);
    editor_->setColour(juce::TextEditor::outlineColourId, theme::text);
    editor_->setColour(juce::TextEditor::focusedOutlineColourId, theme::text);
    if (nameKeys_ == nullptr)
        nameKeys_ = std::make_unique<NameKeys>(*this);
    editor_->addKeyListener(nameKeys_.get());
    editor_->onFocusLost = [this] { finishNameEdit(true); };
    editor_->setVisible(false);
    addAndMakeVisible(*editor_);
}

void MeterGrid::placeNameEditor(bool reveal)
{
    if (editor_ == nullptr || editingChannel_ < 0)
        return;
    refreshMetrics();
    float x = metrics_.originX;
    for (const auto& cell : cells_)
    {
        const float width = cell.header ? metrics_.headerWidth : metrics_.channelWidth;
        const auto bounds = juce::Rectangle<float>(x, 0.0f, width, static_cast<float>(getHeight()));
        x += width;
        if (cell.header || cell.channel != editingChannel_)
            continue;
        const auto name = splitCell(bounds).name.toNearestInt();
        editor_->setBounds(name);
        editor_->setVisible(true);
        editor_->toFront(false);
        if (reveal)
        {
            if (auto* viewport = findParentComponentOfClass<juce::Viewport>())
            {
                const int viewWidth = std::max(1, viewport->getViewWidth());
                const int maxX = std::max(0, getWidth() - viewWidth);
                const int target = std::clamp(name.getX() - 8, 0, maxX);
                viewport->setViewPosition(target, viewport->getViewPositionY());
            }
        }
        return;
    }
    editor_->setVisible(false);
}

void MeterGrid::finishNameEdit(bool commit)
{
    if (editingChannel_ < 0 || editor_ == nullptr)
        return;
    const int channel = editingChannel_;
    const auto text = editor_->getText();
    editingChannel_ = -1;
    editor_->setVisible(false);
    if (commit && onNameCommit_ != nullptr)
        onNameCommit_(channel, text);
}

void MeterGrid::beginNameEdit(int channel)
{
    if (channel < 0)
        return;
    if (editingChannel_ == channel && editor_ != nullptr && editor_->isVisible())
    {
        editor_->grabKeyboardFocus();
        editor_->selectAll();
        return;
    }
    if (editingChannel_ >= 0)
        finishNameEdit(true);

    juce::String title;
    bool found = false;
    for (const auto& cell : cells_)
    {
        if (! cell.header && cell.channel == channel)
        {
            title = cell.title;
            found = true;
            break;
        }
    }
    if (! found)
        return;

    ensureEditor();
    editingChannel_ = channel;
    editor_->setText(title, false);
    placeNameEditor(true);
    editor_->grabKeyboardFocus();
    editor_->selectAll();
}

bool MeterGrid::handleNameKey(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() || mods.isAltDown() || mods.isCtrlDown())
        return false;
    if (editingChannel_ < 0 || editor_ == nullptr)
        return false;

    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::escapeKey)
    {
        finishNameEdit(false);
        return true;
    }
    if (code == juce::KeyPress::returnKey)
    {
        finishNameEdit(true);
        return true;
    }
    if (code != juce::KeyPress::tabKey)
        return false;

    const int channel = editingChannel_;
    const int direction = mods.isShiftDown() ? -1 : 1;
    const auto text = editor_->getText();
    editingChannel_ = -1;
    editor_->setVisible(false);
    if (onNameCommit_ != nullptr)
        onNameCommit_(channel, text);
    if (onNameStep_ == nullptr)
        return true;
    const int next = onNameStep_(channel, direction);
    if (next >= 0)
    {
        juce::Component::SafePointer<MeterGrid> safe(this);
        juce::MessageManager::callAsync([safe, next]
        {
            if (safe != nullptr)
                safe->beginNameEdit(next);
        });
    }
    return true;
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
        else if (parts.name.contains(position))
            hit.name = true;
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
            const int colourId = normaliseX32Colour(cell.color);
            const bool inverted = kX32Colours[colourId].inverted;
            const auto hue = x32Hue(cell.color);
            const auto fill = hue.isTransparent() ? theme::panel
                                                   : (inverted ? juce::Colour(0xff1a1d27) : hue);
            const auto ink = hue.isTransparent() ? theme::text
                                                  : (inverted ? hue
                                                              : (hue.getPerceivedBrightness() > 0.55f
                                                                     ? juce::Colour(0xff141414)
                                                                     : juce::Colours::white));
            graphics.setColour(fill);
            graphics.fillRoundedRectangle(bounds.reduced(1.0f), 4.0f);
            if (! hue.isTransparent())
            {
                graphics.setColour(hue);
                graphics.fillRect(bounds.getX() + 2.0f, bounds.getY() + 2.0f, 7.0f, bounds.getHeight() - 4.0f);
            }
            auto body = bounds.reduced(8.0f, 8.0f);
            graphics.setColour(ink);
            graphics.setFont(juce::Font(juce::FontOptions(13.0f).withStyle("Bold")));
            graphics.drawFittedText(cell.title, body.removeFromTop(40.0f).toNearestInt(), juce::Justification::centred, 3);

            const float level = showPeak_ ? cell.reading.peak : cell.reading.rms;
            auto meter = body.removeFromTop(std::min(80.0f, body.getHeight() * 0.45f)).reduced(10.0f, 4.0f);
            graphics.setColour(theme::meterTrack);
            graphics.fillRoundedRectangle(meter, 2.0f);
            if (level > 0.0f && meter.getHeight() > 1.0f)
            {
                const float filled = normaliseDb(gainToDb(level), span) * meter.getHeight();
                auto levelArea = meter.withTop(meter.getBottom() - filled);
                graphics.setColour(meterLevelColour(level, rmsReferenceDb_));
                graphics.fillRoundedRectangle(levelArea, 2.0f);
            }

            juce::String state = "OFF";
            if (cell.memberCount > 0 && cell.membersRecord == cell.memberCount)
                state = "REC";
            else if (cell.memberCount > 0 && cell.membersInput == cell.memberCount)
                state = "INPUT";
            else if (cell.membersRecord + cell.membersInput > 0)
                state = "mixed";
            if (cell.reading.clipped)
                state << "  CLIP";
            if (cell.anyPlugin)
                state << "  FX";
            if (cell.collapsed)
                state << "  folded";

            graphics.setColour(ink);
            graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
            graphics.drawFittedText(state, body.toNearestInt(), juce::Justification::centred, 3);
            continue;
        }

        graphics.setColour(theme::background);
        graphics.fillRect(bounds);
        const auto wash = x32Wash(cell.color);
        if (! wash.isTransparent())
        {
            graphics.setColour(wash);
            graphics.fillRect(bounds);
        }
        const auto hue = x32Hue(cell.color);
        if (! hue.isTransparent())
        {
            graphics.setColour(hue);
            graphics.fillRect(bounds.getX(), bounds.getY(), 6.0f, bounds.getHeight());
            graphics.fillRect(bounds.getX(), bounds.getY(), bounds.getWidth(), 4.0f);
        }

        const auto parts = splitCell(bounds);
        if (! haveBar)
        {
            firstBar = parts.bar;
            haveBar = true;
        }
        lastBar = parts.bar;

        const auto listen = listenOf(cell.reading);
        const bool on = channelListenAudible(listen);
        const float level = showPeak_ ? cell.reading.peak : cell.reading.rms;
        graphics.setColour(on ? theme::meterTrack : theme::panelEdge.withAlpha(0.45f));
        graphics.fillRoundedRectangle(parts.bar, 2.0f);
        if (on && cell.reading.hasInput && level > 0.0f && parts.bar.getHeight() > 1.0f)
        {
            const float filled = normaliseDb(gainToDb(level), span) * parts.bar.getHeight();
            auto levelArea = parts.bar.withTop(parts.bar.getBottom() - filled);
            graphics.setColour(meterLevelColour(level, rmsReferenceDb_));
            graphics.fillRoundedRectangle(levelArea, 2.0f);
        }

        graphics.setColour(! on ? theme::panelEdge : cell.reading.clipped ? theme::red : theme::panelEdge);
        graphics.fillRoundedRectangle(parts.clip.reduced(juce::jmax(0.0f, (parts.clip.getWidth() - 8.0f) * 0.5f), 0.0f), 1.5f);

        auto button = parts.button.reduced(1.0f, 0.0f);
        juce::Colour buttonFill = theme::button;
        if (listen == ChannelListen::record)
            buttonFill = cell.reading.recordLive ? theme::red : juce::Colour(0xff8d2430);
        else if (listen == ChannelListen::input)
            buttonFill = juce::Colour(0xff245a9a);
        graphics.setColour(buttonFill);
        graphics.fillRoundedRectangle(button, 3.0f);
        graphics.setColour(on ? juce::Colours::white : theme::fainter);
        const float fontSize = juce::jlimit(8.0f, 11.0f, button.getWidth() * 0.28f);
        graphics.setFont(juce::Font(juce::FontOptions(fontSize).withStyle("Bold")));
        graphics.drawText(channelButtonText(cell.reading), button, juce::Justification::centred, false);

        auto nameArea = parts.name;
        if (cell.pdc.isNotEmpty())
        {
            auto pdcArea = nameArea.removeFromBottom(nameArea.getHeight() * 0.45f);
            graphics.setColour(theme::amber);
            graphics.setFont(juce::Font(juce::FontOptions(juce::jlimit(7.0f, 9.0f, width * 0.18f))));
            graphics.drawText(cell.pdc, pdcArea, juce::Justification::centred, true);
        }
        if (cell.title.isNotEmpty())
        {
            graphics.setColour(on ? theme::dim : theme::fainter);
            graphics.setFont(juce::Font(juce::FontOptions(juce::jlimit(8.0f, 11.0f, width * 0.24f))));
            graphics.drawText(cell.title, nameArea, juce::Justification::centred, true);
        }

        graphics.setColour(theme::text);
        graphics.setFont(juce::Font(juce::FontOptions(juce::jlimit(12.0f, 16.0f, width * 0.42f)).withStyle("Bold")));
        graphics.drawText(juce::String(cell.channel + 1), parts.number, juce::Justification::centred, false);

        if (cell.selected)
        {
            graphics.setColour(theme::text);
            graphics.drawRoundedRectangle(bounds.reduced(1.0f), 3.0f, 1.5f);
        }

        if (&cell != &cells_.back())
        {
            graphics.setColour(juce::Colour(0xff8b95a8));
            graphics.fillRect(bounds.getRight() - 1.0f, 0.0f, 1.0f, bounds.getHeight());
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
    refreshMetrics();

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

    if (hit.header)
    {
        if (event.getNumberOfClicks() >= 2)
        {
            if (onGroupToggle_ != nullptr)
                onGroupToggle_(hit.group);
            if (onGroupRename_ != nullptr)
                onGroupRename_(hit.group);
        }
        else if (onGroupToggle_ != nullptr)
        {
            onGroupToggle_(hit.group);
        }
    }
    else if (hit.name && event.getNumberOfClicks() >= 2)
        beginNameEdit(hit.channel);
    else if (hit.record && onRecord_ != nullptr)
        onRecord_(hit.channel);
    else if (hit.clip && onClearClip_ != nullptr)
        onClearClip_(hit.channel);
    else if (hit.channel >= 0 && onSelect_ != nullptr)
        onSelect_(hit.channel, false);
}

} // namespace youhost
