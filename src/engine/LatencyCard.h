#pragma once

#include "LatencyMath.h"

#include <algorithm>
#include <string>

namespace youhost
{

// Fixed stack for the LATENCY card. Nothing is given a share of leftover
// height, so the compensation row and the mode note keep their full size.
inline constexpr int kLatencyPadX = 18;
inline constexpr int kLatencyPadY = 14;
inline constexpr int kLatencyHeroH = 122;
inline constexpr int kLatencyRowH = 28;
inline constexpr int kLatencyModeH = 32;
inline constexpr int kLatencyGroupRowH = 22;
inline constexpr int kLatencyListedGroupRows = 10;
inline constexpr int kLatencyGap = 8;
inline constexpr int kLatencyCharPx = 7;
inline constexpr int kLatencyLineH = 16;
inline constexpr int kLatencyMinContentWidth = 560;
inline constexpr int kLatencyPreferredWidth = 680;

inline constexpr const char* kLatencyModeAll =
    "Global: every included channel lines up on the slowest plugin, including a dry channel next to one.";
inline constexpr const char* kLatencyModeGroup =
    "Per group: only channels inside a group line up on that group's slowest plugin. Ungrouped channels get no extra delay.";
inline constexpr const char* kLatencyFormulaCore =
    "Round trip = input + output - one buffer + compensation. "
    "JUCE 9 CoreAudio includes the buffer in both input and output latency.";
inline constexpr const char* kLatencyFormulaAlsa =
    "Round trip = input + output + one buffer + compensation. "
    "JUCE ALSA reports latency with one period already removed.";
inline constexpr const char* kLatencyFormulaDriver =
    "Round trip = driver input + driver output + compensation.";

struct LatencyBlock
{
    int top = 0;
    int height = 0;
};

struct LatencyCardLayout
{
    int contentWidth = 0;
    int contentHeight = 0;
    int textWidth = 0;
    int noteLines = 1;
    LatencyBlock hero;
    LatencyBlock buffer;
    LatencyBlock input;
    LatencyBlock output;
    LatencyBlock compensation;
    LatencyBlock dropouts;
    LatencyBlock modes;
    LatencyBlock groups;
    int groupRows = 0;
    LatencyBlock note;
};

inline int latencyTextChars(const char* text) noexcept
{
    if (text == nullptr)
        return 0;
    int count = 0;
    for (const char* cursor = text; *cursor != '\0'; ++cursor)
        ++count;
    return count;
}

inline std::string latencyNoteText(bool perGroup, RoundTripFormula formula)
{
    const char* mode = perGroup ? kLatencyModeGroup : kLatencyModeAll;
    const char* roundTrip = kLatencyFormulaDriver;
    if (formula == RoundTripFormula::coreAudioSubtractOneBuffer)
        roundTrip = kLatencyFormulaCore;
    else if (formula == RoundTripFormula::alsaAddOneBuffer)
        roundTrip = kLatencyFormulaAlsa;
    std::string note = mode;
    note += "  ";
    note += roundTrip;
    return note;
}

inline int longestLatencyNoteChars() noexcept
{
    const int allCore = latencyTextChars(kLatencyModeAll) + 2 + latencyTextChars(kLatencyFormulaCore);
    const int groupCore = latencyTextChars(kLatencyModeGroup) + 2 + latencyTextChars(kLatencyFormulaCore);
    const int allAlsa = latencyTextChars(kLatencyModeAll) + 2 + latencyTextChars(kLatencyFormulaAlsa);
    const int groupAlsa = latencyTextChars(kLatencyModeGroup) + 2 + latencyTextChars(kLatencyFormulaAlsa);
    return std::max(std::max(allCore, groupCore), std::max(allAlsa, groupAlsa));
}

inline int latencyWrappedLines(int chars, int textWidth) noexcept
{
    const int perLine = std::max(1, textWidth / kLatencyCharPx);
    if (chars <= 0)
        return 1;
    return (chars + perLine - 1) / perLine;
}

inline int latencyGroupSectionRows(int groupRows) noexcept
{
    if (groupRows < 0)
        groupRows = 0;
    if (groupRows > kLatencyListedGroupRows)
        groupRows = kLatencyListedGroupRows;
    return groupRows;
}

inline LatencyCardLayout layoutLatencyCard(int width, int noteChars, int groupRows, bool ungroupedLine) noexcept
{
    LatencyCardLayout card;
    card.contentWidth = std::max(kLatencyMinContentWidth, width);
    card.textWidth = std::max(1, card.contentWidth - 2 * kLatencyPadX);
    card.noteLines = latencyWrappedLines(noteChars, card.textWidth);
    const int noteHeight = card.noteLines * kLatencyLineH + 8;

    int y = kLatencyPadY;
    const auto place = [&](int height)
    {
        LatencyBlock block;
        block.top = y;
        block.height = height;
        y += height;
        return block;
    };

    card.hero = place(kLatencyHeroH);
    y += kLatencyGap;
    card.buffer = place(kLatencyRowH);
    card.input = place(kLatencyRowH);
    card.output = place(kLatencyRowH);
    card.compensation = place(kLatencyRowH);
    card.dropouts = place(kLatencyRowH);
    y += kLatencyGap;
    card.modes = place(kLatencyModeH);
    y += kLatencyGap;
    card.groupRows = latencyGroupSectionRows(groupRows);
    const int ungrouped = ungroupedLine ? 1 : 0;
    card.groups = place((card.groupRows + ungrouped) * kLatencyGroupRowH);
    y += kLatencyGap;
    card.note = place(noteHeight);
    y += kLatencyPadY;
    card.contentHeight = y;
    return card;
}

inline LatencyCardLayout layoutLatencyCard(int width, int noteChars) noexcept
{
    return layoutLatencyCard(width, noteChars, kLatencyListedGroupRows, true);
}

inline LatencyCardLayout layoutLatencyCard(int width) noexcept
{
    return layoutLatencyCard(width, longestLatencyNoteChars(), kLatencyListedGroupRows, true);
}

} // namespace youhost
