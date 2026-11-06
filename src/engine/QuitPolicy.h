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

// Command-Q quits from every screen, including the start screen before a session exists.
inline bool commandQuitAlwaysReachesApp() noexcept
{
    return true;
}

// The Save / Save As prompt is only for a session that has unsaved changes.
inline bool commandQuitPromptsForUnsaved(bool hasSession, bool dirty) noexcept
{
    return hasSession && dirty;
}

} // namespace youhost
