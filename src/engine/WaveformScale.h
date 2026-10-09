#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace youhost
{

// Display only. 0 dBFS fills the lane at gain 1. Quieter peaks use a dB curve
// so a normal signal is still easy to see. Gain is an extra display zoom.
inline constexpr float kDefaultWaveformGain = 1.0f;
inline constexpr float kMinWaveformGain = 0.35f;
inline constexpr float kMaxWaveformGain = 8.0f;
inline constexpr float kWaveformDbFloor = -60.0f;

inline float clampWaveformGain(float gain) noexcept
{
    if (! std::isfinite(gain) || gain <= 0.0f)
        return kDefaultWaveformGain;
    return std::clamp(gain, kMinWaveformGain, kMaxWaveformGain);
}

inline float waveformDisplayLevel(float sample, float gain) noexcept
{
    const float mag = std::fabs(sample);
    if (mag <= 1.0e-8f)
        return 0.0f;
    const float db = 20.0f * std::log10(std::min(mag, 1.0f));
    const float shaped = (db - kWaveformDbFloor) / (0.0f - kWaveformDbFloor);
    const float scaled = std::clamp(shaped, 0.0f, 1.0f) * clampWaveformGain(gain);
    return std::clamp(scaled, 0.0f, 1.0f);
}

inline float waveformDisplaySigned(float sample, float gain) noexcept
{
    const float level = waveformDisplayLevel(sample, gain);
    return std::copysign(level, sample);
}

inline float stepWaveformGain(float gain, int direction) noexcept
{
    static constexpr float steps[] = { 0.35f, 0.5f, 0.7f, 1.0f, 1.4f, 2.0f, 3.0f, 4.5f, 6.5f, 8.0f };
    const float current = clampWaveformGain(gain);
    int nearest = 0;
    float best = 1000.0f;
    for (int index = 0; index < static_cast<int>(sizeof(steps) / sizeof(steps[0])); ++index)
    {
        const float distance = std::fabs(steps[index] - current);
        if (distance < best)
        {
            best = distance;
            nearest = index;
        }
    }
    const int last = static_cast<int>(sizeof(steps) / sizeof(steps[0])) - 1;
    const int next = std::clamp(nearest + (direction > 0 ? 1 : direction < 0 ? -1 : 0), 0, last);
    return steps[next];
}

} // namespace youhost
