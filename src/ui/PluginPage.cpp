#include "PluginPage.h"
#include "ChannelMenu.h"
#include "Theme.h"
#include "X32Look.h"
#include "engine/DisplayLayout.h"
#include "engine/MeterScale.h"

namespace youhost
{
namespace
{

juce::String slotLabel(int slot, const SlotSnapshot& snap)
{
    if (snap.loading)
        return juce::String(slot + 1) + " ...";
    if (! snap.occupied || snap.name.isEmpty())
        return juce::String(slot + 1);
    auto name = snap.name;
    if (name.length() > 22)
        name = name.substring(0, 22);
    return juce::String(slot + 1) + " " + name;
}

class PluginPicker : public juce::Component,
                     private juce::ListBoxModel
{
public:
    PluginPicker(AudioEngine& engine, std::function<void(const juce::PluginDescription&)> onChoose)
        : engine_(engine),
          onChoose_(std::move(onChoose))
    {
        types_ = engine_.pluginCatalogue().insertTypes();
        addAndMakeVisible(search_);
        addAndMakeVisible(list_);
        search_.setTextToShowWhenEmpty("Search plugins", theme::fainter);
        search_.onTextChange = [this] { rebuild(); };
        search_.onReturnKey = [this] { chooseRow(0); };
        list_.setModel(this);
        list_.setRowHeight(22);
        list_.setColour(juce::ListBox::backgroundColourId, theme::background);
        setSize(360, 320);
        rebuild();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        search_.setBounds(area.removeFromTop(28));
        area.removeFromTop(6);
        list_.setBounds(area);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(theme::panel);
    }

    int getNumRows() override { return static_cast<int>(shown_.size()); }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool selected) override
    {
        if (row < 0 || row >= getNumRows())
            return;
        graphics.setColour(selected ? theme::buttonOn : juce::Colours::transparentBlack);
        graphics.fillRect(0, 0, width, height);
        const auto& type = types_.getReference(shown_[static_cast<std::size_t>(row)]);
        juce::String line = type.name;
        if (type.pluginFormatName.isNotEmpty())
            line << "   " << type.pluginFormatName;
        graphics.setColour(theme::text);
        graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
        graphics.drawText(line, 8, 0, width - 12, height, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent&) override { chooseRow(row); }

private:
    void rebuild()
    {
        shown_.clear();
        const auto query = search_.getText().trim();
        for (int index = 0; index < types_.size(); ++index)
        {
            const auto& type = types_.getReference(index);
            if (query.isEmpty() || type.name.containsIgnoreCase(query) || type.manufacturerName.containsIgnoreCase(query)
                || type.pluginFormatName.containsIgnoreCase(query))
                shown_.push_back(index);
        }
        list_.updateContent();
        list_.repaint();
    }

    void chooseRow(int row)
    {
        if (row < 0 || row >= getNumRows() || onChoose_ == nullptr)
            return;
        const auto description = types_.getReference(shown_[static_cast<std::size_t>(row)]);
        onChoose_(description);
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
    }

    AudioEngine& engine_;
    std::function<void(const juce::PluginDescription&)> onChoose_;
    juce::Array<juce::PluginDescription> types_;
    std::vector<int> shown_;
    juce::TextEditor search_;
    juce::ListBox list_;
};

class SlotButton : public juce::TextButton
{
public:
    std::function<void()> onMenu;

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            if (onMenu != nullptr)
                onMenu();
            return;
        }
        juce::TextButton::mouseDown(event);
    }
};

} // namespace

class PluginPage::GroupHeader : public juce::Component
{
public:
    GroupHeader(AudioEngine& engine, int group)
        : engine_(engine),
          group_(group)
    {
    }

