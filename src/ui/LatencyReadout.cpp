#include "LatencyReadout.h"
#include "Theme.h"

namespace youhost
{
namespace
{

juce::String millisecondsText(int samples, double sampleRate)
{
    return juce::String(samplesToMilliseconds(samples, sampleRate), 2) + " ms";
}

juce::String formulaNote(RoundTripFormula formula)
{
    switch (formula)
    {
        case RoundTripFormula::coreAudioSubtractOneBuffer:
            return "Round trip = input + output - one buffer + compensation. "
                   "JUCE 9 CoreAudio includes the buffer in both input and output latency.";
        case RoundTripFormula::alsaAddOneBuffer:
            return "Round trip = input + output + one buffer + compensation. "
                   "JUCE ALSA reports latency with one period already removed.";
        case RoundTripFormula::driverSum:
            break;
    }

    return "Round trip = driver input + driver output + compensation.";
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

LatencyReadout::CardLayout LatencyReadout::layoutCard(juce::Rectangle<float> bounds)
{
    auto inner = bounds.reduced(18.0f, 14.0f);
    CardLayout layout;
    layout.note = inner.removeFromBottom(36.0f);
    inner.removeFromBottom(4.0f);
    layout.modeRow = inner.removeFromBottom(26.0f);
    inner.removeFromBottom(6.0f);

    const bool wide = inner.getWidth() > 640.0f;
    layout.hero = wide ? inner.removeFromLeft(inner.getWidth() * 0.40f) : inner.removeFromTop(inner.getHeight() * 0.46f);
    if (wide)
        inner.removeFromLeft(16.0f);
    else
        inner.removeFromTop(6.0f);

    layout.bufferRow = inner.removeFromTop(26.0f);
    layout.inputRow = inner.removeFromTop(26.0f);
    layout.outputRow = inner.removeFromTop(26.0f);
    layout.compensationRow = inner.removeFromTop(26.0f);
    layout.dropoutRow = inner.removeFromTop(26.0f);
    return layout;
}

LatencyReadout::LatencyReadout()
{
    setOpaque(false);
    addAndMakeVisible(allButton_);
    addAndMakeVisible(groupButton_);
    addAndMakeVisible(graphButton_);
    addAndMakeVisible(resetButton_);
    allButton_.setTooltip("Line every included channel up on the slowest plugin. A stereo pair stays together.");
    groupButton_.setTooltip("Each group lines up on its own slowest plugin. Ungrouped channels are not delayed. A pair split across groups can comb.");
    graphButton_.setTooltip("Open the dropout timeline (3 or D).");
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
    const auto layout = layoutCard(getLocalBounds().toFloat());
    auto row = layout.dropoutRow.toNearestInt();
    resetButton_.setBounds(row.removeFromRight(72).withSizeKeepingCentre(72, 22));
    row.removeFromRight(6);
    graphButton_.setBounds(row.removeFromRight(86).withSizeKeepingCentre(86, 22));
    auto modes = layout.modeRow.toNearestInt();
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

    const auto layout = layoutCard(bounds);
    auto hero = layout.hero;
    const auto noteArea = layout.note;

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
    drawStat(graphics, layout.bufferRow, "Buffer", numbers_.bufferSamples, rate, theme::text);
    drawStat(graphics, layout.inputRow, "Input", numbers_.inputSamples, rate, theme::text);
    drawStat(graphics, layout.outputRow, "Output", numbers_.outputSamples, rate, theme::text);
    drawStat(graphics, layout.compensationRow, "Compensation", numbers_.compensationSamples, rate, theme::dim);

    auto dropoutRow = layout.dropoutRow;
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
    const juce::String modeNote = alignGroup_ == 1
                                      ? "Per group: each group uses its own slowest plugin. Ungrouped channels are not delayed, so a split pair can comb."
                                      : "All aligned: every included channel ends on the same sample, including a dry channel next to a plugin.";
    graphics.drawFittedText(modeNote + "  " + formulaNote(numbers_.formula),
                            noteArea.toNearestInt(),
                            juce::Justification::topLeft,
                            3);
}

} // namespace youhost
