#include "PluginPage.h"
#include "Theme.h"
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
    if (name.length() > 28)
        name = name.substring(0, 28);
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
        types_ = engine_.pluginCatalogue().types();
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
            line << "  " << type.pluginFormatName;
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
        number_.setText(juce::String(channel + 1).paddedLeft('0', 2), juce::dontSendNotification);
        number_.setFont(juce::Font(juce::FontOptions(14.0f)));
        number_.setJustificationType(juce::Justification::centred);
        name_.setEditable(false, true, false);
        name_.setFont(juce::Font(juce::FontOptions(14.0f)));
        name_.setJustificationType(juce::Justification::centredLeft);
        name_.setTooltip("Double-click to name the channel. The name is used in the WAV filename.");
        name_.onTextChange = [this]
        {
            if (! updating_)
                engine_.setChannelName(channel_, name_.getText());
        };

        arm_.setButtonText("R");
        arm_.setTooltip("Record this channel");
        exclude_.setButtonText("Ex");
        exclude_.setTooltip("Exclude this channel from latency compensation");
        arm_.setMouseClickGrabsKeyboardFocus(false);
        exclude_.setMouseClickGrabsKeyboardFocus(false);
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

        const bool armed = engine_.isRecordArmed(channel_);
        arm_.setColour(juce::TextButton::buttonColourId, armed ? juce::Colour(0xff8d2430) : theme::button);
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
            button.setTooltip(source.error.isNotEmpty() ? source.error : (source.bypassed ? "Bypassed. Right-click for bypass and remove." : "Right-click for bypass and remove."));
        }
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4, 3);
        number_.setBounds(area.removeFromLeft(36));
        arm_.setBounds(area.removeFromLeft(28).reduced(2, 4));
        name_.setBounds(area.removeFromLeft(150));
        meterArea_ = area.removeFromLeft(22).reduced(4, 2);
        exclude_.setBounds(area.removeFromLeft(36).reduced(2, 4));
        area.removeFromLeft(6);
        const int slotWidth = area.getWidth() / kSlotsPerChannel;
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            auto slice = slot == kSlotsPerChannel - 1 ? area : area.removeFromLeft(slotWidth);
            slots_[static_cast<std::size_t>(slot)].setBounds(slice.reduced(3, 2));
        }
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.setColour(theme::panelEdge.withAlpha(0.35f));
        graphics.fillRect(0, getHeight() - 1, getWidth(), 1);

        graphics.setColour(theme::meterTrack);
        graphics.fillRoundedRectangle(meterArea_.toFloat(), 2.0f);
        const float level = showPeak_ ? engine_.peakFor(channel_) : engine_.rmsFor(channel_);
        const MeterSpan span = showPeak_ ? peakMeterSpan() : rmsMeterSpan(referenceDb_);
        if (level > 0.0f && meterArea_.getHeight() > 1)
        {
            const float filled = normaliseDb(gainToDb(level), span) * static_cast<float>(meterArea_.getHeight());
            auto bar = meterArea_.toFloat();
            bar.setTop(bar.getBottom() - filled);
            const float db = gainToDb(level);
            graphics.setColour(db >= -6.0f ? theme::red : db >= -18.0f ? theme::amber : theme::green);
            graphics.fillRoundedRectangle(bar, 2.0f);
        }
        graphics.setColour(engine_.clipFor(channel_) ? theme::red : theme::panelEdge);
        graphics.fillRect(meterArea_.getX(), meterArea_.getY(), meterArea_.getWidth(), 3);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (meterArea_.contains(event.getPosition()) && engine_.clipFor(channel_))
            engine_.requestClipClear(channel_);
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
        menu.addItem(2, snap.bypassed ? "Bypass off" : "Bypass");
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
    empty_.setText("No input channels open. Open Audio setup and enable the inputs.", juce::dontSendNotification);
    empty_.setJustificationType(juce::Justification::centred);
    empty_.setColour(juce::Label::textColourId, theme::dim);
    content_.addAndMakeVisible(empty_);
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
}

PluginPage::~PluginPage()
{
    rows_.clear();
    viewport_.setViewedComponent(nullptr, false);
}

void PluginPage::setMeterMode(bool peak, int referenceDb)
{
    showPeak_ = peak;
    referenceDb_ = referenceDb;
    for (auto& row : rows_)
        row->setMeterMode(showPeak_, referenceDb_);
}

void PluginPage::refresh()
{
    const int channels = std::max(0, engine_.visibleChannels());
    if (channels != channels_)
    {
        channels_ = channels;
        rows_.clear();
        for (int channel = 0; channel < channels_; ++channel)
        {
            auto row = std::make_unique<Row>(engine_, channel);
            row->setMeterMode(showPeak_, referenceDb_);
            content_.addAndMakeVisible(*row);
            rows_.push_back(std::move(row));
        }
        resized();
    }

    empty_.setVisible(channels_ == 0);
    for (auto& row : rows_)
        row->refresh();
}

void PluginPage::resized()
{
    viewport_.setBounds(getLocalBounds());
    const int rowHeight = 42;
    const int height = std::max(getHeight(), channels_ * rowHeight);
    content_.setSize(std::max(1, viewport_.getMaximumVisibleWidth()), height);
    empty_.setBounds(content_.getLocalBounds().reduced(24));
    auto area = content_.getLocalBounds();
    for (auto& row : rows_)
        row->setBounds(area.removeFromTop(rowHeight));
}

} // namespace youhost
