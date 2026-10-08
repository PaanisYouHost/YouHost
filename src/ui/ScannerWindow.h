#pragma once

#include "engine/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace youhost
{

class ScannerWindow : public juce::DocumentWindow,
                      private juce::Timer
{
public:
    explicit ScannerWindow(AudioEngine& engine);
    ~ScannerWindow() override;

    void toggle();
    void closeButtonPressed() override;

private:
    class Content;

    void timerCallback() override;

    AudioEngine& engine_;
    Content* content_ = nullptr;
};

} // namespace youhost
