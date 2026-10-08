#include "MainComponent.h"
#include "ChannelMenu.h"
#include "Theme.h"
#include "WindowMemory.h"
#include "engine/LatencyMath.h"
#include "engine/MeterScale.h"

#include <vector>

namespace youhost
{
namespace
{

constexpr const char* kShortcutHelp =
    "1  Recorder\n"
    "2  Plugins\n"
    "3  Open or close the plugin scanner\n"
    "4 or D  Open or close the dropout timeline\n"
    "Space  Play, or Stop when already playing or recording\n"
    "Shift+Space, R, or Cmd+Space  Record\n"
    "Left / Right  Previous or next take\n"
    "Shift+Left / Shift+Right  Move 5 seconds\n"
    "Cmd+S  Save\n"
    "Cmd+Shift+S  Save a copy of the session folder\n\n"
    "Cmd+Space works only when Spotlight is not using that shortcut. Shift+Space and R always work.\n"
    "Shortcuts do nothing while a text field or a plugin window has focus.";

void hideTestButtons(juce::Component& component)
{
    for (auto* child : component.getChildren())
    {
        if (child == nullptr)
            continue;
        if (auto* button = dynamic_cast<juce::TextButton*>(child))
        {
            if (button->getButtonText() == "Test")
            {
                button->setVisible(false);
                button->setEnabled(false);
                button->onClick = nullptr;
            }
        }
        hideTestButtons(*child);
    }
}

int referenceIdFor(int db)
{
    if (db == -14)
        return 1;
    if (db == -18)
        return 2;
    return 3;
}

int referenceDbFor(int id)
{
    if (id == 1)
        return -14;
    if (id == 2)
        return -18;
    return kDefaultRmsReferenceDb;
}

void showRadio(juce::TextButton& rms, juce::TextButton& peak, bool showPeak)
{
    rms.setToggleState(! showPeak, juce::dontSendNotification);
    peak.setToggleState(showPeak, juce::dontSendNotification);
}

void quiet(juce::Button& button)
{
    button.setMouseClickGrabsKeyboardFocus(false);
    button.setWantsKeyboardFocus(false);
}

class FloatWindow : public juce::DocumentWindow
{
public:
    FloatWindow(const juce::String& title,
                juce::Component& content,
                AppSettings& settings,
                const juce::String& key,
                int width,
                int height,
                int minWidth,
                int minHeight)
        : juce::DocumentWindow(title, theme::panel, juce::DocumentWindow::closeButton),
          settings_(settings),
          key_(key)
    {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&content, false);
        prepareRememberedWindow(*this, settings_, key_, width, height, minWidth, minHeight);
        setVisible(false);
    }

    ~FloatWindow() override
    {
        saveRememberedWindow(*this, settings_, key_);
    }

    void closeButtonPressed() override
    {
        saveRememberedWindow(*this, settings_, key_);
        setVisible(false);
    }

private:
    AppSettings& settings_;
    juce::String key_;
};

class SetupWindow : public juce::DocumentWindow
{
public:
    SetupWindow(juce::AudioDeviceSelectorComponent& selector, AppSettings& settings)
        : juce::DocumentWindow("Audio setup", theme::background, juce::DocumentWindow::closeButton),
          settings_(settings),
          selector_(selector)
    {
        viewport_.setViewedComponent(&selector_, false);
        viewport_.setScrollBarsShown(true, true);
        setUsingNativeTitleBar(true);
        setContentNonOwned(&viewport_, false);
        prepareRememberedWindow(*this, settings_, "windowSetup", 760, 640, 560, 420);
        setVisible(false);
    }

    ~SetupWindow() override
    {
        saveRememberedWindow(*this, settings_, "windowSetup");
        viewport_.setViewedComponent(nullptr, false);
    }

    void closeButtonPressed() override
    {
        saveRememberedWindow(*this, settings_, "windowSetup");
        setVisible(false);
    }

    void resized() override
    {
        juce::DocumentWindow::resized();
        const int width = std::max(520, viewport_.getMaximumVisibleWidth());
        selector_.setSize(width, std::max(selector_.getHeight(), viewport_.getMaximumVisibleHeight()));
    }

private:
    AppSettings& settings_;
    juce::AudioDeviceSelectorComponent& selector_;
    juce::Viewport viewport_;
};

void gatherToggleLists(juce::Component& component, std::vector<std::vector<juce::ToggleButton*>>& lists)
{
    std::vector<juce::ToggleButton*> row;
    for (auto* child : component.getChildren())
        if (auto* toggle = dynamic_cast<juce::ToggleButton*>(child))
            row.push_back(toggle);
    if (! row.empty())
        lists.push_back(std::move(row));

    for (auto* child : component.getChildren())
        if (child != nullptr && dynamic_cast<juce::ToggleButton*>(child) == nullptr)
            gatherToggleLists(*child, lists);
}

} // namespace

struct MainComponent::KeyProxy : juce::KeyListener
{
    explicit KeyProxy(MainComponent& ownerIn)
        : owner(ownerIn)
    {
    }

    bool keyPressed(const juce::KeyPress& key, juce::Component* originating) override
    {
        return owner.handleKey(key, originating);
    }

