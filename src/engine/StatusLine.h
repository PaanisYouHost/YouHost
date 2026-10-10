#pragma once

#include <string>

namespace youhost
{

// Fixed character columns. A changing digit stays inside its column.
inline constexpr int kStatusDeviceChars = 28;
inline constexpr int kStatusIoChars = 18;
inline constexpr int kStatusRateChars = 10;
inline constexpr int kStatusBitsChars = 13;
inline constexpr int kStatusCpuChars = 8;
inline constexpr int kStatusSessionChars = 24;

inline std::string fitStatusColumn(std::string text, int width)
{
    if (width < 0)
        width = 0;
    if (static_cast<int>(text.size()) > width)
        text.resize(static_cast<std::size_t>(width));
    else if (static_cast<int>(text.size()) < width)
        text.append(static_cast<std::size_t>(width - static_cast<int>(text.size())), ' ');
    return text;
}

// 'CPU   7%', 'CPU  42%', 'CPU 100%'. Values outside 0..100 are clamped.
inline std::string formatCpuField(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
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
