#pragma once

#include "AppSettings.h"
#include "engine/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace youhost
{

class CpuWindow : public juce::DocumentWindow,
                  private juce::Timer
{
public:
    CpuWindow(AudioEngine& engine, AppSettings& settings);
    ~CpuWindow() override;

    void toggle();
    void closeButtonPressed() override;

private:
    friend class CpuShell;
    class Content;

    void timerCallback() override;

    AudioEngine& engine_;
    AppSettings& settings_;
    Content* content_ = nullptr;
};

} // namespace youhost