    MainComponent& owner;
};

class MainComponent::FileMenu : public juce::MenuBarModel
{
public:
    explicit FileMenu(MainComponent& ownerIn)
        : owner(ownerIn)
    {
    }

    juce::StringArray getMenuBarNames() override
    {
        return { "File" };
    }

    juce::PopupMenu getMenuForIndex(int, const juce::String&) override
    {
        juce::PopupMenu menu;
        menu.addItem(1, "New");
        menu.addItem(2, "Open…");

        juce::PopupMenu recent;
        const auto sessions = owner.engine_.recentSessions();
        for (int index = 0; index < sessions.size() && index < 10; ++index)
        {
            juce::PopupMenu::Item item;
            item.itemID = 100 + index;
            item.text = juce::File(sessions[index]).getFileName();
            recent.addItem(item);
        }
        menu.addSubMenu("Open Recent", recent, ! sessions.isEmpty());
        menu.addItem(5, "Import Recording Folder…");
        menu.addSeparator();

        juce::PopupMenu::Item save;
        save.itemID = 3;
        save.text = "Save";
        save.shortcutKeyDescription = "Cmd+S";
        menu.addItem(save);

        juce::PopupMenu::Item saveAs;
        saveAs.itemID = 4;
        saveAs.text = "Save As…";
        saveAs.shortcutKeyDescription = "Cmd+Shift+S";
        menu.addItem(saveAs);
        menu.addSeparator();
        menu.addItem(6, "Clear Timeline…");
        return menu;
    }

    void menuItemSelected(int id, int) override
    {
        if (id == 1)
            owner.engine_.startNewSession();
        else if (id == 2)
            owner.openSession();
        else if (id == 3)
            owner.saveSession();
        else if (id == 4)
            owner.saveSessionAs();
        else if (id == 5)
            owner.importRecordings();
        else if (id == 6)
            owner.confirmClearTimeline();
        else if (id >= 100 && id < 110)
            owner.openRecent(id - 100);
        owner.refresh();
    }

private:
    MainComponent& owner;
};

