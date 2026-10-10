#pragma once

#include "HostLimits.h"

#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

// One extra row at the bottom of the audio device list. It is never the default.
inline constexpr const char* kOfflineDeviceName = "Offline (no audio) - 128 channels";
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

struct SessionChannelView
{
    bool offline = false;
    bool deviceOpen = false;
    int cardInputs = 0;
    int visible = 0;
    int hidden = 0;
    bool revealed = false;
};

// The session always stores 128 channels. A selected device shows its own
// count. Offline shows all 128 and processes none. Revealing channels that
// are not on the card puts them on the mixer; they stay silent.
inline SessionChannelView sessionChannelView(int cardInputs, bool deviceOpen, bool offline, bool revealUnsupported) noexcept
{
    SessionChannelView view;
    view.offline = offline;
    view.deviceOpen = deviceOpen && ! offline;
    view.cardInputs = view.deviceOpen ? clampCardInputs(cardInputs) : 0;
    view.revealed = view.deviceOpen && revealUnsupported && view.cardInputs < kMaxChannels;
    if (offline || view.revealed)
        view.visible = kMaxChannels;
    else if (view.deviceOpen)
        view.visible = view.cardInputs;
    else
        view.visible = 0;
    view.hidden = view.deviceOpen ? kMaxChannels - view.visible : 0;
    return view;
}

inline std::string hiddenChannelNote(const SessionChannelView& view)
{
    if (view.hidden <= 0)
        return {};
    std::string text = std::to_string(view.hidden);
    text += view.hidden == 1 ? " channel hidden (card has " : " channels hidden (card has ";
    text += std::to_string(view.cardInputs);
    text += ")";
    return text;
}

inline bool channelUnsupportedByCard(int channel, const SessionChannelView& view) noexcept
{
    return view.deviceOpen && channel >= view.cardInputs && channel < kMaxChannels;
}

inline bool channelUsesCpu(int channel, const SessionChannelView& view) noexcept
{
    return channel >= 0 && channel < view.cardInputs;
}

inline bool timelineShowsChannel(int channel, int visible) noexcept
{
    return channel >= 0 && channel < visible;
}

inline bool cardHidesChannels(const SessionChannelView& view) noexcept
{
    return view.deviceOpen && view.cardInputs < kMaxChannels;
}

inline std::string unsupportedChannelLine(int channel, std::string_view name)
{
    std::string text = std::to_string(channel + 1);
    text += "  ";
    if (! name.empty())
    {
        text += name;
        text += "  ";
    }
    text += "not on this card";
    return text;
}

// Open and Open Recent keep the device selected in the startup window.
inline std::string deviceKeptOnSessionOpen(std::string_view selected, std::string_view savedInSession)
{
    (void) savedInSession;
    return std::string(selected);
}

} // namespace youhost
