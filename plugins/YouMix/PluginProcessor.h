#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class YouMixAudioProcessor : public juce::AudioProcessor
{
public:
    YouMixAudioProcessor();
    ~YouMixAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "YouMix"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;
    void updateTrackProperties(const TrackProperties& properties) override;

    int slot() const noexcept { return slot_; }
    int editorWidth() const noexcept { return editorWidth_; }
    int editorHeight() const noexcept { return editorHeight_; }
    void rememberEditorSize(int width, int height) noexcept;

private:
    int slot_ = -1;
    int editorWidth_ = 0;
    int editorHeight_ = 0;
};
