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

    // An aggregate stays virtual even when its name also says MacBook.
    // "koontilaite" is the other Finnish spelling of the aggregate device.
    const char* aggregate[] = {
        "koostelaite",
        "koontilaite",
        "aggregate",
        "loopback",
    };
    for (const char* pattern : aggregate)
        if (containsFolded(name, pattern))
            return AudioCardKind::virtualDevice;

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

// A plugin runs only on a channel the open device is feeding.
// Hidden channels, channels past the card, and channels with no input do not.
inline bool channelProcessesPlugins(int channel, int activeChannels, int inputPacked) noexcept
{
    if (channel < 0 || activeChannels < 1 || channel >= activeChannels)
        return false;
    return inputPacked >= 0;
}

inline bool timelineShowsChannel(int channel, int visible) noexcept
{
    return channel >= 0 && channel < visible;
}

inline bool cardHidesChannels(const SessionChannelView& view) noexcept
{
    return view.deviceOpen && view.cardInputs < kMaxChannels;
}

// One line under the Audio setup switch. There is no per-channel list.
inline constexpr const char* kShowHiddenChannelsNote =
    "They stay in the session and are not recorded or processed.";

// A 16-input card reads "Show channels not on this card (17-128)".
inline std::string showHiddenChannelsLabel(int cardInputs)
{
    int first = cardInputs + 1;
    if (first < 1)
        first = 1;
    if (first > kMaxChannels)
        first = kMaxChannels;
    return "Show channels not on this card (" + std::to_string(first) + "-" + std::to_string(kMaxChannels) + ")";
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

inline std::string trimDeviceText(std::string text)
{
    while (! text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t'))
        ++begin;
    if (begin > 0)
        text.erase(0, begin);
    return text;
}

// Spaces are ignored and case is folded, so "MacBook Pro -mikrofoni" and
// "MacBook Pro-mikrofoni" are the same HAL name.
inline std::string deviceIdentityKey(std::string_view name)
{
    std::string key;
    key.reserve(name.size());
    for (const char character : name)
    {
        if (character == ' ' || character == '\t')
            continue;
        if (character >= 'A' && character <= 'Z')
            key.push_back(static_cast<char>(character - 'A' + 'a'));
        else
            key.push_back(character);
    }
    return key;
}

inline bool deviceNamesMatch(std::string_view left, std::string_view right)
{
    if (left.empty() || right.empty())
        return false;
    return deviceIdentityKey(left) == deviceIdentityKey(right);
}

// The menu name and either built-in half are the same card.
inline bool listedDeviceCovers(const ListedDevice& device, std::string_view name)
{
    return deviceNamesMatch(device.name, name)
           || deviceNamesMatch(device.inputName, name)
           || deviceNamesMatch(device.outputName, name);
}

inline bool endsWithFolded(std::string_view text, std::string_view suffix) noexcept
{
    if (suffix.empty() || text.size() < suffix.size())
        return false;
    const auto fold = [](char character) noexcept
    {
        if (character >= 'A' && character <= 'Z')
            return static_cast<char>(character - 'A' + 'a');
        return character;
    };
    const auto start = text.size() - suffix.size();
    for (std::size_t index = 0; index < suffix.size(); ++index)
        if (fold(text[start + index]) != fold(suffix[index]))
            return false;
    return true;
}

// "MacBook Pro -mikrofoni" and "MacBook Pro-kaiuttimet" share "MacBook Pro".
// The separator may be a space, a hyphen, or both.
inline std::string builtinFamilyKey(std::string_view name)
{
    std::string text = trimDeviceText(std::string(name));
    const char* roles[] = {
        "microphone",
        "mikrofoni",
        "speakers",
        "kaiuttimet",
        "speaker",
        "kaiutin",
        "output",
    };
    for (const char* role : roles)
    {
        const std::string_view word(role);
        if (text.size() <= word.size() || ! endsWithFolded(text, word))
            continue;
        std::size_t end = text.size() - word.size();
        const std::size_t cut = end;
        while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '-'))
            --end;
        if (end == cut || end == 0)
            continue;
        text.resize(end);
        return trimDeviceText(text);
    }
    return text;
}

