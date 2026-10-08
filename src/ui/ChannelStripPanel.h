#pragma once

#include "engine/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace youhost
{

class ChannelStripPanel : public juce::Component,
                          private juce::ListBoxModel
{
public:
    explicit ChannelStripPanel(AudioEngine& engine);

    void setSelection(int channel, int slot);
    void setSelectionHandler(std::function<void(int channel, int slot)> handler);
    void refresh();

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    int getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics& graphics, int width, int height, bool rowIsSelected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& event) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent& event) override;

    void rebuildRows();
    void loadSelectedPlugin();
    void chooseSlot(int slot);
    const juce::PluginDescription* selectedPlugin() const;

    struct Row
    {
        bool header = false;
        juce::PluginDescription description;
        juce::String text;
    };

    AudioEngine& engine_;
    std::function<void(int, int)> onSelection_;
    int channel_ = 0;
    int slot_ = 0;
    int catalogueToken_ = -1;
    juce::String searchText_;

    juce::Label title_;
    juce::ToggleButton excludeButton_ { "Exclude from alignment" };
    juce::Label chainLabel_;
    juce::Label delayLabel_;
    juce::Label errorLabel_;
    std::array<juce::TextButton, kSlotsPerChannel> slotButtons_;
    juce::TextButton bypassButton_ { "Bypass" };
    juce::TextButton editorButton_ { "Editor" };
    juce::TextButton removeButton_ { "Remove" };
    juce::Label browserTitle_;
    juce::TextEditor search_;
    juce::ListBox list_ { "Plugins", this };
    juce::TextButton scanButton_ { "Scan" };
    juce::TextButton rescanButton_ { "Rescan" };
    juce::TextButton clearFailedButton_ { "Clear failed" };
    juce::TextButton loadButton_ { "Load" };
    juce::Label status_;
    std::vector<Row> rows_;
};

} // namespace youhost
