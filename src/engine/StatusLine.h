#pragma once

#include <string>

namespace youhost
{

// Fixed character columns. A changing digit stays inside its column.
// The last two characters of every column are a gap, so a full field cannot
// run into the next one. The device column fits
// "Offline (no audio) - 128 channels" (33) plus that gap.
inline constexpr int kStatusColumnGap = 2;
inline constexpr int kStatusDeviceChars = 35;
inline constexpr int kStatusIoChars = 18;
inline constexpr int kStatusRateChars = 11;
inline constexpr int kStatusBitsChars = 14;
inline constexpr int kStatusCpuChars = 10;
inline constexpr int kStatusSessionChars = 24;

inline int statusLineChars() noexcept
{
    return kStatusDeviceChars + kStatusIoChars + kStatusRateChars + kStatusBitsChars
         + kStatusCpuChars + kStatusSessionChars;
}

// Fit text into the column and keep a trailing gap. A value that still does
// not fit ends with "..." inside the content area, never against the next field.
inline std::string fitStatusColumn(std::string text, int width)
{
    if (width < 0)
        width = 0;
    const int gap = width < kStatusColumnGap ? width : kStatusColumnGap;
    const int content = width - gap;
    if (static_cast<int>(text.size()) > content)
    {
        if (content <= 0)
            text.clear();
        else if (content < 4)
            text.resize(static_cast<std::size_t>(content));
        else
        {
            text.resize(static_cast<std::size_t>(content - 3));
            text += "...";
        }
    }
    if (static_cast<int>(text.size()) < width)
        text.append(static_cast<std::size_t>(width - static_cast<int>(text.size())), ' ');
    return text;
}

// 'CPU   7%', 'CPU  42%', 'CPU 140%', 'CPU 999%'. Values above 999 clamp.
// 'CPU 999%' is 8 characters and fits the 10-wide column with its 2-character gap.
inline std::string formatCpuField(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 999)
        percent = 999;
    std::string digits = std::to_string(percent);
    while (digits.size() < 3)
        digits.insert(digits.begin(), ' ');
    return "CPU " + digits + "%";
}

inline std::string formatStatusLine(std::string device,
                                    std::string io,
                                    std::string rate,
                                    std::string bits,
                                    int cpuPercent,
                                    std::string session)
{
    std::string line = fitStatusColumn(std::move(device), kStatusDeviceChars);
    line += fitStatusColumn(std::move(io), kStatusIoChars);
    line += fitStatusColumn(std::move(rate), kStatusRateChars);
    line += fitStatusColumn(std::move(bits), kStatusBitsChars);
    line += fitStatusColumn(formatCpuField(cpuPercent), kStatusCpuChars);
    line += fitStatusColumn(std::move(session), kStatusSessionChars);
    return line;
}

inline int statusCpuColumnIndex() noexcept
{
    return kStatusDeviceChars + kStatusIoChars + kStatusRateChars + kStatusBitsChars;
}

} // namespace youhost
