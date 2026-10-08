#pragma once

#include <cstdint>

namespace youhost
{

// Two words cover channels 0–63 and 64–127. The audio thread reads them
// with relaxed atomics. No lock and no allocation.
inline bool channelIsOn(std::uint64_t low, std::uint64_t high, int channel) noexcept
{
    if (channel < 0 || channel >= 128)
        return false;
    const auto word = channel < 64 ? low : high;
    return ((word >> (channel & 63)) & 1ull) != 0ull;
}

inline void setChannelOnBit(std::uint64_t& low, std::uint64_t& high, int channel, bool on) noexcept
{
    if (channel < 0 || channel >= 128)
        return;
    auto& word = channel < 64 ? low : high;
    const auto bit = 1ull << (channel & 63);
    if (on)
        word |= bit;
    else
        word &= ~bit;
}

} // namespace youhost
