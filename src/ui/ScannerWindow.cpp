#include "ScannerWindow.h"
#include "Theme.h"

namespace youhost
{
namespace
{

class NameList : public juce::ListBoxModel
{
public:
    int getNumRows() override { return static_cast<int>(lines_.size()); }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool selected) override
    {
        if (row < 0 || row >= getNumRows())
            return;
        graphics.setColour(selected ? theme::panelEdge : juce::Colours::transparentBlack);
        graphics.fillRect(0, 0, width, height);
        graphics.setColour(theme::text);
        graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
        graphics.drawText(lines_[static_cast<std::size_t>(row)], 6, 0, width - 8, height, juce::Justification::centredLeft, true);
    }

    void setLines(std::vector<juce::String> lines)
    {
        lines_ = std::move(lines);
    }

private:
    std::vector<juce::String> lines_;
};

} // namespace

class ScannerWindow::Content : public juce::Component
{
public:
    explicit Content(AudioEngine& engine)
        : engine_(engine)
    {
        addAndMakeVisible(scanButton_);
        addAndMakeVisible(rescanButton_);
        addAndMakeVisible(clearButton_);
        addAndMakeVisible(status_);
        addAndMakeVisible(knownTitle_);
        addAndMakeVisible(failedTitle_);
        addAndMakeVisible(knownList_);
        addAndMakeVisible(failedList_);

        scanButton_.setButtonText("Scan");
        rescanButton_.setButtonText("Rescan");
        clearButton_.setButtonText("Clear failed");
        scanButton_.onClick = [this] { engine_.pluginCatalogue().scanNew(); };
        rescanButton_.onClick = [this] { engine_.pluginCatalogue().rescan(); };
        clearButton_.onClick = [this] { engine_.pluginCatalogue().clearFailedAndScan(); };
        for (auto* button : { &scanButton_, &rescanButton_, &clearButton_ })
            button->setMouseClickGrabsKeyboardFocus(false);

        knownTitle_.setText("Scanned plugins", juce::dontSendNotification);
        failedTitle_.setText("Failed", juce::dontSendNotification);
        knownTitle_.setFont(juce::Font(juce::FontOptions(13.0f)));
        failedTitle_.setFont(juce::Font(juce::FontOptions(13.0f)));
        status_.setFont(juce::Font(juce::FontOptions(13.0f)));
        status_.setColour(juce::Label::textColourId, theme::dim);

        knownList_.setModel(&knownModel_);
        failedList_.setModel(&failedModel_);
        knownList_.setRowHeight(22);
        failedList_.setRowHeight(22);
        knownList_.setColour(juce::ListBox::backgroundColourId, theme::background);
        failedList_.setColour(juce::ListBox::backgroundColourId, theme::background);
    }

    void refresh()
    {
        const auto status = engine_.pluginCatalogue().status();
        status_.setText(status.text, juce::dontSendNotification);
        scanButton_.setEnabled(! status.scanning);
        rescanButton_.setEnabled(! status.scanning);
        clearButton_.setEnabled(! status.scanning);

        std::vector<juce::String> known;
        for (const auto& type : engine_.pluginCatalogue().types())
        {
            juce::String line = type.name;
            if (type.pluginFormatName.isNotEmpty())
                line << "   " << type.pluginFormatName;
            if (type.manufacturerName.isNotEmpty())
                line << "   " << type.manufacturerName;
            known.push_back(line);
        }
        knownModel_.setLines(std::move(known));
        knownList_.updateContent();

        std::vector<juce::String> failed;
        for (const auto& file : engine_.pluginCatalogue().failedFiles())
            failed.push_back(file);
        failedModel_.setLines(std::move(failed));
        failedList_.updateContent();
        knownTitle_.setText("Scanned plugins (" + juce::String(status.known) + ")", juce::dontSendNotification);
        failedTitle_.setText("Failed (" + juce::String(status.failed) + ")", juce::dontSendNotification);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12);
        auto buttons = area.removeFromTop(28);
        scanButton_.setBounds(buttons.removeFromLeft(90));
        buttons.removeFromLeft(8);
        rescanButton_.setBounds(buttons.removeFromLeft(90));
        buttons.removeFromLeft(8);
        clearButton_.setBounds(buttons.removeFromLeft(120));
        area.removeFromTop(8);
        status_.setBounds(area.removeFromTop(22));
        area.removeFromTop(8);

        auto left = area.removeFromLeft(area.getWidth() * 2 / 3);
        area.removeFromLeft(8);
        knownTitle_.setBounds(left.removeFromTop(20));
        failedTitle_.setBounds(area.removeFromTop(20));
        knownList_.setBounds(left);
        failedList_.setBounds(area);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(theme::background);
    }

private:
    AudioEngine& engine_;
    juce::TextButton scanButton_;
    juce::TextButton rescanButton_;
    juce::TextButton clearButton_;
    juce::Label status_;
    juce::Label knownTitle_;
    juce::Label failedTitle_;
    NameList knownModel_;
    NameList failedModel_;
    juce::ListBox knownList_;
    juce::ListBox failedList_;
};

ScannerWindow::ScannerWindow(AudioEngine& engine)
    : juce::DocumentWindow("Plugin scanner",
                           theme::panel,
                           juce::DocumentWindow::closeButton),
      engine_(engine)
{
    auto content = std::make_unique<Content>(engine_);
    content_ = content.get();
    setUsingNativeTitleBar(true);
    setContentOwned(content.release(), true);
    setResizable(true, false);
    centreWithSize(760, 480);
    setVisible(false);
    startTimerHz(4);
}

ScannerWindow::~ScannerWindow()
{
    stopTimer();
}

void ScannerWindow::toggle()
{
    setVisible(! isVisible());
    if (isVisible())
    {
        if (content_ != nullptr)
            content_->refresh();
        toFront(true);
    }
}

void ScannerWindow::closeButtonPressed()
{
    setVisible(false);
}

void ScannerWindow::timerCallback()
{
    if (isVisible() && content_ != nullptr)
        content_->refresh();
}

} // namespace youhost
