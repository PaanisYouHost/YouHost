#pragma once

#include <string>
#include <string_view>

namespace youhost
{

// Gig mode asks macOS for the most a normal app can have: the device audio
// workgroup, no App Nap, no idle sleep, and user-interactive priority.
// macOS does not let an app reserve CPU cores. These lines are the check the
// GIG window shows. Each hint is one sentence.

inline constexpr const char* kGigClaimLine =
    "While a card is open, YouHost joins that card's audio workgroup, turns App Nap and idle sleep off, and asks macOS for full performance. macOS does not allow an app to reserve CPU cores.";

inline constexpr const char* kGigSpotlightHint =
    "Spotlight is indexing. Turn indexing off until after the gig, then turn it back on.";

inline constexpr const char* kGigTimeMachineHint =
    "Time Machine is copying. Skip this backup from the Time Machine menu until the gig is over.";

inline constexpr const char* kGigUpdatesHint =
    "Updates can install during a gig. Turn off automatic updates in System Settings until the show is over.";

inline constexpr const char* kGigWifiHint =
    "Wi-Fi is on. Turn it off in the menu bar if the gig does not need it.";

inline constexpr const char* kGigBluetoothHint =
    "Bluetooth is on. Turn it off in the menu bar if the gig does not need it.";

inline constexpr const char* kGigCouldNotCheck = "Could not check.";

enum class GigLoad
{
    clear,
    busy,
    unknown
};

inline bool containsText(std::string_view haystack, std::string_view needle) noexcept
{
    return needle.size() > 0 && haystack.find(needle) != std::string_view::npos;
}

// `ps` comm list. mdworker or mds_stores means Spotlight is doing work.
inline GigLoad spotlightLoad(std::string_view processList) noexcept
{
    if (processList.empty())
        return GigLoad::unknown;
    if (containsText(processList, "mdworker") || containsText(processList, "mds_stores"))
        return GigLoad::busy;
    return GigLoad::clear;
}

// `tmutil status`. Running = 1 is a backup in progress.
inline GigLoad timeMachineLoad(std::string_view status) noexcept
{
    if (status.empty())
        return GigLoad::unknown;
    if (containsText(status, "Running = 1"))
        return GigLoad::busy;
    return GigLoad::clear;
}

// AutomaticDownload / AutomaticallyInstallMacOSUpdates, or an installd process.
// softwareupdated is the always-on daemon and is not a backup or an install.
inline GigLoad updatesLoad(std::string_view text) noexcept
{
    if (text.empty())
        return GigLoad::unknown;
    if (containsText(text, "AutomaticDownload = 1")
        || containsText(text, "AutomaticallyInstallMacOSUpdates = 1")
        || containsText(text, "\ninstalld\n"))
        return GigLoad::busy;
    return GigLoad::clear;
}

// `networksetup -getairportpower`. "On" is busy. "Off" is clear.
inline GigLoad wifiLoad(std::string_view text) noexcept
{
    if (text.empty())
        return GigLoad::unknown;
    if (containsText(text, ": On") || containsText(text, ": on"))
        return GigLoad::busy;
    if (containsText(text, ": Off") || containsText(text, ": off"))
        return GigLoad::clear;
    return GigLoad::unknown;
}

// Bluetooth ControllerPowerState. 1 is on.
inline GigLoad bluetoothLoad(std::string_view text) noexcept
{
    if (text.empty())
        return GigLoad::unknown;
    if (containsText(text, "ControllerPowerState = 1") || containsText(text, "State: On"))
        return GigLoad::busy;
    if (containsText(text, "ControllerPowerState = 0") || containsText(text, "State: Off"))
        return GigLoad::clear;
    return GigLoad::unknown;
}

inline const char* gigLoadWord(GigLoad load) noexcept
{
    switch (load)
    {
        case GigLoad::busy: return "on";
        case GigLoad::clear: return "quiet";
        case GigLoad::unknown: return "unknown";
    }
    return "unknown";
}

// One row: name, state, and the one-line hint. Unknown still shows the hint.
inline std::string gigCheckLine(const char* name, GigLoad load, const char* hint)
{
    std::string line = name != nullptr ? name : "Check";
    line += ": ";
    line += gigLoadWord(load);
    line += " - ";
    if (load == GigLoad::unknown)
    {
        line += kGigCouldNotCheck;
        line += " ";
    }
    if (hint != nullptr)
        line += hint;
    return line;
}

} // namespace youhost
