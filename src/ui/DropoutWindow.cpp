#include "DropoutWindow.h"
#include "engine/WindowCatalog.h"
#include "Theme.h"
#include "WindowMemory.h"
#include "engine/DropoutLog.h"

#include <algorithm>

namespace youhost
{
namespace
{

juce::String elapsedText(std::int64_t nanoseconds)
{
    if (nanoseconds < 0)
        return "none";
    const auto seconds = nanoseconds / 1000000000;
    const auto hours = seconds / 3600;
    const auto minutes = (seconds / 60) % 60;
    const auto remain = seconds % 60;
    if (hours > 0)
        return juce::String(hours) + ":" + juce::String(minutes).paddedLeft('0', 2) + ":"
               + juce::String(remain).paddedLeft('0', 2);
    return juce::String(minutes) + ":" + juce::String(remain).paddedLeft('0', 2);
}

int textBlockHeight(const juce::String& text, int width)
{
    if (text.isEmpty() || width < 20)
        return 0;
    int lines = 1;
    for (int index = 0; index < text.length(); ++index)
        if (text[index] == '\n')
            ++lines;
    return lines * 16 + 8;
}

class DropoutBody : public juce::Component
{
public:
    void setData(DropoutSnapshot snapshot, juce::String loadText, std::int64_t windowNs)
    {
        snapshot_ = std::move(snapshot);
        loadText_ = std::move(loadText);
        windowNs_ = windowNs;
        repaint();
    }

    int preferredHeight(int width) const
    {
        return 72 + 12 + textBlockHeight(loadText_, width) + 220;
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(theme::background);
        auto area = getLocalBounds().reduced(4, 0).toFloat();

        const int inWindow = countMarksInWindow(snapshot_.marks.data(),
                                                static_cast<int>(snapshot_.marks.size()),
                                                snapshot_.nowNs,
                                                windowNs_);
        const bool stable = inWindow == 0;

        auto hero = area.removeFromTop(72.0f);
        graphics.setColour(stable ? theme::green : theme::red);
        graphics.setFont(juce::Font(juce::FontOptions(42.0f).withStyle("Bold")));
        graphics.drawText(stable ? "STABLE" : "DROPOUTS", hero.removeFromLeft(hero.getWidth() * 0.46f),
                          juce::Justification::centredLeft, false);

        graphics.setColour(theme::text);
        graphics.setFont(juce::Font(juce::FontOptions(16.0f)));
        auto facts = hero;
        graphics.drawText("Total  " + juce::String(snapshot_.total), facts.removeFromTop(24.0f),
                          juce::Justification::centredLeft, false);
        const auto since = snapshot_.lastSteadyNs > 0 ? snapshot_.nowNs - snapshot_.lastSteadyNs : static_cast<std::int64_t>(-1);
        graphics.setColour(theme::dim);
        graphics.drawText("Since last dropout  " + elapsedText(since), facts.removeFromTop(22.0f),
                          juce::Justification::centredLeft, false);
        graphics.drawText(juce::String(inWindow) + " in this view", facts, juce::Justification::centredLeft, false);

        area.removeFromTop(6.0f);
        if (loadText_.isNotEmpty())
        {
            const int textHeight = textBlockHeight(loadText_, static_cast<int>(area.getWidth()));
            auto dsp = area.removeFromTop(static_cast<float>(textHeight));
            graphics.setColour(theme::text);
            graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
            graphics.drawFittedText(loadText_, dsp.toNearestInt(), juce::Justification::topLeft, 64);
            area.removeFromTop(4.0f);
        }

        auto plot = area.withHeight(std::max(160.0f, area.getHeight()));
        graphics.setColour(theme::panel);
        graphics.fillRoundedRectangle(plot, 8.0f);

        const auto now = snapshot_.nowNs > 0 ? snapshot_.nowNs : 1;
        const auto start = windowNs_ > 0 ? now - windowNs_ : (snapshot_.originNs > 0 ? snapshot_.originNs : now - 60000000000LL);
        const auto span = std::max<std::int64_t>(1, now - start);
        auto inner = plot.reduced(12.0f, 10.0f);

        graphics.setColour(theme::panelEdge);
        graphics.drawLine(inner.getX(), inner.getBottom() - 16.0f, inner.getRight(), inner.getBottom() - 16.0f, 1.0f);

        const float bottom = inner.getBottom() - 16.0f;
        const float top = inner.getY() + 8.0f;
        const float height = std::max(1.0f, bottom - top);
        const auto xFor = [&](std::int64_t stamp)
        {
            const double ratio = static_cast<double>(stamp - start) / static_cast<double>(span);
            return inner.getX() + static_cast<float>(std::clamp(ratio, 0.0, 1.0)) * inner.getWidth();
        };

        juce::Path cpu;
        bool started = false;
        for (const auto& sample : snapshot_.cpu)
        {
            if (sample.steadyNs < start || sample.steadyNs > now)
                continue;
            const float x = xFor(sample.steadyNs);
            const float y = bottom - std::clamp(sample.load, 0.0f, 1.0f) * height;
            if (! started)
            {
                cpu.startNewSubPath(x, y);
                started = true;
            }
            else
            {
                cpu.lineTo(x, y);
            }
        }
        graphics.setColour(theme::green.withAlpha(0.9f));
        graphics.strokePath(cpu, juce::PathStrokeType(1.6f));

        graphics.setColour(theme::red);
        for (const auto& mark : snapshot_.marks)
        {
            if (mark.steadyNs < start || mark.steadyNs > now)
                continue;
            const float x = xFor(mark.steadyNs);
            graphics.fillRect(x - 1.0f, top, 2.5f, height);
        }

        graphics.setColour(theme::fainter);
        graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
        graphics.drawText("CPU", juce::Rectangle<float>(inner.getX(), inner.getY(), 40.0f, 14.0f),
                          juce::Justification::centredLeft, false);
        graphics.drawText("now", juce::Rectangle<float>(inner.getRight() - 36.0f, bottom, 36.0f, 16.0f),
                          juce::Justification::centredRight, false);
    }

private:
    DropoutSnapshot snapshot_;
    juce::String loadText_;
    std::int64_t windowNs_ = 600LL * 1000000000LL;
};

} // namespace

class DropoutWindow::Content : public juce::Component
{
public:
    explicit Content(AudioEngine& engine)
        : engine_(engine)
    {
        addAndMakeVisible(rangeBox_);
        rangeBox_.addItem("Last 10 min", 1);
        rangeBox_.addItem("Last 1 hour", 2);
        rangeBox_.addItem("Whole session", 3);
        rangeBox_.setSelectedId(1, juce::dontSendNotification);
        rangeBox_.onChange = [this]
        {
            body_.setData(snapshot_, loadText_, selectedWindowNs());
            resized();
        };
        rangeBox_.setMouseClickGrabsKeyboardFocus(false);

        addAndMakeVisible(resetButton_);
        resetButton_.setButtonText("Reset");
        resetButton_.setTooltip("Reset the dropout count and this graph. The CSV log is kept, with a reset line.");
        resetButton_.setMouseClickGrabsKeyboardFocus(false);
        resetButton_.onClick = [this]
        {
            engine_.resetDropouts();
            refresh();
        };

        addAndMakeVisible(viewport_);
        viewport_.setViewedComponent(&body_, false);
        viewport_.setScrollBarsShown(true, false);
    }

