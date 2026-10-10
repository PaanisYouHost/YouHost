#pragma once

namespace youhost
{

// Fixed topology from the architecture notes. The recorder and the plugin rack
// use this same channel count. They do not grow this array.
inline constexpr int kMaxChannels = 128;

// JUCE draws a device menu when the maximum is above zero, and it draws
// per-channel checkboxes only when the minimum is below the card's channel
// count. This bound keeps the menus and leaves the checkboxes out.
inline constexpr int kDeviceSelectorChannels = 512;
inline constexpr int kSlotsPerChannel = 4;

inline constexpr float kRmsWindowSeconds = 0.300f;
inline constexpr float kPeakHoldSeconds = 1.5f;
inline constexpr float kPeakDecaySeconds = 0.600f;
inline constexpr float kMeterFloorDb = -60.0f;

// RMS 0 is line level. The default leaves +20 dB of digital headroom above it.
inline constexpr int kDefaultRmsReferenceDb = -20;

// 16 and 24 are integer PCM. 32 is 32-bit float. Anything else becomes 24.
inline constexpr int kDefaultWavBitDepth = 24;

inline int normaliseWavBitDepth(int bits) noexcept
{
    if (bits == 16 || bits == 32)
        return bits;
    return kDefaultWavBitDepth;
}

inline bool wavBitDepthIsFloat(int bits) noexcept
{
    return normaliseWavBitDepth(bits) == 32;
}

} // namespace youhost
