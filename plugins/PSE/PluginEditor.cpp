#include "PluginEditor.h"
#include "ui/SplitMeter.h"

namespace
{

const juce::Colour kBackground { 0xff101218 };
const juce::Colour kText { 0xfff3f5f8 };
const juce::Colour kDim { 0xff9aa3b5 };
const juce::Colour kButton { 0xff262b38 };
const juce::Colour kButtonOn { 0xff2f6d52 };
const juce::Colour kBypass { 0xffd48a20 };

struct Spec
{
    const char* title;
    const char* suffix;
    float minimum;
    float maximum;
    float step;
    float reset;
    int decimals;
    float youmix::PseSettings::* member;
    void (youmix::PseCore::* apply)(float);
};

const Spec kSpecs[6] = {
    { "Threshold", " dB", -80.0f, 0.0f, 0.5f, -40.0f, 1, &youmix::PseSettings::thresholdDb, &youmix::PseCore::setThresholdDb },
    { "Range", " dB", 0.0f, 60.0f, 0.5f, 18.0f, 1, &youmix::PseSettings::rangeDb, &youmix::PseCore::setRangeDb },
    { "Attack", " ms", 1.0f, 200.0f, 1.0f, 12.0f, 0, &youmix::PseSettings::attackMs, &youmix::PseCore::setAttackMs },
    { "Hang", " ms", 0.0f, 500.0f, 1.0f, 80.0f, 0, &youmix::PseSettings::hangMs, &youmix::PseCore::setHangMs },
    { "Release", " ms", 5.0f, 1000.0f, 1.0f, 140.0f, 0, &youmix::PseSettings::releaseMs, &youmix::PseCore::setReleaseMs },
    { "Knee", " dB", 0.0f, 24.0f, 0.5f, 12.0f, 1, &youmix::PseSettings::kneeDb, &youmix::PseCore::setKneeDb },
};

void styleButton(juce::TextButton& button)
{
    button.setColour(juce::TextButton::buttonColourId, kButton);
    button.setColour(juce::TextButton::buttonOnColourId, kButtonOn);
    button.setColour(juce::TextButton::textColourOffId, kText);
    button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
}

} // namespace

PseAudioProcessorEditor::PseAudioProcessorEditor(PseAudioProcessor& plugin)
    : AudioProcessorEditor(&plugin),
      processor_(plugin)
{
    setOpaque(true);
    styleButton(bypassButton_);
    bypassButton_.setTooltip("Turn this expander on or bypass it. It ducks bleed with a smooth curve. It does not hard-gate.");
    bypassButton_.onClick = [this]
    {
        auto& core = processor_.core();
        core.setBypass(! core.settings().bypass);
    };

    title_.setText("Left IN    Right GATE/GR", juce::dontSendNotification);
    title_.setJustificationType(juce::Justification::centredRight);
    title_.setFont(juce::Font(juce::FontOptions(13.0f).withStyle("Bold")));
    title_.setColour(juce::Label::textColourId, kText);
    title_.setInterceptsMouseClicks(false, false);

    for (int index = 0; index < 6; ++index)
    {
        const Spec& spec = kSpecs[index];
        auto& row = rows_[index];
        row.caption.setText(spec.title, juce::dontSendNotification);
        row.caption.setFont(juce::Font(juce::FontOptions(13.0f).withStyle("Bold")));
        row.caption.setColour(juce::Label::textColourId, kDim);
        row.caption.setJustificationType(juce::Justification::centredLeft);
        row.caption.setInterceptsMouseClicks(false, false);

        row.value.minimum = spec.minimum;
        row.value.maximum = spec.maximum;
        row.value.step = spec.step;
        row.value.decimals = spec.decimals;
        row.value.suffix = spec.suffix;
        row.value.textPt = 28.0f;
        row.value.read = [this, member = spec.member]
        {
            return processor_.core().settings().*member;
        };
        row.value.write = [this, apply = spec.apply](float value)
        {
            (processor_.core().*apply)(value);
        };
        row.value.resetTo = [reset = spec.reset] { return reset; };
        row.value.setTooltip("Drag the fader or click it to jump. Hold + or - to keep stepping; it speeds up while held. Double-click the number to type. Option-click resets. The scroll wheel does nothing.");

        addAndMakeVisible(row.caption);
        addAndMakeVisible(row.value);
    }

    addAndMakeVisible(bypassButton_);
    addAndMakeVisible(title_);
    setSize(760, 780);
    startTimerHz(30);
}

PseAudioProcessorEditor::~PseAudioProcessorEditor()
{
    stopTimer();
}

void PseAudioProcessorEditor::paint(juce::Graphics& graphics)
{
    graphics.fillAll(kBackground);
    youmix::paintSplitMeter(graphics, meterArea_.toFloat(), input_, reduction_);
}

void PseAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto top = area.removeFromTop(40);
    bypassButton_.setBounds(top.removeFromLeft(120));
    title_.setBounds(top);
    area.removeFromTop(12);
    meterArea_ = area.removeFromLeft(168).reduced(6, 0);
    area.removeFromLeft(16);

    const int count = 6;
    const int rowHeight = area.getHeight() / count;
    for (int index = 0; index < count; ++index)
    {
        auto line = area.removeFromTop(rowHeight).reduced(0, 4);
        rows_[index].caption.setBounds(line.removeFromTop(18));
        rows_[index].value.setBounds(line);
    }
}

void PseAudioProcessorEditor::timerCallback()
{
    const bool bypassed = processor_.core().settings().bypass;
    bypassButton_.setButtonText(bypassed ? "BYPASS" : "ON");
    bypassButton_.setColour(juce::TextButton::buttonColourId, bypassed ? kBypass : kButtonOn);
    input_ = processor_.core().inputRms();
    reduction_ = processor_.core().reductionDb();
    repaint();
}
