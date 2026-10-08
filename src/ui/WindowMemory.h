#pragma once

#include "AppSettings.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace youhost
{

inline void prepareRememberedWindow(juce::DocumentWindow& window,
                                    AppSettings& settings,
                                    const juce::String& key,
                                    int defaultWidth,
                                    int defaultHeight,
                                    int minWidth,
                                    int minHeight)
{
    window.setResizable(true, false);
    window.setResizeLimits(minWidth, minHeight, 4000, 2400);
    const auto stored = settings.loadNamedWindow(key);
    if (stored.isEmpty() || ! window.restoreWindowStateFromString(stored))
        window.centreWithSize(defaultWidth, defaultHeight);
}

inline void saveRememberedWindow(juce::DocumentWindow& window, AppSettings& settings, const juce::String& key)
{
    settings.saveNamedWindow(key, window.getWindowStateAsString());
}

} // namespace youhost
