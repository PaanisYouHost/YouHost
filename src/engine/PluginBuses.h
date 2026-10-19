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

// How non-main buses are offered. Logic leaves a sidechain input enabled and
// feeds it silence. JUCE's AudioUnit check rejects a disabled sidechain, and
// it also rejects a mono sidechain next to a stereo main bus, so a refused
// check is not the last word: the host still asks the Audio Unit with
// setBusesLayout.
//
// `opened` keeps every bus at the channel count the plugin opened with.
// A sidechain that opened at 0 channels is not a success: it has to be enabled.
// `sidechainStereo` is FabFilter's VST3 layout (main plus a stereo sidechain).
// `sidechainMono` is the mono sidechain Logic also accepts.
// `disable` is only for a plugin that has no extra bus.
enum class ExtraBusMode
{
    disable = 0,
    keepDefault = 1,
    matchMain = 2,
    sidechainMono = 3,
    opened = 4,
    sidechainStereo = 5
};

inline const char* extraBusModeName(ExtraBusMode mode) noexcept
{
    if (mode == ExtraBusMode::opened)
        return "opened";
    if (mode == ExtraBusMode::sidechainStereo)
        return "sidechain2";
    if (mode == ExtraBusMode::sidechainMono)
        return "sidechain";
    if (mode == ExtraBusMode::keepDefault)
        return "keep";
    if (mode == ExtraBusMode::matchMain)
        return "match";
    return "disable";
}

// A sidechain is offered enabled: the layout it opened with (when that
// sidechain is already on), then stereo, then mono, then the other enabled
// layouts. Disabling the sidechain is not an offer. A plugin with no extra
// bus only needs "opened" and one forced main layout.
inline int copyExtraBusAttemptOrder(bool hasExtraBuses, ExtraBusMode* dest, int capacity) noexcept
{
    const ExtraBusMode extraOrder[] = {
        ExtraBusMode::opened,
        ExtraBusMode::sidechainStereo,
        ExtraBusMode::sidechainMono,
        ExtraBusMode::matchMain,
        ExtraBusMode::keepDefault
    };
    const ExtraBusMode plainOrder[] = {
        ExtraBusMode::opened,
        ExtraBusMode::disable
    };
    const ExtraBusMode* source = hasExtraBuses ? extraOrder : plainOrder;
    const int count = hasExtraBuses ? 5 : 2;
    if (dest == nullptr || capacity < count)
        return 0;
    for (int index = 0; index < count; ++index)
        dest[index] = source[index];
    return count;
}

// Unlinked mono prefers 1/1, then the stereo layout the plugin may have
// opened with. A linked pair prefers 2/2 and does not start by rewriting
// the Audio Unit down to mono.
inline void mainLayoutAttemptOrder(bool unlinkedMono, int* mainIn, int* mainOut) noexcept
{
    if (mainIn == nullptr || mainOut == nullptr)
        return;
    if (unlinkedMono)
    {
        mainIn[0] = 1;
        mainOut[0] = 1;
        mainIn[1] = 2;
        mainOut[1] = 2;
        mainIn[2] = 1;
        mainOut[2] = 2;
        return;
    }
    mainIn[0] = 2;
    mainOut[0] = 2;
    mainIn[1] = 1;
    mainOut[1] = 1;
    mainIn[2] = 1;
    mainOut[2] = 2;
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
                if (extra == ExtraBusMode::opened && opened != mainChannels)
                {
                    plan.reason = "opened main bus does not match";
                    return false;
                }
                channels = mainChannels;
            }
            else if (extra == ExtraBusMode::disable)
            {
                channels = 0;
            }
            else if (extra == ExtraBusMode::keepDefault || extra == ExtraBusMode::opened)
            {
                if (! pluginBusCountOk(opened))
                {
                    plan.reason = inputs ? "an input bus is wider than stereo" : "an output bus is wider than stereo";
                    return false;
                }
                // An extra input at 0 channels is a disabled sidechain. Logic
                // does not leave that bus off, and several Audio Units reject
                // the layout. Enabling it is a later attempt.
                if (inputs && opened == 0)
                {
                    plan.reason = "sidechain bus is disabled";
                    return false;
                }
                channels = opened;
            }
            else if (extra == ExtraBusMode::sidechainMono || extra == ExtraBusMode::sidechainStereo)
            {
                // Extra inputs stay enabled. Stereo matches FabFilter's 4/2
                // VST3 layout. Mono is the other layout Logic accepts.
                // Extra outputs stay as the plugin opened them.
                if (inputs)
                {
                    channels = extra == ExtraBusMode::sidechainStereo ? 2 : 1;
                }
                else if (! pluginBusCountOk(opened))
                {
                    plan.reason = "an output bus is wider than stereo";
                    return false;
                }
                else
                {
                    channels = opened;
                }
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

// The unload line always carries a reason. An empty host message is written
// as a fixed sentence so a failed load is not just "loading" then "unload".
inline std::string pluginFailureLogReason(bool gotInstance, const char* hostError, const char* attempts)
{
    if (! gotInstance)
    {
        std::string text = "instantiate failed: ";
        if (hostError != nullptr && hostError[0] != '\0')
            text += hostError;
        else
            text += "no reason from the host";
        return text;
    }
    if (attempts != nullptr && attempts[0] != '\0')
        return std::string(attempts);
    return "no reason from the host";
}

struct KeptLayout
{
    bool ok = false;
    int mainIn = 0;
    int mainOut = 0;
    int totalIn = 0;
    int totalOut = 0;
};

// setBusesLayout rejected every offer. The Audio Unit is still the layout it
// opened with. Keep that when the main bus is already mono or stereo, instead
// of unloading a plugin that instantiated. Extra inputs stay as they are.
inline KeptLayout keepOpenedLayout(const int* inputs,
                                   int inputBusCount,
                                   const int* outputs,
                                   int outputBusCount) noexcept
{
    KeptLayout kept;
    if (inputs == nullptr || outputs == nullptr || inputBusCount < 1 || outputBusCount < 1)
        return kept;
    if (inputBusCount > kPluginBusCap || outputBusCount > kPluginBusCap)
        return kept;
    const int mainIn = inputs[0];
    const int mainOut = outputs[0];
    if ((mainIn != 1 && mainIn != 2) || (mainOut != 1 && mainOut != 2))
        return kept;
    int totalIn = 0;
    int totalOut = 0;
    for (int bus = 0; bus < inputBusCount; ++bus)
        totalIn += inputs[bus];
    for (int bus = 0; bus < outputBusCount; ++bus)
        totalOut += outputs[bus];
    if (totalOut < 1 || totalIn > kPluginChannelCap || totalOut > kPluginChannelCap)
        return kept;
    kept.ok = true;
    kept.mainIn = mainIn;
    kept.mainOut = mainOut;
    kept.totalIn = totalIn;
    kept.totalOut = totalOut;
    return kept;
}

} // namespace youhost
