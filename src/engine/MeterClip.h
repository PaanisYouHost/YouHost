#pragma once

namespace youhost
{

// REC clip mark. Full strip width, and thicker than the old 7 px tick.
inline float clipBandHeight(float bodyHeight) noexcept
{
    if (bodyHeight <= 0.0f)
        return 0.0f;
    const float wanted = bodyHeight * 0.14f;
    if (wanted < 16.0f)
        return bodyHeight < 16.0f ? bodyHeight : 16.0f;
    if (wanted > 22.0f)
        return 22.0f;
    return wanted;
}

} // namespace youhost
