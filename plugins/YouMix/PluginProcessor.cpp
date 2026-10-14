#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/YouMixCore.h"

#include <cstring>
#include <cstdint>

namespace
{

constexpr std::uint32_t kStateMagic = 0x594D5831; // YMX1
constexpr std::uint32_t kStateVersion = 4;
constexpr int kStateBytesV3 = 96;

struct YouMixState
{
    std::uint32_t magic = kStateMagic;
    std::uint32_t version = kStateVersion;
    std::int32_t slot = -1;
    std::int32_t group = 1;
    std::int32_t mode = 0;
    std::int32_t preset = 1;
    float pinkDb[youmix::kYouMixGroups] = { -32.0f, -32.0f, -32.0f, -32.0f };
    std::uint32_t bypass = 0;
    char name[32] = {};
    float attackMs = 5.0f;
    float releaseMs = 70.0f;
    float thresholdDb = -45.0f;
    float weightDb = 0.0f;
    std::uint32_t pinkBypass = 0;
    std::int32_t editorWidth = 0;
    std::int32_t editorHeight = 0;
};

static_assert(sizeof(YouMixState) == 104, "YouMix session blob must stay fixed-size");

} // namespace

YouMixAudioProcessor::YouMixAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    slot_ = youmix::youMixShared().claimSlot();
    setLatencySamples(0);
}

YouMixAudioProcessor::~YouMixAudioProcessor()
{
    youmix::youMixShared().releaseSlot(slot_);
}

void YouMixAudioProcessor::prepareToPlay(double, int)
{
    setLatencySamples(0);
}

void YouMixAudioProcessor::releaseResources() {}

bool YouMixAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& input = layouts.getMainInputChannelSet();
    const auto& output = layouts.getMainOutputChannelSet();
    if (input != output)
        return false;
    return output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo();
}

void YouMixAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = juce::jmin(2, buffer.getNumChannels());
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return;

    float* pointers[2] = { nullptr, nullptr };
    for (int channel = 0; channel < channels; ++channel)
        pointers[channel] = buffer.getWritePointer(channel);
    youmix::youMixShared().process(slot_, pointers, channels, samples, getSampleRate());
}

void YouMixAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&)
{
    // The dry signal is already in the buffer. Leave it untouched.
}

juce::AudioProcessorEditor* YouMixAudioProcessor::createEditor()
{
    return new YouMixAudioProcessorEditor(*this);
}

void YouMixAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    YouMixState state;
    state.slot = slot_;
    if (slot_ >= 0)
    {
        const youmix::YouMixSettings settings = youmix::youMixShared().settings();
        state.group = settings.group[slot_];
        state.mode = settings.mode[slot_];
        state.preset = settings.preset;
        state.bypass = settings.bypass ? 1u : 0u;
        for (int group = 0; group < youmix::kYouMixGroups; ++group)
            state.pinkDb[group] = settings.pinkDb[group];
        state.attackMs = settings.attackMs;
        state.releaseMs = settings.releaseMs;
        state.thresholdDb = settings.thresholdDb;
        state.weightDb = settings.weightDb[slot_];
        state.pinkBypass = 0;
        for (int group = 0; group < youmix::kYouMixGroups; ++group)
        {
            if (settings.pinkBypass[group])
                state.pinkBypass |= 1u << static_cast<unsigned>(group);
        }
        std::memcpy(state.name, settings.name[slot_], sizeof(state.name));
        state.name[31] = 0;
    }
    state.editorWidth = editorWidth_;
    state.editorHeight = editorHeight_;
    destData.append(&state, sizeof(state));
}

void YouMixAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < 76)
        return;

    YouMixState state;
    const int bytes = juce::jmin(sizeInBytes, static_cast<int>(sizeof(state)));
    std::memcpy(&state, data, static_cast<std::size_t>(bytes));
    if (state.magic != kStateMagic || state.version < 1 || state.version > kStateVersion)
        return;
    state.name[31] = 0;

    auto& core = youmix::youMixShared();
    if (slot_ >= 0)
        core.releaseSlot(slot_);
    slot_ = core.claimSlot(state.slot);
    if (slot_ < 0)
        return;

    core.setGroup(slot_, state.group);
    core.setMode(slot_, state.mode);
    core.setName(slot_, state.name);
    if (state.preset == 1 || state.preset == 2)
        core.applyPreset(state.preset);
    else
    {
        for (int group = 0; group < youmix::kYouMixGroups; ++group)
            core.setPinkDb(group, state.pinkDb[group], true);
        if (state.version >= 2 && bytes >= 88)
        {
            core.setAttackMs(state.attackMs, true);
            core.setReleaseMs(state.releaseMs, true);
            core.setThresholdDb(state.thresholdDb, true);
        }
    }
    if (state.version >= 3 && bytes >= kStateBytesV3)
    {
        core.setWeightDb(slot_, state.weightDb);
        for (int group = 0; group < youmix::kYouMixGroups; ++group)
            core.setPinkBypass(group, (state.pinkBypass & (1u << static_cast<unsigned>(group))) != 0);
    }
    if (state.version >= 4 && bytes >= static_cast<int>(sizeof(YouMixState))
        && state.editorWidth >= 320 && state.editorHeight >= 200)
    {
        editorWidth_ = state.editorWidth;
        editorHeight_ = state.editorHeight;
    }
    core.setBypass(state.bypass != 0);
}

void YouMixAudioProcessor::rememberEditorSize(int width, int height) noexcept
{
    if (width >= 320 && height >= 200)
    {
        editorWidth_ = width;
        editorHeight_ = height;
    }
}

void YouMixAudioProcessor::updateTrackProperties(const TrackProperties& properties)
{
    if (slot_ < 0)
        return;
    auto& core = youmix::youMixShared();
    if (properties.name.has_value())
        core.setName(slot_, properties.name->toRawUTF8());
    // 0 means the host did not send a colour. AU context names have no colour.
    if (properties.colourARGB.has_value() && *properties.colourARGB != 0)
        core.setTrackColour(slot_, static_cast<std::uint32_t>(*properties.colourARGB));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new YouMixAudioProcessor();
}
