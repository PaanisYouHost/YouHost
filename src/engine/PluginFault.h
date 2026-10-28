#pragma once

namespace youhost
{

// A plugin Metal/render thread can SIGSEGV. That thread is parked and the
// editor is closed. The audio thread and the message thread still abort.
void installPluginFaultGuard();
void noteAudioThread() noexcept;
bool takePluginFault() noexcept;

} // namespace youhost
