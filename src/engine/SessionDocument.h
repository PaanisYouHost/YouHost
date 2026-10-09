#pragma once

#include "ChannelListen.h"
#include "DisplayLayout.h"
#include "HostLimits.h"
#include "TakePlan.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace youhost
{

struct SessionSlot
{
    bool occupied = false;
    bool bypassed = false;
    int stereoFold = 0;
    juce::PluginDescription description;
    juce::MemoryBlock state;
};

struct SessionGroup
{
    bool used = false;
    juce::String name;
    int color = 0;
    bool collapsed = false;
};

struct SessionChannel
{
    bool excludeFromCompensation = false;
    bool recordEnabled = true;
    bool sceneSafe = false;
    ChannelListen listen = ChannelListen::record;
    float outputDb = 0.0f;
    int color = 0;
    int group = -1;
    juce::String name;
    std::array<SessionSlot, kSlotsPerChannel> slots {};
};

// One stored channel inside a scene. Name, colour, group, exclude, and SAFE
// stay on the live channel. An omitted channel means REC, 0 dB, empty slots.
struct SessionSceneChannel
{
    int index = 0;
    ChannelListen listen = ChannelListen::record;
    float outputDb = 0.0f;
    std::array<SessionSlot, kSlotsPerChannel> slots {};
};

struct SessionScene
{
    juce::String name;
    int remote = -1;
    std::vector<SessionSceneChannel> channels;
};

struct SessionTake
{
    std::int64_t startSample = 0;
    std::int64_t lengthSamples = 0;
    std::array<juce::String, kMaxChannels> files {};
    std::vector<WavePeak> peaks;
    std::array<std::vector<WavePeak>, kMaxChannels> channelPeaks {};
};

struct SessionData
{
    bool peakMeter = false;
    int rmsReferenceDb = kDefaultRmsReferenceDb;
    int wavBitDepth = kDefaultWavBitDepth;
    int page = 1;
    float waveformGain = 1.0f;
    int alignGroup = 0;
    double sampleRate = 0.0;
    std::unique_ptr<juce::XmlElement> device;
    std::array<SessionChannel, kMaxChannels> channels {};
    std::array<SessionGroup, kMaxDisplayGroups> groups {};
    std::vector<SessionTake> takes;
    std::vector<SessionScene> scenes;
    int recalledScene = -1;
    bool sceneDrift = false;
};

// Writes session.youhost. The caller creates the sibling audio/ folder.
bool writeSessionFile(const juce::File& file, const SessionData& data);
bool readSessionFile(const juce::File& file, SessionData& data);

} // namespace youhost
