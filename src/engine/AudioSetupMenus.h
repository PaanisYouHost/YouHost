#pragma once

#include "DeviceWatch.h"
#include "HostLimits.h"
#include "SessionChannels.h"

#include <string>
#include <vector>

namespace youhost
{

// The four menus both Start session and Audio setup show, in this order.
inline constexpr const char* kAudioCardMenuLabel = "Audio card:";
inline constexpr const char* kSampleRateMenuLabel = "Sample rate:";
inline constexpr const char* kBitDepthMenuLabel = "Bit depth:";
inline constexpr const char* kBufferMenuLabel = "Buffer:";

struct AudioSetupChoice
{
    std::string text;
    int id = 0;
    bool heading = false;
    std::string raw;
};

struct AudioSetupMenu
{
    const char* label = "";
    std::vector<AudioSetupChoice> choices;
    int selectedId = 0;
};

inline bool audioSetupChoiceSelected(const AudioSetupMenu& menu, int id) noexcept
{
    for (const auto& choice : menu.choices)
        if (! choice.heading && choice.id == id)
            return true;
    return false;
}

// One card list (Offline included), then sample rate, bit depth, and buffer.
// Buffer falls back to 32. Offline is the selection when no card is open.
inline std::vector<AudioSetupMenu> audioSetupMenus(const std::vector<ListedDevice>& devices,
                                                    std::string_view selectedDevice,
                                                    bool offline,
                                                    double sampleRate,
                                                    int bitDepth,
                                                    int bufferSamples)
{
    std::vector<AudioSetupMenu> menus;
    menus.reserve(4);

    AudioSetupMenu card;
    card.label = kAudioCardMenuLabel;
    const auto rows = buildDeviceList(devices, "");
    int nextId = 1;
    for (const auto& row : rows)
    {
        if (! row.selectable)
        {
            AudioSetupChoice heading;
            heading.text = row.label;
            heading.heading = true;
            card.choices.push_back(std::move(heading));
            continue;
        }
        AudioSetupChoice choice;
        choice.text = row.label;
        choice.raw = row.name;
        choice.id = row.kind == DeviceRowKind::offline ? kOfflineDeviceItemId : nextId++;
        if (offline && row.kind == DeviceRowKind::offline)
            card.selectedId = choice.id;
        else if (! offline && ! selectedDevice.empty()
                 && (row.name == selectedDevice || choice.text == selectedDevice))
            card.selectedId = choice.id;
        card.choices.push_back(std::move(choice));
    }
    if (offline)
        card.selectedId = kOfflineDeviceItemId;
    else if (card.selectedId == 0)
    {
        for (const auto& choice : card.choices)
        {
            if (choice.heading || choice.id == kOfflineDeviceItemId)
                continue;
            card.selectedId = choice.id;
            break;
        }
    }
    menus.push_back(std::move(card));

    AudioSetupMenu rate;
    rate.label = kSampleRateMenuLabel;
    for (const double value : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        AudioSetupChoice choice;
        choice.text = formatRateKhz(value) + " kHz";
        choice.id = static_cast<int>(value);
        rate.choices.push_back(std::move(choice));
    }
    const int rateId = static_cast<int>(sampleRate + 0.5);
    rate.selectedId = audioSetupChoiceSelected(rate, rateId) ? rateId : 48000;
    menus.push_back(std::move(rate));

    AudioSetupMenu bits;
    bits.label = kBitDepthMenuLabel;
    const char* bitLabels[] = { "16-bit", "24-bit", "32-bit float" };
    const int bitIds[] = { 16, 24, 32 };
    for (int index = 0; index < 3; ++index)
    {
        AudioSetupChoice choice;
        choice.text = bitLabels[index];
        choice.id = bitIds[index];
        bits.choices.push_back(std::move(choice));
    }
    bits.selectedId = audioSetupChoiceSelected(bits, bitDepth) ? bitDepth : 24;
    menus.push_back(std::move(bits));

    AudioSetupMenu buffer;
    buffer.label = kBufferMenuLabel;
    for (const int value : { 32, 64, 128, 256, 512, 1024 })
    {
        AudioSetupChoice choice;
        choice.text = std::to_string(value) + " samples";
        choice.id = value;
        buffer.choices.push_back(std::move(choice));
    }
    buffer.selectedId = audioSetupChoiceSelected(buffer, bufferSamples) ? bufferSamples : kNewSessionBufferSamples;
    menus.push_back(std::move(buffer));
    return menus;
}

} // namespace youhost
