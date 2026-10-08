#pragma once

#include "AppSettings.h"
#include "engine/AudioEngine.h"
#include "ui/ChannelStripPanel.h"
#include "ui/LatencyReadout.h"
#include "ui/MeterGrid.h"

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

private:
    void timerCallback() override;
    void refresh();
    void setPeakMode(bool peak, bool fromUser);
    void setRmsReference(int db, bool fromUser);
    void hideDeviceTestTone();
    void selectSlot(int channel, int slot);
    void saveSession();
    void openSession();

    AudioEngine& engine_;
    AppSettings& settings_;
    LatencyReadout latencyReadout_;
    MeterGrid meterGrid_;
    ChannelStripPanel stripPanel_;
    juce::TextButton rmsButton_ { "RMS" };
    juce::TextButton peakButton_ { "Peak" };
    juce::Label referenceLabel_ { {}, "RMS 0" };
    juce::ComboBox referenceBox_;
    juce::TextButton clearClipsButton_ { "Clear clips" };
    juce::TextButton saveButton_ { "Save" };
    juce::TextButton openButton_ { "Open" };
    juce::TextButton setupButton_ { "Hide audio setup" };
    juce::TextButton retryButton_ { "Retry" };
    juce::Viewport viewport_;
    juce::AudioDeviceSelectorComponent deviceSelector_;
    std::unique_ptr<juce::FileChooser> fileChooser_;
    bool showPeak_ = false;
    int rmsReferenceDb_ = kDefaultRmsReferenceDb;
    bool setupVisible_ = true;
    int pollDivider_ = 0;
    int selectedChannel_ = 0;
    int selectedSlot_ = 0;

    juce::Rectangle<int> titleArea_;
    juce::Rectangle<int> statusArea_;
    juce::Rectangle<int> bannerArea_;
    juce::Rectangle<int> hintArea_;
    juce::Rectangle<int> setupPanel_;
    juce::Rectangle<int> setupTitle_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace youhost
