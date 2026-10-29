#include "PluginPage.h"
#include "engine/WindowCatalog.h"
#include "AppSettings.h"
#include "ChannelMenu.h"
#include "MeterColours.h"
#include "Theme.h"
#include "WindowMemory.h"
#include "X32Look.h"
#include "engine/ChannelListen.h"
#include "engine/SignalPath.h"
#include "engine/DisplayLayout.h"
#include "engine/HostColumns.h"
#include "engine/CrashJournal.h"
#include "engine/InsertMenu.h"
#include "engine/PluginBuses.h"
#include "engine/StereoLink.h"
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
    if (snap.note == "crashed, disabled")
        return juce::String(slot + 1) + " " + name + "  crashed, disabled";
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
            const auto described = type.descriptiveName.toStdString();
            const auto buses = isPluginBusMark(described)
                                   ? described
                                   : pluginBusMarkFromCounts(type.numInputChannels, type.numOutputChannels);
            catalog.push_back(CatalogPlugin { type.name.toStdString(),
                                              type.manufacturerName.toStdString(),
                                              type.pluginFormatName.toStdString(),
                                              buses });
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

class LinkButton : public juce::TextButton
{
public:
    LinkButton()
    {
        setButtonText("LINK");
    }

    void paintButton(juce::Graphics& graphics, bool over, bool down) override
    {
        getLookAndFeel().drawButtonBackground(graphics, *this, findColour(juce::TextButton::buttonColourId), over, down);
        graphics.setColour(findColour(juce::TextButton::textColourOffId));
        graphics.setFont(juce::Font(juce::FontOptions(11.0f).withStyle("Bold")));
        graphics.drawFittedText(getButtonText(), getLocalBounds().reduced(2, 0), juce::Justification::centred, 1);
    }
};

