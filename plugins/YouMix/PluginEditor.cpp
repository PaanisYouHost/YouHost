#include "PluginEditor.h"
#include "ui/EditorFit.h"
#include "ui/SplitMeter.h"

#include <cmath>

namespace
{

const juce::Colour kBackground { 0xff101218 };
const juce::Colour kEdge { 0xff2c3242 };
const juce::Colour kText { 0xfff3f5f8 };
const juce::Colour kDim { 0xff9aa3b5 };
const juce::Colour kButton { 0xff262b38 };
const juce::Colour kButtonOn { 0xff2f6d52 };
const juce::Colour kBypass { 0xffd48a20 };
const juce::Colour kMine { 0xff6ee0aa };

constexpr int kMinWidth = 780;
constexpr int kMinHeight = 460;
constexpr int kPrefWidth = 1100;
constexpr int kPrefHeight = 620;

const char* kGroupNames[5] = { "Off", "A", "B", "C", "D" };

juce::String groupLabel(int group)
{
    if (group >= 1 && group <= 4)
        return kGroupNames[group];
    return "Off";
}

juce::String channelTitle(const char* raw, int slot)
{
    if (raw != nullptr && raw[0] != 0)
        return juce::String::fromUTF8(raw);
    return "Ch " + juce::String(slot + 1);
}

void styleButton(juce::TextButton& button)
{
    button.setColour(juce::TextButton::buttonColourId, kButton);
    button.setColour(juce::TextButton::buttonOnColourId, kButtonOn);
    button.setColour(juce::TextButton::textColourOffId, kText);
    button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
}

void styleCaption(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setFont(juce::Font(juce::FontOptions(13.0f).withStyle("Bold")));
    label.setColour(juce::Label::textColourId, kDim);
    label.setInterceptsMouseClicks(false, false);
}

void paintTouchButton(juce::Graphics& graphics, juce::Rectangle<int> bounds, const juce::String& text, bool on, bool alert)
{
    if (bounds.isEmpty())
        return;
    graphics.setColour(alert ? kBypass : (on ? kButtonOn : kButton));
    graphics.fillRoundedRectangle(bounds.toFloat(), 6.0f);
    graphics.setColour(juce::Colours::white);
    const float point = juce::jlimit(11.0f, 18.0f, static_cast<float>(bounds.getHeight()) * 0.42f);
    graphics.setFont(juce::Font(juce::FontOptions(point).withStyle("Bold")));
    graphics.drawFittedText(text, bounds, juce::Justification::centred, 1);
}

bool stillOnPreset(int preset, float value, float voiceValue, float singingValue)
{
    const float expected = preset == 2 ? singingValue : voiceValue;
    return (preset == 1 || preset == 2) && std::fabs(value - expected) <= 0.01f;
}

} // namespace

