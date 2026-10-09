#include "PluginPage.h"
#include "AppSettings.h"
#include "ChannelMenu.h"
#include "MeterColours.h"
#include "Theme.h"
#include "WindowMemory.h"
#include "X32Look.h"
#include "engine/ChannelListen.h"
#include "engine/SignalPath.h"
#include "engine/DisplayLayout.h"
#include "engine/InsertMenu.h"
#include "engine/MeterScale.h"
#include "engine/OutputGain.h"
#include "engine/Shortcuts.h"

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

class PluginPicker : public juce::Component
{
public:
    PluginPicker(AudioEngine& engine, std::function<void(const juce::PluginDescription&)> onChoose)
        : engine_(engine),
          onChoose_(std::move(onChoose))
    {
        addAndMakeVisible(allButton_);
        addAndMakeVisible(auButton_);
        addAndMakeVisible(vstButton_);
        addAndMakeVisible(search_);
        addAndMakeVisible(hiddenNote_);
        addAndMakeVisible(tree_);
        addAndMakeVisible(empty_);
        allButton_.setButtonText("All");
        auButton_.setButtonText("AU");
        vstButton_.setButtonText("VST3");
        for (auto* button : { &allButton_, &auButton_, &vstButton_ })
        {
            button->setRadioGroupId(1);
            button->setClickingTogglesState(true);
            button->setMouseClickGrabsKeyboardFocus(false);
            button->setColour(juce::TextButton::buttonColourId, theme::button);
            button->setColour(juce::TextButton::buttonOnColourId, theme::buttonOn);
            button->setColour(juce::TextButton::textColourOffId, theme::text);
            button->setColour(juce::TextButton::textColourOnId, theme::text);
        }
        allButton_.setToggleState(true, juce::dontSendNotification);
        allButton_.onClick = [this]
        {
            if (allButton_.getToggleState())
                setFormat(InsertFormatFilter::all);
        };
        auButton_.onClick = [this]
        {
            if (auButton_.getToggleState())
                setFormat(InsertFormatFilter::audioUnit);
        };
        vstButton_.onClick = [this]
        {
            if (vstButton_.getToggleState())
                setFormat(InsertFormatFilter::vst3);
        };
        search_.setTextToShowWhenEmpty("Search name or maker", theme::fainter);
        search_.onTextChange = [this] { rebuild(); };
        search_.onReturnKey = [this] { chooseSource(firstMatch_); };
        hiddenNote_.setText(juce::String(kInstrumentsHiddenNote.data(), kInstrumentsHiddenNote.size()),
                            juce::dontSendNotification);
        hiddenNote_.setFont(juce::Font(juce::FontOptions(12.0f)));
        hiddenNote_.setColour(juce::Label::textColourId, theme::dim);
        hiddenNote_.setJustificationType(juce::Justification::centredLeft);
        hiddenNote_.setInterceptsMouseClicks(false, false);
        tree_.setColour(juce::TreeView::backgroundColourId, theme::background);
        tree_.setColour(juce::TreeView::linesColourId, theme::dim);
        tree_.setDefaultOpenness(false);
        tree_.setRootItemVisible(false);
        tree_.setIndentSize(14);
        empty_.setJustificationType(juce::Justification::centred);
        empty_.setColour(juce::Label::textColourId, theme::dim);
        empty_.setInterceptsMouseClicks(false, false);
        setSize(360, 420);
        reload();
    }

    ~PluginPicker() override
    {
        tree_.deleteRootItem();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        auto formats = area.removeFromTop(26);
        allButton_.setBounds(formats.removeFromLeft(64));
        formats.removeFromLeft(6);
        auButton_.setBounds(formats.removeFromLeft(64));
        formats.removeFromLeft(6);
        vstButton_.setBounds(formats.removeFromLeft(72));
        area.removeFromTop(6);
        search_.setBounds(area.removeFromTop(28));
        if (hiddenNote_.isVisible())
        {
            area.removeFromTop(4);
            hiddenNote_.setBounds(area.removeFromTop(18));
        }
        area.removeFromTop(6);
        tree_.setBounds(area);
        empty_.setBounds(area.reduced(12));
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(theme::panel);
    }

    void setChoose(std::function<void(const juce::PluginDescription&)> onChoose)
    {
        onChoose_ = std::move(onChoose);
    }