class SlotButton : public juce::TextButton,
                   public juce::DragAndDropTarget
{
public:
    SlotButton()
    {
        setOpaque(true);
    }

    std::function<void()> onMenu;
    std::uint32_t shownFill = 0;
    std::uint32_t shownText = 0;
    bool shownReady = false;
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
        prepareRememberedWindow(*this, settings_, "windowPluginList", pluginListWindowWidth(), pluginListWindowHeight(), 320, 240);
        setVisible(false);
    }

    ~PluginListWindow() override
    {
        saveRememberedWindow(*this, settings_, "windowPluginList", pluginListWindowWidth(), pluginListWindowHeight());
    }

    void closeButtonPressed() override
    {
        saveRememberedWindow(*this, settings_, "windowPluginList", pluginListWindowWidth(), pluginListWindowHeight());
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

    int groupId() const noexcept { return group_; }

    void refresh()
    {
        const int channels = std::max(0, engine_.visibleChannels());
        const int revision = engine_.displayRevision();
        float level = 0.0f;
        bool clip = false;
        int count = 0;
        bool plugins = false;
        for (int channel = 0; channel < channels; ++channel)
        {
            if (engine_.channelGroup(channel) != group_)
                continue;
            ++count;
            level = std::max(level, showPeak_ ? engine_.peakFor(channel) : engine_.rmsFor(channel));
            clip = clip || engine_.clipFor(channel);
            if (plugins)
                continue;
            const auto snap = engine_.channelSnapshot(channel);
            for (const auto& slot : snap.slots)
                plugins = plugins || slot.occupied;
        }

        const bool chrome = revision != revision_ || count != count_ || plugins != plugins_ || clip != clip_;
        revision_ = revision;
        count_ = count;
        plugins_ = plugins;
        clip_ = clip;
        if (chrome)
        {
            level_ = level;
            repaint();
            return;
        }
        if (std::fabs(level - level_) <= 0.00001f)
            return;
        level_ = level;
        const auto track = hostGroupMeterTrack(static_cast<float>(getWidth()),
                                               static_cast<float>(getHeight()),
                                               kHostGroupMeterOrigin);
        repaint(juce::Rectangle<float>(track.x, track.y, track.width, track.height).getSmallestIntegerContainer());
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

        constexpr int nameX = 16;
        constexpr int nameW = 180;
        constexpr int foldX = 204;
        constexpr int foldW = 72;
        constexpr int clipX = 284;
        constexpr int clipW = 48;
        constexpr int fxX = 340;
        constexpr int fxW = 36;

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
        graphics.drawText(juce::String(groupFoldLabel(engine_.groupCollapsed(group_), count_)),
                          foldX,
                          0,
                          foldW,
                          getHeight(),
                          juce::Justification::centredLeft,
                          true);

        graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
        if (clip_)
            graphics.drawText("CLIP", clipX, 0, clipW, getHeight(), juce::Justification::centredLeft, true);
        if (plugins_)
            graphics.drawText("FX", fxX, 0, fxW, getHeight(), juce::Justification::centredLeft, true);

        const auto track = hostGroupMeterTrack(static_cast<float>(getWidth()),
                                               static_cast<float>(getHeight()),
                                               kHostGroupMeterOrigin);
        const auto meter = juce::Rectangle<float>(track.x, track.y, track.width, track.height);
        graphics.setColour(theme::meterTrack);
        graphics.fillRoundedRectangle(meter, 2.0f);
        if (level_ > 0.0f && meter.getWidth() > 1.0f)
        {
            const MeterSpan span = showPeak_ ? peakMeterSpan() : rmsMeterSpan(referenceDb_);
            const float filled = normaliseDb(gainToDb(level_), span) * meter.getWidth();
            graphics.setColour(meterLevelColour(level_, referenceDb_));
            graphics.fillRect(meter.getX(), meter.getY(), filled, meter.getHeight());
        }

        graphics.setColour(juce::Colour(0xff8b95a8));
        graphics.fillRect(0, getHeight() - 1, getWidth(), 1);
    }

    void setMeterMode(bool peak, int referenceDb)
    {
        if (showPeak_ == peak && referenceDb_ == referenceDb)
            return;
        showPeak_ = peak;
        referenceDb_ = referenceDb;
        const auto track = hostGroupMeterTrack(static_cast<float>(getWidth()),
                                               static_cast<float>(getHeight()),
                                               kHostGroupMeterOrigin);
        repaint(juce::Rectangle<float>(track.x, track.y, track.width, track.height).getSmallestIntegerContainer());
    }

private:
    AudioEngine& engine_;
    int group_ = 0;
    bool showPeak_ = false;
    int referenceDb_ = kDefaultRmsReferenceDb;
    int revision_ = -1;
    int count_ = 0;
    bool plugins_ = false;
    bool clip_ = false;
    float level_ = 0.0f;
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
        dragEdit_ = true;
        engine_.beginEdit();
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
        if (dragEdit_)
            engine_.endEdit();
        dragEdit_ = false;
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
    bool dragEdit_ = false;
    juce::TextEditor editor_;
};

