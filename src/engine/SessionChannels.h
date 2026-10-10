#pragma once

#include "HostLimits.h"
#include "RecordLock.h"

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
    if (name.empty() || name == "No device" || name == "none" || name == "<< none >>"
        || isOfflineDeviceName(name))
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
    bool missingCard = false;
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

// The saved card is gone. Show that card's channel count, not the 128 of an
// explicit Offline choice.
inline SessionChannelView missingCardChannelView(int savedCardChannels) noexcept
{
    SessionChannelView view;
    view.offline = true;
    view.missingCard = true;
    view.cardInputs = clampCardInputs(savedCardChannels);
    view.visible = view.cardInputs;
    return view;
}

struct SessionCardRestore
{
    bool useSaved = false;
    bool offline = false;
    bool missing = false;
    int visible = 0;
    int buffer = 0;
    std::string input;
    std::string output;
    std::string status;
};

// Opening a session restores its card, buffer, and channel count. A missing
// card goes Offline and keeps the saved count. Explicit Offline stays at 128.
inline SessionCardRestore restoreSessionCard(std::string_view savedInput,
                                            std::string_view savedOutput,
                                            int savedCardChannels,
                                            int savedBuffer,
                                            bool explicitOffline,
                                            bool inputPresent)
{
    SessionCardRestore result;
    result.buffer = savedBuffer >= 16 ? savedBuffer : 0;
    if (explicitOffline || isOfflineDeviceName(savedInput))
    {
        result.offline = true;
        result.visible = kMaxChannels;
        return result;
    }
    if (savedInput.empty())
        return result;
    result.useSaved = true;
    result.input = std::string(savedInput);
    result.output = savedOutput.empty() ? result.input : std::string(savedOutput);
    if (! inputPresent)
    {
        result.missing = true;
        result.offline = true;
        result.visible = clampCardInputs(savedCardChannels);
        result.status = "Saved card ";
        result.status += result.input;
        result.status += " not found - Offline";
        return result;
    }
    result.visible = clampCardInputs(savedCardChannels);
    return result;
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
    ListedDevice() = default;
    ListedDevice(std::string deviceName, int deviceInputs, int deviceOutputs)
        : name(std::move(deviceName)),
          inputs(deviceInputs),
          outputs(deviceOutputs)
    {
    }

    std::string name;
    int inputs = 0;
    int outputs = 0;
    std::string inputName;
    std::string outputName;
};

inline bool isBuiltinMicrophone(std::string_view name) noexcept
{
    return audioCardKind(name) == AudioCardKind::builtin
           && (containsFolded(name, "microphone") || containsFolded(name, "mikrofoni"));
}

inline bool isBuiltinSpeaker(std::string_view name) noexcept
{
    if (audioCardKind(name) != AudioCardKind::builtin || containsFolded(name, "headphone"))
        return false;
    // "kaiuttimet" is the plural and does not contain the singular "kaiutin".
    return containsFolded(name, "speaker") || containsFolded(name, "kaiuttimet")
           || containsFolded(name, "kaiutin") || containsFolded(name, "output");
}

inline std::string builtinFamilyKey(std::string_view name)
{
    std::string text(name);
    const char* cuts[] = {
        " Microphone", " microphone", "-mikrofoni", "-Mikrofoni",
        " Speakers", " speakers", "-kaiuttimet", "-Kaiuttimet",
        " Output", " output",
    };
    for (const char* cut : cuts)
    {
        const std::string suffix(cut);
        if (text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            text.resize(text.size() - suffix.size());
            break;
        }
    }
    return text;
}

// MacBook mic + speakers become one card. Headphones stay on their own row.
inline std::vector<ListedDevice> pairBuiltinCards(const std::vector<ListedDevice>& devices)
{
    std::vector<ListedDevice> rest;
    std::vector<ListedDevice> mics;
    std::vector<ListedDevice> speakers;
    rest.reserve(devices.size());
    for (const auto& device : devices)
    {
        if (isBuiltinMicrophone(device.name))
            mics.push_back(device);
        else if (isBuiltinSpeaker(device.name))
            speakers.push_back(device);
        else
            rest.push_back(device);
    }

    std::vector<bool> speakerUsed(speakers.size(), false);
    for (const auto& mic : mics)
    {
        const auto family = builtinFamilyKey(mic.name);
        int match = -1;
        for (std::size_t index = 0; index < speakers.size(); ++index)
        {
            if (speakerUsed[index])
                continue;
            if (builtinFamilyKey(speakers[index].name) == family)
            {
                match = static_cast<int>(index);
                break;
            }
        }
        if (match < 0)
        {
            rest.push_back(mic);
            continue;
        }
        speakerUsed[static_cast<std::size_t>(match)] = true;
        const auto& speaker = speakers[static_cast<std::size_t>(match)];
        ListedDevice paired;
        paired.name = family.empty() ? mic.name : family;
        paired.inputName = mic.name;
        paired.outputName = speaker.name;
        paired.inputs = mic.inputs > 0 ? mic.inputs : speaker.inputs;
        paired.outputs = speaker.outputs > 0 ? speaker.outputs : mic.outputs;
        rest.push_back(std::move(paired));
    }
    for (std::size_t index = 0; index < speakers.size(); ++index)
        if (! speakerUsed[index])
            rest.push_back(speakers[index]);
    return rest;
}

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

// A device change does not rewrite channel names, plugins, or groups.
struct KeptSessionChannel
{
    std::string name;
    int group = -1;
    std::string plugin;
};

inline KeptSessionChannel channelAfterDeviceSwitch(const KeptSessionChannel& channel, int)
{
    return channel;
}

inline std::string deviceSwitchBlockedReason(bool recording, bool lockArmed)
{
    if (recordLockEngaged(lockArmed, recording))
        return "Recording is locked. Click the padlock to unlock, then stop the take before changing the audio device.";
    if (recording)
        return "Stop the take before changing the audio device.";
    return {};
}

inline std::string deviceEntryLabel(std::string_view name, int inputs, int outputs)
{
    if (isOfflineDeviceName(name))
        return std::string(kOfflineDeviceName);
    if (inputs < 0 || outputs < 0)
        return std::string(name);
    std::string label(name);
    if (audioCardKind(name) == AudioCardKind::builtin && inputs > 0 && outputs > 0
        && ! containsFolded(name, "built-in") && ! containsFolded(name, "microphone")
        && ! containsFolded(name, "speaker") && ! containsFolded(name, "mikrofoni")
        && ! containsFolded(name, "kaiutin"))
        label += " (built-in)";
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
    const auto paired = pairBuiltinCards(devices);
    std::vector<ListedDevice> hardware;
    std::vector<ListedDevice> virtualDevices;
    std::vector<ListedDevice> builtin;
    for (const auto& device : paired)
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
