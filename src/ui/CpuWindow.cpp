#include "CpuWindow.h"
#include "Theme.h"
#include "WindowMemory.h"
#include "engine/WindowCatalog.h"

#include <algorithm>

namespace youhost
{

class CpuWindow::Content : public juce::Component
{
public:
    explicit Content(AudioEngine& engine)
        : engine_(engine)
    {
    }

    void refresh()
    {
        total_ = engine_.cpuUsage() * 100.0f;
        meters_ = engine_.cpuMeters();
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(theme::background);
        auto area = getLocalBounds().reduced(16, 14);

        graphics.setColour(theme::dim);
        graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
        graphics.drawText("System usage", area.removeFromTop(16), juce::Justification::centredLeft, false);

        auto hero = area.removeFromTop(36);
        graphics.setColour(theme::text);
        graphics.setFont(juce::Font(juce::FontOptions(28.0f).withStyle("Bold")));
        graphics.drawText(percentText(total_) + "%", hero.removeFromLeft(120), juce::Justification::centredLeft, false);
        graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
        graphics.setColour(theme::dim);
        graphics.drawText("total CPU", hero, juce::Justification::centredLeft, false);

        area.removeFromTop(8);
        drawBar(graphics, area.removeFromTop(28), "Audio", meters_.callbackPercent);
        area.removeFromTop(10);

        graphics.setColour(theme::fainter);
        graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
        graphics.drawText("Cores", area.removeFromTop(16), juce::Justification::centredLeft, false);
        area.removeFromTop(4);

        const int workers = std::clamp(meters_.workers, 0, 7);
        drawBar(graphics, area.removeFromTop(26), "Audio thread", meters_.percent[0]);
        for (int index = 0; index < workers; ++index)
        {
            area.removeFromTop(6);
            drawBar(graphics, area.removeFromTop(26), "Worker " + juce::String(index + 1),
                    meters_.percent[static_cast<std::size_t>(index + 1)]);
        }

        graphics.setColour(theme::fainter);
        graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
        graphics.drawFittedText("Each bar is that thread's time against the buffer. "
                                "The audio thread runs the callback. Workers share the plugin chains.",
                                area.removeFromTop(48),
                                juce::Justification::topLeft,
                                3);
    }

private:
    static juce::String percentText(float value)
    {
        return juce::String(juce::roundToInt(std::clamp(value, 0.0f, 9999.0f)));
    }

    static juce::Colour barColour(float percent)
    {
        if (percent >= 90.0f)
            return theme::red;
        if (percent >= 70.0f)
            return theme::amber;
        return theme::green;
    }

    static void drawBar(juce::Graphics& graphics, juce::Rectangle<int> row, const juce::String& label, float percent)
    {
        graphics.setColour(theme::text);
        graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
        graphics.drawText(label, row.removeFromLeft(110), juce::Justification::centredLeft, true);
        graphics.drawText(percentText(percent) + "%", row.removeFromRight(64), juce::Justification::centredRight, false);
        row = row.reduced(6, 6);
        graphics.setColour(theme::meterTrack);
        graphics.fillRoundedRectangle(row.toFloat(), 3.0f);
        const float filled = std::clamp(percent, 0.0f, 100.0f) / 100.0f * static_cast<float>(row.getWidth());
        if (filled > 0.5f)
        {
            graphics.setColour(barColour(percent));
            graphics.fillRoundedRectangle(row.toFloat().withWidth(filled), 3.0f);
        }
    }

    AudioEngine& engine_;
    float total_ = 0.0f;
    CpuMeters meters_ {};
};

class CpuShell : public juce::Component
{
public:
    explicit CpuShell(AudioEngine& engine)
        : card_(engine)
    {
        addAndMakeVisible(viewport_);
        viewport_.setViewedComponent(&card_, false);
        viewport_.setScrollBarsShown(true, false);
    }

    ~CpuShell() override
    {
        viewport_.setViewedComponent(nullptr, false);
    }

    CpuWindow::Content* card() { return &card_; }

    void resized() override
    {
        viewport_.setBounds(getLocalBounds());
        const int width = std::max(1, viewport_.getMaximumVisibleWidth());
        card_.setSize(width, std::max(cpuCardHeight(7), viewport_.getHeight()));
    }

private:
    CpuWindow::Content card_;
    juce::Viewport viewport_;
};

CpuWindow::CpuWindow(AudioEngine& engine, AppSettings& settings)
    : juce::DocumentWindow("CPU", theme::panel, juce::DocumentWindow::closeButton),
      engine_(engine),
      settings_(settings)
{
    auto content = std::make_unique<CpuShell>(engine_);
    content_ = content->card();
    setUsingNativeTitleBar(true);
    setContentOwned(content.release(), true);
    prepareRememberedWindow(*this, settings_, "windowCpu", cpuWindowWidth(), cpuWindowHeight(), 320, 260);
    setVisible(false);
    startTimerHz(4);
}

CpuWindow::~CpuWindow()
{
    saveRememberedWindow(*this, settings_, "windowCpu", cpuWindowWidth(), cpuWindowHeight());
    stopTimer();
}

void CpuWindow::toggle()
{
    setVisible(! isVisible());
    if (isVisible())
    {
        if (content_ != nullptr)
            content_->refresh();
        toFront(true);
    }
}

void CpuWindow::closeButtonPressed()
{
    saveRememberedWindow(*this, settings_, "windowCpu", cpuWindowWidth(), cpuWindowHeight());
    setVisible(false);
}

void CpuWindow::timerCallback()
{
    if (isVisible() && content_ != nullptr)
        content_->refresh();
}

} // namespace youhost