class PluginPage::Row : public juce::Component
{
public:
    Row(AudioEngine& engine, int channel, std::function<void(int)> onListen)
        : engine_(engine),
          channel_(channel),
          tabKeys_(*this),
          gain_(engine, channel),
          onListen_(std::move(onListen))
    {
        setOpaque(true);
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
        name_.setOpaque(false);
        name_.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
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
        arm_.onClick = [this]
        {
            if (onListen_)
                onListen_(channel_);
            else
                engine_.cycleChannelListen(channel_);
        };
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

        addAndMakeVisible(link_);
        link_.setButtonText("LINK");
        link_.setMouseClickGrabsKeyboardFocus(false);
        link_.setClickingTogglesState(false);
        link_.onClick = [this] { engine_.toggleStereoLink(channel_); };
        addAndMakeVisible(linkedNote_);
        linkedNote_.setFont(juce::Font(juce::FontOptions(12.0f)));
        linkedNote_.setColour(juce::Label::textColourId, theme::dim);
        linkedNote_.setJustificationType(juce::Justification::centredLeft);
        linkedNote_.setInterceptsMouseClicks(false, false);
        linkedNote_.setVisible(false);

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
        arm_.setEnabled(! engine_.recordingLocked());
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
        const bool canLink = canStereoLink(channel_, std::max(0, engine_.visibleChannels()));
        const bool linked = canLink && engine_.stereoLinked(channel_);
        const bool linkBlocked = canLink && engine_.stereoLinkTurnBlocked(channel_) && ! linked;
        const bool showSlots = hostPluginSlotsVisible(channel_, linked);
        link_.setVisible(canLink && (channel_ & 1) == 0);
        link_.setEnabled(! linkBlocked);
        link_.setColour(juce::TextButton::buttonColourId, linkBlocked ? theme::panelEdge : linked ? theme::buttonOn : theme::button);
        link_.setColour(juce::TextButton::textColourOffId, linkBlocked ? theme::fainter : theme::text);
        if (linkBlocked)
            link_.setTooltip(stereoLinkBlockedTip());
        else
            link_.setTooltip("Stereo link with ch " + juce::String(stereoLeftIndex(channel_) + 2)
                             + ". Both channels must be in the same group, or both ungrouped. One plugin row for the pair. Recording stays one mono WAV per channel.");
        linkedNote_.setVisible(linked && ! showSlots);
        if (linked && ! showSlots)
            linkedNote_.setText("Linked to ch " + juce::String(channel_), juce::dontSendNotification);
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            auto& button = slots_[static_cast<std::size_t>(slot)];
            button.setVisible(showSlots);
            if (! showSlots)
                continue;
            const auto& source = snap.slots[static_cast<std::size_t>(slot)];
            const auto text = slotLabel(slot, source);
            if (button.getButtonText() != text)
                button.setButtonText(text);
            juce::Colour fill = theme::button;
            const bool blinkBypass = engine_.bypassAll() && source.occupied;
            if (blinkBypass)
            {
                const bool lit = (juce::Time::getMillisecondCounter() / 500u) % 2u == 0u;
                fill = lit ? juce::Colour(0xffd48a20) : juce::Colour(0xff8a6a22);
            }
            else if (source.occupied && source.bypassed)
                fill = juce::Colour(0xff3a3424);
            else if (source.occupied)
                fill = theme::buttonOn;
            if (! blinkBypass && source.loading)
                fill = juce::Colour(0xff3d3420);
            if (! blinkBypass && engine_.isPluginEditorOpen(channel_, slot))
                fill = fill.brighter(0.2f);
            const int color = engine_.channelColor(channel_);
            if (! blinkBypass && color != 0)
                fill = fill.interpolatedWith(x32Hue(color), 0.28f);
            const auto fillArgb = fill.getARGB();
            const auto textArgb = (source.bypassed ? theme::dim : theme::text).getARGB();
            if (! button.shownReady || button.shownFill != fillArgb || button.shownText != textArgb)
            {
                button.shownReady = true;
                button.shownFill = fillArgb;
                button.shownText = textArgb;
                button.setColour(juce::TextButton::buttonColourId, juce::Colour(fillArgb));
                button.setColour(juce::TextButton::textColourOffId, juce::Colour(textArgb));
            }
            juce::String tip = "Drag to move this plugin. Option-drag copies it. A drag is ignored while a plugin on that channel is still loading.";
            if (source.occupied)
            {
                tip << "\n" << source.inputChannels << " in / " << source.outputChannels << " out, latency "
                    << source.latencySamples << " samples";
                if (source.outputChannels > 1)
                    tip << ", stereo on mono: " << stereoFoldToken(stereoFoldFromInt(source.stereoFold));
            }
            if (source.note.isNotEmpty())
                tip << "\n" << source.note;
            if (source.error.isNotEmpty())
                tip = source.error;
            button.setTooltip(tip);
        }
        const bool selected = engine_.isChannelSelected(channel_);
        const int color = engine_.channelColor(channel_);
        if (selected != selectedDrawn_ || color != colorDrawn_)
        {
            selectedDrawn_ = selected;
            colorDrawn_ = color;
            repaint();
        }
        const float level = showPeak_ ? engine_.peakFor(channel_) : engine_.rmsFor(channel_);
        const bool clip = engine_.clipFor(channel_);
        const int milli = static_cast<int>(level * 1000.0f);
        if (milli != meterMilli_ || clip != meterClip_)
        {
            meterMilli_ = milli;
            meterClip_ = clip;
            if (! meterArea_.isEmpty())
                repaint(meterArea_);
        }
        const float outputShown = showPeak_ ? engine_.outputPeakFor(channel_) : engine_.outputRmsFor(channel_);
        const int outputMilli = static_cast<int>(outputShown * 1000.0f);
        if (outputMilli != outputMilli_)
        {
            outputMilli_ = outputMilli;
            if (! outputMeterArea_.isEmpty())
                repaint(outputMeterArea_);
        }
    }

    void setListenHandler(std::function<void(int)> handler)
    {
        onListen_ = std::move(handler);
    }

    void resized() override
    {
        const auto columns = layoutHostColumns(getWidth());
        const auto row = getLocalBounds().reduced(0, 2);
        number_.setBounds(columns.channel.x, row.getY(), columns.channel.width, row.getHeight());
        arm_.setBounds(columns.mode.x, row.getY() + 2, std::max(0, columns.mode.width - 4), std::max(0, row.getHeight() - 4));
        name_.setBounds(columns.name.x + 3, row.getY() + 3, std::max(0, columns.name.width - 6), std::max(0, row.getHeight() - 6));
        meterArea_ = juce::Rectangle<int>(columns.meter.x + 3, row.getY() + 3,
                                          std::max(0, columns.meter.width - 6), std::max(0, row.getHeight() - 6));
        exclude_.setBounds(columns.exclude.x, row.getY() + 2, std::max(0, columns.exclude.width - 4), std::max(0, row.getHeight() - 4));
        gain_.setBounds(columns.gain.x + 2, row.getY() + 2, std::max(0, columns.gain.width - 4), std::max(0, row.getHeight() - 4));
        outputMeterArea_ = juce::Rectangle<int>(columns.output.x + 2, row.getY() + 3,
                                                std::max(0, columns.output.width - 4), std::max(0, row.getHeight() - 6));
        const int linkWidth = (channel_ & 1) == 0 ? 50 : 0;
        pdc_.setBounds(columns.delay.x, row.getY(), std::max(0, columns.delay.width - linkWidth), row.getHeight());
        link_.setBounds(columns.delay.x + std::max(0, columns.delay.width - 48), row.getY() + 8, 46, 22);
        link_.toFront(false);
        linkedNote_.setBounds(columns.plugins.x + 4, row.getY(), std::max(0, columns.plugins.width - 8), row.getHeight());
        const int slotWidth = kSlotsPerChannel > 0 ? columns.plugins.width / kSlotsPerChannel : 0;
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
            slots_[static_cast<std::size_t>(slot)].setBounds(columns.plugins.x + slot * slotWidth + 2,
                                                            row.getY() + 2,
                                                            std::max(0, slotWidth - 4),
                                                            std::max(0, row.getHeight() - 4));
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

        if (! name_.isBeingEdited())
        {
            // Outside the label. A stroke inside its bounds is covered when the label paints.
            graphics.setColour(juce::Colours::white);
            graphics.drawRoundedRectangle(name_.getBounds().toFloat().expanded(1.5f), 3.0f, 1.0f);
        }

        graphics.setColour(on ? theme::meterTrack : theme::panelEdge.withAlpha(0.45f));
        graphics.fillRoundedRectangle(outputMeterArea_.toFloat(), 2.0f);
        const float outputLevel = showPeak_ ? engine_.outputPeakFor(channel_) : engine_.outputRmsFor(channel_);
        if (on && outputLevel > 0.0f && outputMeterArea_.getHeight() > 1)
        {
            const float filled = normaliseDb(gainToDb(outputLevel), span) * static_cast<float>(outputMeterArea_.getHeight());
            auto bar = outputMeterArea_.toFloat();
            bar.setTop(bar.getBottom() - filled);
            graphics.setColour(meterLevelColour(outputLevel, referenceDb_));
            graphics.fillRoundedRectangle(bar, 2.0f);
        }
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            showChannelMenu(engine_, *this, channel_, nullptr, true);
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
        {
            if (engine_.selectedChannels().size() == 1 && engine_.isChannelSelected(channel_))
                engine_.clearChannelSelection();
            else
                engine_.selectChannel(channel_, false, false);
        }
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
        // No live instance opens the list. After a crash the slot is empty,
        // including ch 1 slot 1, and the click must not try to open a missing editor.
        if (slotClickOpensPluginList(snap.liveInstance, snap.loading))
        {
            if (auto* page = findParentComponentOfClass<PluginPage>())
                page->showPluginList(channel_, slot);
            return;
        }
        if (snap.liveInstance)
            engine_.togglePluginEditor(channel_, slot);
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
        menu.addItem(4, "Vaihda plugin");
        if (snap.outputChannels > 1 && ! engine_.stereoLinked(channel_))
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
                               else if (result == 4)
                               {
                                   if (auto* page = findParentComponentOfClass<PluginPage>())
                                       page->showPluginList(channel_, slot);
                               }
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
    int meterMilli_ = -1;
    bool meterClip_ = false;
    float gainShown_ = 100.0f;
    juce::Label number_;
    juce::Label name_;
    TabKeys tabKeys_;
    juce::TextButton arm_;
    juce::TextButton exclude_;
    LinkButton link_;
    juce::Label linkedNote_;
    juce::Label pdc_;
    OutputTrim gain_;
    std::array<SlotButton, kSlotsPerChannel> slots_;
    juce::Rectangle<int> meterArea_;
    juce::Rectangle<int> outputMeterArea_;
    int outputMilli_ = -1;
    std::function<void(int)> onListen_;
};

