#pragma once

#include <cmath>

// Meter marks shared by the YouMix and PSE editors.
// Same rule as YouHost: green below the line-level point, yellow up to but
// not including 0 dBFS, red at full scale. The floor is -60 dB.

namespace youmix
{

inline constexpr float kMeterFloorDb = -60.0f;
inline constexpr int kMeterReferenceDb = -20;
inline constexpr float kGrMeterFullDb = 40.0f;

inline float decibelsToGain(float db) noexcept
{
    return std::pow(10.0f, db / 20.0f);
}

inline float gainToDecibels(float linear) noexcept
{
    if (linear <= 0.0000001f)
        return -160.0f;
    return 20.0f * std::log10(linear);
}

// 0 green, 1 yellow, 2 red.
inline int meterColourIndex(float dbFs, int referenceDb = kMeterReferenceDb) noexcept
{
    if (dbFs >= 0.0f)
        return 2;
    if (dbFs >= static_cast<float>(referenceDb))
        return 1;
    return 0;
}

// 0 is silence at the floor, 1 is full scale. Drawn from the bottom up.
inline float inputMeterFill(float linear) noexcept
{
    const float db = gainToDecibels(linear);
    const float range = 0.0f - kMeterFloorDb;
    if (range <= 0.0f)
        return 0.0f;
    const float fill = (db - kMeterFloorDb) / range;
    if (fill < 0.0f)
        return 0.0f;
    if (fill > 1.0f)
        return 1.0f;
    return fill;
}

// 0 is no reduction, 1 is kGrMeterFullDb. Drawn from the top downward.
inline float reductionMeterFill(float reductionDb) noexcept
{
    if (reductionDb <= 0.0f)
        return 0.0f;
    if (reductionDb >= kGrMeterFullDb)
        return 1.0f;
    return reductionDb / kGrMeterFullDb;
}

} // namespace youmix