YouMixAudioProcessorEditor::YouMixAudioProcessorEditor(YouMixAudioProcessor& plugin)
    : AudioProcessorEditor(&plugin),
      processor_(plugin)
{
    setOpaque(true);
    styleButton(bypassButton_);
    styleButton(voiceButton_);
    styleButton(singingButton_);
    styleButton(advancedButton_);
    advancedButton_.setClickingTogglesState(true);
    bypassButton_.setTooltip("Turn the automixer on, or bypass every linked YouMix channel.");
    voiceButton_.setTooltip("Voice preset. Pink -32 dB, threshold -45 dB, fast attack and release.");
    singingButton_.setTooltip("Singing preset. Pink -58 dB, threshold -74 dB, slower release, so a quiet vocal can open.");
    advancedButton_.setTooltip("Open attack, threshold, and release. Leave this closed if the presets are enough.");
    bypassButton_.onClick = []
    {
        auto& core = youmix::youMixShared();
        core.setBypass(! core.settings().bypass);
    };
    voiceButton_.onClick = [] { youmix::youMixShared().applyPreset(1); };
    singingButton_.onClick = [] { youmix::youMixShared().applyPreset(2); };
    advancedButton_.onClick = [this]
    {
        advanced_ = advancedButton_.getToggleState();
        resized();
    };

    title_.setText("YouMix", juce::dontSendNotification);
    title_.setJustificationType(juce::Justification::centredRight);
    title_.setFont(juce::Font(juce::FontOptions(13.0f)));
    title_.setColour(juce::Label::textColourId, kDim);
    title_.setInterceptsMouseClicks(false, false);

    meterKey_.setText("Left IN    Right GATE/GR", juce::dontSendNotification);
    meterKey_.setJustificationType(juce::Justification::centredLeft);
    meterKey_.setFont(juce::Font(juce::FontOptions(13.0f).withStyle("Bold")));
    meterKey_.setColour(juce::Label::textColourId, kText);
    meterKey_.setInterceptsMouseClicks(false, false);

    for (int group = 0; group < youmix::kYouMixGroups; ++group)
    {
        styleCaption(pinkCaption_[group], "Pink " + groupLabel(group + 1));
        pink_[group].minimum = -60.0f;
        pink_[group].maximum = 0.0f;
        pink_[group].step = 0.5f;
        pink_[group].decimals = 1;
        pink_[group].suffix = " dB";
        pink_[group].textPt = 18.0f;
        pink_[group].read = [group]
        {
            return youmix::youMixShared().settings().pinkDb[group];
        };
        pink_[group].write = [group](float db)
        {
            auto& core = youmix::youMixShared();
            const bool keep = stillOnPreset(core.settings().preset, db, -32.0f, -58.0f);
            core.setPinkDb(group, db, ! keep);
        };
        pink_[group].resetTo = []
        {
            return youmix::youMixShared().settings().preset == 2 ? -58.0f : -32.0f;
        };
        pink_[group].setTooltip("Group sensitivity. This is the level of a silent virtual mic in the share. Drag the fader or click it. Hold + or - to repeat; it speeds up. Double-click the number to type. Option-click resets. The scroll wheel does nothing. Pink is never mixed into the output.");
    }

    const char* faderHelp = "Drag the fader or click it to jump. Hold + or - to keep stepping; it speeds up while held. Double-click the number to type. Option-click resets. The scroll wheel does nothing.";
    for (int slot = 0; slot < youmix::kYouMixSlots; ++slot)
    {
        weight_[slot].minimum = -12.0f;
        weight_[slot].maximum = 12.0f;
        weight_[slot].step = 0.5f;
        weight_[slot].decimals = 1;
        weight_[slot].suffix = " dB";
        weight_[slot].textPt = 14.0f;
        weight_[slot].read = [slot]
        {
            return youmix::youMixShared().settings().weightDb[slot];
        };
        weight_[slot].write = [slot](float db)
        {
            youmix::youMixShared().setWeightDb(slot, db);
        };
        weight_[slot].resetTo = [] { return 0.0f; };
        weight_[slot].setTooltip("Weight. Raises or lowers this mic's share inside its group. 0 dB is even. A quiet singer can be brought up until they open fully.");
        weight_[slot].setVisible(false);
        addAndMakeVisible(weight_[slot]);
    }

    styleCaption(attackCaption_, "Attack");
    styleCaption(releaseCaption_, "Release");
    styleCaption(thresholdCaption_, "Threshold");
    attack_.minimum = 1.0f;
    attack_.maximum = 500.0f;
    attack_.step = 1.0f;
    attack_.decimals = 0;
    attack_.suffix = " ms";
    attack_.textPt = 18.0f;
    attack_.read = [] { return youmix::youMixShared().settings().attackMs; };
    attack_.write = [](float ms)
    {
        auto& core = youmix::youMixShared();
        const bool keep = stillOnPreset(core.settings().preset, ms, 5.0f, 10.0f);
        core.setAttackMs(ms, ! keep);
    };
    attack_.resetTo = [] { return youmix::youMixShared().settings().preset == 2 ? 10.0f : 5.0f; };

    release_.minimum = 5.0f;
    release_.maximum = 2000.0f;
    release_.step = 1.0f;
    release_.decimals = 0;
    release_.suffix = " ms";
    release_.textPt = 18.0f;
    release_.read = [] { return youmix::youMixShared().settings().releaseMs; };
    release_.write = [](float ms)
    {
        auto& core = youmix::youMixShared();
        const bool keep = stillOnPreset(core.settings().preset, ms, 70.0f, 400.0f);
        core.setReleaseMs(ms, ! keep);
    };
    release_.resetTo = [] { return youmix::youMixShared().settings().preset == 2 ? 400.0f : 70.0f; };

    threshold_.minimum = -80.0f;
    threshold_.maximum = 0.0f;
    threshold_.step = 0.5f;
    threshold_.decimals = 1;
    threshold_.suffix = " dB";
    threshold_.textPt = 18.0f;
    threshold_.read = [] { return youmix::youMixShared().settings().thresholdDb; };
    threshold_.write = [](float db)
    {
        auto& core = youmix::youMixShared();
        const bool keep = stillOnPreset(core.settings().preset, db, -45.0f, -74.0f);
        core.setThresholdDb(db, ! keep);
    };
    threshold_.resetTo = [] { return youmix::youMixShared().settings().preset == 2 ? -74.0f : -45.0f; };
    attack_.setTooltip(faderHelp);
    release_.setTooltip(faderHelp);
    threshold_.setTooltip(juce::String(faderHelp) + " Channels quieter than this close.");

    addAndMakeVisible(bypassButton_);
    addAndMakeVisible(voiceButton_);
    addAndMakeVisible(singingButton_);
    addAndMakeVisible(advancedButton_);
    addAndMakeVisible(title_);
    addAndMakeVisible(meterKey_);
    for (int group = 0; group < youmix::kYouMixGroups; ++group)
    {
        addAndMakeVisible(pinkCaption_[group]);
        addAndMakeVisible(pink_[group]);
    }
    addAndMakeVisible(attackCaption_);
    addAndMakeVisible(releaseCaption_);
    addAndMakeVisible(thresholdCaption_);
    addAndMakeVisible(attack_);
    addAndMakeVisible(release_);
    addAndMakeVisible(threshold_);
    attackCaption_.setVisible(false);
    releaseCaption_.setVisible(false);
    thresholdCaption_.setVisible(false);
    attack_.setVisible(false);
    release_.setVisible(false);
    threshold_.setVisible(false);

    snapshot_ = youmix::youMixShared().settings();
    for (int slot = 0; slot < youmix::kYouMixSlots; ++slot)
        trackColour_[slot] = youmix::youMixShared().trackColour(slot);
    refreshButtons();
    if (processor_.slot() < 0)
    {
        bypassButton_.setVisible(false);
        voiceButton_.setVisible(false);
        singingButton_.setVisible(false);
        advancedButton_.setVisible(false);
        title_.setVisible(false);
        meterKey_.setVisible(false);
        for (int group = 0; group < youmix::kYouMixGroups; ++group)
        {
            pinkCaption_[group].setVisible(false);
            pink_[group].setVisible(false);
        }
    }

    setResizable(true, true);
    setResizeLimits(kMinWidth, kMinHeight, 2400, 1600);
    const auto size = youmix::clampEditorSize(processor_.editorWidth(), processor_.editorHeight(),
                                               kMinWidth, kMinHeight, kPrefWidth, kPrefHeight);
    setSize(size.width, size.height);
    startTimerHz(30);
}

