#pragma once

#include "AppSettings.h"
#include "engine/WindowFit.h"

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
    const auto saved = parseWindowState(settings.loadNamedWindow(key).toStdString());
    int screenWidth = 0;
    int screenHeight = 0;
    const auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay().userArea;
    if (! area.isEmpty())
    {
        screenWidth = area.getWidth();
        screenHeight = area.getHeight();
    }
    const auto open = windowOpenSize(defaultWidth, defaultHeight, screenWidth, screenHeight, saved);
    if (keepRememberedWindow(saved, defaultWidth, defaultHeight)
        && window.restoreWindowStateFromString(juce::String(juceWindowState(saved))))
    {
        if (window.getWidth() != open.width || window.getHeight() != open.height)
            window.setSize(open.width, open.height);
        return;
    }
    window.centreWithSize(open.width, open.height);
}

inline void saveRememberedWindow(juce::DocumentWindow& window,
                                 AppSettings& settings,
                                 const juce::String& key,
                                 int fitWidth,
                                 int fitHeight)
{
    const auto stamped = stampWindowState(window.getWindowStateAsString().toStdString(), fitWidth, fitHeight);
    settings.saveNamedWindow(key, juce::String(stamped));
}

} // namespace youhost