    void reload()
    {
        knownCount_ = engine_.pluginCatalogue().types().size();
        types_ = engine_.pluginCatalogue().insertTypes();
        hideInstruments_ = ! engine_.pluginCatalogue().showInstrumentsInInserts();
        hiddenNote_.setVisible(hideInstruments_);
        rebuild();
        search_.grabKeyboardFocus();
    }

    void chooseSource(int source)
    {
        if (onChoose_ == nullptr || ! juce::isPositiveAndBelow(source, types_.size()))
            return;
        onChoose_(types_.getReference(source));
    }

private:
    class MakerNode : public juce::TreeViewItem
    {
    public:
        explicit MakerNode(juce::String name)
            : name_(std::move(name))
        {
        }

        bool mightContainSubItems() override { return true; }

        int getItemHeight() const override { return 22; }

        void paintItem(juce::Graphics& graphics, int width, int height) override
        {
            graphics.setColour(theme::text);
            graphics.setFont(juce::Font(juce::FontOptions(13.0f).withStyle("Bold")));
            graphics.drawText(name_, 4, 0, width - 6, height, juce::Justification::centredLeft, true);
        }

        void itemClicked(const juce::MouseEvent&) override { setOpen(! isOpen()); }

        void itemDoubleClicked(const juce::MouseEvent&) override {}

    private:
        juce::String name_;
    };

    class PluginNode : public juce::TreeViewItem
    {
    public:
        PluginNode(PluginPicker& owner, int source, juce::String label)
            : owner_(owner),
              source_(source),
              label_(std::move(label))
        {
        }

        bool mightContainSubItems() override { return false; }

        int getItemHeight() const override { return 22; }

        void paintItem(juce::Graphics& graphics, int width, int height) override
        {
            graphics.setColour(theme::text);
            graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
            graphics.drawText(label_, 4, 0, width - 6, height, juce::Justification::centredLeft, true);
        }

        void itemClicked(const juce::MouseEvent&) override { owner_.chooseSource(source_); }

        void itemDoubleClicked(const juce::MouseEvent&) override {}

    private:
        PluginPicker& owner_;
        int source_ = -1;
        juce::String label_;
    };

    class RootNode : public juce::TreeViewItem
    {
    public:
        bool mightContainSubItems() override { return true; }
    };

    void rebuild()
    {
        std::vector<CatalogPlugin> catalog;
        catalog.reserve(static_cast<std::size_t>(types_.size()));
        for (const auto& type : types_)
        {
            catalog.push_back(CatalogPlugin { type.name.toStdString(),
                                              type.manufacturerName.toStdString(),
                                              type.pluginFormatName.toStdString() });
        }

        const auto query = search_.getText().trim().toStdString();
        const auto groups = groupInsertPlugins(catalog, query, format_);
        const bool searching = ! textIsBlank(query);
        firstMatch_ = -1;

        tree_.deleteRootItem();
        auto* root = new RootNode();
        tree_.setRootItem(root);
        for (const auto& group : groups)
        {
            auto* maker = new MakerNode(juce::String::fromUTF8(group.manufacturer.c_str()));
            root->addSubItem(maker);
            for (const auto& plugin : group.plugins)
            {
                if (firstMatch_ < 0)
                    firstMatch_ = plugin.source;
                maker->addSubItem(new PluginNode(*this, plugin.source, juce::String::fromUTF8(plugin.label.c_str())));
            }
            if (searching)
                maker->setOpenness(juce::TreeViewItem::Openness::opennessOpen);
        }

        empty_.setVisible(groups.empty());
        juce::String emptyText = "No plugins match.";
        if (types_.isEmpty())
            emptyText = knownCount_ == 0 ? "No plugins yet. Open the scanner and scan."
                                         : "No effects in this list. Instruments and generators are hidden.";
        empty_.setText(emptyText, juce::dontSendNotification);
        resized();
        tree_.repaint();
    }

    void setFormat(InsertFormatFilter format)
    {
        if (format_ == format)
            return;
        format_ = format;
        rebuild();
    }

    AudioEngine& engine_;
    std::function<void(const juce::PluginDescription&)> onChoose_;
    juce::Array<juce::PluginDescription> types_;
    int knownCount_ = 0;
    int firstMatch_ = -1;
    bool hideInstruments_ = true;
    InsertFormatFilter format_ = InsertFormatFilter::all;
    juce::TextButton allButton_;
    juce::TextButton auButton_;
    juce::TextButton vstButton_;
    juce::TextEditor search_;
    juce::Label hiddenNote_;
    juce::TreeView tree_;
    juce::Label empty_;
};

