#pragma once

#include "HostLimits.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>

namespace youhost
{

struct SessionSlot
{
    bool occupied = false;
    bool bypassed = false;
    juce::PluginDescription description;
    juce::MemoryBlock state;
};

struct SessionChannel
{
    bool excludeFromCompensation = false;
    std::array<SessionSlot, kSlotsPerChannel> slots {};
};

struct SessionData
{
    bool peakMeter = false;
    int rmsReferenceDb = kDefaultRmsReferenceDb;
    std::unique_ptr<juce::XmlElement> device;
    std::array<SessionChannel, kMaxChannels> channels {};
};

// Writes session.youhost. The caller creates the sibling audio/ folder.
bool writeSessionFile(const juce::File& file, const SessionData& data);
bool readSessionFile(const juce::File& file, SessionData& data);

} // namespace youhost
