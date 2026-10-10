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
    real,
    virtualDevice
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

    // Network and bus drivers count as hardware even when the name says "virtual".
    const char* hardware[] = {
        "dante",
        "soundgrid",
        "thunderbolt",
        "avb",
        "x-usb",
        "x32",
        "wing",
        "edirol",
        "ua-1a",
        "fast track",
        "fasttrack",
        "scarlett",
        "usb",
    };
    for (const char* pattern : hardware)
        if (containsFolded(name, pattern))
            return AudioCardKind::real;

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

    const char* virtualDevice[] = {
        "audio bridge",
        "pro tools aggregate",
        "microsoft teams",
        "teams audio",
        "mjaudiorecorder",
        "koostelaite",
        "aggregate",
        "loopback",
    };
    for (const char* pattern : virtualDevice)
        if (containsFolded(name, pattern))
            return AudioCardKind::virtualDevice;
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

inline constexpr const char* kVirtualDeviceHeading = "Virtual / aggregate devices";

struct ListedDevice
{
    std::string name;
    int inputs = 0;
    int outputs = 0;
};

enum class DeviceRowKind
{
    hardware,
    heading,
    virtualDevice,
    builtin,
    offline
};

struct DeviceRow
{
    DeviceRowKind kind = DeviceRowKind::hardware;
    std::string name;
    std::string label;
    bool selectable = true;
};

inline std::string deviceEntryLabel(std::string_view name, int inputs, int outputs)
{
    if (isOfflineDeviceName(name))
        return std::string(kOfflineDeviceName);
    if (inputs < 0)
        inputs = 0;
    if (outputs < 0)
        outputs = 0;
    std::string label(name);
    label += " - ";
    label += std::to_string(inputs);
    label += " in / ";
    label += std::to_string(outputs);
    label += " out";
    return label;
}

inline bool deviceNamePresent(const std::vector<ListedDevice>& devices, std::string_view name)
{
    for (const auto& device : devices)
        if (device.name == name)
            return true;
    return false;
}

// Last explicit virtual choice, then the last real interface, then any real
// interface, then a MacBook built-in. Virtual devices are not a fallback.
inline std::string chooseStartupDevice(std::string_view savedName,
                                       std::string_view userChosenName,
                                       const std::vector<ListedDevice>& present)
{
    if (! userChosenName.empty() && deviceNamePresent(present, userChosenName)
        && audioCardKind(userChosenName) == AudioCardKind::virtualDevice)
        return std::string(userChosenName);

    if (! savedName.empty() && deviceNamePresent(present, savedName)
        && audioCardKind(savedName) == AudioCardKind::real)
        return std::string(savedName);
    if (! userChosenName.empty() && deviceNamePresent(present, userChosenName)
        && audioCardKind(userChosenName) == AudioCardKind::real)
        return std::string(userChosenName);

    for (const auto& device : present)
        if (audioCardKind(device.name) == AudioCardKind::real)
            return device.name;

    if (! savedName.empty() && deviceNamePresent(present, savedName)
        && audioCardKind(savedName) == AudioCardKind::builtin)
        return std::string(savedName);
    for (const auto& device : present)
        if (audioCardKind(device.name) == AudioCardKind::builtin)
            return device.name;
    return {};
}

inline bool deviceFilterMatches(std::string_view label, std::string_view filter)
{
    if (filter.empty())
        return true;
    return containsFolded(label, filter);
}

inline std::vector<DeviceRow> buildDeviceList(const std::vector<ListedDevice>& devices, std::string_view filter)
{
    std::vector<ListedDevice> hardware;
    std::vector<ListedDevice> virtualDevices;
    std::vector<ListedDevice> builtin;
    for (const auto& device : devices)
    {
        if (device.name.empty() || isOfflineDeviceName(device.name))
            continue;
        const auto label = deviceEntryLabel(device.name, device.inputs, device.outputs);
        if (! deviceFilterMatches(label, filter) && ! deviceFilterMatches(device.name, filter))
            continue;
        switch (audioCardKind(device.name))
        {
            case AudioCardKind::virtualDevice:
                virtualDevices.push_back(device);
                break;
            case AudioCardKind::builtin:
                builtin.push_back(device);
                break;
            case AudioCardKind::real:
                hardware.push_back(device);
                break;
            case AudioCardKind::none:
                break;
        }
    }

    std::vector<DeviceRow> rows;
    const auto pushDevice = [&rows](const ListedDevice& device, DeviceRowKind kind)
    {
        DeviceRow row;
        row.kind = kind;
        row.name = device.name;
        row.label = deviceEntryLabel(device.name, device.inputs, device.outputs);
        row.selectable = true;
        rows.push_back(std::move(row));
    };
    for (const auto& device : hardware)
        pushDevice(device, DeviceRowKind::hardware);
    if (! virtualDevices.empty())
    {
        DeviceRow heading;
        heading.kind = DeviceRowKind::heading;
        heading.label = kVirtualDeviceHeading;
        heading.selectable = false;
        rows.push_back(std::move(heading));
        for (const auto& device : virtualDevices)
            pushDevice(device, DeviceRowKind::virtualDevice);
    }
    for (const auto& device : builtin)
        pushDevice(device, DeviceRowKind::builtin);

    const auto offlineLabel = std::string(kOfflineDeviceName);
    if (deviceFilterMatches(offlineLabel, filter))
    {
        DeviceRow offline;
        offline.kind = DeviceRowKind::offline;
        offline.name = kOfflineDeviceName;
        offline.label = offlineLabel;
        offline.selectable = true;
        rows.push_back(std::move(offline));
    }
    return rows;
}

} // namespace youhost
