#include "MainComponent.h"
#include "Theme.h"
#include "engine/LatencyMath.h"
#include "engine/MeterScale.h"

namespace youhost
{
namespace
{

constexpr const char* kShortcutHelp =
    "1  Recorder\n"
    "2  Plugins\n"
    "3  Open or close the plugin scanner\n"
    "Space  Play, or Stop if YouHost is already playing or recording\n"
    "Shift+Space, R, or Cmd+Space  Record\n"
    "Left / Right  Previous or next take marker\n"
    "Shift+Left / Shift+Right  Move 5 seconds\n\n"
    "Cmd+Space only arrives if Spotlight is not using that shortcut. Shift+Space and R always work.\n"
    "Shortcuts stay quiet while you are typing in a text field or a plugin window.";

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
    FloatWindow(const juce::String& title, juce::Component& content, int width, int height)
        : juce::DocumentWindow(title, theme::panel, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&content, true);
        setResizable(true, false);
        centreWithSize(width, height);
        setVisible(false);
    }

    void closeButtonPressed() override { setVisible(false); }
};

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

MainComponent::MainComponent(AudioEngine& engine, AppSettings& settings)
    : engine_(engine),
      settings_(settings),
      pluginPage_(engine),
      scanner_(engine),
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

    addAndMakeVisible(meterGrid_);
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
    addAndMakeVisible(clearClipsButton_);
    addAndMakeVisible(newButton_);
    addAndMakeVisible(openButton_);
    addAndMakeVisible(setupButton_);
    addAndMakeVisible(latencyButton_);
    addAndMakeVisible(retryButton_);
    addAndMakeVisible(latencyLabel_);
    addAndMakeVisible(viewport_);

    latencyWindow_ = std::make_unique<FloatWindow>("Latency", latencyReadout_, 440, 280);
    latencyReadout_.setResetHandler([this] { engine_.resetDropouts(); });

    for (auto* button : { &recorderButton_, &pluginsButton_, &scannerButton_, &prevButton_, &nextButton_,
                          &stopButton_, &playButton_, &recButton_, &helpButton_, &rmsButton_, &peakButton_,
                          &clearClipsButton_, &newButton_, &openButton_, &setupButton_, &latencyButton_, &retryButton_ })
        quiet(*button);

    recorderButton_.onClick = [this] { showPage(1); };
    pluginsButton_.onClick = [this] { showPage(2); };
    scannerButton_.onClick = [this] { toggleScanner(); };
    prevButton_.onClick = [this] { engine_.transportJump(-1); };
    nextButton_.onClick = [this] { engine_.transportJump(1); };
    stopButton_.onClick = [this] { engine_.transportStop(); };
    playButton_.onClick = [this] { engine_.transportPlay(); };
    recButton_.onClick = [this] { engine_.transportRecord(); };
    helpButton_.onClick = [this] { showHelp(); };
    newButton_.onClick = [this] { engine_.startNewSession(); };
    openButton_.onClick = [this] { openSession(); };
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

    prevButton_.setTooltip("Previous take marker  (Left)");
    nextButton_.setTooltip("Next take marker  (Right)");
    stopButton_.setTooltip("Stop  (Space)");
    playButton_.setTooltip("Play the recorded takes through the plugins  (Space)");
    recButton_.setTooltip("Record a new take  (Shift+Space, R, or Cmd+Space)");
    helpButton_.setTooltip(kShortcutHelp);
    scannerButton_.setTooltip("Open or close the plugin scanner  (3)");
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
    viewport_.setViewedComponent(&deviceSelector_, false);
    viewport_.setScrollBarsShown(true, false);

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
    referenceBox_.setTooltip("Line level for the RMS scale. 0 VU sits at this many dBFS.");

    rmsButton_.onClick = [this] { setPeakMode(false, true); };
    peakButton_.onClick = [this] { setPeakMode(true, true); };
    referenceBox_.onChange = [this] { setRmsReference(referenceDbFor(referenceBox_.getSelectedId()), true); };
    clearClipsButton_.onClick = [this] { engine_.requestClipClearAll(); };
    clearClipsButton_.setTooltip("Clear every latched clip mark");
    setupButton_.onClick = [this]
    {
        setupVisible_ = ! setupVisible_;
        setupButton_.setButtonText(setupVisible_ ? "Hide audio setup" : "Audio setup");
        resized();
    };

    meterGrid_.setRecordMode(true);
    meterGrid_.setClearHandler([this](int channel)
    {
        if (engine_.clipFor(channel))
            engine_.requestClipClear(channel);
    });
    meterGrid_.setRecordHandler([this](int channel)
    {
        engine_.setRecordArmed(channel, ! engine_.isRecordArmed(channel));
    });
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

    fileChooser_ = std::make_unique<juce::FileChooser>("Open a session",
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

    hideDeviceTestTone();
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
    modeLabel_.setText(playing ? "PLAYBACK" : recording ? "REC" : juce::String(), juce::dontSendNotification);
    modeLabel_.setColour(juce::Label::textColourId, playing ? theme::amber : theme::red);
    recButton_.setToggleState(recording, juce::dontSendNotification);
    playButton_.setToggleState(playing, juce::dontSendNotification);
    scannerButton_.setToggleState(scanner_.isVisible(), juce::dontSendNotification);

    const int channels = engine_.visibleChannels();
    std::vector<MeterReading> readings(static_cast<std::size_t>(std::max(0, channels)));
    for (int channel = 0; channel < channels; ++channel)
    {
        auto& reading = readings[static_cast<std::size_t>(channel)];
        reading.rms = engine_.rmsFor(channel);
        reading.peak = engine_.peakFor(channel);
        reading.clipped = engine_.clipFor(channel);
        reading.hasInput = engine_.inputActive(channel);
        reading.recordArmed = engine_.isRecordArmed(channel);
        reading.recordLive = recording && reading.recordArmed && reading.hasInput;
    }
    meterGrid_.setReadings(std::move(readings), showPeak_, rmsReferenceDb_);
    pluginPage_.setMeterMode(showPeak_, rmsReferenceDb_);
    pluginPage_.refresh();
    repaint();
}