MainComponent::MainComponent(AudioEngine& engine, AppSettings& settings)
    : engine_(engine),
      settings_(settings),
      pluginPage_(engine, settings),
      scanner_(engine, settings),
      dropouts_(engine, settings),
      deviceSelector_(engine.deviceManager(), 0, kMaxChannels, 0, kMaxChannels, false, false, false, false)
{
    setOpaque(true);
    setWantsKeyboardFocus(true);
    keys_ = std::make_unique<KeyProxy>(*this);
    showPeak_ = false;
    rmsReferenceDb_ = settings_.loadRmsReferenceDb();
    engine_.setSessionMeters(false, rmsReferenceDb_);
    engine_.setMeterRestoreHandler([this](bool peak, int reference)
    {
        rmsReferenceDb_ = normaliseRmsReferenceDb(reference);
        referenceBox_.setSelectedId(referenceIdFor(rmsReferenceDb_), juce::dontSendNotification);
        setPeakMode(peak, false);
    });
    engine_.setPageRestoreHandler([this](int page) { showPage(page); });

    addAndMakeVisible(meterViewport_);
    addAndMakeVisible(leftScale_);
    addAndMakeVisible(rightScale_);
    addAndMakeVisible(timeline_);
    addAndMakeVisible(pluginPage_);
    addAndMakeVisible(recorderButton_);
    addAndMakeVisible(pluginsButton_);
    addAndMakeVisible(scannerButton_);
    addAndMakeVisible(prevButton_);
    addAndMakeVisible(nextButton_);
    addAndMakeVisible(stopButton_);
    addAndMakeVisible(playButton_);
    addAndMakeVisible(recButton_);
    addAndMakeVisible(timeLabel_);
    addAndMakeVisible(modeLabel_);
    addAndMakeVisible(helpButton_);
    addAndMakeVisible(rmsButton_);
    addAndMakeVisible(peakButton_);
    addAndMakeVisible(referenceLabel_);
    addAndMakeVisible(referenceBox_);
    addAndMakeVisible(bitDepthLabel_);
    addAndMakeVisible(bitDepthBox_);
    addAndMakeVisible(clearClipsButton_);
    addAndMakeVisible(newButton_);
    addAndMakeVisible(openButton_);
    addAndMakeVisible(saveButton_);
    addAndMakeVisible(fileButton_);
    addAndMakeVisible(dropoutsButton_);
    addAndMakeVisible(setupButton_);
    addAndMakeVisible(latencyButton_);
    addAndMakeVisible(retryButton_);
    addAndMakeVisible(allButton_);
    addAndMakeVisible(hideButton_);
    addAndMakeVisible(latencyLabel_);
    meterViewport_.setViewedComponent(&meterGrid_, false);
    meterViewport_.setScrollBarsShown(false, true);

    latencyWindow_ = std::make_unique<FloatWindow>("Latency", latencyReadout_, settings_, "windowLatency", 480, 360, 420, 280);
    setupWindow_ = std::make_unique<SetupWindow>(deviceSelector_, settings_);
    latencyReadout_.setResetHandler([this] { engine_.resetDropouts(); });
    latencyReadout_.setGraphHandler([this] { toggleDropouts(); });
    fileMenu_ = std::make_unique<FileMenu>(*this);
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(fileMenu_.get());
#endif

    for (auto* button : { &recorderButton_, &pluginsButton_, &scannerButton_, &prevButton_, &nextButton_,
                          &stopButton_, &playButton_, &recButton_, &helpButton_, &rmsButton_, &peakButton_,
                          &clearClipsButton_, &newButton_, &openButton_, &saveButton_, &fileButton_,
                          &dropoutsButton_, &setupButton_, &latencyButton_, &retryButton_, &allButton_, &hideButton_ })
        quiet(*button);

    for (auto* button : { &recorderButton_, &pluginsButton_, &scannerButton_, &dropoutsButton_, &latencyButton_,
                          &setupButton_, &allButton_, &hideButton_ })
        button->setColour(juce::TextButton::buttonOnColourId, theme::buttonOn);

    recorderButton_.onClick = [this] { showPage(1); };
    pluginsButton_.onClick = [this] { showPage(2); };
    scannerButton_.onClick = [this] { toggleScanner(); };
    prevButton_.onClick = [this] { engine_.transportJump(-1); };
    nextButton_.onClick = [this] { engine_.transportJump(1); };
    stopButton_.onClick = [this] { engine_.transportStop(); };
    playButton_.onClick = [this] { engine_.transportPlay(); };
    recButton_.onClick = [this] { engine_.transportRecord(); };
    helpButton_.onClick = [this] { showHelp(); };
    newButton_.onClick = [this] { engine_.startNewSession(); refresh(); };
    openButton_.onClick = [this] { openSession(); };
    saveButton_.onClick = [this] { saveSession(); };
    fileButton_.onClick = [this] { showFileMenu(); };
    dropoutsButton_.onClick = [this] { toggleDropouts(); };
    recorderButton_.setTooltip("Recorder page (1).");
    pluginsButton_.setTooltip("Plugins page (2).");
    dropoutsButton_.setTooltip("Dropout timeline (4 or D).");
    latencyButton_.setTooltip("Open the latency card.");
    fileButton_.setTooltip("New, Open, Open Recent, Save As, Import Recording Folder, and Clear Timeline.");
    helpButton_.setTooltip("Show keyboard shortcuts.");
    allButton_.setTooltip("Show every channel. Opens every group.");
    hideButton_.setTooltip("Fold every channel that belongs to a group. Channels with no group stay visible.");
    saveButton_.setTooltip("Save session.youhost (Cmd+S).");
    latencyButton_.onClick = [this]
    {
        const bool show = latencyWindow_ == nullptr || ! latencyWindow_->isVisible();
        if (latencyWindow_ != nullptr)
        {
            latencyWindow_->setVisible(show);
            if (show)
                latencyWindow_->toFront(true);
        }
    };

    prevButton_.setTooltip("Previous take (Left).");
    nextButton_.setTooltip("Next take (Right).");
    stopButton_.setTooltip("Stop (Space).");
    playButton_.setTooltip("Play the recorded takes through the plugins (Space). Channels that are OFF stay silent.");
    recButton_.setTooltip("Record a new take (Shift+Space, R, or Cmd+Space).");
    helpButton_.setTooltip(kShortcutHelp);
    scannerButton_.setTooltip("Open or close the plugin scanner (3).");
    allButton_.onClick = [this] { engine_.expandAllGroups(); refresh(); };
    hideButton_.onClick = [this] { engine_.hideGroupedChannels(); refresh(); };
    recButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff8d2430));
    playButton_.setColour(juce::TextButton::buttonOnColourId, theme::buttonOn);

    timeLabel_.setFont(juce::Font(juce::FontOptions(28.0f).withStyle("Bold")));
    timeLabel_.setJustificationType(juce::Justification::centred);
    timeLabel_.setText("00:00:00", juce::dontSendNotification);
    modeLabel_.setFont(juce::Font(juce::FontOptions(13.0f).withStyle("Bold")));
    modeLabel_.setJustificationType(juce::Justification::centredLeft);
    latencyLabel_.setFont(juce::Font(juce::FontOptions(12.0f)));
    latencyLabel_.setJustificationType(juce::Justification::centredRight);
    latencyLabel_.setColour(juce::Label::textColourId, theme::dim);

    deviceSelector_.setItemHeight(22);

    rmsButton_.setRadioGroupId(1);
    peakButton_.setRadioGroupId(1);
    rmsButton_.setClickingTogglesState(true);
    peakButton_.setClickingTogglesState(true);
    showRadio(rmsButton_, peakButton_, false);
    referenceLabel_.setJustificationType(juce::Justification::centredRight);
    referenceLabel_.setFont(juce::Font(juce::FontOptions(13.0f)));
    referenceBox_.addItem("-14 dBFS", 1);
    referenceBox_.addItem("-18 dBFS", 2);
    referenceBox_.addItem("-20 dBFS", 3);
    referenceBox_.setSelectedId(referenceIdFor(rmsReferenceDb_), juce::dontSendNotification);
    referenceBox_.setTooltip("Line level for the RMS scale. 0 VU sits at this many dBFS. Meters are green below this point and yellow above it.");
    bitDepthLabel_.setJustificationType(juce::Justification::centredRight);
    bitDepthLabel_.setFont(juce::Font(juce::FontOptions(13.0f)));
    bitDepthBox_.addItem("16-bit", 16);
    bitDepthBox_.addItem("24-bit", 24);
    bitDepthBox_.addItem("32-bit float", 32);
    bitDepthBox_.setSelectedId(engine_.wavBitDepth(), juce::dontSendNotification);
    bitDepthBox_.setTooltip("Bit depth for the next take. 24-bit is the default. 32-bit is float. A take that is already recording keeps its depth.");
    bitDepthBox_.onChange = [this]
    {
        const int chosen = bitDepthBox_.getSelectedId();
        if (chosen > 0)
            engine_.setWavBitDepth(chosen, true);
    };

    rmsButton_.onClick = [this] { setPeakMode(false, true); };
    peakButton_.onClick = [this] { setPeakMode(true, true); };
    referenceBox_.onChange = [this] { setRmsReference(referenceDbFor(referenceBox_.getSelectedId()), true); };
    clearClipsButton_.onClick = [this] { engine_.requestClipClearAll(); };
    clearClipsButton_.setTooltip("Clear every latched clip mark");
    setupButton_.onClick = [this] { toggleSetup(); };
    setupButton_.setTooltip("Channel ticks are the same as REC and OFF. The audio device stays open.");

    meterGrid_.setClearHandler([this](int channel)
    {
        if (engine_.clipFor(channel))
            engine_.requestClipClear(channel);
    });
    meterGrid_.setRecordHandler([this](int channel)
    {
        engine_.setRecordArmed(channel, ! engine_.isRecordArmed(channel));
    });
    meterGrid_.setChannelMenuHandler([this](int channel) { showChannelMenu(engine_, meterGrid_, channel); });
    meterGrid_.setGroupMenuHandler([this](int group) { showGroupMenu(engine_, meterGrid_, group); });
    meterGrid_.setGroupRenameHandler([this](int group) { renameGroup(engine_, group); });
    meterGrid_.setGroupToggleHandler([this](int group) { engine_.toggleGroupCollapsed(group); });
    meterGrid_.setSelectHandler([this](int channel, bool extend) { engine_.selectChannel(channel, extend); });
    timeline_.setLocateHandler([this](std::int64_t sample) { engine_.transportLocate(sample); });

    retryButton_.onClick = [this]
    {
        juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio,
                                          [this](bool granted)
                                          {
                                              juce::MessageManager::callAsync([this, granted]
                                              {
                                                  engine_.start(granted);
                                                  resized();
                                                  refresh();
                                              });
                                          });
    };

    startTimerHz(30);
    hideDeviceTestTone();
    engine_.pluginCatalogue().startIfEmpty();
    showPage(1);
    refresh();
}

