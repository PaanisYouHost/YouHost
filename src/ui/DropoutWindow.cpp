#include "DropoutWindow.h"
#include "Theme.h"
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
        rangeBox_.onChange = [this] { repaint(); };
        rangeBox_.setMouseClickGrabsKeyboardFocus(false);
    }

    void refresh()
    {
        engine_.drainDropoutLog();
        snapshot_ = engine_.dropoutSnapshot();
        repaint();
    }

    void resized() override
    {
        rangeBox_.setBounds(getLocalBounds().reduced(16, 12).removeFromTop(28).removeFromRight(180));
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(theme::background);
        auto area = getLocalBounds().reduced(16, 12).toFloat();
        rangeBox_.setBounds(area.removeFromTop(28.0f).removeFromRight(180.0f).toNearestInt());
        area.removeFromTop(8.0f);

        const auto windowNs = selectedWindowNs();
        const int inWindow = countMarksInWindow(snapshot_.marks.data(),
                                                static_cast<int>(snapshot_.marks.size()),
                                                snapshot_.nowNs,
                                                windowNs);
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

        area.removeFromTop(8.0f);
        auto plot = area;
        graphics.setColour(theme::panel);
        graphics.fillRoundedRectangle(plot, 8.0f);

        const auto now = snapshot_.nowNs > 0 ? snapshot_.nowNs : 1;
        const auto start = windowNs > 0 ? now - windowNs : (snapshot_.originNs > 0 ? snapshot_.originNs : now - 60000000000LL);
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
    DropoutSnapshot snapshot_;
};

DropoutWindow::DropoutWindow(AudioEngine& engine)
    : juce::DocumentWindow("Dropouts", theme::panel, juce::DocumentWindow::closeButton),
      engine_(engine)
{
    auto content = std::make_unique<Content>(engine_);
    content_ = content.get();
    setUsingNativeTitleBar(true);
    setContentOwned(content.release(), true);
    setResizable(true, false);
    centreWithSize(720, 360);
    setVisible(false);
    startTimerHz(4);
}

DropoutWindow::~DropoutWindow()
{
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
    setVisible(false);
}

void DropoutWindow::timerCallback()
{
    if (isVisible() && content_ != nullptr)
        content_->refresh();
}

} // namespace youhost
