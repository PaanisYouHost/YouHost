#pragma once

namespace youhost
{

// A quit the user asked for (close button, Quit menu, Command-Q) may proceed.
// A plugin calling exit or asking the process to terminate may not.
inline bool quitIsUserInitiated(bool userArmed, bool userEvent) noexcept
{
    return userArmed || userEvent;
}

inline bool refuseUnsolicitedQuit(bool userArmed, bool userEvent, bool processExitAllowed) noexcept
{
    if (processExitAllowed)
        return false;
    return ! quitIsUserInitiated(userArmed, userEvent);
}

} // namespace youhost
