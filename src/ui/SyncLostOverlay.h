#pragma once

#include "engine/SyncLost.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>

namespace youhost
{

// One solid layer over the main window. It repaints only when the words change.
// A click after audio is back hides it. The click does not touch the device.
class SyncLostOverlay : public juce::Component
{
public:
    std::function<void()> onAcknowledge;

    SyncLostOverlay()
    {
        setOpaque(true);
        setAlwaysOnTop(true);
        setInterceptsMouseClicks(true, false);
        setVisible(false);
    }

    // True only when the alarm appears, changes, or goes away. A steady
    // overlay compares a few integers and does not repaint.
    bool sync(const SyncLostState& state)
    {
        const bool visible = state.showAlarm();
        const bool lost = state.isMissing();
        if (visible == showing_ && lost == lost_ && state.hour == hour_ && state.minute == minute_
            && state.second == second_ && state.gapMs == gapMs_)
            return false;

        showing_ = visible;
        lost_ = lost;
        hour_ = state.hour;
        minute_ = state.minute;
        second_ = state.second;
        gapMs_ = state.gapMs;
        if (lost_)
        {
            primary_ = kSyncLostTitle;
            secondary_ = kSyncLostFinnish;
        }
        else if (showing_)
        {
            primary_ = syncRestoredLine(hour_, minute_, second_, gapMs_);
            secondary_ = kSyncRestoredFinnish;
        }
        else
        {
            primary_.clear();
            secondary_.clear();
        }
        setVisible(showing_);
        if (showing_)
            repaint();
        return true;
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(juce::Colour(0xff14060a));
        auto area = getLocalBounds().reduced(48, 32);
        const int titleH = lost_ ? 150 : 200;
        const int block = titleH + 16 + 64 + (lost_ ? 0 : 44);
        if (area.getHeight() > block)
            area = area.withSizeKeepingCentre(area.getWidth(), block);

        graphics.setColour(lost_ ? juce::Colour(0xffff2438) : juce::Colours::white);
        graphics.setFont(juce::Font(juce::FontOptions(lost_ ? 84.0f : 40.0f).withStyle("Bold")));
        graphics.drawFittedText(juce::String(primary_), area.removeFromTop(titleH), juce::Justification::centred, lost_ ? 2 : 4);

        area.removeFromTop(16);
        graphics.setColour(juce::Colour(0xffffc9c9));
        graphics.setFont(juce::Font(juce::FontOptions(36.0f).withStyle("Bold")));
        graphics.drawFittedText(juce::String(secondary_), area.removeFromTop(64), juce::Justification::centred, 2);

        if (! lost_)
        {
            graphics.setColour(juce::Colour(0xffc8b8b8));
            graphics.setFont(juce::Font(juce::FontOptions(18.0f)));
            graphics.drawFittedText("Click to close  /  Sulje napsauttamalla",
                                    area.removeFromTop(44),
                                    juce::Justification::centred,
                                    1);
        }
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        // Message thread only. While the card is still gone the overlay stays.
        // After audio is back, the click hides the notice and does not close the device.
        if (lost_ || onAcknowledge == nullptr)
            return;
        onAcknowledge();
    }

private:
    bool showing_ = false;
    bool lost_ = false;
    int hour_ = 0;
    int minute_ = 0;
    int second_ = 0;
    std::int64_t gapMs_ = 0;
    std::string primary_;
    std::string secondary_;
};

} // namespace youhost
