#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace youmix
{

struct EditorSize
{
    int width = 0;
    int height = 0;
};

// A missing or tiny display (headless build) keeps the 1280x720-class default.
inline juce::Rectangle<int> displayWorkArea()
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto* display = displays.getPrimaryDisplay();
    if (display == nullptr)
        return { 0, 0, 1280, 720 };
    const auto area = display->userBounds.toNearestInt();
    if (area.getWidth() < 800 || area.getHeight() < 500)
        return { 0, 0, 1280, 720 };
    return area;
}

// Preferred size, then clamped so the editor itself fits under a host title bar
// on a small laptop. A remembered size from a larger monitor is shrunk here.
inline EditorSize clampEditorSize(int width, int height, int minW, int minH, int prefW, int prefH)
{
    if (width < minW || height < minH)
    {
        width = prefW;
        height = prefH;
    }
    const auto screen = displayWorkArea();
    const int maxW = juce::jmax(minW, screen.getWidth() - 32);
    const int maxH = juce::jmax(minH, screen.getHeight() - 64);
    return { juce::jlimit(minW, maxW, width), juce::jlimit(minH, maxH, height) };
}

// Hosts can restore a window that is taller than the current screen after the
// editor's setSize. Shrink that native window once, into the display work area.
inline void fitTopLevelToDisplay(juce::Component& editor)
{
    auto* top = editor.getTopLevelComponent();
    if (top == nullptr)
        top = &editor;
    if (top->getPeer() == nullptr)
        return;

    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto* display = displays.getDisplayForRect(top->getScreenBounds());
    if (display == nullptr)
        display = displays.getPrimaryDisplay();
    if (display == nullptr)
        return;

    auto limit = display->userBounds.toNearestInt().reduced(12);
    if (limit.getWidth() < 480 || limit.getHeight() < 320)
        return;

    const auto screen = top->getScreenBounds();
    int width = screen.getWidth();
    int height = screen.getHeight();
    int x = screen.getX();
    int y = screen.getY();
    bool adjust = false;
    if (width > limit.getWidth())
    {
        width = limit.getWidth();
        adjust = true;
    }
    if (height > limit.getHeight())
    {
        height = limit.getHeight();
        adjust = true;
    }
    if (x < limit.getX())
    {
        x = limit.getX();
        adjust = true;
    }
    if (y < limit.getY())
    {
        y = limit.getY();
        adjust = true;
    }
    if (x + width > limit.getRight())
    {
        x = limit.getRight() - width;
        adjust = true;
    }
    if (y + height > limit.getBottom())
    {
        y = limit.getBottom() - height;
        adjust = true;
    }
    if (adjust)
        top->setBounds(x, y, width, height);
}

} // namespace youmix
