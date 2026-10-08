#pragma once

namespace youhost
{

// Fixed topology from the architecture notes. Later phases fill the slots,
// the recorder, and the compensation delay. They do not grow this array.
inline constexpr int kMaxChannels = 128;
inline constexpr int kSlotsPerChannel = 4;

inline constexpr float kRmsWindowSeconds = 0.300f;
inline constexpr float kPeakHoldSeconds = 1.5f;
inline constexpr float kPeakDecaySeconds = 0.600f;
inline constexpr float kClipHoldSeconds = 2.0f;
inline constexpr float kMeterFloorDb = -60.0f;

} // namespace youhost
