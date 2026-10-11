#pragma once

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

} // namespace youhost
