#pragma once

#include "AppSettings.h"
#include "engine/AudioEngine.h"
#include "ui/LatencyReadout.h"
#include "ui/MeterGrid.h"
#include "ui/PluginPage.h"
#include "ui/ScannerWindow.h"
#include "ui/TimelineView.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>

namespace youhost
{

class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    MainComponent(AudioEngine& engine, AppSettings& settings);
    ~MainComponent() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void parentHierarchyChanged() override;

private:
    void timerCallback() override;
    bool handleKey(const juce::KeyPress& key, juce::Component* originating);
    void refresh();
    void setPeakMode(bool peak, bool fromUser);
    void setRmsReference(int db, bool fromUser);
    void hideDeviceTestTone();
    void showPage(int page);
    void toggleScanner();
    void openSession();
    void showHelp();
    bool shortcutBlocked(juce::Component* originating) const;

    AudioEngine& engine_;
    AppSettings& settings_;
    LatencyReadout latencyReadout_;
    MeterGrid meterGrid_;
    TimelineView timeline_;
    PluginPage pluginPage_;
    ScannerWindow scanner_;
    juce::TooltipWindow tooltipWindow_ { this, 700 };

    juce::TextButton recorderButton_ { "1  Recorder" };
    juce::TextButton pluginsButton_ { "2  Plugins" };
    juce::TextButton scannerButton_ { "3  Scanner" };
    juce::TextButton prevButton_ { "Prev" };
    juce::TextButton nextButton_ { "Next" };
    juce::TextButton stopButton_ { "Stop" };
    juce::TextButton playButton_ { "Play" };
    juce::TextButton recButton_ { "Rec" };
    juce::Label timeLabel_;
    juce::Label modeLabel_;
    juce::TextButton helpButton_ { "?" };
    juce::TextButton rmsButton_ { "RMS" };
    juce::TextButton peakButton_ { "Peak" };
    juce::Label referenceLabel_ { {}, "RMS 0" };
    juce::ComboBox referenceBox_;
    juce::TextButton clearClipsButton_ { "Clear clips" };
    juce::TextButton newButton_ { "New" };
    juce::TextButton openButton_ { "Open" };
    juce::TextButton setupButton_ { "Audio setup" };
    juce::TextButton latencyButton_ { "Latency" };
    juce::TextButton retryButton_ { "Retry" };
    juce::Label latencyLabel_;
    juce::Viewport viewport_;
    juce::AudioDeviceSelectorComponent deviceSelector_;
    std::unique_ptr<juce::DocumentWindow> latencyWindow_;
    std::unique_ptr<juce::FileChooser> fileChooser_;
    struct KeyProxy;
    std::unique_ptr<KeyProxy> keys_;
    juce::Component* keyTarget_ = nullptr;

    bool showPeak_ = false;
    int rmsReferenceDb_ = kDefaultRmsReferenceDb;
    bool setupVisible_ = false;
    int page_ = 1;
    int pollDivider_ = 0;

    juce::Rectangle<int> titleArea_;
    juce::Rectangle<int> statusArea_;
    juce::Rectangle<int> bannerArea_;
    juce::Rectangle<int> hintArea_;
    juce::Rectangle<int> setupPanel_;
    juce::Rectangle<int> setupTitle_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace youhost