MainComponent::~MainComponent()
{
    stopTimer();
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    setupWindow_.reset();
    fileMenu_.reset();
    if (keyTarget_ != nullptr && keys_ != nullptr)
        keyTarget_->removeKeyListener(keys_.get());
    engine_.setMeterRestoreHandler(nullptr);
    engine_.setPageRestoreHandler(nullptr);
    latencyWindow_.reset();
}

void MainComponent::parentHierarchyChanged()
{
    auto* top = getTopLevelComponent();
    if (top == keyTarget_)
        return;
    if (keyTarget_ != nullptr && keys_ != nullptr)
        keyTarget_->removeKeyListener(keys_.get());
    keyTarget_ = top;
    if (keyTarget_ != nullptr && keys_ != nullptr)
        keyTarget_->addKeyListener(keys_.get());
    grabKeyboardFocus();
}

bool MainComponent::shortcutBlocked(juce::Component* originating) const
{
    if (originating == nullptr)
        return false;
    if (dynamic_cast<juce::TextEditor*>(originating) != nullptr)
        return true;
    if (originating->findParentComponentOfClass<juce::TextEditor>() != nullptr)
        return true;
    if (dynamic_cast<juce::AudioProcessorEditor*>(originating) != nullptr)
        return true;
    if (originating->findParentComponentOfClass<juce::AudioProcessorEditor>() != nullptr)
        return true;
    return false;
}

bool MainComponent::handleKey(const juce::KeyPress& key, juce::Component* originating)
{
    if (shortcutBlocked(originating))
        return false;

    const auto character = juce::CharacterFunctions::toLowerCase(key.getTextCharacter());
    const bool shift = key.getModifiers().isShiftDown();
    const bool command = key.getModifiers().isCommandDown();

    if (character == '1' && ! shift && ! command)
    {
        showPage(1);
        return true;
    }
    if (character == '2' && ! shift && ! command)
    {
        showPage(2);
        return true;
    }
    if (character == '3' && ! shift && ! command)
    {
        toggleScanner();
        return true;
    }
    if ((character == '4' || character == 'd') && ! shift && ! command)
    {
        toggleDropouts();
        return true;
    }
    if (character == 's' && command)
    {
        if (shift)
            saveSessionAs();
        else
            saveSession();
        return true;
    }

    if (key.isKeyCode(juce::KeyPress::spaceKey))
    {
        const auto mode = engine_.transportView().mode;
        if (shift || command || (character == 'r'))
            engine_.transportRecord();
        else if (mode == TransportMode::stopped)
            engine_.transportPlay();
        else
            engine_.transportStop();
        return true;
    }

    if ((character == 'r') && ! command)
    {
        engine_.transportRecord();
        return true;
    }

    if (key.isKeyCode(juce::KeyPress::leftKey))
    {
        if (shift)
            engine_.transportNudge(-5.0);
        else
            engine_.transportJump(-1);
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::rightKey))
    {
        if (shift)
            engine_.transportNudge(5.0);
        else
            engine_.transportJump(1);
        return true;
    }

    return false;
}

