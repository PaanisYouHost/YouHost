#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace youhost
{

// A window opens at max(remembered size, content size), then clamps to the
// screen. A remembered size smaller than the content is ignored.
struct WindowFit
{
    int width = 0;
    int height = 0;
};

struct SavedWindowSize
{
    bool valid = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int fitWidth = 0;
    int fitHeight = 0;
    bool hasFit = false;
};

inline int windowFitToken(const std::string& token, bool& ok) noexcept
{
    if (token.empty())
    {
        ok = false;
        return 0;
    }
    int sign = 1;
    std::size_t index = 0;
    if (token[0] == '-')
    {
        sign = -1;
        index = 1;
    }
    if (index >= token.size())
    {
        ok = false;
        return 0;
    }
    int value = 0;
    for (; index < token.size(); ++index)
    {
        const char character = token[index];
        if (character < '0' || character > '9')
        {
            ok = false;
            return 0;
        }
        value = value * 10 + (character - '0');
    }
    return sign * value;
}

inline SavedWindowSize parseWindowState(const std::string& text)
{
    SavedWindowSize saved;
    std::vector<std::string> parts;
    std::string token;
    for (const char character : text)
    {
        if (character == ' ' || character == '\n' || character == '\t')
        {
            if (! token.empty())
            {
                parts.push_back(token);
                token.clear();
            }
        }
        else
        {
            token.push_back(character);
        }
    }
    if (! token.empty())
        parts.push_back(token);
    if (parts.size() < 4)
        return saved;

    bool ok = true;
    const int x = windowFitToken(parts[0], ok);
    const int y = windowFitToken(parts[1], ok);
    const int width = windowFitToken(parts[2], ok);
    const int height = windowFitToken(parts[3], ok);
    if (! ok || width <= 0 || height <= 0)
        return saved;

    saved.valid = true;
    saved.x = x;
    saved.y = y;
    saved.width = width;
    saved.height = height;

    for (std::size_t index = 4; index + 2 < parts.size(); ++index)
    {
        if (parts[index] != "fit")
            continue;
        bool fitOk = true;
        const int fitWidth = windowFitToken(parts[index + 1], fitOk);
        const int fitHeight = windowFitToken(parts[index + 2], fitOk);
        if (fitOk && fitWidth > 0 && fitHeight > 0)
        {
            saved.fitWidth = fitWidth;
            saved.fitHeight = fitHeight;
            saved.hasFit = true;
        }
    }
    return saved;
}

inline std::string juceWindowState(const SavedWindowSize& saved)
{
    return std::to_string(saved.x) + " " + std::to_string(saved.y) + " "
         + std::to_string(saved.width) + " " + std::to_string(saved.height);
}

inline std::string stampWindowState(const std::string& juceState, int fitWidth, int fitHeight)
{
    return juceState + " fit " + std::to_string(fitWidth) + " " + std::to_string(fitHeight);
}

// True only when the saved frame is already large enough. A smaller frame,
// including one stamped by an older version, is not restored.
inline bool keepRememberedWindow(const SavedWindowSize& saved, int fitWidth, int fitHeight) noexcept
{
    if (! saved.valid || fitWidth <= 0 || fitHeight <= 0)
        return false;
    return saved.width >= fitWidth && saved.height >= fitHeight;
}

inline WindowFit clampWindowToScreen(int width, int height, int screenWidth, int screenHeight, int margin) noexcept
{
    WindowFit fit;
    fit.width = width;
    fit.height = height;
    if (screenWidth <= 0 || screenHeight <= 0)
        return fit;
    const int maxWidth = std::max(1, screenWidth - std::max(0, margin));
    const int maxHeight = std::max(1, screenHeight - std::max(0, margin));
    if (fit.width > maxWidth)
        fit.width = maxWidth;
    if (fit.height > maxHeight)
        fit.height = maxHeight;
    return fit;
}

inline WindowFit windowOpenSize(int fitWidth,
                                int fitHeight,
                                int screenWidth,
                                int screenHeight,
                                const SavedWindowSize& saved) noexcept
{
    int width = fitWidth;
    int height = fitHeight;
    if (saved.valid)
    {
        if (saved.width > width)
            width = saved.width;
        if (saved.height > height)
            height = saved.height;
    }
    return clampWindowToScreen(width, height, screenWidth, screenHeight, 48);
}

// The plugin applies the display scale itself (VST3 content scale, Audio Unit
// backing scale). Returning that scale from the host window as well scales
// the view a second time, so the editor is cropped or stretched.
inline float editorHostScale(float displayScale) noexcept
{
    (void) displayScale;
    return 1.0f;
}

// An Audio Unit view reports its size in points, so the first size is the
// one the window keeps. A VST3 view applies content scale after the window
// has a peer, and the size before that scale is the wrong frame.
inline bool editorWaitsForPeerScale(const char* formatName) noexcept
{
    return formatName != nullptr && formatName[0] == 'V' && formatName[1] == 'S'
        && formatName[2] == 'T' && formatName[3] == '3' && formatName[4] == '\0';
}

// VST3 keeps the size from after the peer scale. An Audio Unit keeps the size
// it reported first.
inline int editorFrameAfterScale(bool waitsForPeerScale, int sizeBeforePeer, int sizeAfterPeer) noexcept
{
    if (sizeAfterPeer < 1)
        sizeAfterPeer = sizeBeforePeer;
    if (sizeBeforePeer < 1)
        sizeBeforePeer = sizeAfterPeer;
    return waitsForPeerScale ? sizeAfterPeer : sizeBeforePeer;
}

struct EditorOpenPlan
{
    int width = 0;
    int height = 0;
    int minWidth = 0;
    int maxWidth = 0;
    int minHeight = 0;
    int maxHeight = 0;
    bool resizable = false;
    bool restoreSaved = false;
    float hostScale = 1.0f;
};

// Opens at the plugin's size. A remembered frame smaller than that is not
// restored. Limits come from the plugin, not from a host minimum.
inline EditorOpenPlan planEditorWindow(int pluginWidth,
                                       int pluginHeight,
                                       bool pluginResizable,
                                       int minWidth,
                                       int minHeight,
                                       int maxWidth,
                                       int maxHeight,
                                       float displayScale,
                                       int screenWidth,
                                       int screenHeight,
                                       const SavedWindowSize& saved) noexcept
{
    EditorOpenPlan plan;
    plan.hostScale = editorHostScale(displayScale);
    plan.resizable = pluginResizable;

    int fitWidth = pluginWidth > 0 ? pluginWidth : 0;
    int fitHeight = pluginHeight > 0 ? pluginHeight : 0;
    if (fitWidth < 1 && minWidth > 0)
        fitWidth = minWidth;
    if (fitHeight < 1 && minHeight > 0)
        fitHeight = minHeight;

    int lowWidth = minWidth > 0 ? minWidth : (fitWidth > 0 ? fitWidth : 1);
    int lowHeight = minHeight > 0 ? minHeight : (fitHeight > 0 ? fitHeight : 1);
    int highWidth = maxWidth > lowWidth ? maxWidth : std::max(lowWidth, fitWidth > 0 ? fitWidth : lowWidth);
    int highHeight = maxHeight > lowHeight ? maxHeight : std::max(lowHeight, fitHeight > 0 ? fitHeight : lowHeight);
    if (! pluginResizable)
    {
        if (fitWidth > 0)
            lowWidth = highWidth = fitWidth;
        if (fitHeight > 0)
            lowHeight = highHeight = fitHeight;
    }
    if (highWidth < lowWidth)
        highWidth = lowWidth;
    if (highHeight < lowHeight)
        highHeight = lowHeight;
    if (fitWidth < lowWidth)
        fitWidth = lowWidth;
    if (fitHeight < lowHeight)
        fitHeight = lowHeight;
    if (fitWidth > highWidth)
        fitWidth = highWidth;
    if (fitHeight > highHeight)
        fitHeight = highHeight;

    plan.minWidth = lowWidth;
    plan.minHeight = lowHeight;
    plan.maxWidth = highWidth;
    plan.maxHeight = highHeight;
    // A fixed plugin opens at its own size. A larger remembered frame would
    // stretch it. A resizable plugin may open larger, never smaller.
    plan.restoreSaved = pluginResizable && fitWidth > 0 && fitHeight > 0 && keepRememberedWindow(saved, fitWidth, fitHeight);
    if (fitWidth > 0 && fitHeight > 0)
    {
        const auto opened = windowOpenSize(fitWidth, fitHeight, screenWidth, screenHeight, plan.restoreSaved ? saved : SavedWindowSize {});
        plan.width = opened.width;
        plan.height = opened.height;
    }
    return plan;
}

// Every pixel of a block can be brought into a viewport of the given height.
inline bool blockReachable(int blockTop, int blockHeight, int contentHeight, int viewportHeight) noexcept
{
    if (blockHeight <= 0 || viewportHeight <= 0 || contentHeight <= 0)
        return false;
    if (blockTop < 0 || blockTop + blockHeight > contentHeight)
        return false;
    const int scroll = std::max(0, contentHeight - viewportHeight);
    const auto pixel = [&](int y)
    {
        const int low = std::max(0, y - viewportHeight + 1);
        const int high = std::min(scroll, y);
        return low <= high;
    };
    return pixel(blockTop) && pixel(blockTop + blockHeight - 1);
}

} // namespace youhost
