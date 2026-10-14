#pragma once

#include "PluginProcessor.h"
#include "ui/Fader.h"

// Closing this window does not change the audio. Each PSE keeps its own settings.
class PseAudioProcessorEditor : public juce::AudioProcessorEditor,
                                private juce::Timer
{
public:
    explicit PseAudioProcessorEditor(PseAudioProcessor&);
    ~PseAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    struct Row
    {
        juce::Label caption;
        Fader value;
    };

    PseAudioProcessor& processor_;
    juce::TextButton bypassButton_ { "ON" };
    juce::Label title_;
    Row rows_[6];
    juce::Rectangle<int> meterArea_;
    float input_ = 0.0f;
    float reduction_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PseAudioProcessorEditor)
};
