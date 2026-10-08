#pragma once

#include "engine/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace youhost
{

class PluginPage : public juce::Component
{
public:
    explicit PluginPage(AudioEngine& engine);
    ~PluginPage() override;

    void setMeterMode(bool peak, int referenceDb);
    void refresh();
    void resized() override;

private:
    class Row;
    class GroupHeader;
    void rebuild();

    AudioEngine& engine_;
    juce::Component content_;
    juce::Label empty_;
    juce::Viewport viewport_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::vector<std::unique_ptr<GroupHeader>> headers_;
    std::vector<juce::Component*> order_;
    std::vector<int> heights_;
    bool showPeak_ = false;
    int referenceDb_ = kDefaultRmsReferenceDb;
    int channels_ = -1;
    int revision_ = -1;
};

} // namespace youhost