void MainComponent::showPage(int page)
{
    page_ = page == 2 ? 2 : 1;
    engine_.setSessionPage(page_);
    recorderButton_.setToggleState(page_ == 1, juce::dontSendNotification);
    pluginsButton_.setToggleState(page_ == 2, juce::dontSendNotification);
    resized();
}

void MainComponent::toggleScanner()
{
    scanner_.toggle();
    scannerButton_.setToggleState(scanner_.isVisible(), juce::dontSendNotification);
}

void MainComponent::toggleDropouts()
{
    dropouts_.toggle();
    dropoutsButton_.setToggleState(dropouts_.isVisible(), juce::dontSendNotification);
}

void MainComponent::saveSession()
{
    engine_.saveSession();
    refresh();
}

void MainComponent::saveSessionAs()
{
    if (fileChooser_ != nullptr)
        return;

    fileChooser_ = std::make_unique<juce::FileChooser>("Save a Copy of This Session",
                                                       engine_.suggestedSessionFolder().getParentDirectory(),
                                                       juce::String(),
                                                       true);
    fileChooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectDirectories
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this](const juce::FileChooser& chooser)
                              {
                                  const auto chosen = chooser.getResult();
                                  juce::MessageManager::callAsync([this, chosen]
                                  {
                                      fileChooser_.reset();
                                      if (chosen.getFullPathName().isNotEmpty())
                                          engine_.saveSessionAs(chosen);
                                      refresh();
                                  });
                              });
}

void MainComponent::importRecordings()
{
    if (fileChooser_ != nullptr)
        return;

    fileChooser_ = std::make_unique<juce::FileChooser>("Import a Recording Folder",
                                                       engine_.suggestedSessionFolder(),
                                                       juce::String(),
                                                       true);
    fileChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this](const juce::FileChooser& chooser)
                              {
                                  const auto chosen = chooser.getResult();
                                  juce::MessageManager::callAsync([this, chosen]
                                  {
                                      fileChooser_.reset();
                                      if (chosen.getFullPathName().isNotEmpty())
                                          engine_.importRecordingFolder(chosen);
                                      refresh();
                                  });
                              });
}

void MainComponent::confirmClearTimeline()
{
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon,
                                       "Clear the Timeline?",
                                       "This removes the takes from the timeline. The WAV files stay in the audio folder.",
                                       "Clear Timeline",
                                       "Cancel",
                                       nullptr,
                                       juce::ModalCallbackFunction::create([this](int result)
                                       {
                                           if (result == 1)
                                           {
                                               engine_.clearTimeline();
                                               refresh();
                                           }
                                       }));
}

void MainComponent::openRecent(int index)
{
    const auto sessions = engine_.recentSessions();
    if (index < 0 || index >= sessions.size())
        return;
    if (engine_.hasSession())
        engine_.saveSession();
    engine_.loadSessionFrom(juce::File(sessions[index]));
    refresh();
}

void MainComponent::showFileMenu()
{
    if (fileMenu_ == nullptr)
        return;
    auto menu = fileMenu_->getMenuForIndex(0, "File");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&fileButton_),
                       [this](int result)
                       {
                           if (fileMenu_ != nullptr)
                               fileMenu_->menuItemSelected(result, 0);
                       });
}

void MainComponent::showHelp()
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                           "YouHost shortcuts",
                                           kShortcutHelp);
}

void MainComponent::setPeakMode(bool peak, bool fromUser)
{
    showPeak_ = peak;
    showRadio(rmsButton_, peakButton_, showPeak_);
    referenceBox_.setEnabled(! showPeak_);
    referenceLabel_.setEnabled(! showPeak_);
    if (fromUser)
    {
        engine_.setSessionMeters(showPeak_, rmsReferenceDb_);
        engine_.noteSessionEdit();
    }
    pluginPage_.setMeterMode(showPeak_, rmsReferenceDb_);
    refresh();
}

void MainComponent::setRmsReference(int db, bool fromUser)
{
    rmsReferenceDb_ = normaliseRmsReferenceDb(db);
    settings_.saveRmsReferenceDb(rmsReferenceDb_);
    referenceBox_.setSelectedId(referenceIdFor(rmsReferenceDb_), juce::dontSendNotification);
    if (fromUser)
    {
        engine_.setSessionMeters(showPeak_, rmsReferenceDb_);
        engine_.noteSessionEdit();
    }
    pluginPage_.setMeterMode(showPeak_, rmsReferenceDb_);
    refresh();
}