bool parseSlotDrag(const juce::var& description, int& channel, int& slot, bool& copy)
{
    const auto text = description.toString();
    if (! text.startsWith("youhost-slot:"))
        return false;
    const auto parts = juce::StringArray::fromTokens(text.fromFirstOccurrenceOf("youhost-slot:", false, false), ":", "");
    if (parts.size() < 3)
        return false;
    channel = parts[0].getIntValue();
    slot = parts[1].getIntValue();
    copy = parts[2] == "copy";
    return true;
}

class SlotButton : public juce::TextButton,
                   public juce::DragAndDropTarget
{
public:
    std::function<void()> onMenu;
    std::function<bool()> canDrag;
    std::function<void(bool copy)> onDrag;
    std::function<void(int fromChannel, int fromSlot, bool copy)> onDrop;
    bool dropHover = false;

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            if (onMenu != nullptr)
                onMenu();
            return;
        }
        dragged_ = false;
        juce::TextButton::mouseDown(event);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (dragged_ || event.mods.isPopupMenu())
            return;
        if (event.getDistanceFromDragStart() < 8.0f)
            return;
        if (canDrag == nullptr || ! canDrag())
            return;
        dragged_ = true;
        if (onDrag != nullptr)
            onDrag(event.mods.isAltDown());
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (dragged_)
        {
            dragged_ = false;
            setState(juce::Button::buttonNormal);
            return;
        }
        juce::TextButton::mouseUp(event);
    }

    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        int channel = 0;
        int slot = 0;
        bool copy = false;
        return parseSlotDrag(details.description, channel, slot, copy);
    }

    void itemDragEnter(const SourceDetails&) override
    {
        dropHover = true;
        repaint();
    }

    void itemDragExit(const SourceDetails&) override
    {
        dropHover = false;
        repaint();
    }

    void itemDropped(const SourceDetails& details) override
    {
        dropHover = false;
        repaint();
        int channel = 0;
        int slot = 0;
        bool copy = false;
        if (parseSlotDrag(details.description, channel, slot, copy) && onDrop != nullptr)
            onDrop(channel, slot, copy);
    }

    void paintButton(juce::Graphics& graphics, bool over, bool down) override
    {
        juce::TextButton::paintButton(graphics, over, down);
        if (dropHover)
        {
            graphics.setColour(theme::text);
            graphics.drawRect(getLocalBounds(), 2);
        }
    }

private:
    bool dragged_ = false;
};

} // namespace

class PluginListWindow : public juce::DocumentWindow
{
public:
    PluginListWindow(AudioEngine& engine, AppSettings& settings)
        : juce::DocumentWindow("Plugins", theme::panel, juce::DocumentWindow::closeButton),
          settings_(settings)
    {
        auto picker = std::make_unique<PluginPicker>(engine, [](const juce::PluginDescription&) {});
        picker_ = picker.get();
        setUsingNativeTitleBar(true);
        setContentOwned(picker.release(), true);
        prepareRememberedWindow(*this, settings_, "windowPluginList", 420, 480, 320, 240);
        setVisible(false);
    }

    ~PluginListWindow() override
    {
        saveRememberedWindow(*this, settings_, "windowPluginList");
    }

    void closeButtonPressed() override
    {
        saveRememberedWindow(*this, settings_, "windowPluginList");
        setVisible(false);
    }

