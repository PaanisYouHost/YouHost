#pragma once

#include "engine/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace youhost
{

class AppSettings;
class PluginListWindow;

class PluginPage : public juce::Component,
                   public juce::DragAndDropContainer
{
public:
    PluginPage(AudioEngine& engine, AppSettings& settings);
    ~PluginPage() override;

    void setMeterMode(bool peak, int referenceDb);
    void refresh();
    void resized() override;

private:
    class Row;
    class GroupHeader;
    void rebuild();
    void showPluginList(int channel, int slot);
    void beginNameEdit(int channel);
    void stepNameEdit(int channel, int direction);

    AudioEngine& engine_;
    AppSettings& settings_;
    juce::TextButton nullButton_ { "Null test" };
    juce::Component content_;
    juce::Label empty_;
    juce::Viewport viewport_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::vector<std::unique_ptr<GroupHeader>> headers_;
    std::vector<juce::Component*> order_;
    std::vector<int> heights_;
    std::unique_ptr<PluginListWindow> pluginList_;
    bool showPeak_ = false;
    int referenceDb_ = kDefaultRmsReferenceDb;
    int channels_ = -1;
    int revision_ = -1;
};

} // namespace youhost
