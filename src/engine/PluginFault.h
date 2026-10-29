#pragma once

namespace youhost
{

// 1: a plugin thread was parked and the editor should close.
// 2: the message thread crashed inside a plugin and jumped back to the run loop.
inline constexpr int kPluginFaultOtherThread = 1;
inline constexpr int kPluginFaultMessageThread = 2;

// A plugin SIGSEGV on its own thread is parked. A plugin SIGSEGV on the
// message thread jumps back to the run loop. The audio thread still aborts.
void installPluginFaultGuard();
void noteAudioThread() noexcept;
int takePluginFault() noexcept;

} // namespace youhost
