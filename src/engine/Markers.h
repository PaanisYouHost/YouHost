#pragma once

#include <cstdint>
#include <string>

namespace youhost
{

// A marker is a session label at a sample. Dropping one does not start,
// stop, or relocate the recorder. The audio thread never reads this list.
inline bool markerDropAffectsTransport() noexcept
{
    return false;
}

inline bool markerDropAllowedWhileLocked() noexcept
{
    return true;
}

struct MarkerPoint
{
    std::int64_t sample = 0;
    std::string name;
};

inline std::string markerRulerText(const std::string& name)
{
    if (name.empty())
        return "MARK";
    return name;
}

inline MarkerPoint makeMarker(std::int64_t sample, int ordinal)
{
    MarkerPoint marker;
    marker.sample = sample < 0 ? 0 : sample;
    marker.name = "Marker " + std::to_string(ordinal > 0 ? ordinal : 1);
    return marker;
}

} // namespace youhost
