#pragma once

namespace youhost
{

// Held while a card is open. Not called from the audio callback.
// On macOS this is NSActivityLatencyCritical plus idle-sleep assertions.
// Elsewhere it does nothing.

#if defined(__APPLE__)
void beginLivePowerClaim() noexcept;
void endLivePowerClaim() noexcept;
#else
inline void beginLivePowerClaim() noexcept {}
inline void endLivePowerClaim() noexcept {}
#endif

} // namespace youhost
