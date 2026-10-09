#include "ScenesWindow.h"
#include "Theme.h"
#include "WindowMemory.h"
#include "engine/SceneRecall.h"

namespace youhost
{

class ActivityLight : public juce::Component,
                       public juce::SettableTooltipClient
{
public:
    void setLit(bool lit)
    {
        if (lit_ == lit)
            return;
        lit_ = lit;
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        auto dot = getLocalBounds().toFloat().reduced(4.0f);
        graphics.setColour(lit_ ? theme::green : theme::panelEdge);
        graphics.fillEllipse(dot);
    }

private:
    bool lit_ = false;
};

class ScenesWindow::Content : public juce::Component,
                              private juce::ListBoxModel
{
public:
    explicit Content(AudioEngine& engine)
        : engine_(engine)
    {
        addAndMakeVisible(list_);
        list_.setModel(this);
        list_.setRowHeight(24);
        list_.setColour(juce::ListBox::backgroundColourId, theme::background);
        list_.setColour(juce::ListBox::outlineColourId, theme::panelEdge);

        for (auto* button : { &store_, &storeNew_, &recall_, &rename_, &remove_, &follow_ })
        {
            addAndMakeVisible(*button);
            button->setMouseClickGrabsKeyboardFocus(false);
        }
        follow_.setClickingTogglesState(true);

        addAndMakeVisible(remoteLabel_);
        addAndMakeVisible(remoteEditor_);
        addAndMakeVisible(channelLabel_);
        addAndMakeVisible(channelBox_);
        addAndMakeVisible(deviceLabel_);
        addAndMakeVisible(deviceBox_);
        addAndMakeVisible(activity_);
        addAndMakeVisible(hint_);

        remoteLabel_.setText("Remote #", juce::dontSendNotification);
        remoteLabel_.setColour(juce::Label::textColourId, theme::dim);
        remoteLabel_.setJustificationType(juce::Justification::centredLeft);
        channelLabel_.setText("MIDI channel", juce::dontSendNotification);
        channelLabel_.setColour(juce::Label::textColourId, theme::dim);
        deviceLabel_.setText("MIDI input", juce::dontSendNotification);
        deviceLabel_.setColour(juce::Label::textColourId, theme::dim);
        hint_.setText("Program Change recalls the scene with that Remote #. SAFE channels stay as they are. OFF is the silent state a scene stores.",
                      juce::dontSendNotification);
        hint_.setColour(juce::Label::textColourId, theme::fainter);
        hint_.setJustificationType(juce::Justification::centredLeft);

        remoteEditor_.setColour(juce::TextEditor::backgroundColourId, theme::panel);
        remoteEditor_.setColour(juce::TextEditor::textColourId, theme::text);
        remoteEditor_.setColour(juce::TextEditor::outlineColourId, theme::panelEdge);
        remoteEditor_.setInputRestrictions(3, "0123456789");
        remoteEditor_.setTooltip("Program Change number, 0 to 127. Empty means this scene ignores Program Change.");

        for (int channel = 1; channel <= 16; ++channel)
            channelBox_.addItem(juce::String(channel), channel);

        store_.onClick = [this]
        {
            const int row = list_.getSelectedRow();
            if (row < 0)
                return;
            engine_.storeScene(row);
            reload();
        };
        storeNew_.onClick = [this]
        {
            engine_.storeNewScene();
            reload();
            if (engine_.sceneCount() > 0)
                list_.selectRow(engine_.sceneCount() - 1);
        };
        recall_.onClick = [this] { recallSelected(); };
        rename_.onClick = [this] { renameSelected(); };
        remove_.onClick = [this] { deleteSelected(); };
        follow_.onClick = [this]
        {
            engine_.setMidiFollow(follow_.getToggleState());
            if (! engine_.midiFollow())
                activity_.setLit(false);
        };
        follow_.setTooltip("Follow Program Change on the MIDI channel. The desk sends that when its scene changes.");
        channelBox_.onChange = [this]
        {
            if (channelBox_.getSelectedId() > 0)
                engine_.setMidiFollowChannel(channelBox_.getSelectedId());
        };
        deviceBox_.onChange = [this] { applyDevice(); };
        remoteEditor_.onReturnKey = [this] { commitRemote(); };
        remoteEditor_.onFocusLost = [this] { commitRemote(); };

        store_.setTooltip("Replace the selected scene with the current plugins, bypass, gain, and REC / INPUT / OFF.");
        storeNew_.setTooltip("Store the current mix as a new scene.");
        recall_.setTooltip("Recall the selected scene. The same plugin keeps its instance and only takes the stored settings.");
        rename_.setTooltip("Rename the selected scene.");
        remove_.setTooltip("Delete the selected scene.");
        activity_.setTooltip("Lights when a Program Change arrives on the MIDI channel.");

        follow_.setToggleState(engine_.midiFollow(), juce::dontSendNotification);
        channelBox_.setSelectedId(engine_.midiFollowChannel(), juce::dontSendNotification);
        refreshDevices();
        reload();
    }

    ~Content() override
    {
        list_.setModel(nullptr);
    }

    void refresh()
    {
        const int revision = engine_.sceneRevision();
        if (revision != revision_)
        {
            revision_ = revision;
            list_.updateContent();
            list_.repaint();
            if (! remoteEditor_.hasKeyboardFocus(true))
                showRemote();
            const bool follow = engine_.midiFollow();
            if (follow_.getToggleState() != follow)
                follow_.setToggleState(follow, juce::dontSendNotification);
            if (channelBox_.getSelectedId() != engine_.midiFollowChannel())
                channelBox_.setSelectedId(engine_.midiFollowChannel(), juce::dontSendNotification);
        }

        const bool lit = engine_.midiActivityLit();
        if (lit != activityDrawn_)
        {
            activityDrawn_ = lit;
            activity_.setLit(lit);
        }

        const bool hasRow = list_.getSelectedRow() >= 0;
        if (store_.isEnabled() != hasRow)
            store_.setEnabled(hasRow);
        if (recall_.isEnabled() != hasRow)
            recall_.setEnabled(hasRow);
        if (rename_.isEnabled() != hasRow)
            rename_.setEnabled(hasRow);
        if (remove_.isEnabled() != hasRow)
            remove_.setEnabled(hasRow);
        const bool room = engine_.sceneCount() < kMaxScenes;
        if (storeNew_.isEnabled() != room)
            storeNew_.setEnabled(room);

        if (++deviceTick_ >= 30)
        {
            deviceTick_ = 0;
            refreshDevices();
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12, 10);
        hint_.setBounds(area.removeFromBottom(36));
        area.removeFromBottom(6);
        auto deviceRow = area.removeFromBottom(28);
        deviceLabel_.setBounds(deviceRow.removeFromLeft(84));
        deviceBox_.setBounds(deviceRow.reduced(0, 2));
        area.removeFromBottom(6);
        auto followRow = area.removeFromBottom(28);
        activity_.setBounds(followRow.removeFromRight(22));
        followRow.removeFromRight(6);
        channelBox_.setBounds(followRow.removeFromRight(72).reduced(0, 2));
        followRow.removeFromRight(6);
        channelLabel_.setBounds(followRow.removeFromRight(96));
        follow_.setBounds(followRow.reduced(0, 2));
        area.removeFromBottom(6);
        auto remoteRow = area.removeFromBottom(28);
        remoteLabel_.setBounds(remoteRow.removeFromLeft(84));
        remoteEditor_.setBounds(remoteRow.removeFromLeft(72).reduced(0, 2));
        area.removeFromBottom(8);
        auto buttons = area.removeFromBottom(30);
        const int gap = 4;
        const int width = std::max(52, (buttons.getWidth() - gap * 4) / 5);
        store_.setBounds(buttons.removeFromLeft(width));
        buttons.removeFromLeft(gap);
        storeNew_.setBounds(buttons.removeFromLeft(width));
        buttons.removeFromLeft(gap);
        recall_.setBounds(buttons.removeFromLeft(width));
        buttons.removeFromLeft(gap);
        rename_.setBounds(buttons.removeFromLeft(width));
        buttons.removeFromLeft(gap);
        remove_.setBounds(buttons);
        area.removeFromBottom(8);
        list_.setBounds(area);
    }

private:
    int getNumRows() override
    {
        return engine_.sceneCount();
    }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool selected) override
    {
        if (row < 0 || row >= engine_.sceneCount())
            return;
        if (selected)
        {
            graphics.setColour(theme::buttonOn);
            graphics.fillRect(0, 0, width, height);
        }
        else if (row == engine_.recalledScene())
        {
            graphics.setColour(theme::panel);
            graphics.fillRect(0, 0, width, height);
        }

        if (row == engine_.recalledScene())
        {
            graphics.setColour(theme::amber);
            graphics.fillRect(0, 0, 4, height);
        }

        auto text = juce::String(row + 1) + "   " + engine_.sceneName(row);
        const int remote = engine_.sceneRemote(row);
        if (remote >= 0)
            text += "    " + juce::String(remote);
        if (row == engine_.recalledScene() && engine_.sceneDrift())
            text += "  *";

        graphics.setColour(selected ? juce::Colours::white : theme::text);
        graphics.setFont(juce::Font(juce::FontOptions(14.0f)));
        graphics.drawText(text, 12, 0, width - 16, height, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent&) override
    {
        juce::ignoreUnused(row);
        showRemote();
    }

    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override
    {
        recallSelected();
    }

    void reload()
    {
        revision_ = engine_.sceneRevision();
        list_.updateContent();
        list_.repaint();
        showRemote();
    }

    void showRemote()
    {
        const int row = list_.getSelectedRow();
        if (row < 0 || row >= engine_.sceneCount())
        {
            remoteEditor_.setText({}, juce::dontSendNotification);
            remoteEditor_.setEnabled(false);
            return;
        }
        remoteEditor_.setEnabled(true);
        const int remote = engine_.sceneRemote(row);
        const auto text = remote >= 0 ? juce::String(remote) : juce::String();
        if (remoteEditor_.getText() != text)
            remoteEditor_.setText(text, juce::dontSendNotification);
    }

    void commitRemote()
    {
        const int row = list_.getSelectedRow();
        if (row < 0)
            return;
        const auto text = remoteEditor_.getText().trim();
        const int program = text.isEmpty() ? -1 : clampRemoteProgram(text.getIntValue());
        engine_.setSceneRemote(row, program);
        showRemote();
    }

    void recallSelected()
    {
        const int row = list_.getSelectedRow();
        if (row < 0)
            return;
        engine_.recallScene(row);
        reload();
    }

    void renameSelected()
    {
        const int row = list_.getSelectedRow();
        if (row < 0)
            return;
        auto* window = new juce::AlertWindow("Rename scene", "The name is only a label.", juce::MessageBoxIconType::QuestionIcon);
        window->addTextEditor("name", engine_.sceneName(row), "Name");
        window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<Content> safe(this);
        window->enterModalState(true,
                                juce::ModalCallbackFunction::create([safe, window, row](int result)
                                {
                                    if (safe == nullptr || result != 1)
                                        return;
                                    safe->engine_.renameScene(row, window->getTextEditorContents("name"));
                                    safe->reload();
                                }),
                                true);
    }

    void deleteSelected()
    {
        const int row = list_.getSelectedRow();
        if (row < 0)
            return;
        juce::Component::SafePointer<Content> safe(this);
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon,
                                           "Delete scene?",
                                           "Delete \"" + engine_.sceneName(row) + "\"? The current mix stays as it is.",
                                           "Delete",
                                           "Cancel",
                                           nullptr,
                                           juce::ModalCallbackFunction::create([safe, row](int result)
                                           {
                                               if (safe == nullptr || result != 1)
                                                   return;
                                               safe->engine_.deleteScene(row);
                                               safe->reload();
                                           }));
    }

