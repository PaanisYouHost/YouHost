#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace youhost
{

// Scanned bus layouts. M is mono only, ST is stereo only, M/ST accepts both.
inline std::string pluginBusMark(bool mono, bool stereo)
{
    if (mono && stereo)
        return "M/ST";
    if (stereo)
        return "ST";
    if (mono)
        return "M";
    return {};
}

// Channel counts from a scan that did not probe both layouts. A 2-in/2-out
// plugin is marked ST. Mono-only is M. Both-zero stays blank.
inline std::string pluginBusMarkFromCounts(int inputs, int outputs)
{
    if (inputs <= 0 && outputs <= 0)
        return {};
    if (inputs <= 1 && outputs <= 1)
        return "M";
    return "ST";
}

inline bool isPluginBusMark(std::string_view text) noexcept
{
    return text == "M" || text == "ST" || text == "M/ST";
}


// Main bus plus a sidechain or aux. Eight channels covers stereo plus a few
// extra buses. A wider bus is refused and written to youhost.log.
inline constexpr int kPluginChannelCap = 8;
inline constexpr int kPluginBusCap = 8;

// How non-main buses are offered. Disable is tried first. Some Audio Units
// refuse a disabled sidechain, so the same main layout is tried again with
// the buses left as the plugin opened them, then with each extra bus set to
// the main bus channel count. Extra inputs are silence.
enum class ExtraBusMode
{
    disable = 0,
    keepDefault = 1,
    matchMain = 2
};

inline const char* extraBusModeName(ExtraBusMode mode) noexcept
{
    if (mode == ExtraBusMode::keepDefault)
        return "keep";
    if (mode == ExtraBusMode::matchMain)
        return "match";
    return "disable";
}

struct LayoutPlan
{
    int mainIn = 0;
    int mainOut = 0;
    ExtraBusMode extra = ExtraBusMode::disable;
    int inputBuses[kPluginBusCap] {};
    int outputBuses[kPluginBusCap] {};
    int inputBusCount = 0;
    int outputBusCount = 0;
    int totalIn = 0;
    int totalOut = 0;
    bool ok = false;
    const char* reason = "";
};

inline bool pluginBusCountOk(int count) noexcept
{
    return count == 0 || count == 1 || count == 2;
}

// `inputDefaults` / `outputDefaults` are the channel counts the plugin opened
// with. 0 is a disabled bus. Counts above 2 cannot be hosted.
inline LayoutPlan planPluginLayout(const int* inputDefaults,
                                   int inputBusCount,
                                   const int* outputDefaults,
                                   int outputBusCount,
                                   int mainIn,
                                   int mainOut,
                                   ExtraBusMode extra) noexcept
{
    LayoutPlan plan;
    plan.mainIn = mainIn;
    plan.mainOut = mainOut;
    plan.extra = extra;
    if (mainIn < 1 || mainIn > 2 || mainOut < 1 || mainOut > 2)
    {
        plan.reason = "main bus is not mono or stereo";
        return plan;
    }
    if (inputBusCount < 0 || outputBusCount < 0)
    {
        plan.reason = "bus count is negative";
        return plan;
    }
    if (inputBusCount > kPluginBusCap || outputBusCount > kPluginBusCap)
    {
        plan.reason = "more buses than YouHost can host";
        return plan;
    }
    if (outputBusCount < 1)
    {
        plan.reason = "plugin has no output bus";
        return plan;
    }
    plan.inputBusCount = inputBusCount;
    plan.outputBusCount = outputBusCount;

    auto fill = [&](const int* defaults, int count, int* dest, int mainChannels, int& total, bool inputs) -> bool
    {
        for (int bus = 0; bus < count; ++bus)
        {
            const int opened = defaults != nullptr ? defaults[bus] : 0;
            int channels = 0;
            if (bus == 0)
            {
                channels = mainChannels;
            }
            else if (extra == ExtraBusMode::disable)
            {
                channels = 0;
            }
            else if (extra == ExtraBusMode::keepDefault)
            {
                if (! pluginBusCountOk(opened))
                {
                    plan.reason = inputs ? "an input bus is wider than stereo" : "an output bus is wider than stereo";
                    return false;
                }
                channels = opened;
            }
            else
            {
                channels = mainChannels;
            }
            dest[bus] = channels;
            total += channels;
        }
        return true;
    };

    if (! fill(inputDefaults, inputBusCount, plan.inputBuses, mainIn, plan.totalIn, true))
        return plan;
    if (! fill(outputDefaults, outputBusCount, plan.outputBuses, mainOut, plan.totalOut, false))
        return plan;
    if (plan.totalOut < 1)
    {
        plan.reason = "plugin has no output channels";
        return plan;
    }
    if (plan.totalIn > kPluginChannelCap || plan.totalOut > kPluginChannelCap)
    {
        plan.reason = "more channels than YouHost can host";
        return plan;
    }
    plan.ok = true;
    plan.reason = "ok";
    return plan;
}

// Channels [0, mainIn) carry the host audio. Channels [mainIn, totalIn) are
// extra inputs and stay silent. No allocation.
inline void feedPluginChannels(float* const* plugin,
                               int width,
                               int mainIn,
                               int numSamples,
                               const float* left,
                               const float* right) noexcept
{
    if (plugin == nullptr || numSamples <= 0 || width <= 0)
        return;
    const auto bytes = sizeof(float) * static_cast<std::size_t>(numSamples);
    const int used = std::min(width, kPluginChannelCap);
    for (int index = 0; index < used; ++index)
    {
        float* dest = plugin[index];
        if (dest == nullptr)
            continue;
        if (index == 0 && mainIn >= 1 && left != nullptr && dest != left)
            std::memcpy(dest, left, bytes);
        else if (index == 1 && mainIn >= 2 && right != nullptr && dest != right)
            std::memcpy(dest, right, bytes);
        else if (dest != left && dest != right)
            std::memset(dest, 0, bytes);
    }
}

inline const char* kPluginMonoChannelMessage = "This plugin can't be used on a mono channel";
inline const char* kPluginStereoChannelMessage = "A mono plugin can't be used on a stereo channel";
inline const char* kPluginLayoutMessage = "This plugin needs a sidechain/layout YouHost can't host";
inline const char* kPluginDidNotOpenMessage = "The plugin did not open. See youhost.log.";

// `hostError` is the text the AU or VST3 wrapper returned. It wins when the
// instance never existed. A layout miss uses one of the three specific lines.
inline const char* pluginLoadMessage(bool gotInstance,
                                     bool unlinkedMono,
                                     bool monoOk,
                                     bool stereoOk,
                                     bool sideOk,
                                     bool openedStereo,
                                     bool hasExtraBuses,
                                     const char* hostError) noexcept
{
    if (! gotInstance)
    {
        if (hostError != nullptr && hostError[0] != '\0')
            return hostError;
        return kPluginDidNotOpenMessage;
    }
    if (! unlinkedMono && monoOk && ! stereoOk)
        return kPluginStereoChannelMessage;
    if (unlinkedMono && ! monoOk && stereoOk)
        return kPluginMonoChannelMessage;
    if (unlinkedMono && ! monoOk && ! stereoOk && ! sideOk && openedStereo && ! hasExtraBuses)
        return kPluginMonoChannelMessage;
    return kPluginLayoutMessage;
}

} // namespace youhost
