#pragma once

#include <cmath>
#include <string>

namespace youhost
{

// Same shapes as the sync-lost boxes: a centred window, then a small
// bottom-left box. The restored box appears at the moment audio fades back,
// not after an extra wait.
enum class PluginNoticeKind
{
    none,
    crashed,
    restored,
    notRestored,
    protectedAsk,
    corner
};

struct PluginNoticeBox
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct PluginNoticeModel
{
    PluginNoticeKind kind = PluginNoticeKind::none;
    int channel = 0;
    int slot = 0;
    std::string channelName;
    std::string pluginName;
    double mutedSeconds = 0.0;
    bool blink = true;
    bool detailsOpen = false;
};

inline PluginNoticeKind noticeWhileRestarting() noexcept
{
    return PluginNoticeKind::crashed;
}

// The big restored window is not held open. Audio fade and the corner box
// are the same moment. Clicking the box opens the detail window.
inline PluginNoticeKind noticeWhenAudioReturns(bool detailsHeldOpen) noexcept
{
    return detailsHeldOpen ? PluginNoticeKind::restored : PluginNoticeKind::corner;
}

inline PluginNoticeKind noticeIfRestartFailed() noexcept
{
    return PluginNoticeKind::notRestored;
}

inline PluginNoticeKind noticeForNewProtection() noexcept
{
    return PluginNoticeKind::protectedAsk;
}

inline PluginNoticeBox pluginNoticeCenterBox(int parentWidth, int parentHeight, PluginNoticeKind kind) noexcept
{
    int width = parentWidth - 64;
    if (width > 720)
        width = 720;
    if (width < 420)
        width = 420;
    int height = 280;
    if (kind == PluginNoticeKind::restored || kind == PluginNoticeKind::protectedAsk)
        height = 340;
    if (kind == PluginNoticeKind::notRestored)
        height = 300;
    if (height > parentHeight - 40)
        height = parentHeight - 40;
    if (height < 220)
        height = 220;
    return { (parentWidth - width) / 2, (parentHeight - height) / 2, width, height };
}

inline PluginNoticeBox pluginNoticeCornerBox(int parentWidth, int parentHeight) noexcept
{
    (void) parentWidth;
    const int width = 280;
    const int height = 36;
    return { 12, parentHeight - 92, width, height };
}

inline PluginNoticeBox pluginNoticeBox(int parentWidth, int parentHeight, PluginNoticeKind kind, bool detailsOpen) noexcept
{
    if (kind == PluginNoticeKind::corner && ! detailsOpen)
        return pluginNoticeCornerBox(parentWidth, parentHeight);
    if (kind == PluginNoticeKind::corner && detailsOpen)
        return pluginNoticeCenterBox(parentWidth, parentHeight, PluginNoticeKind::restored);
    return pluginNoticeCenterBox(parentWidth, parentHeight, kind);
}

inline const char* pluginNoticeTitle(PluginNoticeKind kind) noexcept
{
    switch (kind)
    {
        case PluginNoticeKind::crashed: return "PLUGIN CRASHED";
        case PluginNoticeKind::restored: return "PLUGIN RESTORED";
        case PluginNoticeKind::notRestored: return "PLUGIN NOT RESTORED";
        case PluginNoticeKind::protectedAsk: return "PLUGIN PROTECTED";
        case PluginNoticeKind::corner: return "PLUGIN RESTORED";
        case PluginNoticeKind::none: break;
    }
    return "";
}

inline std::string pluginCrashedLine(int channel, const std::string& channelName, const std::string& plugin)
{
    std::string line = "Channel " + std::to_string(channel);
    if (! channelName.empty())
    {
        line.push_back(' ');
        line += channelName;
    }
    line += " / ";
    line += plugin.empty() ? std::string("Plugin") : plugin;
    line += " / Channel muted. Restarting plugin…";
    return line;
}

inline const char* pluginRestoredLine() noexcept
{
    return "Settings restored. Audio fading back in.";
}

inline const char* pluginNotRestoredLine() noexcept
{
    return "Channel is still muted. Check the level before bypassing.";
}

inline std::string pluginProtectedLine(const std::string& plugin)
{
    const std::string name = plugin.empty() ? std::string("It") : plugin;
    return name + " crashed during the test. It now runs in protected mode, so a crash will not stop YouHost. Extra latency on this channel: 32 samples";
}

inline std::string pluginRestoredCornerLabel(int channel)
{
    return std::string("✓ PLUGIN RESTORED Ch ") + std::to_string(channel);
}

inline std::string pluginMutedForLine(double seconds)
{
    if (seconds < 0.0)
        seconds = 0.0;
    const int shown = static_cast<int>(std::lround(seconds * 10.0));
    const int whole = shown / 10;
    const int tenth = shown % 10;
    return "Muted for: " + std::to_string(whole) + "." + std::to_string(tenth) + " s";
}

inline std::string pluginRestoredDetails(int channel,
                                         const std::string& channelName,
                                         const std::string& plugin,
                                         double mutedSeconds)
{
    std::string text = "PLUGIN RESTORED – DETAILS\n";
    text += "Channel " + std::to_string(channel);
    if (! channelName.empty())
    {
        text.push_back(' ');
        text += channelName;
    }
    text += "\n";
    text += plugin.empty() ? "Plugin" : plugin;
    text += "\n\n";
    text += "1. The plugin crashed.\n";
    text += "2. The channel was muted to protect your ears.\n";
    text += "3. The plugin was restarted with the same settings.\n";
    text += "4. Audio faded back in automatically.\n\n";
    text += pluginMutedForLine(mutedSeconds);
    text += "\nYouHost and other channels kept running.";
    return text;
}

inline bool pluginNoticeBlinks(PluginNoticeKind kind) noexcept
{
    return kind == PluginNoticeKind::crashed || kind == PluginNoticeKind::corner;
}

} // namespace youhost
