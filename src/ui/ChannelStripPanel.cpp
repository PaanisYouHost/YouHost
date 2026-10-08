#include "ChannelStripPanel.h"
#include "Theme.h"

#include <algorithm>

namespace youhost
{

ChannelStripPanel::ChannelStripPanel(AudioEngine& engine)
    : engine_(engine)
{
    setOpaque(true);

    auto styleLabel = [](juce::Label& label)
    {
        label.setJustificationType(juce::Justification::centredLeft);
        label.setFont(juce::Font(juce::FontOptions(13.0f)));
        label.setColour(juce::Label::textColourId, theme::text);
        label.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    };

    styleLabel(title_);
    styleLabel(chainLabel_);
    styleLabel(delayLabel_);
    styleLabel(errorLabel_);
    styleLabel(browserTitle_);
    styleLabel(status_);
    title_.setFont(juce::Font(juce::FontOptions(16.0f)));
    errorLabel_.setColour(juce::Label::textColourId, theme::amber);
    status_.setFont(juce::Font(juce::FontOptions(12.0f)));
    status_.setColour(juce::Label::textColourId, theme::dim);
    browserTitle_.setText("Plugins", juce::dontSendNotification);

    addAndMakeVisible(title_);
    addAndMakeVisible(excludeButton_);
    addAndMakeVisible(chainLabel_);
    addAndMakeVisible(delayLabel_);
    addAndMakeVisible(errorLabel_);
    addAndMakeVisible(browserTitle_);
    addAndMakeVisible(search_);
    addAndMakeVisible(list_);
    addAndMakeVisible(scanButton_);
    addAndMakeVisible(rescanButton_);
    addAndMakeVisible(clearFailedButton_);
    addAndMakeVisible(loadButton_);
    addAndMakeVisible(status_);
    addAndMakeVisible(bypassButton_);
    addAndMakeVisible(editorButton_);
    addAndMakeVisible(removeButton_);

    for (int slot = 0; slot < kSlotsPerChannel; ++slot)
    {
        slotButtons_[static_cast<std::size_t>(slot)].setClickingTogglesState(false);
        addAndMakeVisible(slotButtons_[static_cast<std::size_t>(slot)]);
        slotButtons_[static_cast<std::size_t>(slot)].onClick = [this, slot] { chooseSlot(slot); };
    }

    excludeButton_.onClick = [this]
    {
        engine_.setChannelExcluded(channel_, excludeButton_.getToggleState());
        refresh();
    };
    bypassButton_.onClick = [this]
    {
        const auto snap = engine_.channelSnapshot(channel_);
        const auto& slot = snap.slots[static_cast<std::size_t>(slot_)];
        if (slot.occupied)
            engine_.setSlotBypassed(channel_, slot_, ! slot.bypassed);
        refresh();
    };
    editorButton_.onClick = [this] { engine_.openPluginEditor(channel_, slot_); };
    removeButton_.onClick = [this]
    {
        engine_.removePlugin(channel_, slot_);
        refresh();
    };
    search_.setTextToShowWhenEmpty("Search", theme::fainter);
    search_.setColour(juce::TextEditor::backgroundColourId, theme::background);
    search_.setColour(juce::TextEditor::textColourId, theme::text);
    search_.setColour(juce::TextEditor::outlineColourId, theme::panelEdge);
    search_.onTextChange = [this] { rebuildRows(); };
    list_.setRowHeight(22);
    list_.setColour(juce::ListBox::backgroundColourId, theme::background);

    scanButton_.onClick = [this] { engine_.pluginCatalogue().scanNew(); };
    rescanButton_.onClick = [this] { engine_.pluginCatalogue().rescan(); };
    clearFailedButton_.onClick = [this] { engine_.pluginCatalogue().clearFailedAndScan(); };
    loadButton_.onClick = [this] { loadSelectedPlugin(); };

    excludeButton_.setTooltip("Leave this channel out of the alignment delay. Its plugins still run.");
    refresh();
}

void ChannelStripPanel::setSelection(int channel, int slot)
{
    channel_ = juce::jlimit(0, kMaxChannels - 1, channel);
    slot_ = juce::jlimit(0, kSlotsPerChannel - 1, slot);
    refresh();
}

void ChannelStripPanel::setSelectionHandler(std::function<void(int, int)> handler)
{
    onSelection_ = std::move(handler);
}

void ChannelStripPanel::refresh()
{
    const auto snap = engine_.channelSnapshot(channel_);
    title_.setText("Channel " + juce::String(channel_ + 1), juce::dontSendNotification);
    excludeButton_.setToggleState(snap.excluded, juce::dontSendNotification);
    chainLabel_.setText("Chain  " + juce::String(snap.chainSamples) + " smp", juce::dontSendNotification);
    delayLabel_.setText("Delay  " + juce::String(snap.delaySamples) + " smp", juce::dontSendNotification);

    juce::String error;
    for (int slot = 0; slot < kSlotsPerChannel; ++slot)
    {
        const auto& slotSnap = snap.slots[static_cast<std::size_t>(slot)];
        juce::String text = juce::String(slot + 1) + "   ";
        if (slotSnap.loading)
            text << "loading";
        else if (! slotSnap.occupied)
            text << "empty";
        else
            text << slotSnap.name << (slotSnap.bypassed ? "  bypass" : "");

        auto& button = slotButtons_[static_cast<std::size_t>(slot)];
        button.setButtonText(text);
        button.setToggleState(slot == slot_, juce::dontSendNotification);
        if (slot == slot_ && slotSnap.error.isNotEmpty())
            error = slotSnap.error;
    }

    const auto& selected = snap.slots[static_cast<std::size_t>(slot_)];
    bypassButton_.setEnabled(selected.occupied);
    bypassButton_.setButtonText(selected.bypassed ? "Bypassed" : "Bypass");
    editorButton_.setEnabled(selected.occupied && ! selected.loading);
    removeButton_.setEnabled(selected.occupied || selected.loading);
    errorLabel_.setText(error, juce::dontSendNotification);

    const auto status = engine_.pluginCatalogue().status();
    status_.setText(status.text, juce::dontSendNotification);
    scanButton_.setEnabled(! status.scanning);
    rescanButton_.setEnabled(! status.scanning);
    clearFailedButton_.setEnabled(! status.scanning);
    if (status.token != catalogueToken_ || search_.getText() != searchText_)
        rebuildRows();
}

void ChannelStripPanel::paint(juce::Graphics& graphics)
{
    graphics.setColour(theme::panel);
    graphics.fillRoundedRectangle(getLocalBounds().toFloat(), 10.0f);
}

void ChannelStripPanel::resized()
{
    auto area = getLocalBounds().reduced(10);
    title_.setBounds(area.removeFromTop(22));
    area.removeFromTop(2);
    excludeButton_.setBounds(area.removeFromTop(22));
    chainLabel_.setBounds(area.removeFromTop(16));
    delayLabel_.setBounds(area.removeFromTop(16));
    errorLabel_.setBounds(area.removeFromTop(16));
    area.removeFromTop(4);

    for (auto& button : slotButtons_)
    {
        button.setBounds(area.removeFromTop(24));
        area.removeFromTop(3);
    }

    auto actions = area.removeFromTop(26);
    const int actionWidth = actions.getWidth() / 3;
    bypassButton_.setBounds(actions.removeFromLeft(actionWidth).reduced(1, 0));
    editorButton_.setBounds(actions.removeFromLeft(actionWidth).reduced(1, 0));
    removeButton_.setBounds(actions.reduced(1, 0));
    area.removeFromTop(8);

    browserTitle_.setBounds(area.removeFromTop(16));
    search_.setBounds(area.removeFromTop(24));
    area.removeFromTop(4);

    status_.setBounds(area.removeFromBottom(18));
    auto scanRow = area.removeFromBottom(26);
    area.removeFromBottom(4);
    loadButton_.setBounds(area.removeFromBottom(26));
    area.removeFromBottom(4);
    const int scanWidth = scanRow.getWidth() / 3;
    scanButton_.setBounds(scanRow.removeFromLeft(scanWidth).reduced(1, 0));
    rescanButton_.setBounds(scanRow.removeFromLeft(scanWidth).reduced(1, 0));
    clearFailedButton_.setBounds(scanRow.reduced(1, 0));
    list_.setBounds(area);
}

int ChannelStripPanel::getNumRows()
{
    return static_cast<int>(rows_.size());
}

void ChannelStripPanel::paintListBoxItem(int rowNumber, juce::Graphics& graphics, int width, int height, bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= static_cast<int>(rows_.size()))
        return;