// The menu name for one physical Mac: "MacBook Pro (mic + speakers)".
inline constexpr const char* kCombinedMicSpeakers = " (mic + speakers)";

inline std::string combinedBuiltinName(std::string_view family)
{
    auto name = trimDeviceText(std::string(family));
    if (name.empty())
        return {};
    name += kCombinedMicSpeakers;
    return name;
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
            if (deviceNamesMatch(builtinFamilyKey(speakers[index].name), family))
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
        const auto shown = combinedBuiltinName(family);
        paired.name = shown.empty() ? mic.name : shown;
        paired.inputName = mic.name;
        paired.outputName = speaker.name;
        paired.inputs = mic.inputs > 0 ? mic.inputs : speaker.inputs;
        paired.outputs = speaker.outputs > 0 ? speaker.outputs : mic.outputs;
        rest.push_back(std::move(paired));
    }
    for (std::size_t index = 0; index < speakers.size(); ++index)
        if (! speakerUsed[index])
            rest.push_back(speakers[index]);

    // A cached pair and the same card built from the raw names must stay one row.
    std::vector<ListedDevice> unique;
    unique.reserve(rest.size());
    for (auto& device : rest)
    {
        bool merged = false;
        for (auto& kept : unique)
        {
            if (! deviceNamesMatch(kept.name, device.name))
                continue;
            if (kept.inputName.empty() && ! device.inputName.empty())
                kept.inputName = std::move(device.inputName);
            if (kept.outputName.empty() && ! device.outputName.empty())
                kept.outputName = std::move(device.outputName);
            if (device.inputs > kept.inputs)
                kept.inputs = device.inputs;
            if (device.outputs > kept.outputs)
                kept.outputs = device.outputs;
            merged = true;
            break;
        }
        if (! merged)
            unique.push_back(std::move(device));
    }
    return unique;
}

// The menu, a session file, and reopening that same card are the only changes.
// A card the system opened is not one of them.
inline bool deviceChangeCauseAllowed(std::string_view cause) noexcept
{
    return cause == "user chose the card"
           || cause == "session open"
           || cause == "reopening the chosen card";
}

// youhost.log line. from is empty when the previous card is the same one.
inline std::string deviceChangeLogLine(std::string_view from, std::string_view to, std::string_view cause)
{
    std::string line = "device change";
    if (! from.empty())
    {
        line += " from ";
        line.append(from.begin(), from.end());
    }
    line += " to ";
    line.append(to.begin(), to.end());
    line += " cause=";
    line.append(cause.begin(), cause.end());
    return line;
}

// The open HAL device is still the card the user chose when its name is the
// menu name or either half of a built-in pair. JUCE can report an aggregate
// name while the setup still points at those two halves: that is the same card.
// A setup that names the aggregate is a different card.
inline bool openDeviceIsChosen(std::string_view openName,
                              std::string_view chosenName,
                              std::string_view inputName,
                              std::string_view outputName,
                              std::string_view setupInput,
                              std::string_view setupOutput)
{
    if (! chosenName.empty() && deviceNamesMatch(openName, chosenName))
        return true;
    if (! inputName.empty() && deviceNamesMatch(openName, inputName))
        return true;
    if (! outputName.empty() && deviceNamesMatch(openName, outputName))
        return true;
    if (! inputName.empty() && ! outputName.empty()
        && deviceNamesMatch(setupInput, inputName)
        && deviceNamesMatch(setupOutput, outputName))
        return true;
    return false;
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
    std::string inputName;
    std::string outputName;
    bool selectable = true;
};

inline bool deviceRowMatches(const DeviceRow& row, std::string_view selected)
{
    if (selected.empty())
        return false;
    if (deviceNamesMatch(row.name, selected))
        return true;
    if (! row.inputName.empty() && deviceNamesMatch(row.inputName, selected))
        return true;
    if (! row.outputName.empty() && deviceNamesMatch(row.outputName, selected))
        return true;
    return false;
}

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
        row.inputName = device.inputName;
        row.outputName = device.outputName;
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