    void refresh()
    {
        repaint();
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            showGroupMenu(engine_, *this, group_);
            return;
        }
        engine_.toggleGroupCollapsed(group_);
    }

    void paint(juce::Graphics& graphics) override
    {
        const int color = engine_.groupColor(group_);
        graphics.setColour(x32Fill(color));
        graphics.fillRect(getLocalBounds());
        graphics.setColour(x32Ink(color));
        if (kX32Colours[normaliseX32Colour(color)].inverted)
            graphics.fillRect(0, 0, 4, getHeight());

        const int channels = std::max(0, engine_.visibleChannels());
        float level = 0.0f;
        bool clip = false;
        bool plugins = false;
        int on = 0;
        int count = 0;
        for (int channel = 0; channel < channels; ++channel)
        {
            if (engine_.channelGroup(channel) != group_)
                continue;
            ++count;
            if (engine_.isRecordArmed(channel))
                ++on;
            level = std::max(level, showPeak_ ? engine_.peakFor(channel) : engine_.rmsFor(channel));
            clip = clip || engine_.clipFor(channel);
            if (! plugins)
            {
                const auto snap = engine_.channelSnapshot(channel);
                for (const auto& slot : snap.slots)
                    plugins = plugins || slot.occupied;
            }
        }

        auto area = getLocalBounds().reduced(10, 4);
        graphics.setColour(x32Ink(color));
        graphics.setFont(juce::Font(juce::FontOptions(14.0f).withStyle("Bold")));
        auto title = engine_.groupName(group_);
        if (engine_.groupCollapsed(group_))
            title << "    folded";
        graphics.drawText(title, area.removeFromLeft(std::min(220, area.getWidth() / 3)), juce::Justification::centredLeft, true);

        juce::String state = "OFF";
        if (count > 0 && on == count)
            state = "REC";
        else if (on > 0)
            state = juce::String(on) + " on";
        if (clip)
            state << "   CLIP";
        if (plugins)
            state << "   FX";
        graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
        graphics.drawText(state, area.removeFromRight(180), juce::Justification::centredRight, true);

        auto meter = area.reduced(8, 6);
        graphics.setColour(theme::meterTrack);
        graphics.fillRoundedRectangle(meter.toFloat(), 2.0f);
        if (level > 0.0f && meter.getWidth() > 1)
        {
            const MeterSpan span = showPeak_ ? peakMeterSpan() : rmsMeterSpan(referenceDb_);
            const float filled = normaliseDb(gainToDb(level), span) * static_cast<float>(meter.getWidth());
            graphics.setColour(gainToDb(level) >= -6.0f ? theme::red : gainToDb(level) >= -18.0f ? theme::amber : theme::green);
            graphics.fillRect(meter.getX(), meter.getY(), static_cast<int>(filled), meter.getHeight());
        }
    }

    void setMeterMode(bool peak, int referenceDb)
    {
        showPeak_ = peak;
        referenceDb_ = referenceDb;
    }

private:
    AudioEngine& engine_;
    int group_ = 0;
    bool showPeak_ = false;
    int referenceDb_ = kDefaultRmsReferenceDb;
};