YouMixAudioProcessorEditor::~YouMixAudioProcessorEditor()
{
    stopTimer();
}

void YouMixAudioProcessorEditor::fitOnce()
{
    if (fitted_ || ! isShowing())
        return;
    auto* top = getTopLevelComponent();
    if (top == nullptr || top->getPeer() == nullptr)
        return;
    fitted_ = true;
    juce::Component::SafePointer<YouMixAudioProcessorEditor> alive(this);
    juce::MessageManager::callAsync([alive]
    {
        if (alive != nullptr)
            youmix::fitTopLevelToDisplay(*alive);
    });
}

void YouMixAudioProcessorEditor::visibilityChanged()
{
    fitOnce();
}

void YouMixAudioProcessorEditor::parentHierarchyChanged()
{
    fitOnce();
}

void YouMixAudioProcessorEditor::refreshButtons()
{
    const bool bypassed = snapshot_.bypass;
    bypassButton_.setButtonText(bypassed ? "BYPASS" : "ON");
    bypassButton_.setColour(juce::TextButton::buttonColourId, bypassed ? kBypass : kButtonOn);
    voiceButton_.setColour(juce::TextButton::buttonColourId, snapshot_.preset == 1 ? kButtonOn : kButton);
    singingButton_.setColour(juce::TextButton::buttonColourId, snapshot_.preset == 2 ? kButtonOn : kButton);
}

