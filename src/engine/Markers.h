#pragma once

#include <cstdint>
#include <optional>
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

// MARKERS, CPU, DROPOUTS, and LATENCY keep Space, M, and the number keys.
// A text field, including a marker name editor, keeps the key.
inline bool toolWindowForwardsTransportKeys(bool textFieldFocused) noexcept
{
    return ! textFieldFocused;
}

// Space while a marker row is selected starts playback at that row.
// The name editor, a closed list, and a transport that is not starting
// playback leave the playhead where it is.
inline std::optional<std::int64_t> playSampleFromSelectedMarker(bool markersOpen,
                                                               bool nameEditing,
                                                               bool rowSelected,
                                                               std::int64_t sample,
                                                               bool startingPlayback) noexcept
{
    if (! markersOpen || nameEditing || ! rowSelected || ! startingPlayback)
        return std::nullopt;
    return sample < 0 ? std::int64_t { 0 } : sample;
}

} // namespace youhost
