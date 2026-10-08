#pragma once

namespace youhost
{

// Fixed topology from the architecture notes. The recorder and the plugin rack
// use this same channel count. They do not grow this array.
inline constexpr int kMaxChannels = 128;
inline constexpr int kSlotsPerChannel = 4;

inline constexpr float kRmsWindowSeconds = 0.300f;
inline constexpr float kPeakHoldSeconds = 1.5f;
inline constexpr float kPeakDecaySeconds = 0.600f;
inline constexpr float kMeterFloorDb = -60.0f;

// RMS 0 is line level. The default leaves +20 dB of digital headroom above it.
inline constexpr int kDefaultRmsReferenceDb = -20;

} // namespace youhost