void YouMixAudioProcessorEditor::layoutStrips()
{
    stripCount_ = 0;
    bool showWeight[youmix::kYouMixSlots] = {};

    int active[youmix::kYouMixSlots];
    int count = 0;
    for (int slot = 0; slot < youmix::kYouMixSlots; ++slot)
    {
        if (snapshot_.active[slot])
            active[count++] = slot;
    }
    if (count == 0 || stripArea_.isEmpty() || processor_.slot() < 0)
    {
        for (int slot = 0; slot < youmix::kYouMixSlots; ++slot)
        {
            if (weight_[slot].isVisible())
                weight_[slot].setVisible(false);
        }
        return;
    }

    constexpr int gap = 8;
    int width = (stripArea_.getWidth() - gap * (count - 1)) / juce::jmax(1, count);
    if (width > 168)
        width = 168;
    if (width < 36)
        width = juce::jmax(28, stripArea_.getWidth() / count);

    int x = stripArea_.getX();
    for (int index = 0; index < count; ++index)
    {
        Strip strip;
        strip.slot = active[index];
        const int stripWidth = juce::jmin(width, juce::jmax(0, stripArea_.getRight() - x));
        strip.bounds = { x, stripArea_.getY(), stripWidth, stripArea_.getHeight() };
        auto body = strip.bounds.reduced(3, 3);
        strip.swatch = body.removeFromTop(4);
        body.removeFromTop(3);
        const int titleH = juce::jmin(28, juce::jmax(18, body.getHeight() / 8));
        strip.title = body.removeFromTop(titleH);

        const int rest = juce::jmax(0, body.getHeight());
        int meterH = juce::jmax(28, rest / 5);
        int controls = rest - meterH;
        if (controls < 72 && rest > 0)
        {
            controls = rest * 3 / 4;
            meterH = rest - controls;
        }
        int groupBlock = controls * 50 / 100;
        int muteH = controls * 16 / 100;
        int weightH = controls - groupBlock - muteH;
        if (weightH < 0)
            weightH = 0;

        auto buttons = body.removeFromTop(groupBlock);
        const int rowHeight = buttons.getHeight() / 5;
        for (int group = 0; group < 4; ++group)
            strip.groupButton[group] = buttons.removeFromTop(rowHeight).reduced(2, 1);
        strip.groupButton[4] = buttons.reduced(2, 1);
        strip.mute = body.removeFromTop(muteH).reduced(2, 1);
        auto weightZone = body.removeFromTop(weightH).reduced(1, 1);
        if (weightZone.getHeight() >= 44)
            strip.weightLabel = weightZone.removeFromTop(14);
        weight_[strip.slot].textPt = weightZone.getHeight() >= 70 ? 14.0f : 12.0f;
        weight_[strip.slot].setBounds(weightZone);
        showWeight[strip.slot] = weightZone.getHeight() >= 28;
        strip.meter = body.reduced(1, 1);
        strips_[stripCount_++] = strip;
        x += stripWidth + gap;
    }

    for (int slot = 0; slot < youmix::kYouMixSlots; ++slot)
    {
        if (weight_[slot].isVisible() != showWeight[slot])
            weight_[slot].setVisible(showWeight[slot]);
    }
}