void MainComponent::openSession()
{
    if (fileChooser_ != nullptr)
        return;

    if (engine_.hasSession())
        engine_.saveSession();
    fileChooser_ = std::make_unique<juce::FileChooser>("Open a Session",
                                                       engine_.suggestedSessionFolder(),
                                                       "*.youhost",
                                                       true);
    fileChooser_->launchAsync(juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [this](const juce::FileChooser& chooser)
                              {
                                  const auto chosen = chooser.getResult();
                                  juce::MessageManager::callAsync([this, chosen]
                                  {
                                      fileChooser_.reset();
                                      if (chosen.getFullPathName().isNotEmpty())
                                          engine_.loadSessionFrom(chosen);
                                      refresh();
                                  });
                              });
}

void MainComponent::timerCallback()
{
    if (++pollDivider_ >= 6)
    {
        pollDivider_ = 0;
        engine_.pollDeviceStats();
        engine_.maintainSession();
    }

    if (setupWindow_ != nullptr && setupWindow_->isVisible())
    {
        hideDeviceTestTone();
        mirrorSetupToggles();
    }
    refresh();
}

void MainComponent::refresh()
{
    latencyReadout_.setNumbers(engine_.latencyNumbers());
    const auto numbers = engine_.latencyNumbers();
    if (numbers.deviceOpen)
    {
        latencyLabel_.setText("Delay " + juce::String(samplesToMilliseconds(numbers.compensationSamples, numbers.sampleRate), 1)
                                  + " ms   USB " + juce::String(samplesToMilliseconds(numbers.roundTripSamples, numbers.sampleRate), 1)
                                  + " ms",
                              juce::dontSendNotification);
    }
    else
    {
        latencyLabel_.setText("No device", juce::dontSendNotification);
    }

    auto transport = engine_.transportView();
    if (transport.naturalEnd || transport.failed)
    {
        engine_.transportStop();
        transport = engine_.transportView();
    }
    if (transport.mode == TransportMode::recording)
        engine_.touchSession();
    timeline_.setTransport(transport);
    timeLabel_.setText(timecodeText(transport.position, transport.sampleRate), juce::dontSendNotification);
    const bool recording = transport.mode == TransportMode::recording;
    const bool playing = transport.mode == TransportMode::playing;
    timeLabel_.setColour(juce::Label::textColourId, recording ? juce::Colours::white : theme::text);
    timeLabel_.setColour(juce::Label::backgroundColourId, recording ? juce::Colour(0xff8d2430) : theme::panel);
    modeLabel_.setText(playing ? "PLAYBACK" : recording ? "RECORD" : juce::String(), juce::dontSendNotification);
    modeLabel_.setColour(juce::Label::textColourId, playing ? theme::amber : theme::red);
    recButton_.setToggleState(recording, juce::dontSendNotification);
    playButton_.setToggleState(playing, juce::dontSendNotification);
    scannerButton_.setToggleState(scanner_.isVisible(), juce::dontSendNotification);
    dropoutsButton_.setToggleState(dropouts_.isVisible(), juce::dontSendNotification);
    latencyButton_.setToggleState(latencyWindow_ != nullptr && latencyWindow_->isVisible(), juce::dontSendNotification);
    setupButton_.setToggleState(setupWindow_ != nullptr && setupWindow_->isVisible(), juce::dontSendNotification);
    if (bitDepthBox_.getSelectedId() != engine_.wavBitDepth())
        bitDepthBox_.setSelectedId(engine_.wavBitDepth(), juce::dontSendNotification);
    allButton_.setToggleState(engine_.groupsAreExpanded(), juce::dontSendNotification);
    hideButton_.setToggleState(engine_.groupsAreHidden(), juce::dontSendNotification);
    leftScale_.setScale(showPeak_, rmsReferenceDb_, false);
    rightScale_.setScale(showPeak_, rmsReferenceDb_, true);

    const int channels = engine_.visibleChannels();
    const auto strips = engine_.displayStrips(channels);
    std::vector<BridgeCell> cells;
    cells.reserve(strips.size());
    for (const auto& item : strips)
    {
        BridgeCell cell;
        if (item.kind == StripKind::groupHeader)
        {
            cell.header = true;
            cell.group = item.group;
            cell.color = engine_.groupColor(item.group);
            cell.collapsed = engine_.groupCollapsed(item.group);
            cell.title = engine_.groupName(item.group);
            for (int channel = 0; channel < channels; ++channel)
            {
                if (engine_.channelGroup(channel) != item.group)
                    continue;
                ++cell.memberCount;
                if (engine_.isRecordArmed(channel))
                    ++cell.membersOn;
                const float level = showPeak_ ? engine_.peakFor(channel) : engine_.rmsFor(channel);
                if (showPeak_)
                    cell.reading.peak = std::max(cell.reading.peak, level);
                else
                    cell.reading.rms = std::max(cell.reading.rms, level);
                cell.reading.clipped = cell.reading.clipped || engine_.clipFor(channel);
                if (! cell.anyPlugin)
                {
                    const auto snap = engine_.channelSnapshot(channel);
                    for (const auto& slot : snap.slots)
                        cell.anyPlugin = cell.anyPlugin || slot.occupied;
                }
            }
        }
        else
        {
            cell.channel = item.channel;
            cell.color = engine_.channelColor(item.channel);
            cell.selected = engine_.isChannelSelected(item.channel);
            cell.title = engine_.channelName(item.channel);
            cell.reading.rms = engine_.rmsFor(item.channel);
            cell.reading.peak = engine_.peakFor(item.channel);
            cell.reading.clipped = engine_.clipFor(item.channel);
            cell.reading.hasInput = engine_.inputActive(item.channel);
            cell.reading.recordArmed = engine_.isRecordArmed(item.channel);
            cell.reading.recordLive = recording && cell.reading.recordArmed && cell.reading.hasInput;
        }
        cells.push_back(std::move(cell));
    }
    meterGrid_.setCells(std::move(cells), showPeak_, rmsReferenceDb_);
    if (page_ == 1 && meterViewport_.getWidth() > 0)
    {
        meterGrid_.setFitWidth(meterViewport_.getWidth());
        const int width = std::max(meterViewport_.getWidth(), meterGrid_.preferredWidth(meterViewport_.getWidth()));
        const int height = std::max(1, meterViewport_.getMaximumVisibleHeight());
        if (meterGrid_.getWidth() != width || meterGrid_.getHeight() != height)
            meterGrid_.setSize(width, height);
    }
    pluginPage_.setMeterMode(showPeak_, rmsReferenceDb_);
    pluginPage_.refresh();
    repaint();
}