class PluginPage::Row : public juce::Component
{
public:
    Row(AudioEngine& engine, int channel)
        : engine_(engine),
          channel_(channel)
    {
        addAndMakeVisible(number_);
        addAndMakeVisible(name_);
        addAndMakeVisible(arm_);
        addAndMakeVisible(exclude_);
        number_.setInterceptsMouseClicks(false, false);
        number_.setFont(juce::Font(juce::FontOptions(13.0f)));
        number_.setJustificationType(juce::Justification::centred);
        name_.setEditable(false, true, false);
        name_.setFont(juce::Font(juce::FontOptions(13.0f)));
        name_.setJustificationType(juce::Justification::centredLeft);
        name_.setTooltip("Double-click to rename. Right-click for color and group. The name is used in the WAV file name.");
        name_.onTextChange = [this]
        {
            if (! updating_)
                engine_.setChannelName(channel_, name_.getText());
        };
        name_.addMouseListener(this, false);

        arm_.setMouseClickGrabsKeyboardFocus(false);
        exclude_.setMouseClickGrabsKeyboardFocus(false);
        arm_.setTooltip("Channel on. Meter, plugins, and output are live, and the next take records this channel. Click for OFF.");
        exclude_.setButtonText("Ex");
        exclude_.setTooltip("Exclude from alignment. This channel stays undelayed and does not move the others.");
        arm_.onClick = [this] { engine_.setRecordArmed(channel_, ! engine_.isRecordArmed(channel_)); };
        exclude_.onClick = [this]
        {
            const bool excluded = engine_.channelSnapshot(channel_).excluded;
            engine_.setChannelExcluded(channel_, ! excluded);
        };

        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            auto& button = slots_[static_cast<std::size_t>(slot)];
            addAndMakeVisible(button);
            button.setMouseClickGrabsKeyboardFocus(false);
            button.onClick = [this, slot] { slotClicked(slot); };
            button.onMenu = [this, slot] { showMenu(slot); };
        }
    }

    void setMeterMode(bool peak, int referenceDb)
    {
        showPeak_ = peak;
        referenceDb_ = referenceDb;
    }

    void refresh()
    {
        const auto name = engine_.channelName(channel_);
        if (! name_.isBeingEdited() && name_.getText() != name)
        {
            updating_ = true;
            name_.setText(name, juce::dontSendNotification);
            updating_ = false;
        }
        number_.setText(juce::String(channel_ + 1), juce::dontSendNotification);

        const bool armed = engine_.isRecordArmed(channel_);
        arm_.setButtonText(armed ? "REC" : "OFF");
        arm_.setTooltip(armed
                            ? "Channel on. Meter, plugins, and output are live, and the next take records this channel. Click for OFF."
                            : "Channel off. The meter stays still, plugins are skipped, and the output is silent. This take's files stay as they were. Click for REC.");
        arm_.setColour(juce::TextButton::buttonColourId, armed ? juce::Colour(0xff8d2430) : theme::button);
        arm_.setColour(juce::TextButton::textColourOffId, armed ? juce::Colours::white : theme::fainter);
        const bool excluded = engine_.channelSnapshot(channel_).excluded;
        exclude_.setColour(juce::TextButton::buttonColourId, excluded ? theme::buttonOn : theme::button);

        const auto snap = engine_.channelSnapshot(channel_);
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            const auto& source = snap.slots[static_cast<std::size_t>(slot)];
            auto& button = slots_[static_cast<std::size_t>(slot)];
            const auto text = slotLabel(slot, source);
            if (button.getButtonText() != text)
                button.setButtonText(text);
            juce::Colour fill = theme::button;
            if (source.occupied && source.bypassed)
                fill = juce::Colour(0xff3a3424);
            else if (source.occupied)
                fill = theme::buttonOn;
            if (source.loading)
                fill = juce::Colour(0xff3d3420);
            if (engine_.isPluginEditorOpen(channel_, slot))
                fill = fill.brighter(0.2f);
            button.setColour(juce::TextButton::buttonColourId, fill);
            button.setColour(juce::TextButton::textColourOffId, source.bypassed ? theme::dim : theme::text);
            button.setTooltip(source.error.isNotEmpty()
                                  ? source.error
                                  : (source.bypassed ? "Bypassed. Right-click for bypass and remove."
                                                     : "Right-click for bypass and remove."));
        }
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4, 2);
        area.removeFromLeft(6);
        number_.setBounds(area.removeFromLeft(32));
        arm_.setBounds(area.removeFromLeft(46).reduced(2, 4));
        name_.setBounds(area.removeFromLeft(128));
        meterArea_ = area.removeFromLeft(18).reduced(3, 3);
        exclude_.setBounds(area.removeFromLeft(34).reduced(2, 4));
        area.removeFromLeft(4);
        const int slotWidth = std::min(128, std::max(64, area.getWidth() / kSlotsPerChannel));
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
            slots_[static_cast<std::size_t>(slot)].setBounds(area.removeFromLeft(slotWidth).reduced(2, 2));
    }

    void paint(juce::Graphics& graphics) override
    {
        const int color = engine_.channelColor(channel_);
        graphics.setColour(x32Fill(color));
        graphics.fillRect(0, 0, 6, getHeight());
        if (kX32Colours[normaliseX32Colour(color)].inverted)
        {
            graphics.setColour(x32Ink(color));
            graphics.fillRect(0, 0, 3, getHeight());
        }

        if (engine_.isChannelSelected(channel_))
        {
            graphics.setColour(theme::text.withAlpha(0.85f));
            graphics.drawRect(getLocalBounds(), 1);
        }

        graphics.setColour(theme::panelEdge.withAlpha(0.35f));
        graphics.fillRect(0, getHeight() - 1, getWidth(), 1);

        const bool on = engine_.isRecordArmed(channel_);
        graphics.setColour(on ? theme::meterTrack : theme::panelEdge.withAlpha(0.45f));
        graphics.fillRoundedRectangle(meterArea_.toFloat(), 2.0f);
        const float level = showPeak_ ? engine_.peakFor(channel_) : engine_.rmsFor(channel_);
        const MeterSpan span = showPeak_ ? peakMeterSpan() : rmsMeterSpan(referenceDb_);
        if (on && level > 0.0f && meterArea_.getHeight() > 1)
        {
            const float filled = normaliseDb(gainToDb(level), span) * static_cast<float>(meterArea_.getHeight());
            auto bar = meterArea_.toFloat();
            bar.setTop(bar.getBottom() - filled);
            const float db = gainToDb(level);
            graphics.setColour(db >= -6.0f ? theme::red : db >= -18.0f ? theme::amber : theme::green);
            graphics.fillRoundedRectangle(bar, 2.0f);
        }
        graphics.setColour(! on ? theme::panelEdge : engine_.clipFor(channel_) ? theme::red : theme::panelEdge);
        graphics.fillRect(meterArea_.getX(), meterArea_.getY(), meterArea_.getWidth(), 3);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            showChannelMenu(engine_, *this, channel_);
            return;
        }
        if (event.mods.isShiftDown())
        {
            engine_.selectChannel(channel_, true);
            return;
        }
        if (event.eventComponent == this && meterArea_.contains(event.getPosition()) && engine_.clipFor(channel_))
            engine_.requestClipClear(channel_);
        else if (event.eventComponent == this)
            engine_.selectChannel(channel_, false);
    }

