#include "LatencyReadout.h"
#include "engine/LatencyCard.h"
#include "engine/Shortcuts.h"
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
    addAndMakeVisible(graphButton_);
    addAndMakeVisible(resetButton_);
    allButton_.setTooltip("Line every included channel up on the slowest plugin. A stereo pair stays together.");
    groupButton_.setTooltip("Each group lines up on its own slowest plugin. Ungrouped channels are not delayed. A pair split across groups can comb.");
    graphButton_.setTooltip("Open the dropout timeline (" + juce::String(shortcutChord(ShortcutId::dropouts)) + ").");
    resetButton_.setTooltip("Reset the dropout count and the graph. The CSV log is kept.");
    for (auto* button : { &allButton_, &groupButton_, &graphButton_, &resetButton_ })
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

void LatencyReadout::setGraphHandler(std::function<void()> handler)
{
    graphButton_.onClick = std::move(handler);
}

void LatencyReadout::resized()
{
    const auto layout = layoutLatencyCard(std::max(1, getWidth()));
    auto row = cardBlock(layout.dropouts, layout.textWidth).toNearestInt();
    resetButton_.setBounds(row.removeFromRight(72).withSizeKeepingCentre(72, 22));
    row.removeFromRight(6);
    graphButton_.setBounds(row.removeFromRight(86).withSizeKeepingCentre(86, 22));
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

    const auto layout = layoutLatencyCard(std::max(1, getWidth()));
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
    dropoutRow.removeFromRight(156.0f);
    graphics.setColour(theme::dim);
    graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
    graphics.drawText("Dropouts", dropoutRow.removeFromLeft(132.0f), juce::Justification::centredLeft, false);
    const int dropouts = juce::jmax(0, numbers_.xruns);
    graphics.setColour(dropouts > 0 ? theme::red : theme::text);
    graphics.setFont(juce::Font(juce::FontOptions(15.0f)));
    graphics.drawText(juce::String(dropouts), dropoutRow, juce::Justification::centredRight, false);

    graphics.setColour(theme::fainter);
    graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
    const auto note = latencyNoteText(alignGroup_ == 1, numbers_.formula);
    graphics.drawFittedText(juce::String(note),
                            noteArea.toNearestInt(),
                            juce::Justification::topLeft,
                            layout.noteLines);
}

} // namespace youhost
