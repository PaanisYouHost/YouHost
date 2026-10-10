#pragma once

#include <string>
#include <vector>

namespace youhost
{

// A window opens at the size that shows its content. A smaller size is kept
// only after the user has resized it for the current content. If the content
// grows, a remembered size that would hide part of it opens at the fit size.
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

// A remembered size is kept when it was chosen for this content. A size saved
// before the content grew, or an old size with no content stamp that is
// smaller than the fit, opens at the fit size instead.
inline bool keepRememberedWindow(const SavedWindowSize& saved, int fitWidth, int fitHeight) noexcept
{
    if (! saved.valid || fitWidth <= 0 || fitHeight <= 0)
        return false;
    if (! saved.hasFit)
        return saved.width >= fitWidth && saved.height >= fitHeight;
    const bool grewWider = fitWidth > saved.fitWidth && saved.width < fitWidth;
    const bool grewTaller = fitHeight > saved.fitHeight && saved.height < fitHeight;
    return ! grewWider && ! grewTaller;
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
    const auto fit = clampWindowToScreen(fitWidth, fitHeight, screenWidth, screenHeight, 48);
    if (! keepRememberedWindow(saved, fitWidth, fitHeight))
        return fit;
    return clampWindowToScreen(saved.width, saved.height, screenWidth, screenHeight, 24);
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