    void refreshDevices()
    {
        if (deviceBox_.isPopupActive())
            return;
        const auto devices = engine_.midiInputs();
        juce::String joined;
        for (const auto& device : devices)
            joined << device.identifier << "\n";
        if (devicesReady_ && joined == deviceList_)
            return;
        devicesReady_ = true;
        deviceList_ = joined;

        const auto current = engine_.midiFollowDevice();
        deviceIds_.clear();
        deviceIds_.add({});
        deviceBox_.clear(juce::dontSendNotification);
        deviceBox_.addItem("No MIDI input", 1);
        int selected = 1;
        int id = 2;
        for (const auto& device : devices)
        {
            const auto label = device.name.isNotEmpty() ? device.name : device.identifier;
            deviceBox_.addItem(label, id);
            deviceIds_.add(device.identifier);
            if (device.identifier == current)
                selected = id;
            ++id;
        }
        deviceBox_.setSelectedId(selected, juce::dontSendNotification);
    }

    void applyDevice()
    {
        const int id = deviceBox_.getSelectedId();
        if (id <= 1)
        {
            engine_.setMidiFollowDevice({});
            return;
        }
        const int index = id - 1;
        if (index >= 0 && index < deviceIds_.size())
            engine_.setMidiFollowDevice(deviceIds_[index]);
    }

