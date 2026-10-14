#pragma once

#include "PluginProcessor.h"
#include "dsp/YouMixCore.h"
#include "ui/Fader.h"

// Closing this window does not change the audio. It only mirrors the shared core.
class YouMixAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit YouMixAudioProcessorEditor(YouMixAudioProcessor&);
    ~YouMixAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void layoutStrips();
    void refreshButtons();
    void applyWindowSize();

    struct Strip
    {
        int slot = 0;
        juce::Rectangle<int> bounds;
        juce::Rectangle<int> meter;
        juce::Rectangle<int> mute;
        juce::Rectangle<int> weightLabel;
        juce::Rectangle<int> groupButton[5];
    };

    YouMixAudioProcessor& processor_;
    youmix::YouMixSettings snapshot_ {};
    float input_[youmix::kYouMixSlots] {};
    float reduction_[youmix::kYouMixSlots] {};
    Strip strips_[youmix::kYouMixSlots];
    int stripCount_ = 0;
    bool advanced_ = false;
    juce::Rectangle<int> stripArea_;

    juce::TextButton bypassButton_ { "ON" };
    juce::TextButton voiceButton_ { "VOICE" };
    juce::TextButton singingButton_ { "SINGING" };
    juce::TextButton customButton_ { "CUSTOM" };
    juce::TextButton advancedButton_ { "ADVANCED" };
    juce::Rectangle<int> pinkBypassArea_[youmix::kYouMixGroups];
    Fader weight_[youmix::kYouMixSlots];
    juce::Label title_;
    juce::Label meterKey_;
    juce::Label pinkCaption_[youmix::kYouMixGroups];
    Fader pink_[youmix::kYouMixGroups];
    juce::Label attackCaption_;
    juce::Label releaseCaption_;
    juce::Label thresholdCaption_;
    Fader attack_;
    Fader release_;
    Fader threshold_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(YouMixAudioProcessorEditor)
};