class HostColumnHeader : public juce::Component
{
public:
    HostColumnHeader()
    {
        setInterceptsMouseClicks(false, false);
    }

    // Row width can be narrower than this bar when the vertical scrollbar is up.
    // Labels use that width so they stay on the channel columns.
    void setLayoutWidth(int width)
    {
        if (layoutWidth_ == width)
            return;
        layoutWidth_ = width;
        rebuildCache();
        repaint();
    }

    void resized() override
    {
        rebuildCache();
    }

    void paint(juce::Graphics& graphics) override
    {
        if (cache_.isValid())
            graphics.drawImageAt(cache_, 0, 0);
    }

private:
    void rebuildCache()
    {
        const int width = getWidth();
        const int height = getHeight();
        if (width < 1 || height < 1)
            return;
        const int layoutWidth = layoutWidth_ > 0 ? std::min(layoutWidth_, width) : width;
        if (cache_.isValid() && cache_.getWidth() == width && cache_.getHeight() == height && cachedLayout_ == layoutWidth)
            return;

        cache_ = juce::Image(juce::Image::ARGB, width, height, true);
        juce::Graphics graphics(cache_);
        graphics.fillAll(theme::panel);
        graphics.setColour(theme::panelEdge);
        graphics.fillRect(0, height - 1, width, 1);
        graphics.setColour(theme::dim);
        graphics.setFont(juce::Font(juce::FontOptions(11.0f).withStyle("Bold")));
        const auto columns = layoutHostColumns(layoutWidth);
        auto draw = [&graphics, height](const char* text, HostColumnBox box)
        {
            graphics.drawFittedText(text, box.x + 2, 0, std::max(0, box.width - 4), height - 1,
                                    juce::Justification::centredLeft, 1);
        };
        draw(kHostColumnChannel, columns.channel);
        draw(kHostColumnMode, columns.mode);
        draw(kHostColumnName, columns.name);
        const auto delayHeader = delayCompHeaderBox(columns);
        graphics.drawFittedText(kHostColumnDelay, delayHeader.x, 0, std::max(0, delayHeader.width), height - 1,
                                juce::Justification::centredLeft, 1);
        draw(kHostColumnPlugins, columns.plugins);
        draw(kHostColumnGain, columns.gain);
        draw(kHostColumnOutput, columns.output);
        cachedLayout_ = layoutWidth;
    }