void YouMixAudioProcessorEditor::paint(juce::Graphics& graphics)
{
    graphics.fillAll(kBackground);
    if (processor_.slot() < 0)
    {
        graphics.setColour(kText);
        graphics.setFont(juce::Font(juce::FontOptions(15.0f)));
        graphics.drawText("All 16 YouMix channels are already open.", getLocalBounds().reduced(16), juce::Justification::centred, true);
        return;
    }

    for (int group = 0; group < youmix::kYouMixGroups; ++group)
    {
        const bool bypassed = snapshot_.pinkBypass[group];
        paintTouchButton(graphics, pinkBypassArea_[group], bypassed ? "BYPASS" : "PINK", ! bypassed, bypassed);
    }

    for (int index = 0; index < stripCount_; ++index)
    {
        const Strip& strip = strips_[index];
        const bool mine = strip.slot == processor_.slot();
        graphics.setColour(mine ? kMine : kEdge);
        graphics.drawRoundedRectangle(strip.bounds.toFloat().reduced(0.5f), 6.0f, mine ? 2.0f : 1.0f);

        const auto argb = trackColour_[strip.slot];
        if (argb != 0 && ! strip.swatch.isEmpty())
        {
            graphics.setColour(juce::Colour(argb));
            graphics.fillRoundedRectangle(strip.swatch.toFloat(), 2.0f);
        }

        const juce::String name = channelTitle(snapshot_.name[strip.slot], strip.slot);
        const float point = strip.bounds.getWidth() >= 110 ? 15.0f : 13.0f;
        graphics.setColour(kText);
        graphics.setFont(juce::Font(juce::FontOptions(point).withStyle("Bold")));
        graphics.drawText(name, strip.title, juce::Justification::centred, true);

        const int selected = snapshot_.group[strip.slot];
        for (int group = 0; group < 5; ++group)
            paintTouchButton(graphics, strip.groupButton[group], kGroupNames[group], selected == group, false);

        const bool muted = snapshot_.mode[strip.slot] == 1;
        paintTouchButton(graphics, strip.mute, muted ? "MUTE" : "AUTO", ! muted, muted);
        if (! strip.weightLabel.isEmpty())
        {
            graphics.setColour(kDim);
            graphics.setFont(juce::Font(juce::FontOptions(11.0f).withStyle("Bold")));
            graphics.drawText("Weight", strip.weightLabel, juce::Justification::centred, false);
        }
        youmix::paintSplitMeter(graphics, strip.meter.toFloat(), input_[strip.slot], reduction_[strip.slot]);
    }
}

void YouMixAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(10);
    auto top = area.removeFromTop(juce::jlimit(34, 40, area.getHeight() / 14));
    auto take = [&top](int preferred)
    {
        const int width = juce::jmin(preferred, juce::jmax(0, top.getWidth()));
        auto box = top.removeFromLeft(width);
        if (top.getWidth() > 4)
            top.removeFromLeft(juce::jmin(6, top.getWidth()));
        return box;
    };
    bypassButton_.setBounds(take(100));
    voiceButton_.setBounds(take(108));
    singingButton_.setBounds(take(124));
    advancedButton_.setBounds(take(128));
    title_.setBounds(top.removeFromRight(juce::jmin(72, juce::jmax(0, top.getWidth() / 5))));
    meterKey_.setBounds(top.reduced(6, 0));

    const bool showDetail = advanced_ && processor_.slot() >= 0;
    int below = juce::jmax(0, area.getHeight() - (showDetail ? 18 : 12));
    int pinkH = 132;
    int detailH = showDetail ? 112 : 0;
    constexpr int stripsMin = 180;
    while (pinkH + detailH + stripsMin > below && (pinkH > 96 || detailH > 96))
    {
        if (detailH > 96)
            detailH -= 4;
        else
            pinkH -= 4;
    }
    if (pinkH + detailH > below)
    {
        if (showDetail)
        {
            pinkH = juce::jmax(0, below / 2);
            detailH = juce::jmax(0, below - pinkH);
        }
        else
        {
            pinkH = below;
            detailH = 0;
        }
    }

    area.removeFromTop(juce::jmin(6, area.getHeight()));
    auto pinkRow = area.removeFromTop(juce::jmin(pinkH, area.getHeight()));
    constexpr int gap = 8;
    const int columnWidth = youmix::kYouMixGroups > 0
        ? (pinkRow.getWidth() - gap * (youmix::kYouMixGroups - 1)) / youmix::kYouMixGroups
        : 0;
    for (int group = 0; group < youmix::kYouMixGroups; ++group)
    {
        auto column = pinkRow.removeFromLeft(juce::jmax(0, columnWidth));
        pinkCaption_[group].setBounds(column.removeFromTop(juce::jmin(16, column.getHeight())));
        const int bypassH = juce::jlimit(26, 36, juce::jmax(26, column.getHeight() / 4));
        pinkBypassArea_[group] = column.removeFromBottom(juce::jmin(bypassH, column.getHeight())).reduced(2, 1);
        pink_[group].textPt = column.getHeight() >= 88 ? 18.0f : 14.0f;
        pink_[group].setBounds(column.reduced(0, 1));
        if (group + 1 < youmix::kYouMixGroups)
            pinkRow.removeFromLeft(juce::jmin(gap, pinkRow.getWidth()));
    }

    attackCaption_.setVisible(showDetail);
    releaseCaption_.setVisible(showDetail);
    thresholdCaption_.setVisible(showDetail);
    attack_.setVisible(showDetail);
    release_.setVisible(showDetail);
    threshold_.setVisible(showDetail);
    if (showDetail)
    {
        area.removeFromTop(juce::jmin(6, area.getHeight()));
        auto detail = area.removeFromTop(juce::jmin(detailH, area.getHeight()));
        const int each = detail.getWidth() / 3;
        auto attackColumn = detail.removeFromLeft(each).reduced(4, 0);
        auto releaseColumn = detail.removeFromLeft(each).reduced(4, 0);
        auto thresholdColumn = detail.reduced(4, 0);
        attackCaption_.setBounds(attackColumn.removeFromTop(juce::jmin(16, attackColumn.getHeight())));
        attack_.textPt = attackColumn.getHeight() >= 80 ? 18.0f : 14.0f;
        attack_.setBounds(attackColumn);
        releaseCaption_.setBounds(releaseColumn.removeFromTop(juce::jmin(16, releaseColumn.getHeight())));
        release_.textPt = attack_.textPt;
        release_.setBounds(releaseColumn);
        thresholdCaption_.setBounds(thresholdColumn.removeFromTop(juce::jmin(16, thresholdColumn.getHeight())));
        threshold_.textPt = attack_.textPt;
        threshold_.setBounds(thresholdColumn);
    }

    area.removeFromTop(juce::jmin(6, area.getHeight()));
    stripArea_ = area;
    layoutStrips();
    processor_.rememberEditorSize(getWidth(), getHeight());
}

void YouMixAudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    if (processor_.slot() < 0)
        return;

    for (int group = 0; group < youmix::kYouMixGroups; ++group)
    {
        if (pinkBypassArea_[group].contains(event.getPosition()))
        {
            auto& core = youmix::youMixShared();
            core.setPinkBypass(group, ! core.settings().pinkBypass[group]);
            return;
        }
    }

    for (int index = 0; index < stripCount_; ++index)
    {
        const Strip& strip = strips_[index];
        for (int group = 0; group < 5; ++group)
        {
            if (strip.groupButton[group].contains(event.getPosition()))
            {
                youmix::youMixShared().setGroup(strip.slot, group);
                return;
            }
        }
        if (strip.mute.contains(event.getPosition()))
        {
            const int mode = snapshot_.mode[strip.slot] == 1 ? 0 : 1;
            youmix::youMixShared().setMode(strip.slot, mode);
            return;
        }
    }
}

void YouMixAudioProcessorEditor::timerCallback()
{
    fitOnce();
    snapshot_ = youmix::youMixShared().settings();
    auto& core = youmix::youMixShared();
    for (int slot = 0; slot < youmix::kYouMixSlots; ++slot)
    {
        input_[slot] = core.inputRms(slot);
        reduction_[slot] = core.reductionDb(slot);
        trackColour_[slot] = core.trackColour(slot);
    }

    const bool usable = processor_.slot() >= 0;
    bypassButton_.setVisible(usable);
    voiceButton_.setVisible(usable);
    singingButton_.setVisible(usable);
    advancedButton_.setVisible(usable);
    title_.setVisible(usable);
    meterKey_.setVisible(usable);
    for (int group = 0; group < youmix::kYouMixGroups; ++group)
    {
        pinkCaption_[group].setVisible(usable);
        pink_[group].setVisible(usable);
    }

    refreshButtons();
    layoutStrips();
    repaint();
}
