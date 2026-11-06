#pragma once

namespace youhost
{

void noteQuitLogPath(const char* path) noexcept;
void noteQuitMarkDirectory(const char* path) noexcept;
void armUserQuit() noexcept;
void installCommandQuitMonitor() noexcept;
void allowProcessExit() noexcept;
bool consumeUserQuit() noexcept;
bool macCurrentEventIsUserQuit() noexcept;

// True when this quit did not come from the user. The caller must not quit.
// A quit that is allowed also arms process exit so a later exit() can finish.
bool shouldRefuseQuitRequest() noexcept;

} // namespace youhost
