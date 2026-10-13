#pragma once

#include "EditHistory.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

// Deleting a marker does not start, stop, or relocate the recorder, and the
// record lock does not block it. The audio thread never reads the list.
inline bool markerDeleteAffectsTransport() noexcept
{
    return false;
}

inline bool markerDeleteAllowedWhileLocked() noexcept
{
    return true;
}

inline constexpr const char* kMarkerDeleteItem = "Delete";

// Cmd+Backspace deletes the selected marker. Plain Backspace, plain Delete,
// and a text field do not. Recording is not an input: it never blocks this.
inline bool markerDeleteShortcut(bool command, bool backspace, bool forwardDelete, bool textField) noexcept
{
    if (textField || forwardDelete || ! command || ! backspace)
        return false;
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

inline bool eraseMarker(std::vector<MarkerPoint>& markers, int index)
{
    if (index < 0 || index >= static_cast<int>(markers.size()))
        return false;
    markers.erase(markers.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

inline int findMarkerPoint(const std::vector<MarkerPoint>& markers, const MarkerPoint& point)
{
    for (int index = 0; index < static_cast<int>(markers.size()); ++index)
    {
        const auto& marker = markers[static_cast<std::size_t>(index)];
        if (marker.sample == point.sample && marker.name == point.name)
            return index;
    }
    return -1;
}

enum class MarkerEditKind
{
    insert,
    erase
};

// Undo stores the marker that was removed and puts it back. Redo erases that
// same marker. Markers added afterwards stay in the list.
struct MarkerEdit
{
    MarkerEditKind kind = MarkerEditKind::insert;
    int index = -1;
    MarkerPoint marker;
};

inline bool applyMarkerEdit(std::vector<MarkerPoint>& markers, const MarkerEdit& edit)
{
    if (edit.kind == MarkerEditKind::erase)
    {
        int at = edit.index;
        if (at < 0 || at >= static_cast<int>(markers.size())
            || markers[static_cast<std::size_t>(at)].sample != edit.marker.sample
            || markers[static_cast<std::size_t>(at)].name != edit.marker.name)
            at = findMarkerPoint(markers, edit.marker);
        return eraseMarker(markers, at);
    }

    if (findMarkerPoint(markers, edit.marker) >= 0)
        return false;
    int at = edit.index < 0 ? 0 : edit.index;
    if (at > static_cast<int>(markers.size()))
        at = static_cast<int>(markers.size());
    markers.insert(markers.begin() + static_cast<std::ptrdiff_t>(at), edit.marker);
    return true;
}

inline int markerIndexAfterDelete(int selected, int deleted, int countAfter) noexcept
{
    if (countAfter <= 0 || selected < 0)
        return -1;
    if (selected > deleted)
        return selected - 1;
    if (selected >= countAfter)
        return countAfter - 1;
    return selected;
}

struct MarkerBand
{
    float lineX = 0.0f;
    float lineTop = 0.0f;
    float lineBottom = 0.0f;
    float pillX = 0.0f;
    float pillY = 0.0f;
    float pillW = 0.0f;
    float pillH = 0.0f;
};

inline bool markerBandContains(const MarkerBand& band, float x, float y, float slop = 5.0f) noexcept
{
    const bool pill = band.pillW > 0.0f && band.pillH > 0.0f
                      && x >= band.pillX && x <= band.pillX + band.pillW
                      && y >= band.pillY && y <= band.pillY + band.pillH;
    const bool line = y >= band.lineTop && y <= band.lineBottom && std::fabs(x - band.lineX) <= slop;
    return pill || line;
}

// One undo step touches either the gain or the marker list, never both, and
// never the recorder. Delete, then change gain: the first undo restores the
// gain and the second puts the marker back. Redo walks those steps backward.
struct UndoTapeStep
{
    bool changesGain = false;
    float gain = 0.0f;
    bool changesMarker = false;
    MarkerEdit marker {};
};

struct UndoTape
{
    float gain = 0.0f;
    bool recording = false;
    std::vector<MarkerPoint> markers;
    EditHistory<UndoTapeStep> history;
};

inline void applyUndoTapeStep(UndoTape& tape, const UndoTapeStep& step)
{
    const bool recording = tape.recording;
    if (step.changesGain)
        tape.gain = step.gain;
    if (step.changesMarker)
        applyMarkerEdit(tape.markers, step.marker);
    tape.recording = recording;
}

inline UndoTapeStep inverseUndoTapeStep(const UndoTapeStep& step, float currentGain)
{
    UndoTapeStep inverse;
    if (step.changesMarker)
    {
        inverse.changesMarker = true;
        inverse.marker = step.marker;
        inverse.marker.kind = step.marker.kind == MarkerEditKind::erase ? MarkerEditKind::insert
                                                                        : MarkerEditKind::erase;
    }
    if (step.changesGain)
    {
        inverse.changesGain = true;
        inverse.gain = currentGain;
    }
    return inverse;
}

inline bool undoTapeStep(UndoTape& tape, bool redo)
{
    const UndoTapeStep* top = redo ? tape.history.redoTop() : tape.history.undoTop();
    if (top == nullptr)
        return false;
    UndoTapeStep current = inverseUndoTapeStep(*top, tape.gain);
    UndoTapeStep restored;
    const bool moved = redo ? tape.history.redo(std::move(current), restored)
                            : tape.history.undo(std::move(current), restored);
    if (! moved)
        return false;
    applyUndoTapeStep(tape, restored);
    return true;
}

inline void undoTapeDeleteMarker(UndoTape& tape, int index)
{
    if (index < 0 || index >= static_cast<int>(tape.markers.size()))
        return;
    UndoTapeStep before;
    before.changesMarker = true;
    before.marker.kind = MarkerEditKind::insert;
    before.marker.index = index;
    before.marker.marker = tape.markers[static_cast<std::size_t>(index)];
    tape.history.push(std::move(before));
    eraseMarker(tape.markers, index);
}

inline void undoTapeSetGain(UndoTape& tape, float gain)
{
    UndoTapeStep before;
    before.changesGain = true;
    before.gain = tape.gain;
    tape.history.push(before);
    tape.gain = gain;
}

inline int markerIndexAt(const MarkerBand* bands, int count, float x, float y, float slop = 5.0f) noexcept
{
    if (bands == nullptr || count <= 0)
        return -1;
    int found = -1;
    float best = 1.0e9f;
    for (int index = 0; index < count; ++index)
    {
        if (! markerBandContains(bands[index], x, y, slop))
            continue;
        const float distance = std::fabs(x - bands[index].lineX);
        if (distance < best)
        {
            best = distance;
            found = index;
        }
    }
    return found;
}

} // namespace youhost
