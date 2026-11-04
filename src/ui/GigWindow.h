#pragma once

#include "AppSettings.h"
#include "engine/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace youhost
{

class GigWindow : public juce::DocumentWindow
{
public:
    GigWindow(AudioEngine& engine, AppSettings& settings);
    ~GigWindow() override;

    void toggle();
    void closeButtonPressed() override;

private:
    class Content;

    AppSettings& settings_;
    Content* content_ = nullptr;
};

} // namespace youhost