    void showFor(std::function<void(const juce::PluginDescription&)> onChoose)
    {
        if (picker_ != nullptr)
        {
            picker_->setChoose(std::move(onChoose));
            picker_->reload();
        }
        setVisible(true);
        toFront(true);
    }

private:
    AppSettings& settings_;
    PluginPicker* picker_ = nullptr;
};

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
        if (event.getNumberOfClicks() >= 2)
        {
            engine_.toggleGroupCollapsed(group_);
            renameGroup(engine_, group_);
            return;
        }
        engine_.toggleGroupCollapsed(group_);
    }

    void paint(juce::Graphics& graphics) override
    {
        const int color = engine_.groupColor(group_);
        const int colourId = normaliseX32Colour(color);
        const bool inverted = kX32Colours[colourId].inverted;
        const auto hue = x32Hue(color);
        const auto fill = hue.isTransparent() ? theme::panel
                                               : (inverted ? juce::Colour(0xff1a1d27) : hue);
        const auto ink = hue.isTransparent() ? theme::text
                                              : (inverted ? hue
                                                          : (hue.getPerceivedBrightness() > 0.55f
                                                                 ? juce::Colour(0xff141414)
                                                                 : juce::Colours::white));
        graphics.setColour(fill);
        graphics.fillRect(getLocalBounds());
        if (! hue.isTransparent())
        {
            graphics.setColour(hue);
            graphics.fillRect(0, 0, 8, getHeight());
        }
        graphics.setColour(ink);

        const int channels = std::max(0, engine_.visibleChannels());
        float level = 0.0f;
        bool clip = false;
        bool plugins = false;
        int count = 0;
        for (int channel = 0; channel < channels; ++channel)
        {
            if (engine_.channelGroup(channel) != group_)
                continue;
            ++count;
            level = std::max(level, showPeak_ ? engine_.peakFor(channel) : engine_.rmsFor(channel));
            clip = clip || engine_.clipFor(channel);
            if (! plugins)
            {
                const auto snap = engine_.channelSnapshot(channel);
                for (const auto& slot : snap.slots)
                    plugins = plugins || slot.occupied;
            }
        }

        constexpr int nameX = 16;
        constexpr int nameW = 180;
        constexpr int foldX = 204;
        constexpr int foldW = 72;
        constexpr int clipX = 284;
        constexpr int clipW = 48;
        constexpr int fxX = 340;
        constexpr int fxW = 36;
        constexpr int meterX = 384;

        graphics.setColour(ink);
        graphics.setFont(juce::Font(juce::FontOptions(15.0f).withStyle("Bold")));
        graphics.drawText(engine_.groupName(group_),
                          nameX,
                          0,
                          nameW,
                          getHeight(),
                          juce::Justification::centredLeft,
                          true);

        graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
        graphics.drawText(juce::String(groupFoldLabel(engine_.groupCollapsed(group_), count)),
                          foldX,
                          0,
                          foldW,
                          getHeight(),
                          juce::Justification::centredLeft,
                          true);

        graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
        if (clip)
            graphics.drawText("CLIP", clipX, 0, clipW, getHeight(), juce::Justification::centredLeft, true);
        if (plugins)
            graphics.drawText("FX", fxX, 0, fxW, getHeight(), juce::Justification::centredLeft, true);

        auto meter = juce::Rectangle<int>(meterX, 6, std::max(0, getWidth() - meterX - 12), std::max(0, getHeight() - 12));
        graphics.setColour(theme::meterTrack);
        graphics.fillRoundedRectangle(meter.toFloat(), 2.0f);
        if (level > 0.0f && meter.getWidth() > 1)
        {
            const MeterSpan span = showPeak_ ? peakMeterSpan() : rmsMeterSpan(referenceDb_);
            const float filled = normaliseDb(gainToDb(level), span) * static_cast<float>(meter.getWidth());
            graphics.setColour(meterLevelColour(level, referenceDb_));
            graphics.fillRect(meter.getX(), meter.getY(), static_cast<int>(filled), meter.getHeight());
        }

        graphics.setColour(juce::Colour(0xff8b95a8));
        graphics.fillRect(0, getHeight() - 1, getWidth(), 1);
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

class OutputTrim : public juce::Component,
                     public juce::SettableTooltipClient
{
public:
    OutputTrim(AudioEngine& engine, int channel)
        : engine_(engine),
          channel_(channel)
    {
        setMouseClickGrabsKeyboardFocus(false);
        editor_.setJustification(juce::Justification::centred);
        editor_.setInputRestrictions(8, "0123456789.-+");
        editor_.setFont(juce::Font(juce::FontOptions(12.0f)));
        editor_.onReturnKey = [this] { commitEditor(); };
        editor_.onFocusLost = [this] { commitEditor(); };
        editor_.onEscapeKey = [this] { editor_.setVisible(false); repaint(); };
        addChildComponent(editor_);
        juce::SettableTooltipClient::setTooltip("Output gain after the plugins, from -9 dB to +9 dB. Drag for 0.5 dB steps. Double-click to type a value. Option-click resets to 0 dB. The scroll wheel does not change it. Recorded WAVs stay at the raw input level.");
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override
    {
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isAltDown())
        {
            dragging_ = false;
            engine_.setOutputDb(channel_, 0.0f);
            repaint();
            return;
        }
        if (event.getNumberOfClicks() >= 2)
        {
            dragging_ = false;
            editor_.setText(juce::String(engine_.outputDb(channel_), 1), false);
            editor_.setVisible(true);
            editor_.grabKeyboardFocus();
            editor_.selectAll();
            return;
        }
        dragging_ = true;
        startDb_ = engine_.outputDb(channel_);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (! dragging_)
            return;
        const float db = startDb_ - static_cast<float>(event.getDistanceFromDragStartY()) * (kOutputDbStep / 10.0f);
        engine_.setOutputDb(channel_, db);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        dragging_ = false;
    }

    void paint(juce::Graphics& graphics) override
    {
        if (editor_.isVisible())
            return;
        const float db = engine_.outputDb(channel_);
        const bool unity = outputDbIsUnity(db);
        auto plate = getLocalBounds().toFloat().reduced(1.0f);
        graphics.setColour(unity ? theme::button : juce::Colour(0xff8a6a22));
        graphics.fillRoundedRectangle(plate, 3.0f);
        graphics.setColour(juce::Colours::white.withAlpha(0.18f));
        graphics.drawLine(plate.getX() + 2.0f, plate.getY() + 1.0f, plate.getRight() - 2.0f, plate.getY() + 1.0f, 1.0f);
        graphics.setColour(juce::Colours::black.withAlpha(0.35f));
        graphics.drawLine(plate.getX() + 2.0f, plate.getBottom() - 1.5f, plate.getRight() - 2.0f, plate.getBottom() - 1.5f, 1.0f);
        graphics.setColour(unity ? theme::text : juce::Colours::white);
        auto font = juce::FontOptions(12.0f);
        if (! unity)
            font = font.withStyle("Bold");
        graphics.setFont(juce::Font(font));
        graphics.drawText(juce::String(db, 1) + " dB", getLocalBounds(), juce::Justification::centred, false);
    }

    void resized() override
    {
        editor_.setBounds(getLocalBounds().reduced(1));
    }

private:
    void commitEditor()
    {
        if (! editor_.isVisible())
            return;
        engine_.setOutputDb(channel_, editor_.getText().getFloatValue());
        editor_.setVisible(false);
        repaint();
    }

    AudioEngine& engine_;
    int channel_ = 0;
    float startDb_ = 0.0f;
    bool dragging_ = false;
    juce::TextEditor editor_;
};

class PluginPage::Row : public juce::Component
{
public:
    Row(AudioEngine& engine, int channel)
        : engine_(engine),
          channel_(channel),
          tabKeys_(*this),
          gain_(engine, channel)
    {
        addAndMakeVisible(number_);
        addAndMakeVisible(name_);
        addAndMakeVisible(arm_);
        addAndMakeVisible(exclude_);
        addAndMakeVisible(pdc_);
        addAndMakeVisible(gain_);
        number_.setInterceptsMouseClicks(false, false);
        number_.setFont(juce::Font(juce::FontOptions(15.0f).withStyle("Bold")));
        number_.setColour(juce::Label::textColourId, theme::text);
        number_.setMinimumHorizontalScale(1.0f);
        number_.setJustificationType(juce::Justification::centred);
        name_.setEditable(false, true, false);
        name_.setFont(juce::Font(juce::FontOptions(13.0f)));
        name_.setMinimumHorizontalScale(0.6f);
        name_.setJustificationType(juce::Justification::centredLeft);
        name_.setTooltip("Double-click to rename. Tab moves to the next visible channel. Right-click for color and group.");
        name_.onTextChange = [this]
        {
            if (! updating_)
                engine_.setChannelName(channel_, name_.getText());
        };
        name_.onEditorShow = [this]
        {
            if (auto* editor = name_.getCurrentTextEditor())
            {
                editor->setSelectAllWhenFocused(true);
                editor->addKeyListener(&tabKeys_);
                juce::Component::SafePointer<juce::TextEditor> safe(editor);
                juce::MessageManager::callAsync([safe]
                {
                    if (safe != nullptr)
                    {
                        safe->grabKeyboardFocus();
                        safe->selectAll();
                    }
                });
            }
        };
        name_.addMouseListener(this, false);

        arm_.setMouseClickGrabsKeyboardFocus(false);
        exclude_.setMouseClickGrabsKeyboardFocus(false);
        exclude_.setButtonText("Ex");
        exclude_.setTooltip("Exclude from alignment. This channel stays undelayed and does not move the others.");
        arm_.onClick = [this] { engine_.cycleChannelListen(channel_); };
        pdc_.setFont(juce::Font(juce::FontOptions(11.0f)));
        pdc_.setColour(juce::Label::textColourId, theme::dim);
        pdc_.setJustificationType(juce::Justification::centredLeft);
        pdc_.setMinimumHorizontalScale(0.7f);
        pdc_.setInterceptsMouseClicks(false, false);
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
            button.canDrag = [this, slot]
            {
                const auto snap = engine_.channelSnapshot(channel_).slots[static_cast<std::size_t>(slot)];
                return snap.occupied && ! snap.loading;
            };
            button.onDrag = [this, slot](bool copy)
            {
                auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this);
                if (container == nullptr)
                    return;
                auto& source = slots_[static_cast<std::size_t>(slot)];
                const auto image = source.createComponentSnapshot(source.getLocalBounds(), true);
                const auto description = "youhost-slot:" + juce::String(channel_) + ":" + juce::String(slot) + ":"
                                         + (copy ? "copy" : "move");
                container->startDragging(description, &source, juce::ScaledImage(image), true);
            };
            button.onDrop = [this, slot](int fromChannel, int fromSlot, bool copy)
            {
                engine_.transferPlugin(fromChannel, fromSlot, channel_, slot, copy);
            };
        }
    }

    void setMeterMode(bool peak, int referenceDb)
    {
        showPeak_ = peak;
        referenceDb_ = referenceDb;
    }

    bool isChannel(int channel) const noexcept { return channel_ == channel; }

    void editName()
    {
        name_.showEditor();
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

        const auto listen = engine_.channelListen(channel_);
        arm_.setButtonText(channelListenLabel(listen));
        if (listen == ChannelListen::record)
        {
            arm_.setTooltip("REC. Live sound passes through and the next take records this channel. Click for INPUT.");
            arm_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff8d2430));
            arm_.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        }
        else if (listen == ChannelListen::input)
        {
            arm_.setTooltip("INPUT. Live sound passes through the plugins and is not recorded. Click for OFF.");
            arm_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff245a9a));
            arm_.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        }
        else
        {
            arm_.setTooltip("OFF. The meter stays still, plugins are skipped, and the output is silent. Click for REC.");
            arm_.setColour(juce::TextButton::buttonColourId, theme::button);
            arm_.setColour(juce::TextButton::textColourOffId, theme::fainter);
        }
        pdc_.setText(engine_.channelPdcText(channel_), juce::dontSendNotification);
        const float db = engine_.outputDb(channel_);
        const float dbDelta = db - gainShown_;
        if (dbDelta > 0.01f || dbDelta < -0.01f)
        {
            gainShown_ = db;
            gain_.repaint();
        }
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
            const int color = engine_.channelColor(channel_);
            if (color != 0)
                fill = fill.interpolatedWith(x32Hue(color), 0.28f);
            button.setColour(juce::TextButton::buttonColourId, fill);
            button.setColour(juce::TextButton::textColourOffId, source.bypassed ? theme::dim : theme::text);
            juce::String tip = "Drag to move this plugin. Option-drag copies it. A drag is ignored while a plugin on that channel is still loading.";
            if (source.occupied)
            {
                tip << "\n" << source.inputChannels << " in / " << source.outputChannels << " out, latency "
                    << source.latencySamples << " samples";
                if (source.outputChannels > 1)
                    tip << ", stereo on mono: " << stereoFoldToken(stereoFoldFromInt(source.stereoFold));
            }
            button.setTooltip(source.error.isNotEmpty() ? source.error : tip);
        }
        const bool selected = engine_.isChannelSelected(channel_);
        const int color = engine_.channelColor(channel_);
        if (selected != selectedDrawn_ || color != colorDrawn_)
        {
            selectedDrawn_ = selected;
            colorDrawn_ = color;
            repaint();
        }
        else if (! meterArea_.isEmpty())
        {
            repaint(meterArea_);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4, 2);
        area.removeFromLeft(8);
        number_.setBounds(area.removeFromLeft(46));
        arm_.setBounds(area.removeFromLeft(62).reduced(2, 4));
        name_.setBounds(area.removeFromLeft(120));
        meterArea_ = area.removeFromLeft(18).reduced(3, 3);
        exclude_.setBounds(area.removeFromLeft(34).reduced(2, 4));
        gain_.setBounds(area.removeFromRight(72).reduced(2, 4));
        area.removeFromLeft(4);
        const int slotBudget = std::max(0, area.getWidth() - 150);
        const int slotWidth = std::min(128, std::max(56, slotBudget / kSlotsPerChannel));
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
            slots_[static_cast<std::size_t>(slot)].setBounds(area.removeFromLeft(slotWidth).reduced(2, 2));
        pdc_.setBounds(area.reduced(4, 4));
    }

    void paint(juce::Graphics& graphics) override
    {
        const int color = engine_.channelColor(channel_);
        graphics.setColour(theme::background);
        graphics.fillRect(getLocalBounds());
        const auto wash = x32Wash(color);
        if (! wash.isTransparent())
        {
            graphics.setColour(wash);
            graphics.fillRect(getLocalBounds());
        }
        const auto hue = x32Hue(color);
        if (! hue.isTransparent())
        {
            graphics.setColour(hue);
            graphics.fillRect(0, 0, 8, getHeight());
        }

        if (engine_.isChannelSelected(channel_))
        {
            graphics.setColour(theme::green);
            graphics.drawRect(getLocalBounds().reduced(1), 2);
        }

        graphics.setColour(juce::Colour(0xff8b95a8));
        graphics.fillRect(0, getHeight() - 1, getWidth(), 1);

        const bool on = channelListenAudible(engine_.channelListen(channel_));
        graphics.setColour(on ? theme::meterTrack : theme::panelEdge.withAlpha(0.45f));
        graphics.fillRoundedRectangle(meterArea_.toFloat(), 2.0f);
        const float level = showPeak_ ? engine_.peakFor(channel_) : engine_.rmsFor(channel_);
        const MeterSpan span = showPeak_ ? peakMeterSpan() : rmsMeterSpan(referenceDb_);
        if (on && level > 0.0f && meterArea_.getHeight() > 1)
        {
            const float filled = normaliseDb(gainToDb(level), span) * static_cast<float>(meterArea_.getHeight());
            auto bar = meterArea_.toFloat();
            bar.setTop(bar.getBottom() - filled);
            graphics.setColour(meterLevelColour(level, referenceDb_));
            graphics.fillRoundedRectangle(bar, 2.0f);
        }
        graphics.setColour(! on ? theme::panelEdge : engine_.clipFor(channel_) ? theme::red : theme::panelEdge);
        graphics.fillRect(meterArea_.getX(), meterArea_.getY(), meterArea_.getWidth(), 3);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            showChannelMenu(engine_, *this, channel_, [this](int) { editName(); });
            return;
        }
        const bool toggle = event.mods.isCommandDown() || event.mods.isCtrlDown();
        const bool extend = event.mods.isShiftDown() && ! toggle;
        if (toggle || extend)
        {
            engine_.selectChannel(channel_, extend, toggle);
            return;
        }
        if (event.eventComponent == this && meterArea_.contains(event.getPosition()) && engine_.clipFor(channel_))
            engine_.requestClipClear(channel_);
        else if (event.eventComponent == this)
            engine_.selectChannel(channel_, false, false);
    }

