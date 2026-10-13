#include "PluginEditor.h"
#include "ui/SplitMeter.h"

#include <cmath>

namespace
{

const juce::Colour kBackground { 0xff101218 };
const juce::Colour kEdge { 0xff2c3242 };
const juce::Colour kText { 0xfff3f5f8 };
const juce::Colour kDim { 0xff9aa3b5 };
const juce::Colour kGreen { 0xff6ee0aa };
const juce::Colour kButton { 0xff262b38 };
const juce::Colour kButtonOn { 0xff2f6d52 };
const juce::Colour kBypass { 0xffd48a20 };

juce::String groupLabel(int group)
{
    switch (group)
    {
        case 1: return "A";
        case 2: return "B";
        case 3: return "C";
        case 4: return "D";
        default: return "Off";
    }
}

void styleButton(juce::TextButton& button)
{
    button.setColour(juce::TextButton::buttonColourId, kButton);
    button.setColour(juce::TextButton::buttonOnColourId, kButtonOn);
    button.setColour(juce::TextButton::textColourOffId, kText);
    button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
}

void paintChip(juce::Graphics& graphics, juce::Rectangle<int> bounds, const juce::String& text, bool alert)
{
    graphics.setColour(alert ? kBypass : kButton);
    graphics.fillRoundedRectangle(bounds.toFloat(), 3.0f);
    graphics.setColour(juce::Colours::white);
    graphics.setFont(juce::Font(juce::FontOptions(11.0f).withStyle("Bold")));
    graphics.drawText(text, bounds, juce::Justification::centred, false);
}

} // namespace

YouMixAudioProcessorEditor::YouMixAudioProcessorEditor(YouMixAudioProcessor& plugin)
    : AudioProcessorEditor(&plugin),
      processor_(plugin)
{
    setOpaque(true);
    styleButton(bypassButton_);
    styleButton(advancedButton_);
    styleButton(speechButton_);
    styleButton(singingButton_);
    advancedButton_.setClickingTogglesState(true);
    advancedButton_.setTooltip("Show presets and per-channel auto, mute, and group.");
    bypassButton_.setTooltip("Turn the automixer on, or bypass every linked YouMix channel.");
    bypassButton_.onClick = []
    {
        auto& core = youmix::youMixShared();
        core.setBypass(! core.settings().bypass);
    };
    advancedButton_.onClick = [this]
    {
        advanced_ = advancedButton_.getToggleState();
        speechButton_.setVisible(advanced_);
        singingButton_.setVisible(advanced_);
        layoutStrips();
        repaint();
    };
    speechButton_.onClick = [] { youmix::youMixShared().applyPreset(1); };
    singingButton_.onClick = [] { youmix::youMixShared().applyPreset(2); };
    speechButton_.setTooltip("Speech preset. Pink -32 dB, faster attack and release.");
    singingButton_.setTooltip("Singing preset. Pink -44 dB, slower attack and release.");

    title_.setText("YouMix", juce::dontSendNotification);
    title_.setJustificationType(juce::Justification::centredRight);
    title_.setFont(juce::Font(juce::FontOptions(13.0f)));
    title_.setColour(juce::Label::textColourId, kDim);
    title_.setInterceptsMouseClicks(false, false);

    for (int group = 0; group < youmix::kYouMixGroups; ++group)
    {
        pinkCaption_[group].setText("Pink " + groupLabel(group + 1), juce::dontSendNotification);
        pinkCaption_[group].setJustificationType(juce::Justification::centred);
        pinkCaption_[group].setFont(juce::Font(juce::FontOptions(12.0f)));
        pinkCaption_[group].setColour(juce::Label::textColourId, kDim);
        pinkCaption_[group].setInterceptsMouseClicks(false, false);

        pink_[group].minimum = -60.0f;
        pink_[group].maximum = 0.0f;
        pink_[group].step = 0.5f;
        pink_[group].decimals = 1;
        pink_[group].suffix = " dB";
        pink_[group].read = [group]
        {
            return youmix::youMixShared().settings().pinkDb[group];
        };
        pink_[group].write = [group](float db)
        {
            auto& core = youmix::youMixShared();
            const int preset = core.settings().preset;
            const float expected = preset == 2 ? -44.0f : -32.0f;
            const bool stillPreset = (preset == 1 || preset == 2) && std::fabs(db - expected) <= 0.01f;
            core.setPinkDb(group, db, ! stillPreset);
        };
        pink_[group].resetTo = []
        {
            return youmix::youMixShared().settings().preset == 2 ? -44.0f : -32.0f;
        };
        pink_[group].setTooltip("Pink-noise sensitivity. Drag for 0.5 dB steps. Double-click to type. Option-click resets. The scroll wheel does nothing. Pink is never mixed into the output.");
    }

    addAndMakeVisible(bypassButton_);
    addAndMakeVisible(advancedButton_);
    addAndMakeVisible(speechButton_);
    addAndMakeVisible(singingButton_);
    addAndMakeVisible(title_);
    speechButton_.setVisible(false);
    singingButton_.setVisible(false);
    for (int group = 0; group < youmix::kYouMixGroups; ++group)
    {
        addAndMakeVisible(pinkCaption_[group]);
        addAndMakeVisible(pink_[group]);
    }

    snapshot_ = youmix::youMixShared().settings();
    refreshButtons();
    if (processor_.slot() < 0)
    {
        bypassButton_.setVisible(false);
        advancedButton_.setVisible(false);
        speechButton_.setVisible(false);
        singingButton_.setVisible(false);
        title_.setVisible(false);
        for (int group = 0; group < youmix::kYouMixGroups; ++group)
        {
            pinkCaption_[group].setVisible(false);
            pink_[group].setVisible(false);
        }
    }
    setSize(1060, 560);
    startTimerHz(30);
}