    AudioEngine& engine_;
    juce::ListBox list_;
    juce::TextButton store_ { "Store" };
    juce::TextButton storeNew_ { "Store New" };
    juce::TextButton recall_ { "Recall" };
    juce::TextButton rename_ { "Rename" };
    juce::TextButton remove_ { "Delete" };
    juce::TextButton follow_ { "Follow Program Change" };
    juce::Label remoteLabel_;
    juce::TextEditor remoteEditor_;
    juce::Label channelLabel_;
    juce::ComboBox channelBox_;
    juce::Label deviceLabel_;
    juce::ComboBox deviceBox_;
    ActivityLight activity_;
    juce::Label hint_;
    juce::StringArray deviceIds_;
    juce::String deviceList_;
    bool devicesReady_ = false;
    int revision_ = -1;
    int deviceTick_ = 0;
    bool activityDrawn_ = false;
};

ScenesWindow::ScenesWindow(AudioEngine& engine, AppSettings& settings)
    : juce::DocumentWindow("SCENES", theme::panel, juce::DocumentWindow::closeButton),
      engine_(engine),
      settings_(settings)
{
    auto content = std::make_unique<Content>(engine_);
    content_ = content.get();
    setUsingNativeTitleBar(true);
    setContentOwned(content.release(), true);
    prepareRememberedWindow(*this, settings_, "windowScenes", 560, 520, 440, 380);
    setVisible(false);
}

ScenesWindow::~ScenesWindow()
{
    stopTimer();
    saveRememberedWindow(*this, settings_, "windowScenes");
}

void ScenesWindow::toggle()
{
    const bool show = ! isVisible();
    setVisible(show);
    if (show)
    {
        startTimerHz(15);
        if (content_ != nullptr)
            content_->refresh();
        toFront(true);
    }
    else
    {
        stopTimer();
    }
}

void ScenesWindow::closeButtonPressed()
{
    stopTimer();
    saveRememberedWindow(*this, settings_, "windowScenes");
    setVisible(false);
}

void ScenesWindow::timerCallback()
{
    if (! isVisible())
    {
        stopTimer();
        return;
    }
    if (content_ != nullptr)
        content_->refresh();
}

} // namespace youhost
