#pragma once

#include <algorithm>
#include <cmath>

namespace youhost
{

inline constexpr float kMinOutputDb = -9.0f;
inline constexpr float kMaxOutputDb = 9.0f;
inline constexpr float kOutputDbStep = 0.5f;

// Post-plugin output trim. 0 dB is unity and is the default.
inline float snapOutputDb(float db) noexcept
{
    if (! std::isfinite(db))
        return 0.0f;
    db = std::clamp(db, kMinOutputDb, kMaxOutputDb);
    const float steps = std::round(db / kOutputDbStep);
    return std::clamp(steps * kOutputDbStep, kMinOutputDb, kMaxOutputDb);
}

inline float outputDbToLinear(float db) noexcept
{
    const float snapped = snapOutputDb(db);
    // 0 dB has to be the bit pattern 1, not a libm result that is almost 1.
    // Anything else would scale every sample and fail a null test.
    if (std::fabs(snapped) < 0.01f)
        return 1.0f;
    return std::pow(10.0f, snapped / 20.0f);
}

inline bool outputDbIsUnity(float db) noexcept
{
    return std::fabs(snapOutputDb(db)) < 0.01f;
}

} // namespace youhost
