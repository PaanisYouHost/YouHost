#pragma once

#include "PluginProcessor.h"
#include "dsp/YouMixCore.h"
#include "ui/DragValue.h"

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

    struct Strip
    {
        int slot = 0;
        juce::Rectangle<int> bounds;
        juce::Rectangle<int> meter;
        juce::Rectangle<int> mute;
        juce::Rectangle<int> group;
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
    juce::TextButton advancedButton_ { "ADVANCED" };
    juce::TextButton speechButton_ { "SPEECH" };
    juce::TextButton singingButton_ { "SINGING" };
    juce::Label title_;
    juce::Label pinkCaption_[youmix::kYouMixGroups];
    DragValue pink_[youmix::kYouMixGroups];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(YouMixAudioProcessorEditor)
};