    juce::Image cache_;
    int layoutWidth_ = 0;
    int cachedLayout_ = -1;
};

PluginPage::PluginPage(AudioEngine& engine, AppSettings& settings)
    : engine_(engine),
      settings_(settings)
{
    pluginList_ = std::make_unique<PluginListWindow>(engine_, settings_);
    columnHeader_ = std::make_unique<HostColumnHeader>();
    addAndMakeVisible(*columnHeader_);
    addAndMakeVisible(nullButton_);
    nullButton_.setMouseClickGrabsKeyboardFocus(false);
    nullButton_.setTooltip("ALL PLUGIN BYPASS. Bypasses every plugin and the compensation delay, so the USB output is the clean input. Click again to bring the plugins back. Loaded slots blink while this is on. The recorded WAV is always the raw input.");
    nullButton_.onClick = [this]
    {
        engine_.setBypassAll(! engine_.bypassAll());
        refresh();
    };
    addAndMakeVisible(viewport_);
    empty_.setText("This interface has no input channels.", juce::dontSendNotification);
    empty_.setJustificationType(juce::Justification::centred);
    empty_.setColour(juce::Label::textColourId, theme::dim);
    content_.addAndMakeVisible(empty_);
    content_.addMouseListener(this, false);
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

void PluginPage::setListenHandler(std::function<void(int channel)> handler)
{
    listenHandler_ = std::move(handler);
    for (auto& row : rows_)
        row->setListenHandler(listenHandler_);
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
            auto row = std::make_unique<Row>(engine_, item.channel, listenHandler_);
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

void PluginPage::refreshChannel(int channel)
{
    const int channels = std::max(0, engine_.visibleChannels());
    if (channels != channels_ || engine_.displayRevision() != revision_)
        rebuild();

    for (auto& row : rows_)
        if (row != nullptr && row->isChannel(channel))
            row->refresh();

    const int group = engine_.channelGroup(channel);
    if (group < 0)
        return;
    for (auto& header : headers_)
        if (header != nullptr && header->groupId() == group)
            header->refresh();
}

void PluginPage::refresh()
{
    const int channels = std::max(0, engine_.visibleChannels());
    if (channels != channels_ || engine_.displayRevision() != revision_)
        rebuild();

    empty_.setVisible(channels_ == 0);
    const bool bypass = engine_.bypassAll();
    nullButton_.setButtonText("ALL PLUGIN BYPASS");
    nullButton_.setToggleState(bypass, juce::dontSendNotification);
    nullButton_.setColour(juce::TextButton::buttonColourId, bypass ? juce::Colour(0xffd48a20) : theme::button);
    nullButton_.setColour(juce::TextButton::textColourOffId, bypass ? juce::Colours::white : theme::text);
    for (auto& row : rows_)
        row->refresh();
    for (auto& header : headers_)
        header->refresh();
}

void PluginPage::mouseDown(const juce::MouseEvent& event)
{
    if (event.eventComponent == &content_)
        engine_.clearChannelSelection();
}

void PluginPage::resized()
{
    auto bounds = getLocalBounds();
    auto bar = bounds.removeFromTop(32);
    nullButton_.setBounds(bar.removeFromLeft(176).reduced(0, 4));
    auto list = bounds;
    auto headerArea = list.removeFromTop(kHostColumnHeaderHeight);
    viewport_.setBounds(list);
    int height = 0;
    for (int rowHeight : heights_)
        height += rowHeight;
    const int contentHeight = std::max(viewport_.getHeight(), height);
    int contentWidth = std::max(1, viewport_.getMaximumVisibleWidth());
    content_.setSize(contentWidth, contentHeight);
    contentWidth = std::max(1, viewport_.getMaximumVisibleWidth());
    if (content_.getWidth() != contentWidth)
        content_.setSize(contentWidth, contentHeight);
    if (columnHeader_ != nullptr)
    {
        columnHeader_->setLayoutWidth(contentWidth);
        columnHeader_->setBounds(headerArea);
        columnHeader_->toFront(false);
    }
    empty_.setBounds(content_.getLocalBounds().reduced(24));
    auto area = content_.getLocalBounds();
    for (std::size_t index = 0; index < order_.size(); ++index)
        order_[index]->setBounds(area.removeFromTop(heights_[index]));
}

} // namespace youhost