void MainComponent::hideDeviceTestTone()
{
    hideTestButtons(deviceSelector_);
}

void MainComponent::toggleSetup()
{
    if (setupWindow_ == nullptr)
        return;
    const bool show = ! setupWindow_->isVisible();
    setupWindow_->setVisible(show);
    if (show)
    {
        setupWindow_->toFront(true);
        hideDeviceTestTone();
        mirrorSetupToggles();
    }
}

void MainComponent::mirrorSetupToggles()
{
    if (setupWindow_ == nullptr || ! setupWindow_->isVisible())
        return;

    std::vector<std::vector<juce::ToggleButton*>> lists;
    gatherToggleLists(deviceSelector_, lists);
    for (const auto& list : lists)
    {
        const int count = std::min(static_cast<int>(list.size()), kMaxChannels);
        for (int index = 0; index < count; ++index)
        {
            auto* button = list[static_cast<std::size_t>(index)];
            if (button == nullptr)
                continue;
            const bool armed = engine_.isRecordArmed(index);
            if (button->getToggleState() != armed)
                button->setToggleState(armed, juce::dontSendNotification);
            button->onClick = [this, index, button]
            {
                engine_.setRecordArmed(index, button->getToggleState());
            };
            button->setTooltip(armed
                                   ? "Same as REC. Untick to turn this channel OFF. The audio device stays open."
                                   : "Same as OFF. Tick to turn this channel on. The audio device stays open.");
        }
    }
}

void MainComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(theme::background);
    graphics.setColour(theme::text);
    graphics.setFont(juce::Font(juce::FontOptions(18.0f)));
    graphics.drawText("YouHost", titleArea_, juce::Justification::centredLeft, false);

    const auto numbers = engine_.latencyNumbers();
    juce::String status = engine_.deviceName();
    if (numbers.deviceOpen && numbers.sampleRate > 0.0)
    {
        status << "   " << engine_.inputCount() << " in / " << engine_.outputCount() << " out"
               << "   " << juce::String(numbers.sampleRate / 1000.0, 1) << " kHz";
    }
    status << "   " << engine_.wavBitDepthLabel();
    status << "   CPU " << juce::String(juce::roundToInt(engine_.cpuUsage() * 100.0f)) << "%";
    if (engine_.hasSession())
        status << "   " << engine_.sessionName();
    const auto transport = engine_.transportView();
    if (transport.status.isNotEmpty())
        status << "   " << transport.status;

    graphics.setColour(theme::dim);
    graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
    graphics.drawText(status, statusArea_, juce::Justification::centredRight, true);

    if (! engine_.microphoneGranted())
    {
        graphics.setColour(juce::Colour(0xff3a2a22));
        graphics.fillRoundedRectangle(bannerArea_.toFloat(), 8.0f);
        graphics.setColour(theme::amber);
        graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
        graphics.drawFittedText("Microphone access is off, so inputs stay silent. Allow YouHost under "
                                "System Settings, Privacy & Security, Microphone, then press Retry.",
                                bannerArea_.reduced(12, 4).withTrimmedRight(96),
                                juce::Justification::centredLeft,
                                2);
    }

    juce::String hint = engine_.openError().isNotEmpty() ? engine_.openError() : juce::String();
    if (hint.isEmpty() && page_ == 1)
        hint = "The channel number stays visible. REC is on. OFF cuts the meter, plugins, and output immediately. Live sound still passes through while recording. Right-click a channel for color and groups.";
    if (hint.isEmpty())
        hint = "The channel number stays visible. Drag a slot to move it. Option-drag to copy the plugin and its settings. Right-click a group bar to rename it or set its color.";

    graphics.setColour(engine_.openError().isNotEmpty() ? theme::red : theme::fainter);
    graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
    graphics.drawFittedText(hint, hintArea_, juce::Justification::centredLeft, 2);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(12);
    auto title = area.removeFromTop(22);
    titleArea_ = title.removeFromLeft(90);
    statusArea_ = title;
    area.removeFromTop(6);

    auto views = area.removeFromTop(34);
    recorderButton_.setBounds(views.removeFromLeft(112).reduced(0, 2));
    views.removeFromLeft(4);
    pluginsButton_.setBounds(views.removeFromLeft(100).reduced(0, 2));
    views.removeFromLeft(4);
    scannerButton_.setBounds(views.removeFromLeft(104).reduced(0, 2));
    views.removeFromLeft(4);
    dropoutsButton_.setBounds(views.removeFromLeft(112).reduced(0, 2));
    views.removeFromLeft(4);
    latencyButton_.setBounds(views.removeFromLeft(84).reduced(0, 2));
    views.removeFromLeft(4);
    fileButton_.setBounds(views.removeFromLeft(58).reduced(0, 2));
    views.removeFromLeft(4);
    helpButton_.setBounds(views.removeFromLeft(36).reduced(0, 2));
    views.removeFromLeft(10);
    allButton_.setBounds(views.removeFromLeft(48).reduced(0, 2));
    views.removeFromLeft(4);
    hideButton_.setBounds(views.removeFromLeft(58).reduced(0, 2));

    area.removeFromTop(4);
    auto transport = area.removeFromTop(40);
    prevButton_.setBounds(transport.removeFromLeft(72).reduced(0, 4));
    nextButton_.setBounds(transport.removeFromLeft(64).reduced(0, 4));
    stopButton_.setBounds(transport.removeFromLeft(64).reduced(0, 4));
    playButton_.setBounds(transport.removeFromLeft(64).reduced(0, 4));
    recButton_.setBounds(transport.removeFromLeft(78).reduced(0, 4));
    transport.removeFromLeft(8);
    timeLabel_.setBounds(transport.removeFromLeft(148).reduced(0, 2));
    modeLabel_.setBounds(transport.removeFromLeft(96).reduced(4, 8));

    area.removeFromTop(4);
    auto tools = area.removeFromTop(32);
    rmsButton_.setBounds(tools.removeFromLeft(52).reduced(0, 2));
    peakButton_.setBounds(tools.removeFromLeft(58).reduced(0, 2));
    tools.removeFromLeft(8);
    referenceLabel_.setBounds(tools.removeFromLeft(52));
    referenceBox_.setBounds(tools.removeFromLeft(110).reduced(0, 2));
    tools.removeFromLeft(8);
    bitDepthLabel_.setBounds(tools.removeFromLeft(36));
    bitDepthBox_.setBounds(tools.removeFromLeft(124).reduced(0, 2));
    tools.removeFromLeft(8);
    clearClipsButton_.setBounds(tools.removeFromLeft(96).reduced(0, 2));
    tools.removeFromLeft(8);
    newButton_.setBounds(tools.removeFromLeft(52).reduced(0, 2));
    openButton_.setBounds(tools.removeFromLeft(58).reduced(0, 2));
    saveButton_.setBounds(tools.removeFromLeft(54).reduced(0, 2));
    tools.removeFromLeft(8);
    setupButton_.setBounds(tools.removeFromLeft(108).reduced(0, 2));
    latencyLabel_.setBounds(tools.reduced(8, 0));

    const bool showBanner = ! engine_.microphoneGranted();
    retryButton_.setVisible(showBanner);
    if (showBanner)
    {
        area.removeFromTop(8);
        bannerArea_ = area.removeFromTop(48);
        retryButton_.setBounds(bannerArea_.removeFromRight(88).reduced(8, 10));
    }
    else
    {
        bannerArea_ = {};
    }

    area.removeFromTop(8);
    hintArea_ = area.removeFromBottom(32);
    area.removeFromBottom(6);

    const bool recorder = page_ == 1;
    timeline_.setVisible(true);
    meterViewport_.setVisible(recorder);
    leftScale_.setVisible(recorder);
    rightScale_.setVisible(recorder);
    pluginPage_.setVisible(! recorder);

    const int timelineHeight = recorder ? juce::jlimit(72, 160, getHeight() / 8)
                                        : juce::jlimit(52, 72, getHeight() / 12);
    timeline_.setBounds(area.removeFromTop(timelineHeight));
    area.removeFromTop(8);
    if (recorder)
    {
        auto bridge = area;
        leftScale_.setBounds(bridge.removeFromLeft(44));
        rightScale_.setBounds(bridge.removeFromRight(44));
        meterViewport_.setBounds(bridge);
        meterGrid_.setFitWidth(meterViewport_.getWidth());
        const int width = std::max(meterViewport_.getWidth(), meterGrid_.preferredWidth(meterViewport_.getWidth()));
        meterGrid_.setSize(width, std::max(1, meterViewport_.getMaximumVisibleHeight()));
    }
    else
    {
        pluginPage_.setBounds(area);
    }
}

} // namespace youhost
