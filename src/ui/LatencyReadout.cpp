#include "LatencyReadout.h"
#include "engine/LatencyCard.h"
#include "Theme.h"

namespace youhost
{
namespace
{

juce::String millisecondsText(int samples, double sampleRate)
{
    return juce::String(samplesToMilliseconds(samples, sampleRate), 2) + " ms";
}

juce::Rectangle<float> cardBlock(const LatencyBlock& block, int textWidth)
{
    return { static_cast<float>(kLatencyPadX),
             static_cast<float>(block.top),
             static_cast<float>(textWidth),
             static_cast<float>(block.height) };
}

void drawStat(juce::Graphics& graphics,
              juce::Rectangle<float> row,
              const juce::String& label,
              int samples,
              double sampleRate,
              juce::Colour valueColour)
{
    graphics.setColour(theme::dim);
    graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
    graphics.drawText(label, row.removeFromLeft(132.0f), juce::Justification::centredLeft, false);

    graphics.setColour(valueColour);
    graphics.setFont(juce::Font(juce::FontOptions(15.0f)));
    const auto samplesText = juce::String(samples) + " smp";
    graphics.drawText(samplesText, row.removeFromLeft(108.0f), juce::Justification::centredRight, false);
    graphics.setColour(theme::text);
    graphics.drawText(millisecondsText(samples, sampleRate), row, juce::Justification::centredRight, false);
}

} // namespace

LatencyReadout::LatencyReadout()
{
    setOpaque(false);
    addAndMakeVisible(allButton_);
    addAndMakeVisible(groupButton_);
    addAndMakeVisible(resetButton_);
    allButton_.setTooltip("Global. All channels aligned: every included channel lines up on the slowest plugin.");
    groupButton_.setTooltip("Per group. Only channels inside a group line up on that group's slowest plugin. Ungrouped channels get no extra delay.");
    resetButton_.setTooltip("Reset the dropout count and the graph. The CSV log is kept.");
    for (auto* button : { &allButton_, &groupButton_, &resetButton_ })
        button->setMouseClickGrabsKeyboardFocus(false);
    allButton_.onClick = [this]
    {
        if (onAlign_ != nullptr)
            onAlign_(0);
    };
    groupButton_.onClick = [this]
    {
        if (onAlign_ != nullptr)
            onAlign_(1);
    };
    setAlignGroup(0);
}

void LatencyReadout::setNumbers(const LatencyNumbers& numbers)
{
    numbers_ = numbers;
    repaint();
}

void LatencyReadout::setGroupLines(const GroupLatencyLine* lines, int count)
{
    if (count < 0)
        count = 0;
    if (count > kMaxDisplayGroups)
        count = kMaxDisplayGroups;
    groupCount_ = count;
    for (int index = 0; index < count; ++index)
        groupLines_[static_cast<std::size_t>(index)] = lines != nullptr ? lines[index] : GroupLatencyLine {};
    repaint();
}

void LatencyReadout::setAlignGroup(int perGroup)
{
    alignGroup_ = perGroup == 1 ? 1 : 0;
    allButton_.setToggleState(alignGroup_ == 0, juce::dontSendNotification);
    groupButton_.setToggleState(alignGroup_ == 1, juce::dontSendNotification);
    allButton_.setColour(juce::TextButton::buttonColourId, alignGroup_ == 0 ? theme::buttonOn : theme::button);
    groupButton_.setColour(juce::TextButton::buttonColourId, alignGroup_ == 1 ? theme::buttonOn : theme::button);
    repaint();
}

void LatencyReadout::setAlignHandler(std::function<void(int)> handler)
{
    onAlign_ = std::move(handler);
}

void LatencyReadout::setResetHandler(std::function<void()> handler)
{
    resetButton_.onClick = std::move(handler);
}

void LatencyReadout::resized()
{
    const bool perGroup = alignGroup_ == 1;
    const auto layout = layoutLatencyCard(std::max(1, getWidth()), longestLatencyNoteChars(), perGroup ? groupCount_ : 0, perGroup);
    auto row = cardBlock(layout.dropouts, layout.textWidth).toNearestInt();
    resetButton_.setBounds(row.removeFromRight(72).withSizeKeepingCentre(72, 22));
    auto modes = cardBlock(layout.modes, layout.textWidth).toNearestInt();
    allButton_.setBounds(modes.removeFromLeft(110).reduced(0, 2));
    modes.removeFromLeft(6);
    groupButton_.setBounds(modes.removeFromLeft(110).reduced(0, 2));
}

void LatencyReadout::paint(juce::Graphics& graphics)
{
    auto bounds = getLocalBounds().toFloat();
    graphics.setColour(theme::panel);
    graphics.fillRoundedRectangle(bounds, 12.0f);
    graphics.setColour(theme::panelEdge);
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), 12.0f, 1.0f);

    const bool perGroup = alignGroup_ == 1;
    const auto layout = layoutLatencyCard(std::max(1, getWidth()), longestLatencyNoteChars(), perGroup ? groupCount_ : 0, perGroup);
    auto hero = cardBlock(layout.hero, layout.textWidth);
    const auto noteArea = cardBlock(layout.note, layout.textWidth);

    graphics.setColour(theme::dim);
    graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
    graphics.drawText("USB IN to USB OUT", hero.removeFromTop(16.0f), juce::Justification::centredLeft, false);

    hero.removeFromTop(2.0f);
    const bool open = numbers_.deviceOpen && numbers_.sampleRate > 0.0;
    const juce::String ms = open ? juce::String(samplesToMilliseconds(numbers_.roundTripSamples, numbers_.sampleRate), 2)
                                 : juce::String("--");
    graphics.setColour(theme::text);
    graphics.setFont(juce::Font(juce::FontOptions(54.0f)));
    graphics.drawText(ms, hero.removeFromTop(60.0f), juce::Justification::centredLeft, false);

    graphics.setColour(theme::green);
    graphics.setFont(juce::Font(juce::FontOptions(18.0f)));
    const juce::String samples = open ? juce::String(numbers_.roundTripSamples) + " samples" : "no device";
    graphics.drawText(samples, hero.removeFromTop(24.0f), juce::Justification::centredLeft, false);

    graphics.setColour(theme::fainter);
    graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
    graphics.drawText("milliseconds", hero, juce::Justification::topLeft, false);

    const double rate = numbers_.sampleRate;
    drawStat(graphics, cardBlock(layout.buffer, layout.textWidth), "Buffer", numbers_.bufferSamples, rate, theme::text);
    drawStat(graphics, cardBlock(layout.input, layout.textWidth), "Input", numbers_.inputSamples, rate, theme::text);
    drawStat(graphics, cardBlock(layout.output, layout.textWidth), "Output", numbers_.outputSamples, rate, theme::text);
    drawStat(graphics, cardBlock(layout.compensation, layout.textWidth), "Compensation", numbers_.compensationSamples, rate, theme::dim);

    auto dropoutRow = cardBlock(layout.dropouts, layout.textWidth);
    dropoutRow.removeFromRight(80.0f);
    graphics.setColour(theme::dim);
    graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
    graphics.drawText("Dropouts", dropoutRow.removeFromLeft(132.0f), juce::Justification::centredLeft, false);
    const int dropouts = juce::jmax(0, numbers_.xruns);
    graphics.setColour(dropouts > 0 ? theme::red : theme::text);
    graphics.setFont(juce::Font(juce::FontOptions(15.0f)));
    graphics.drawText(juce::String(dropouts), dropoutRow, juce::Justification::centredRight, false);

    if (alignGroup_ == 1)
    {
        auto list = cardBlock(layout.groups, layout.textWidth);
        graphics.setColour(theme::dim);
        graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
        for (int index = 0; index < groupCount_ && index < layout.groupRows; ++index)
        {
            auto row = list.removeFromTop(static_cast<float>(kLatencyGroupRowH));
            const auto& line = groupLines_[static_cast<std::size_t>(index)];
            const auto title = line.name[0] != '\0' ? juce::String::fromUTF8(line.name)
                                                    : "Group " + juce::String(line.group + 1);
            graphics.setColour(theme::text);
            graphics.drawText(title, row.removeFromLeft(180.0f), juce::Justification::centredLeft, true);
            graphics.setColour(theme::dim);
            const auto detail = juce::String(line.members) + " ch   " + juce::String(line.alignSamples) + " smp";
            graphics.drawText(detail, row, juce::Justification::centredRight, true);
        }
        if (layout.groups.height >= kLatencyGroupRowH)
        {
            auto ungrouped = list.removeFromTop(static_cast<float>(kLatencyGroupRowH));
            graphics.setColour(theme::dim);
            graphics.drawText("Ungrouped", ungrouped.removeFromLeft(180.0f), juce::Justification::centredLeft, true);
            graphics.drawText("0 smp", ungrouped, juce::Justification::centredRight, true);
        }
    }

    graphics.setColour(theme::fainter);
    graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
    const auto note = latencyNoteText(alignGroup_ == 1, numbers_.formula);
    graphics.drawFittedText(juce::String(note),
                            noteArea.toNearestInt(),
                            juce::Justification::topLeft,
                            layout.noteLines);
}

} // namespace youhost
