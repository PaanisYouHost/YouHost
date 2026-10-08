#include "MainComponent.h"
#include "Theme.h"
#include "engine/MeterScale.h"

namespace youhost
{
namespace
{

void hideTestButtons(juce::Component& component)
{
    for (auto* child : component.getChildren())
    {
        if (child == nullptr)
            continue;

        if (auto* button = dynamic_cast<juce::TextButton*>(child))
        {
            // JUCE's device panel plays a 440 Hz tone at -6 dBFS. Drop the button
            // instead of letting that tone hit a live desk.
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

} // namespace

MainComponent::MainComponent(AudioEngine& engine, AppSettings& settings)
    : engine_(engine),
      settings_(settings),
      deviceSelector_(engine.deviceManager(),
                      0,
                      kMaxChannels,
                      0,
                      kMaxChannels,
                      false,
                      false,
                      false,
                      false)
{
    setOpaque(true);
    showPeak_ = settings_.loadPeakMeter();
    rmsReferenceDb_ = settings_.loadRmsReferenceDb();

    addAndMakeVisible(latencyReadout_);
    addAndMakeVisible(meterGrid_);
    addAndMakeVisible(rmsButton_);
    addAndMakeVisible(peakButton_);
    addAndMakeVisible(referenceLabel_);
    addAndMakeVisible(referenceBox_);
    addAndMakeVisible(clearClipsButton_);
    addAndMakeVisible(setupButton_);
    addAndMakeVisible(retryButton_);
    addAndMakeVisible(viewport_);

    deviceSelector_.setItemHeight(22);
    viewport_.setViewedComponent(&deviceSelector_, false);
    viewport_.setScrollBarsShown(true, false);

    rmsButton_.setRadioGroupId(1);
    peakButton_.setRadioGroupId(1);
    rmsButton_.setClickingTogglesState(true);
    peakButton_.setClickingTogglesState(true);
    rmsButton_.setToggleState(! showPeak_, juce::dontSendNotification);
    peakButton_.setToggleState(showPeak_, juce::dontSendNotification);

    referenceLabel_.setJustificationType(juce::Justification::centredRight);
    referenceLabel_.setFont(juce::Font(juce::FontOptions(13.0f)));
    referenceBox_.addItem("-14 dBFS", 1);
    referenceBox_.addItem("-18 dBFS", 2);
    referenceBox_.addItem("-20 dBFS", 3);
    referenceBox_.setSelectedId(referenceIdFor(rmsReferenceDb_), juce::dontSendNotification);
    referenceBox_.setTooltip("Line level for the RMS scale. 0 VU sits at this many dBFS.");
    referenceBox_.setEnabled(! showPeak_);
    referenceLabel_.setEnabled(! showPeak_);

    rmsButton_.onClick = [this] { setPeakMode(false); };
    peakButton_.onClick = [this] { setPeakMode(true); };
    referenceBox_.onChange = [this] { setRmsReference(referenceDbFor(referenceBox_.getSelectedId())); };
    clearClipsButton_.onClick = [this] { engine_.requestClipClearAll(); };
    clearClipsButton_.setTooltip("Clear every latched clip mark");
    latencyReadout_.setResetHandler([this] { engine_.resetDropouts(); });
    setupButton_.onClick = [this]
    {
        setupVisible_ = ! setupVisible_;
        setupButton_.setButtonText(setupVisible_ ? "Hide audio setup" : "Show audio setup");
        resized();
    };
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

    meterGrid_.setClearHandler([this](int channel)
    {
        if (engine_.clipFor(channel))
            engine_.requestClipClear(channel);
    });

    startTimerHz(30);
    hideDeviceTestTone();
    refresh();
}

MainComponent::~MainComponent()
{
    stopTimer();
}

void MainComponent::setPeakMode(bool peak)
{
    if (showPeak_ == peak)
        return;

    showPeak_ = peak;
    rmsButton_.setToggleState(! showPeak_, juce::dontSendNotification);
    peakButton_.setToggleState(showPeak_, juce::dontSendNotification);
    settings_.savePeakMeter(showPeak_);
    referenceBox_.setEnabled(! showPeak_);
    referenceLabel_.setEnabled(! showPeak_);
    refresh();
}

void MainComponent::setRmsReference(int db)
{
    db = normaliseRmsReferenceDb(db);
    if (rmsReferenceDb_ == db)
        return;

    rmsReferenceDb_ = db;
    referenceBox_.setSelectedId(referenceIdFor(db), juce::dontSendNotification);
    settings_.saveRmsReferenceDb(db);
    refresh();
}

void MainComponent::timerCallback()
{
    if (++pollDivider_ >= 6)
    {
        pollDivider_ = 0;
        engine_.pollDeviceStats();
    }

    hideDeviceTestTone();
    refresh();
}

void MainComponent::refresh()
{
    latencyReadout_.setNumbers(engine_.latencyNumbers());

    const int channels = engine_.visibleChannels();
    std::vector<MeterReading> readings(static_cast<std::size_t>(channels > 0 ? channels : 0));
    for (int channel = 0; channel < channels; ++channel)
    {
        auto& reading = readings[static_cast<std::size_t>(channel)];
        reading.rms = engine_.rmsFor(channel);
        reading.peak = engine_.peakFor(channel);
        reading.clipped = engine_.clipFor(channel);
        reading.hasInput = engine_.inputActive(channel);
    }

    meterGrid_.setReadings(std::move(readings), showPeak_, rmsReferenceDb_);
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
    graphics.setFont(juce::Font(juce::FontOptions(20.0f)));
    graphics.drawText("YouHost", titleArea_, juce::Justification::centredLeft, false);

    const auto numbers = engine_.latencyNumbers();
    juce::String status = engine_.deviceName();
    if (numbers.deviceOpen && numbers.sampleRate > 0.0)
    {
        status << "   " << engine_.inputCount() << " in / " << engine_.outputCount() << " out"
               << "   " << juce::String(numbers.sampleRate / 1000.0, 1) << " kHz";
    }
    status << "   P0 passthrough";

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

    graphics.setColour(engine_.openError().isNotEmpty() ? theme::red : theme::fainter);
    graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
    const juce::String hint = engine_.openError().isNotEmpty()
                                  ? engine_.openError()
                                  : (showPeak_ ? "Peak scale is dBFS, 0 at the top. Clip marks stay on until you clear them."
                                               : "RMS scale is VU. 0 is line level. Clip marks stay on until you clear them.");
    graphics.drawFittedText(hint, hintArea_, juce::Justification::centredLeft, 2);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto header = area.removeFromTop(28);
    titleArea_ = header.removeFromLeft(110);
    statusArea_ = header;
    area.removeFromTop(8);

    const bool showBanner = ! engine_.microphoneGranted();
    retryButton_.setVisible(showBanner);
    if (showBanner)
    {
        bannerArea_ = area.removeFromTop(40);
        retryButton_.setBounds(bannerArea_.withTrimmedLeft(bannerArea_.getWidth() - 88).reduced(0, 6));
        area.removeFromTop(10);
    }
    else
    {
        bannerArea_ = {};
    }

    latencyReadout_.setBounds(area.removeFromTop(196));
    area.removeFromTop(10);

    auto tools = area.removeFromTop(30);
    rmsButton_.setBounds(tools.removeFromLeft(64));
    tools.removeFromLeft(6);
    peakButton_.setBounds(tools.removeFromLeft(70));
    tools.removeFromLeft(14);
    referenceLabel_.setBounds(tools.removeFromLeft(52));
    tools.removeFromLeft(4);
    referenceBox_.setBounds(tools.removeFromLeft(112).reduced(0, 2));
    tools.removeFromLeft(8);
    clearClipsButton_.setBounds(tools.removeFromLeft(108));
    setupButton_.setBounds(tools.removeFromRight(158));
    area.removeFromTop(4);
    hintArea_ = area.removeFromTop(32);
    area.removeFromTop(6);

    if (setupVisible_)
    {
        auto setup = area.removeFromBottom(juce::jmin(320, juce::jmax(200, area.getHeight() / 3)));
        area.removeFromBottom(8);
        setupPanel_ = setup;
        setupTitle_ = setup.removeFromTop(18);
        setup.removeFromTop(4);
        viewport_.setVisible(true);
        viewport_.setBounds(setup);
        const int width = viewport_.getMaximumVisibleWidth();
        if (width > 0)
            deviceSelector_.setSize(width, juce::jmax(deviceSelector_.getHeight(), 220));
        hideDeviceTestTone();
    }
    else
    {
        viewport_.setVisible(false);
        setupPanel_ = {};
        setupTitle_ = {};
    }

    meterGrid_.setBounds(area);
}

} // namespace youhost
