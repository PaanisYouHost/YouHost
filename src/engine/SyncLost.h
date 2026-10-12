#pragma once

#include <cstdint>

namespace youhost
{

// The visual alarm and the reopen scan are separate. OK hides the blink.
// The scan keeps going until the card is back.
struct SyncLostState
{
    bool missing = false;
    bool acked = false;

    void deviceLost() noexcept
    {
        missing = true;
        acked = false;
    }

    void deviceReturned() noexcept
    {
        missing = false;
        acked = false;
    }

    void acknowledge() noexcept
    {
        acked = true;
    }

    bool showAlarm() const noexcept
    {
        return missing && ! acked;
    }

    bool keepReattaching() const noexcept
    {
        return missing;
    }
};

inline bool syncLostBlink(std::uint32_t millisecondCounter) noexcept
{
    return (millisecondCounter / 400u) % 2u == 0u;
}

} // namespace youhost
