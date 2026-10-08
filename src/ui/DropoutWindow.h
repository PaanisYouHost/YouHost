#pragma once

#include "AppSettings.h"
#include "engine/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace youhost
{

class DropoutWindow : public juce::DocumentWindow,
                      private juce::Timer
{
public:
    DropoutWindow(AudioEngine& engine, AppSettings& settings);
    ~DropoutWindow() override;

    void toggle();
    void closeButtonPressed() override;

private:
    class Content;

    void timerCallback() override;

    AudioEngine& engine_;
    AppSettings& settings_;
    Content* content_ = nullptr;
};

} // namespace youhost