YouMixAudioProcessorEditor::~YouMixAudioProcessorEditor()
{
    stopTimer();
}

void YouMixAudioProcessorEditor::refreshButtons()
{
    const bool bypassed = snapshot_.bypass;
    bypassButton_.setButtonText(bypassed ? "BYPASS" : "ON");
    bypassButton_.setColour(juce::TextButton::buttonColourId, bypassed ? kBypass : kButtonOn);
    speechButton_.setColour(juce::TextButton::buttonColourId, snapshot_.preset == 1 ? kButtonOn : kButton);
    singingButton_.setColour(juce::TextButton::buttonColourId, snapshot_.preset == 2 ? kButtonOn : kButton);
}

void YouMixAudioProcessorEditor::layoutStrips()
{
    stripCount_ = 0;
    int active[youmix::kYouMixSlots];
    int count = 0;
    for (int slot = 0; slot < youmix::kYouMixSlots; ++slot)
    {
        if (snapshot_.active[slot])
            active[count++] = slot;
    }
    if (count == 0 || stripArea_.isEmpty())
        return;

    constexpr int gap = 6;
    int width = (stripArea_.getWidth() - gap * (count - 1)) / count;
    if (width > 118)
        width = 118;

    int x = stripArea_.getX();
    for (int index = 0; index < count; ++index)
    {
        Strip strip;
        strip.slot = active[index];
        strip.bounds = { x, stripArea_.getY(), width, stripArea_.getHeight() };
        auto body = strip.bounds.reduced(4, 4);
        body.removeFromTop(18);
        body.removeFromTop(16);
        body.removeFromTop(16);
        if (advanced_)
        {
            auto buttons = body.removeFromBottom(56);
            strip.mute = buttons.removeFromTop(26).reduced(1, 1);
            strip.group = buttons.removeFromTop(26).reduced(1, 1);
        }
        strip.meter = body.reduced(2, 2);
        strips_[stripCount_++] = strip;
        x += width + gap;
    }
}