private:
    struct TabKeys : juce::KeyListener
    {
        explicit TabKeys(Row& owner)
            : row(owner)
        {
        }

        bool keyPressed(const juce::KeyPress& key, juce::Component*) override
        {
            const auto mods = key.getModifiers();
            const auto code = key.getKeyCode();
            const auto nameKey = matchNameKey(code == juce::KeyPress::tabKey,
                                               false,
                                               false,
                                               mods.isShiftDown(),
                                               mods.isCommandDown(),
                                               mods.isAltDown(),
                                               mods.isCtrlDown());
            if (nameKey != NameKey::next && nameKey != NameKey::previous)
                return false;

            const int direction = nameKey == NameKey::previous ? -1 : 1;
            const int channel = row.channel_;
            juce::Component::SafePointer<Row> safe(&row);
            juce::MessageManager::callAsync([safe, channel, direction]
            {
                if (safe == nullptr)
                    return;
                safe->name_.hideEditor(false);
                if (auto* page = safe->findParentComponentOfClass<PluginPage>())
                    page->stepNameEdit(channel, direction);
            });
            return true;
        }

        Row& row;
    };

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

        if (auto* page = findParentComponentOfClass<PluginPage>())
            page->showPluginList(channel_, slot);
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
        if (snap.outputChannels > 1)
        {
            juce::PopupMenu fold;
            fold.addItem(10, "Use L", true, snap.stereoFold == 0);
            fold.addItem(11, "Use R", true, snap.stereoFold == 1);
            fold.addItem(12, "Sum (L+R)/2", true, snap.stereoFold == 2);
            menu.addSubMenu("Stereo plugin on mono channel", fold);
        }
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
                               else if (result >= 10 && result <= 12)
                                   engine_.setStereoFold(channel_, slot, result - 10);
                           });
    }

    AudioEngine& engine_;
    int channel_ = 0;
    bool showPeak_ = false;
    int referenceDb_ = kDefaultRmsReferenceDb;
    bool updating_ = false;
    bool selectedDrawn_ = false;
    int colorDrawn_ = -1;
    float gainShown_ = 100.0f;
    juce::Label number_;
    juce::Label name_;
    TabKeys tabKeys_;
    juce::TextButton arm_;
    juce::TextButton exclude_;
    juce::Label pdc_;
    OutputTrim gain_;
    std::array<SlotButton, kSlotsPerChannel> slots_;
    juce::Rectangle<int> meterArea_;
};