    ~Content() override
    {
        viewport_.setViewedComponent(nullptr, false);
    }

    void refresh()
    {
        engine_.drainDropoutLog();
        snapshot_ = engine_.dropoutSnapshot();
        loadText_ = engine_.dspLoadText();
        body_.setData(snapshot_, loadText_, selectedWindowNs());
        resized();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16, 12);
        auto row = area.removeFromTop(28);
        rangeBox_.setBounds(row.removeFromRight(180));
        row.removeFromRight(8);
        resetButton_.setBounds(row.removeFromRight(78));
        area.removeFromTop(8);
        viewport_.setBounds(area);
        const int width = std::max(1, viewport_.getMaximumVisibleWidth());
        body_.setSize(width, std::max(area.getHeight(), body_.preferredHeight(width)));
    }

private:
    std::int64_t selectedWindowNs() const
    {
        if (rangeBox_.getSelectedId() == 2)
            return 3600LL * 1000000000LL;
        if (rangeBox_.getSelectedId() == 3)
            return 0;
        return 600LL * 1000000000LL;
    }

    AudioEngine& engine_;
    juce::ComboBox rangeBox_;
    juce::TextButton resetButton_;
    juce::Viewport viewport_;
    DropoutBody body_;
    DropoutSnapshot snapshot_;
    juce::String loadText_;
};

DropoutWindow::DropoutWindow(AudioEngine& engine, AppSettings& settings)
    : juce::DocumentWindow("DROPOUTS", theme::panel, juce::DocumentWindow::closeButton),
      engine_(engine),
      settings_(settings)
{
    auto content = std::make_unique<Content>(engine_);
    content_ = content.get();
    setUsingNativeTitleBar(true);
    setContentOwned(content.release(), true);
    prepareRememberedWindow(*this, settings_, "windowDropouts", dropoutWindowWidth(), dropoutWindowHeight(), 520, 300);
    setVisible(false);
    startTimerHz(4);
}

DropoutWindow::~DropoutWindow()
{
    saveRememberedWindow(*this, settings_, "windowDropouts", dropoutWindowWidth(), dropoutWindowHeight());
    stopTimer();
}

void DropoutWindow::toggle()
{
    setVisible(! isVisible());
    if (isVisible())
    {
        if (content_ != nullptr)
            content_->refresh();
        toFront(true);
    }
}

void DropoutWindow::closeButtonPressed()
{
    saveRememberedWindow(*this, settings_, "windowDropouts", dropoutWindowWidth(), dropoutWindowHeight());
    setVisible(false);
}

void DropoutWindow::timerCallback()
{
    if (isVisible() && content_ != nullptr)
        content_->refresh();
}

} // namespace youhost
