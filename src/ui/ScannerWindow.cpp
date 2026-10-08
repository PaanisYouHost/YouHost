#include "ScannerWindow.h"
#include "Theme.h"
#include "WindowMemory.h"

#include <algorithm>

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

    void setRows(std::vector<juce::String> lines, std::vector<juce::String> identifiers)
    {
        lines_ = std::move(lines);
        identifiers_ = std::move(identifiers);
    }

    juce::String identifier(int row) const
    {
        if (row < 0 || row >= static_cast<int>(identifiers_.size()))
            return {};
        return identifiers_[static_cast<std::size_t>(row)];
    }

private:
    std::vector<juce::String> lines_;
    std::vector<juce::String> identifiers_;
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
        addAndMakeVisible(stopButton_);
        addAndMakeVisible(clearButton_);
        addAndMakeVisible(fileButton_);
        addAndMakeVisible(selectedButton_);
        addAndMakeVisible(wavesButton_);
        addAndMakeVisible(appleButton_);
        addAndMakeVisible(instrumentButton_);
        addAndMakeVisible(status_);
        addAndMakeVisible(knownTitle_);
        addAndMakeVisible(failedTitle_);
        addAndMakeVisible(knownList_);
        addAndMakeVisible(failedList_);

        scanButton_.setButtonText("Scan");
        rescanButton_.setButtonText("Rescan");
        stopButton_.setButtonText("Stop");
        clearButton_.setButtonText("Clear failed");
        fileButton_.setButtonText("Scan file…");
        selectedButton_.setButtonText("Rescan selected");
        wavesButton_.setButtonText("Scan Waves shells");
        appleButton_.setButtonText("Show Apple Audio Units in inserts");
        instrumentButton_.setButtonText("Show instruments in inserts");

        wavesButton_.setToggleState(engine_.pluginCatalogue().scanWavesShells(), juce::dontSendNotification);
        appleButton_.setToggleState(engine_.pluginCatalogue().showAppleInInserts(), juce::dontSendNotification);
        instrumentButton_.setToggleState(engine_.pluginCatalogue().showInstrumentsInInserts(), juce::dontSendNotification);

        scanButton_.onClick = [this] { engine_.pluginCatalogue().scanNew(); };
        rescanButton_.onClick = [this] { engine_.pluginCatalogue().rescan(); };
        stopButton_.onClick = [this] { engine_.pluginCatalogue().stopScan(); };
        clearButton_.onClick = [this] { engine_.pluginCatalogue().clearFailedAndScan(); };
        fileButton_.onClick = [this] { chooseFile(); };
        selectedButton_.onClick = [this]
        {
            const auto identifier = knownModel_.identifier(knownList_.getSelectedRow());
            if (identifier.isNotEmpty())
                engine_.pluginCatalogue().rescanIdentifier(identifier);
        };
        wavesButton_.onClick = [this]
        {
            engine_.pluginCatalogue().setScanWavesShells(wavesButton_.getToggleState());
        };
        appleButton_.onClick = [this]
        {
            engine_.pluginCatalogue().setShowAppleInInserts(appleButton_.getToggleState());
        };
        instrumentButton_.onClick = [this]
        {
            engine_.pluginCatalogue().setShowInstrumentsInInserts(instrumentButton_.getToggleState());
        };

        for (auto* button : { &scanButton_, &rescanButton_, &stopButton_, &clearButton_, &fileButton_, &selectedButton_ })
            button->setMouseClickGrabsKeyboardFocus(false);
        for (auto* toggle : { &wavesButton_, &appleButton_, &instrumentButton_ })
        {
            toggle->setMouseClickGrabsKeyboardFocus(false);
            toggle->setClickingTogglesState(true);
        }

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

        wavesButton_.setTooltip("Off by default. A Waves shell lists hundreds of plugins and is scanned last when this is on.");
        appleButton_.setTooltip("Apple's built-in Audio Units stay out of the insert list until this is on.");
        instrumentButton_.setTooltip("Instruments and plugins with no audio input stay out of the insert list until this is on.");
    }

    void refresh()
    {
        const auto status = engine_.pluginCatalogue().status();
        juce::String line = status.text;
        if (status.scanning)
        {
            line = juce::String(status.done) + " / " + juce::String(status.total);
            if (status.current.isNotEmpty())
                line << "   " << status.current;
            const int seconds = std::max(0, status.elapsedMs / 1000);
            line << "   " << (seconds / 60) << ":" << juce::String(seconds % 60).paddedLeft('0', 2);
        }
        status_.setText(line, juce::dontSendNotification);
        progress_ = status.total > 0 ? static_cast<float>(status.done) / static_cast<float>(status.total) : (status.scanning ? 0.0f : 1.0f);
        scanning_ = status.scanning;

        scanButton_.setEnabled(! status.scanning);
        rescanButton_.setEnabled(! status.scanning);
        clearButton_.setEnabled(! status.scanning);
        fileButton_.setEnabled(! status.scanning);
        selectedButton_.setEnabled(! status.scanning);
        stopButton_.setEnabled(status.scanning);

        const double knownScroll = knownList_.getVerticalPosition();
        const double failedScroll = failedList_.getVerticalPosition();
        const int knownSelection = knownList_.getSelectedRow();

        std::vector<juce::String> known;
        std::vector<juce::String> knownIds;
        for (const auto& type : engine_.pluginCatalogue().types())
        {
            juce::String text = type.name;
            if (type.pluginFormatName.isNotEmpty())
                text << "   " << type.pluginFormatName;
            if (type.manufacturerName.isNotEmpty())
                text << "   " << type.manufacturerName;
            known.push_back(text);
            knownIds.push_back(type.fileOrIdentifier);
        }
        knownModel_.setRows(std::move(known), std::move(knownIds));
        knownList_.updateContent();
        knownList_.setVerticalPosition(knownScroll);
        if (knownSelection >= 0)
            knownList_.selectRow(knownSelection, true, true);

        std::vector<juce::String> failed;
        std::vector<juce::String> failedIds;
        for (const auto& failure : engine_.pluginCatalogue().failures())
        {
            juce::String text = failure.reason;
            if (failure.identifier.isNotEmpty())
                text << "   " << failure.identifier;
            failed.push_back(text);
            failedIds.push_back(failure.identifier);
        }
        failedModel_.setRows(std::move(failed), std::move(failedIds));
        failedList_.updateContent();
        failedList_.setVerticalPosition(failedScroll);

        knownTitle_.setText("Found (" + juce::String(status.known) + ")", juce::dontSendNotification);
        auto failedLabel = "Failed (" + juce::String(status.failed) + ")";
        if (status.skippedWaves > 0)
            failedLabel << "   Waves skipped " << status.skippedWaves;
        failedTitle_.setText(failedLabel, juce::dontSendNotification);
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12);
        juce::Component* buttons[] = { &scanButton_, &rescanButton_, &stopButton_, &clearButton_, &fileButton_, &selectedButton_ };
        const int widths[] = { 72, 84, 72, 118, 100, 148 };
        auto row = area.removeFromTop(28);
        for (int index = 0; index < 6; ++index)
        {
            if (row.getWidth() < widths[index])
            {
                area.removeFromTop(4);
                row = area.removeFromTop(28);
            }
            buttons[index]->setBounds(row.removeFromLeft(std::min(widths[index], row.getWidth())));
            row.removeFromLeft(6);
        }
        area.removeFromTop(6);
        wavesButton_.setBounds(area.removeFromTop(22));
        appleButton_.setBounds(area.removeFromTop(22));
        instrumentButton_.setBounds(area.removeFromTop(22));
        area.removeFromTop(6);
        progressArea_ = area.removeFromTop(14);
        area.removeFromTop(6);
        status_.setBounds(area.removeFromTop(22));
        area.removeFromTop(6);

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
        if (progressArea_.isEmpty())
            return;
        graphics.setColour(theme::panel);
        graphics.fillRoundedRectangle(progressArea_.toFloat(), 4.0f);
        if (progress_ > 0.0f)
        {
            auto filled = progressArea_.toFloat();
            filled.setWidth(filled.getWidth() * std::clamp(progress_, 0.0f, 1.0f));
            graphics.setColour(scanning_ ? theme::amber : theme::green);
            graphics.fillRoundedRectangle(filled, 4.0f);
        }
    }