void MainComponent::hideDeviceTestTone()
{
    hideTestButtons(deviceSelector_);
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

    if (setupVisible_ && ! setupPanel_.isEmpty())
    {
        graphics.setColour(theme::panel);
        graphics.fillRoundedRectangle(setupPanel_.toFloat(), 10.0f);
        graphics.setColour(theme::dim);
        graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
        graphics.drawText("Audio device", setupTitle_, juce::Justification::centredLeft, false);
    }

    juce::String hint = engine_.openError().isNotEmpty() ? engine_.openError() : juce::String();
    if (hint.isEmpty() && page_ == 1)
        hint = "Red dots are record-armed. Click the timeline to move the playhead. ? shows the keys.";
    if (hint.isEmpty())
        hint = "Click an empty slot to load a plugin. Click it again to close its window. Right-click for bypass and remove.";

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

    auto transport = area.removeFromTop(46);
    recorderButton_.setBounds(transport.removeFromLeft(108).reduced(0, 6));
    transport.removeFromLeft(4);
    pluginsButton_.setBounds(transport.removeFromLeft(104).reduced(0, 6));
    transport.removeFromLeft(4);
    scannerButton_.setBounds(transport.removeFromLeft(104).reduced(0, 6));
    transport.removeFromLeft(10);
    prevButton_.setBounds(transport.removeFromLeft(58).reduced(0, 8));
    nextButton_.setBounds(transport.removeFromLeft(58).reduced(0, 8));
    stopButton_.setBounds(transport.removeFromLeft(64).reduced(0, 8));
    playButton_.setBounds(transport.removeFromLeft(64).reduced(0, 8));
    recButton_.setBounds(transport.removeFromLeft(64).reduced(0, 8));
    transport.removeFromLeft(8);
    timeLabel_.setBounds(transport.removeFromLeft(148).reduced(0, 4));
    modeLabel_.setBounds(transport.removeFromLeft(92).reduced(4, 10));
    helpButton_.setBounds(transport.removeFromLeft(36).reduced(0, 8));

    area.removeFromTop(4);
    auto tools = area.removeFromTop(32);
    rmsButton_.setBounds(tools.removeFromLeft(52).reduced(0, 2));
    peakButton_.setBounds(tools.removeFromLeft(58).reduced(0, 2));
    tools.removeFromLeft(8);
    referenceLabel_.setBounds(tools.removeFromLeft(52));
    referenceBox_.setBounds(tools.removeFromLeft(110).reduced(0, 2));
    tools.removeFromLeft(8);
    clearClipsButton_.setBounds(tools.removeFromLeft(96).reduced(0, 2));
    tools.removeFromLeft(8);
    newButton_.setBounds(tools.removeFromLeft(58).reduced(0, 2));
    openButton_.setBounds(tools.removeFromLeft(64).reduced(0, 2));
    setupButton_.setBounds(tools.removeFromLeft(118).reduced(0, 2));
    latencyButton_.setBounds(tools.removeFromLeft(84).reduced(0, 2));
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

    viewport_.setVisible(setupVisible_);
    if (setupVisible_)
    {
        setupPanel_ = area.removeFromBottom(230);
        setupTitle_ = setupPanel_.removeFromTop(22).reduced(12, 0);
        viewport_.setBounds(setupPanel_.reduced(8, 4));
        deviceSelector_.setSize(viewport_.getMaximumVisibleWidth(), deviceSelector_.getHeight());
        area.removeFromBottom(8);
    }
    else
    {
        setupPanel_ = {};
        setupTitle_ = {};
    }

    const bool recorder = page_ == 1;
    timeline_.setVisible(recorder);
    meterGrid_.setVisible(recorder);
    pluginPage_.setVisible(! recorder);
    if (recorder)
    {
        const int timelineHeight = juce::jlimit(72, 160, getHeight() / 8);
        timeline_.setBounds(area.removeFromTop(timelineHeight));
        area.removeFromTop(8);
        meterGrid_.setBounds(area);
    }
    else
    {
        pluginPage_.setBounds(area);
    }
}

} // namespace youhost
