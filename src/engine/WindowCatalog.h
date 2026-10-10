#pragma once

#include "LatencyCard.h"

namespace youhost
{

// Heights are the content that must be on screen the first time a window opens.
// A smaller remembered size is kept only after the user has resized it, and the
// content then scrolls.

inline int cpuCardHeight(int workers) noexcept
{
    if (workers < 0)
        workers = 0;
    if (workers > 7)
        workers = 7;
    const int pad = 28;
    const int inner = 16 + 36 + 8 + 28 + 10 + 16 + 4 + 26 + workers * 32 + 8 + 48;
    return pad + inner;
}

inline int cpuWindowHeight() noexcept
{
    return cpuCardHeight(7) + 36;
}

inline int cpuWindowWidth() noexcept
{
    return 520;
}

inline int dropoutBodyHeight(int textLines) noexcept
{
    if (textLines < 1)
        textLines = 1;
    return 72 + 12 + textLines * 16 + 220;
}

inline int dropoutWindowContentHeight(int textLines) noexcept
{
    return 12 + 28 + 8 + dropoutBodyHeight(textLines) + 12;
}

inline int dropoutWindowWidth() noexcept
{
    return 760;
}

inline int dropoutWindowHeight() noexcept
{
    return dropoutWindowContentHeight(4) + 36;
}

inline int scannerButtonRowWidth() noexcept
{
    return 12 * 2 + 72 + 84 + 72 + 118 + 100 + 148 + 6 * 5;
}

inline int scannerControlHeight() noexcept
{
    return 12 * 2 + 28 + 4 + 28 + 6 + 22 + 22 + 22 + 6 + 14 + 6 + 22 + 6 + 20 + 120;
}

inline int scannerWindowWidth() noexcept
{
    return 860;
}

inline int scannerWindowHeight() noexcept
{
    return 560;
}

inline int groupRenameContentHeight() noexcept
{
    return 12 + 28 + 8 + 28 + 12 + 28 + 12;
}

inline int groupRenameWindowWidth() noexcept
{
    return 560;
}

inline int groupRenameWindowHeight() noexcept
{
    return groupRenameContentHeight() + 36;
}

inline int pluginPickerContentHeight() noexcept
{
    return 8 + 26 + 6 + 28 + 4 + 18 + 6 + 240 + 8;
}

inline int pluginListWindowWidth() noexcept
{
    return 480;
}

inline int pluginListWindowHeight() noexcept
{
    return pluginPickerContentHeight() + 48;
}

inline int helpContentHeight(int characters, int textWidth) noexcept
{
    const int perLine = std::max(1, textWidth / 7);
    const int lines = characters <= 0 ? 1 : (characters + perLine - 1) / perLine;
    return 16 + lines * 16 + 16;
}

inline int helpWindowWidth() noexcept
{
    return 720;
}

inline int helpWindowHeightFor(int characters) noexcept
{
    const int content = helpContentHeight(characters, helpWindowWidth() - 48);
    const int window = content + 36;
    return window > 860 ? 860 : window;
}

inline int setupWindowWidth() noexcept
{
    return 720;
}

inline int setupWindowHeight() noexcept
{
    return 640;
}

inline int startupWindowWidth() noexcept
{
    return 780;
}

inline int startupWindowHeight() noexcept
{
    return 680;
}

inline int placeSessionWindowWidth() noexcept
{
    return 560;
}

inline int placeSessionWindowHeight() noexcept
{
    return 320;
}

} // namespace youhost
