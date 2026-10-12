#pragma once

#include "engine/SyncLost.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>

namespace youhost
{

// juce::String(const char*) is ASCII and turns ä/ö into the wrong glyphs.
inline juce::String syncLostUtf8(const char* text)
{
    return juce::String(juce::CharPointer_UTF8(text == nullptr ? "" : text));
}

// The default UI face on some Macs has no ä/ö. Pick a face that does.
inline const juce::String& syncLostTypefaceName()
{
    static const juce::String chosen = []
    {
        const char* faces[] = {
            "Helvetica Neue",
            "Helvetica",
            "Lucida Grande",
            "Arial",
            "DejaVu Sans",
            "Noto Sans",
            "Liberation Sans",
            "Cantarell",
        };
        const auto installed = juce::Font::findAllTypefaceNames();
        for (const char* face : faces)
        {
            const juce::String candidate(face);
            if (installed.contains(candidate))
                return candidate;
        }
        return juce::Font::getDefaultSansSerifFontName();
    }();
    return chosen;
}

inline juce::Font syncLostFont(float height)
{
    return juce::Font(juce::FontOptions(height).withName(syncLostTypefaceName()).withStyle("Bold"));
}

// Centered while the card is gone. A small bottom-left box after audio returns.
// The box repaints itself only when its blink phase changes.
class SyncLostOverlay : public juce::Component
{
public:
    std::function<void()> onOpenDetails;

    SyncLostOverlay()
    {
        setOpaque(true);
        setAlwaysOnTop(true);
        setInterceptsMouseClicks(true, false);
        setVisible(false);
    }

    SyncLostChrome place() const noexcept { return place_; }

    std::string restoredLine() const
    {
        return syncRestoredLine(hour_, minute_, second_, gapMs_);
    }

    // True only when the alarm appears, moves, or goes away.
    bool sync(const SyncLostState& state)
    {
        const auto next = syncLostChrome(state);
        if (next == place_ && state.hour == hour_ && state.minute == minute_ && state.second == second_ && state.gapMs == gapMs_)
            return false;

        place_ = next;
        hour_ = state.hour;
        minute_ = state.minute;
        second_ = state.second;
        gapMs_ = state.gapMs;
        if (place_ == SyncLostChrome::hidden)
            lit_ = true;
        repaint();
        return true;
    }

    // Corner only. A steady phase does not repaint.
    bool setBlinkPhase(bool lit)
    {
        if (place_ != SyncLostChrome::corner || lit == lit_)
            return false;
        lit_ = lit;
        repaint();
        return true;
    }

    void paint(juce::Graphics& graphics) override
    {
        const bool center = place_ == SyncLostChrome::center;
        graphics.fillAll(center || lit_ ? juce::Colour(0xff8d1424) : juce::Colour(0xff3a1016));
        graphics.setColour(juce::Colours::white);
        if (center)
        {
            auto area = getLocalBounds().reduced(24, 16);
            graphics.setFont(syncLostFont(72.0f));
            graphics.drawFittedText(syncLostUtf8(kSyncLostTitle), area.removeFromTop(area.getHeight() * 2 / 3), juce::Justification::centred, 2);
            graphics.setColour(juce::Colour(0xffffc9c9));
            graphics.setFont(syncLostFont(28.0f));
            graphics.drawFittedText(syncLostUtf8(kSyncLostFinnish), area, juce::Justification::centred, 2);
            return;
        }

        graphics.setFont(syncLostFont(15.0f));
        graphics.drawFittedText(syncLostUtf8(kSyncLostTitle), getLocalBounds().reduced(8, 2), juce::Justification::centred, 1);
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        // The centered window stays up while the card is gone.
        // The corner box only asks for the dialog. It does not touch the device.
        if (place_ != SyncLostChrome::corner || onOpenDetails == nullptr)
            return;
        onOpenDetails();
    }

private:
    SyncLostChrome place_ = SyncLostChrome::hidden;
    bool lit_ = true;
    int hour_ = 0;
    int minute_ = 0;
    int second_ = 0;
    std::int64_t gapMs_ = 0;
};

// Non-modal. OK clears the notice and does not stop audio or a take.
class SyncRestoredDialog : public juce::Component
{
public:
    std::function<void()> onOk;

    SyncRestoredDialog()
    {
        setOpaque(true);
        setAlwaysOnTop(true);
        setVisible(false);
        addAndMakeVisible(ok_);
        ok_.onClick = [this]
        {
            if (onOk != nullptr)
                onOk();
        };
    }

    void setMessage(const std::string& line)
    {
        if (line == message_)
            return;
        message_ = line;
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(juce::Colour(0xff14060a));
        graphics.setColour(juce::Colours::white);
        graphics.setFont(syncLostFont(22.0f));
        auto area = getLocalBounds().reduced(24, 18);
        area.removeFromBottom(48);
        graphics.drawFittedText(juce::String(message_), area, juce::Justification::centred, 4);
    }

    void resized() override
    {
        ok_.setBounds(getWidth() / 2 - 44, getHeight() - 52, 88, 32);
    }

private:
    juce::TextButton ok_ { "OK" };
    std::string message_;
};

} // namespace youhost
