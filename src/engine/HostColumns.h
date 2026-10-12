#pragma once

#include <algorithm>

namespace youhost
{

// HOST channel row, left to right. The header uses the same boxes, so the
// labels sit on the controls. Meter and Ex stay between NAME and DELAY COMP
// and have no header of their own.
struct HostColumnBox
{
    int x = 0;
    int width = 0;
};

struct HostColumns
{
    HostColumnBox channel;
    HostColumnBox mode;
    HostColumnBox name;
    HostColumnBox meter;
    HostColumnBox exclude;
    HostColumnBox delay;
    HostColumnBox plugins;
    HostColumnBox gain;
    HostColumnBox output;
};

inline constexpr int kHostColumnHeaderHeight = 22;
inline constexpr const char* kHostColumnChannel = "CH";
inline constexpr const char* kHostColumnMode = "MODE";
inline constexpr const char* kHostColumnName = "NAME";
inline constexpr const char* kHostColumnDelay = "DELAY COMP";
inline constexpr const char* kHostColumnPlugins = "PLUGINS";
inline constexpr const char* kHostColumnGain = "GAIN";
inline constexpr const char* kHostColumnOutput = "OUT";

inline HostColumns layoutHostColumns(int rowWidth) noexcept
{
    HostColumns columns;
    int x = 12;
    const int rightLimit = std::max(x, rowWidth - 4);
    auto take = [&](int width)
    {
        HostColumnBox box;
        box.x = x;
        const int room = std::max(0, rightLimit - x);
        box.width = std::min(width, room);
        x += box.width;
        return box;
    };

    columns.channel = take(46);
    columns.mode = take(62);
    columns.name = take(120);
    columns.meter = take(18);
    columns.exclude = take(34);

    constexpr int gainWidth = 72;
    constexpr int delayWidth = 112;
    constexpr int outputWidth = 20;
    const int tail = std::max(0, rightLimit - x);
    columns.output.width = std::min(outputWidth, tail);
    columns.output.x = std::max(x, rightLimit - columns.output.width);
    const int gainRoom = std::max(0, columns.output.x - x);
    columns.gain.width = std::min(gainWidth, gainRoom);
    columns.gain.x = std::max(x, columns.output.x - columns.gain.width);

    const int middle = std::max(0, columns.gain.x - x);
    columns.delay.x = x;
    columns.delay.width = std::min(delayWidth, middle);
    columns.plugins.x = columns.delay.x + columns.delay.width;
    columns.plugins.width = std::max(0, columns.gain.x - columns.plugins.x);
    return columns;
}

// DELAY COMP starts at the EX button, not at the delay readout to its right.
inline HostColumnBox delayCompHeaderBox(const HostColumns& columns) noexcept
{
    HostColumnBox box;
    box.x = columns.exclude.x;
    const int right = columns.delay.x + columns.delay.width;
    box.width = std::max(0, right - box.x);
    return box;
}

} // namespace youhost