    const auto& row = rows_[static_cast<std::size_t>(rowNumber)];
    if (row.header)
    {
        graphics.setColour(theme::panelEdge);
        graphics.fillRect(0, 0, width, height);
        graphics.setColour(theme::dim);
        graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
    }
    else if (rowIsSelected)
    {
        graphics.setColour(theme::buttonOn);
        graphics.fillRect(0, 0, width, height);
        graphics.setColour(theme::text);
        graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
    }
    else
    {
        graphics.setColour(theme::text);
        graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
    }

    graphics.drawText(row.text, 8, 0, width - 12, height, juce::Justification::centredLeft, true);
}

void ChannelStripPanel::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(row)].header)
        list_.deselectAllRows();
}

void ChannelStripPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(row)].header)
        return;
    list_.selectRow(row);
    loadSelectedPlugin();
}

void ChannelStripPanel::rebuildRows()
{
    searchText_ = search_.getText();
    catalogueToken_ = engine_.pluginCatalogue().status().token;
    const auto types = engine_.pluginCatalogue().types();
    std::vector<juce::PluginDescription> matches;
    matches.reserve(static_cast<std::size_t>(types.size()));
    for (const auto& description : types)
    {
        if (searchText_.isNotEmpty())
        {
            const auto haystack = description.name + " " + description.manufacturerName + " " + description.pluginFormatName;
            if (! haystack.containsIgnoreCase(searchText_))
                continue;
        }
        matches.push_back(description);
    }

    std::sort(matches.begin(), matches.end(), [](const juce::PluginDescription& left, const juce::PluginDescription& right)
    {
        if (left.manufacturerName != right.manufacturerName)
            return left.manufacturerName.compareIgnoreCase(right.manufacturerName) < 0;
        return left.name.compareIgnoreCase(right.name) < 0;
    });

    rows_.clear();
    juce::String manufacturer;
    for (const auto& description : matches)
    {
        const auto group = description.manufacturerName.isNotEmpty() ? description.manufacturerName : "Other";
        if (group != manufacturer)
        {
            manufacturer = group;
            Row header;
            header.header = true;
            header.text = group;
            rows_.push_back(header);
        }
        Row row;
        row.description = description;
        row.text = description.name + "   " + description.pluginFormatName;
        rows_.push_back(row);
    }

    list_.updateContent();
    list_.repaint();
}

void ChannelStripPanel::loadSelectedPlugin()
{
    if (const auto* description = selectedPlugin())
        engine_.loadPlugin(channel_, slot_, *description);
    refresh();
}

void ChannelStripPanel::chooseSlot(int slot)
{
    slot_ = slot;
    if (onSelection_ != nullptr)
        onSelection_(channel_, slot_);
    refresh();
}

const juce::PluginDescription* ChannelStripPanel::selectedPlugin() const
{
    const int row = list_.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(row)].header)
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)].description;
}

} // namespace youhost