void YouMixAudioProcessorEditor::paint(juce::Graphics& graphics)
{
    graphics.fillAll(kBackground);
    if (processor_.slot() < 0)
    {
        graphics.setColour(kText);
        graphics.setFont(juce::Font(juce::FontOptions(15.0f)));
        graphics.drawText("All 16 YouMix channels are already open.", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    for (int index = 0; index < stripCount_; ++index)
    {
        const Strip& strip = strips_[index];
        const bool mine = strip.slot == processor_.slot();
        graphics.setColour(mine ? kGreen : kEdge);
        graphics.drawRoundedRectangle(strip.bounds.toFloat().reduced(0.5f), 4.0f, mine ? 2.0f : 1.0f);

        auto text = strip.bounds.reduced(4, 4);
        graphics.setColour(kText);
        graphics.setFont(juce::Font(juce::FontOptions(15.0f).withStyle("Bold")));
        graphics.drawText(juce::String(strip.slot + 1), text.removeFromTop(18), juce::Justification::centred, false);

        graphics.setColour(kDim);
        graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
        const char* raw = snapshot_.name[strip.slot];
        const juce::String name = raw[0] == 0
            ? "Channel " + juce::String(strip.slot + 1)
            : juce::String::fromUTF8(raw);
        graphics.drawText(name, text.removeFromTop(16), juce::Justification::centred, true);

        graphics.setColour(kText);
        graphics.setFont(juce::Font(juce::FontOptions(13.0f).withStyle("Bold")));
        graphics.drawText(groupLabel(snapshot_.group[strip.slot]), text.removeFromTop(16), juce::Justification::centred, false);

        youmix::paintSplitMeter(graphics, strip.meter.toFloat(), input_[strip.slot], reduction_[strip.slot]);
        if (advanced_)
        {
            const bool muted = snapshot_.mode[strip.slot] == 1;
            paintChip(graphics, strip.mute, muted ? "MUTE" : "AUTO", muted);
            paintChip(graphics, strip.group, groupLabel(snapshot_.group[strip.slot]), false);
        }
    }
}

void YouMixAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto top = area.removeFromTop(34);
    bypassButton_.setBounds(top.removeFromLeft(110));
    top.removeFromLeft(8);
    advancedButton_.setBounds(top.removeFromLeft(120));
    top.removeFromLeft(16);
    speechButton_.setBounds(top.removeFromLeft(100));
    top.removeFromLeft(8);
    singingButton_.setBounds(top.removeFromLeft(110));
    title_.setBounds(top);

    area.removeFromTop(12);
    auto pinkRow = area.removeFromTop(64);
    for (int group = 0; group < youmix::kYouMixGroups; ++group)
    {
        auto column = pinkRow.removeFromLeft(156);
        pinkCaption_[group].setBounds(column.removeFromTop(16));
        pink_[group].setBounds(column.removeFromTop(40));
        pinkRow.removeFromLeft(8);
    }

    area.removeFromTop(12);
    stripArea_ = area;
    layoutStrips();
}

void YouMixAudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    if (! advanced_ || processor_.slot() < 0)
        return;

    for (int index = 0; index < stripCount_; ++index)
    {
        const Strip& strip = strips_[index];
        if (strip.mute.contains(event.getPosition()))
        {
            const int mode = snapshot_.mode[strip.slot] == 1 ? 0 : 1;
            youmix::youMixShared().setMode(strip.slot, mode);
            return;
        }
        if (strip.group.contains(event.getPosition()))
        {
            int group = snapshot_.group[strip.slot] + 1;
            if (group > youmix::kYouMixGroups)
                group = 0;
            youmix::youMixShared().setGroup(strip.slot, group);
            return;
        }
    }
}

void YouMixAudioProcessorEditor::timerCallback()
{
    snapshot_ = youmix::youMixShared().settings();
    auto& core = youmix::youMixShared();
    for (int slot = 0; slot < youmix::kYouMixSlots; ++slot)
    {
        input_[slot] = core.inputRms(slot);
        reduction_[slot] = core.reductionDb(slot);
    }

    const bool usable = processor_.slot() >= 0;
    bypassButton_.setVisible(usable);
    advancedButton_.setVisible(usable);
    title_.setVisible(usable);
    speechButton_.setVisible(usable && advanced_);
    singingButton_.setVisible(usable && advanced_);
    for (int group = 0; group < youmix::kYouMixGroups; ++group)
    {
        pinkCaption_[group].setVisible(usable);
        pink_[group].setVisible(usable);
    }

    refreshButtons();
    layoutStrips();
    repaint();
}