private:
    void slotClicked(int slot)
    {
        const auto snap = engine_.channelSnapshot(channel_).slots[static_cast<std::size_t>(slot)];
        if (snap.loading)
            return;
        if (snap.occupied)
        {
            engine_.togglePluginEditor(channel_, slot);
            return;
        }

        auto* picker = new PluginPicker(engine_, [this, slot](const juce::PluginDescription& description)
        {
            engine_.loadPlugin(channel_, slot, description, true);
        });
        juce::CallOutBox::launchAsynchronously(std::unique_ptr<juce::Component>(picker),
                                               slots_[static_cast<std::size_t>(slot)].getScreenBounds(),
                                               nullptr);
    }

    void showMenu(int slot)
    {
        const auto snap = engine_.channelSnapshot(channel_).slots[static_cast<std::size_t>(slot)];
        if (! snap.occupied && ! snap.loading)
            return;

        juce::PopupMenu menu;
        menu.addItem(1, "Open");
        menu.addItem(2, snap.bypassed ? "Turn bypass off" : "Bypass");
        menu.addItem(3, "Remove");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&slots_[static_cast<std::size_t>(slot)]),
                           [this, slot](int result)
                           {
                               const auto current = engine_.channelSnapshot(channel_).slots[static_cast<std::size_t>(slot)];
                               if (result == 1)
                                   engine_.openPluginEditor(channel_, slot);
                               else if (result == 2)
                                   engine_.setSlotBypassed(channel_, slot, ! current.bypassed);
                               else if (result == 3)
                                   engine_.removePlugin(channel_, slot);
                           });
    }

    AudioEngine& engine_;
    int channel_ = 0;
    bool showPeak_ = false;
    int referenceDb_ = kDefaultRmsReferenceDb;
    bool updating_ = false;
    juce::Label number_;
    juce::Label name_;
    juce::TextButton arm_;
    juce::TextButton exclude_;
    std::array<SlotButton, kSlotsPerChannel> slots_;
    juce::Rectangle<int> meterArea_;
};

PluginPage::PluginPage(AudioEngine& engine)
    : engine_(engine)
{
    addAndMakeVisible(viewport_);
    empty_.setText("No input channels are open. Open Audio setup and enable the inputs.", juce::dontSendNotification);
    empty_.setJustificationType(juce::Justification::centred);
    empty_.setColour(juce::Label::textColourId, theme::dim);
    content_.addAndMakeVisible(empty_);
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
}

PluginPage::~PluginPage()
{
    order_.clear();
    rows_.clear();
    headers_.clear();
    viewport_.setViewedComponent(nullptr, false);
}

void PluginPage::setMeterMode(bool peak, int referenceDb)
{
    showPeak_ = peak;
    referenceDb_ = referenceDb;
    for (auto& row : rows_)
        row->setMeterMode(showPeak_, referenceDb_);
    for (auto& header : headers_)
        header->setMeterMode(showPeak_, referenceDb_);
}

void PluginPage::rebuild()
{
    const int channels = std::max(0, engine_.visibleChannels());
    channels_ = channels;
    revision_ = engine_.displayRevision();
    order_.clear();
    rows_.clear();
    headers_.clear();
    heights_.clear();

    const auto strips = engine_.displayStrips(channels);
    for (const auto& item : strips)
    {
        if (item.kind == StripKind::groupHeader)
        {
            auto header = std::make_unique<GroupHeader>(engine_, item.group);
            header->setMeterMode(showPeak_, referenceDb_);
            content_.addAndMakeVisible(*header);
            order_.push_back(header.get());
            heights_.push_back(34);
            headers_.push_back(std::move(header));
        }
        else
        {
            auto row = std::make_unique<Row>(engine_, item.channel);
            row->setMeterMode(showPeak_, referenceDb_);
            content_.addAndMakeVisible(*row);
            order_.push_back(row.get());
            heights_.push_back(40);
            rows_.push_back(std::move(row));
        }
    }
    resized();
}

void PluginPage::refresh()
{
    const int channels = std::max(0, engine_.visibleChannels());
    if (channels != channels_ || engine_.displayRevision() != revision_)
        rebuild();

    empty_.setVisible(channels_ == 0);
    for (auto& row : rows_)
        row->refresh();
    for (auto& header : headers_)
        header->refresh();
}

void PluginPage::resized()
{
    viewport_.setBounds(getLocalBounds());
    int height = 0;
    for (int rowHeight : heights_)
        height += rowHeight;
    content_.setSize(std::max(1, viewport_.getMaximumVisibleWidth()), std::max(getHeight(), height));
    empty_.setBounds(content_.getLocalBounds().reduced(24));
    auto area = content_.getLocalBounds();
    for (std::size_t index = 0; index < order_.size(); ++index)
        order_[index]->setBounds(area.removeFromTop(heights_[index]));
}

} // namespace youhost
