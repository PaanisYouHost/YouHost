#pragma once

#include "HostLimits.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace youhost
{

// Allowed line-level references. 0 on the RMS scale sits at this many dBFS.
inline constexpr std::array<int, 3> kRmsReferenceChoices { -14, -18, -20 };

inline constexpr std::array<int, 9> kRmsTickLabels { 20, 10, 5, 0, -5, -10, -20, -30, -40 };
inline constexpr std::array<int, 8> kPeakTickLabels { 0, -3, -6, -10, -20, -30, -40, -60 };

struct MeterSpan
{
    float bottomDb = kMeterFloorDb;
    float topDb = 0.0f;
};

struct MeterTick
{
    int label = 0;
    float dbFs = 0.0f;
};

// Nearest allowed reference. A tie prefers the lower one, which keeps more headroom.
inline int normaliseRmsReferenceDb(int db)
{
    int best = kDefaultRmsReferenceDb;
    int bestDistance = 100000;
    for (int choice : kRmsReferenceChoices)
    {
        const int distance = std::abs(choice - db);
        if (distance < bestDistance || (distance == bestDistance && choice < best))
        {
            best = choice;
            bestDistance = distance;
        }
    }
    return best;
}

// RMS ticks run from +20 to -40 around the reference, a 60 dB ruler.
// With the default -20 dBFS reference the top tick is 0 dBFS.
inline MeterSpan rmsMeterSpan(int referenceDb)
{
    const int reference = normaliseRmsReferenceDb(referenceDb);
    return { static_cast<float>(reference - 40), static_cast<float>(reference + 20) };
}

inline MeterSpan peakMeterSpan()
{
    return { kMeterFloorDb, 0.0f };
}

inline int rmsTicks(int referenceDb, MeterTick* out, int capacity)
{
    const int reference = normaliseRmsReferenceDb(referenceDb);
    int count = 0;
    if (out == nullptr || capacity <= 0)
        return 0;

    for (int label : kRmsTickLabels)
    {
        if (count >= capacity)
            break;
        out[count++] = MeterTick { label, static_cast<float>(reference + label) };
    }
    return count;
}

inline int peakTicks(MeterTick* out, int capacity)
{
    int count = 0;
    if (out == nullptr || capacity <= 0)
        return 0;

    for (int label : kPeakTickLabels)
    {
        if (count >= capacity)
            break;
        out[count++] = MeterTick { label, static_cast<float>(label) };
    }
    return count;
}

inline float gainToDb(float linearGain)
{
    if (linearGain <= 0.0000001f)
        return -160.0f;
    return 20.0f * std::log10(linearGain);
}

inline float normaliseDb(float db, MeterSpan span)
{
    const float range = span.topDb - span.bottomDb;
    if (range <= 0.0f)
        return 0.0f;
    const float normalised = (db - span.bottomDb) / range;
    if (normalised < 0.0f)
        return 0.0f;
    if (normalised > 1.0f)
        return 1.0f;
    return normalised;
}

enum class MeterColour
{
    green,
    yellow,
    red
};

// Green below line level (RMS 0), yellow from that point up to but not including
// 0 dBFS, red at full scale. Peak mode uses the same dBFS points on its own ruler.
inline MeterColour meterColourForDb(float dbFs, int referenceDb) noexcept
{
    if (dbFs >= 0.0f)
        return MeterColour::red;
    if (dbFs >= static_cast<float>(normaliseRmsReferenceDb(referenceDb)))
        return MeterColour::yellow;
    return MeterColour::green;
}

// 0 is the bottom of the ruler, 1 is the top. Peak is full scale. RMS is the VU ruler.
inline float meterNormal(float linearGain, bool peak, int referenceDb)
{
    const MeterSpan span = peak ? peakMeterSpan() : rmsMeterSpan(referenceDb);
    return normaliseDb(gainToDb(linearGain), span);
}

} // namespace youhost