private:
    void chooseFile()
    {
        if (chooser_ != nullptr)
            return;
        const auto start = juce::File("/Library/Audio/Plug-Ins");
        chooser_ = std::make_unique<juce::FileChooser>("Scan one plugin",
                                                       start.isDirectory() ? start : juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                                                       "*.vst3;*.component",
                                                       true);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [this](const juce::FileChooser& chooser)
                              {
                                  const auto chosen = chooser.getResult();
                                  juce::MessageManager::callAsync([this, chosen]
                                  {
                                      chooser_.reset();
                                      if (chosen.getFullPathName().isNotEmpty())
                                          engine_.pluginCatalogue().scanFile(chosen);
                                      refresh();
                                  });
                              });
    }

    AudioEngine& engine_;
    juce::TextButton scanButton_;
    juce::TextButton rescanButton_;
    juce::TextButton stopButton_;
    juce::TextButton clearButton_;
    juce::TextButton fileButton_;
    juce::TextButton selectedButton_;
    juce::ToggleButton wavesButton_;
    juce::ToggleButton appleButton_;
    juce::ToggleButton instrumentButton_;
    juce::Label status_;
    juce::Label knownTitle_;
    juce::Label failedTitle_;
    NameList knownModel_;
    NameList failedModel_;
    juce::ListBox knownList_;
    juce::ListBox failedList_;
    juce::Rectangle<int> progressArea_;
    float progress_ = 0.0f;
    bool scanning_ = false;
    std::unique_ptr<juce::FileChooser> chooser_;
};

ScannerWindow::ScannerWindow(AudioEngine& engine, AppSettings& settings)
    : juce::DocumentWindow("Plugin scanner",
                           theme::panel,
                           juce::DocumentWindow::closeButton),
      engine_(engine),
      settings_(settings)
{
    auto content = std::make_unique<Content>(engine_);
    content_ = content.get();
    setUsingNativeTitleBar(true);
    setContentOwned(content.release(), true);
    prepareRememberedWindow(*this, settings_, "windowScanner", 860, 560, 640, 420);
    setVisible(false);
    startTimerHz(4);
}

ScannerWindow::~ScannerWindow()
{
    saveRememberedWindow(*this, settings_, "windowScanner");
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
    saveRememberedWindow(*this, settings_, "windowScanner");
    setVisible(false);
}

void ScannerWindow::timerCallback()
{
    if (isVisible() && content_ != nullptr)
        content_->refresh();
}

} // namespace youhost