PluginPage::PluginPage(AudioEngine& engine, AppSettings& settings)
    : engine_(engine),
      settings_(settings)
{
    pluginList_ = std::make_unique<PluginListWindow>(engine_, settings_);
    addAndMakeVisible(nullButton_);
    nullButton_.setMouseClickGrabsKeyboardFocus(false);
    nullButton_.setTooltip("Bypass every plugin and the compensation delay, so the USB output is the clean input. Click again to bring the plugins back. The recorded WAV is always the raw input.");
    nullButton_.onClick = [this]
    {
        engine_.setBypassAll(! engine_.bypassAll());
        refresh();
    };
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

void PluginPage::beginNameEdit(int channel)
{
    for (const auto& row : rows_)
    {
        if (row == nullptr || ! row->isChannel(channel))
            continue;
        row->editName();
        int y = 0;
        for (std::size_t index = 0; index < order_.size(); ++index)
        {
            if (order_[index] == row.get())
            {
                const int maxY = std::max(0, content_.getHeight() - viewport_.getViewHeight());
                viewport_.setViewPosition(0, std::clamp(y, 0, maxY));
                break;
            }
            if (index < heights_.size())
                y += heights_[index];
        }
        return;
    }
}

void PluginPage::stepNameEdit(int channel, int direction)
{
    const int channels = std::max(0, engine_.visibleChannels());
    const auto strips = engine_.displayStrips(channels);
    const int next = adjacentVisibleChannel(strips.data(), static_cast<int>(strips.size()), channel, direction);
    if (next >= 0)
        beginNameEdit(next);
}

void PluginPage::showPluginList(int channel, int slot)
{
    if (pluginList_ == nullptr)
        return;
    pluginList_->showFor([this, channel, slot](const juce::PluginDescription& description)
    {
        engine_.loadPlugin(channel, slot, description, true);
        if (pluginList_ != nullptr)
            pluginList_->setVisible(false);
    });
}

void PluginPage::refresh()
{
    const int channels = std::max(0, engine_.visibleChannels());
    if (channels != channels_ || engine_.displayRevision() != revision_)
        rebuild();

    empty_.setVisible(channels_ == 0);
    const bool bypass = engine_.bypassAll();
    nullButton_.setButtonText(bypass ? "Bypass all" : "Null test");
    nullButton_.setColour(juce::TextButton::buttonColourId, bypass ? juce::Colour(0xff8a6a22) : theme::button);
    nullButton_.setColour(juce::TextButton::textColourOffId, bypass ? juce::Colours::white : theme::text);
    for (auto& row : rows_)
        row->refresh();
    for (auto& header : headers_)
        header->refresh();
}

void PluginPage::resized()
{
    auto bounds = getLocalBounds();
    auto bar = bounds.removeFromTop(32);
    nullButton_.setBounds(bar.removeFromLeft(128).reduced(0, 4));
    viewport_.setBounds(bounds);
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
