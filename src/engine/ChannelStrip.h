#pragma once

#include "HostLimits.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace youhost
{

// Phase 2 stores a plugin id and state here. Phase 3 processes the slot in a
// sandbox process. P0 leaves every slot empty.
struct PluginSlot
{
    bool occupied = false;
    bool bypassed = true;
};

struct MeterState
{
    float meanSquare = 0.0f;
    float heldPeak = 0.0f;
    int peakHoldSamples = 0;
    bool clipped = false;
};

// Written by the audio thread, read by the UI. clearRequested goes the other way.
struct MeterSnapshot
{
    std::atomic<float> rms { 0.0f };
    std::atomic<float> peak { 0.0f };
    std::atomic<bool> clipped { false };
    std::atomic<bool> clearRequested { false };
};

struct ChannelStrip
{
    ChannelStrip() = default;
    ChannelStrip(const ChannelStrip&) = delete;
    ChannelStrip& operator=(const ChannelStrip&) = delete;
    ChannelStrip(ChannelStrip&&) = delete;
    ChannelStrip& operator=(ChannelStrip&&) = delete;

    int inputIndex = 0;
    int outputIndex = 0;
    bool recordEnabled = false;              // Phase 1: raw input, before plugins
    bool excludeFromCompensation = false;    // Later: leave this channel undelayed
    std::array<PluginSlot, kSlotsPerChannel> slots {};
    MeterState meterState {};
    MeterSnapshot meter {};
};

} // namespace youhost
