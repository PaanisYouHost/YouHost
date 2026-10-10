#pragma once

#include "HostLimits.h"

#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

// One extra row at the bottom of the audio device list. It is never the default.
// The session channel count is chosen separately.
inline constexpr const char* kOfflineDeviceName = "Offline (no audio)";
inline constexpr const char* kNoInputLabel = "no input";
inline constexpr int kOfflineDeviceItemId = 100000;

inline bool isOfflineDeviceName(std::string_view name) noexcept
{
    return name == kOfflineDeviceName;
}

inline void appendOfflineDeviceEntry(std::vector<std::string>& names)
{
    for (auto it = names.begin(); it != names.end(); ++it)
    {
        if (*it != kOfflineDeviceName)
            continue;
        names.erase(it);
        break;
    }
    names.push_back(kOfflineDeviceName);
}

enum class AudioCardKind
{
    none,
    builtin,
    real
};

inline bool containsFolded(std::string_view haystack, std::string_view needle) noexcept
{
    if (needle.empty() || haystack.size() < needle.size())
        return false;
    const auto fold = [](char character) noexcept
    {
        if (character >= 'A' && character <= 'Z')
            return static_cast<char>(character - 'A' + 'a');
        return character;
    };
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start)
    {
        bool match = true;
        for (std::size_t index = 0; index < needle.size(); ++index)
        {
            if (fold(haystack[start + index]) != fold(needle[index]))
            {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

inline AudioCardKind audioCardKind(std::string_view name) noexcept
{
    if (name.empty() || name == "No device" || name == "none" || isOfflineDeviceName(name))
        return AudioCardKind::none;
    const char* builtin[] = {
        "built-in",
        "builtin",
        "macbook",
        "imac",
        "mac mini",
        "mac studio",
        "external headphones",
    };
    for (const char* pattern : builtin)
        if (containsFolded(name, pattern))
            return AudioCardKind::builtin;
    return AudioCardKind::real;
}

inline int clampCardInputs(int cardInputs) noexcept
{
    if (cardInputs < 0)
        return 0;
    if (cardInputs > kMaxChannels)
        return kMaxChannels;
    return cardInputs;
}

inline constexpr int kSessionChannelCounts[] = { 8, 16, 32, 48, 64, 128 };
inline constexpr int kSessionChannelCountChoices = 6;

// Missing or unknown counts round up to the next choice, so a session never
// hides a channel a file already asked to show. 128 is the default.
inline int normaliseSessionChannelCount(int count) noexcept
{
    if (count <= 0)
        return kMaxChannels;
    for (const int allowed : kSessionChannelCounts)
        if (count <= allowed)
            return allowed;
    return kMaxChannels;
}

struct SessionChannelView
{
    bool offline = false;
    bool deviceOpen = false;
    int cardInputs = 0;
    int sessionChannels = kMaxChannels;
    int visible = 0;
    int processed = 0;
    int silent = 0;
};

// The session count is independent of the card. Channel N is device input N
// and output N. Channels past the card stay visible, silent, and unprocessed.
inline SessionChannelView sessionChannelView(int sessionChannels, int cardInputs, bool deviceOpen, bool offline) noexcept
{
    SessionChannelView view;
    view.offline = offline;
    view.deviceOpen = deviceOpen && ! offline;
    view.sessionChannels = normaliseSessionChannelCount(sessionChannels);
    view.cardInputs = view.deviceOpen ? clampCardInputs(cardInputs) : 0;
    view.visible = view.sessionChannels;
    view.processed = view.cardInputs < view.visible ? view.cardInputs : view.visible;
    view.silent = view.visible - view.processed;
    return view;
}

inline std::string hiddenChannelNote(const SessionChannelView& view)
{
    if (view.silent <= 0)
        return {};
    std::string text = std::to_string(view.silent);
    text += view.silent == 1 ? " channel has no input" : " channels have no input";
    return text;
}

inline bool channelHasNoInput(int channel, const SessionChannelView& view) noexcept
{
    return channel >= 0 && channel < view.visible && channel >= view.cardInputs;
}

inline bool channelUsesCpu(int channel, const SessionChannelView& view) noexcept
{
    return channel >= 0 && channel < view.processed;
}

inline bool timelineShowsChannel(int channel, int visible) noexcept
{
    return channel >= 0 && channel < visible;
}

inline std::string noInputChannelLine(int channel, std::string_view name)
{
    std::string text = std::to_string(channel + 1);
    text += "  ";
    if (! name.empty())
    {
        text += name;
        text += "  ";
    }
    text += kNoInputLabel;
    return text;
}

// Open and Open Recent keep the device selected in the startup window.
inline std::string deviceKeptOnSessionOpen(std::string_view selected, std::string_view savedInSession)
{
    (void) savedInSession;
    return std::string(selected);
}

} // namespace youhost
