#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/YouMixCore.h"

#include <cstring>
#include <cstdint>

namespace
{

constexpr std::uint32_t kStateMagic = 0x594D5831; // YMX1
constexpr std::uint32_t kStateVersion = 1;

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
};

static_assert(sizeof(YouMixState) == 76, "YouMix session blob must stay fixed-size");

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
        std::memcpy(state.name, settings.name[slot_], sizeof(state.name));
        state.name[31] = 0;
    }
    destData.append(&state, sizeof(state));
}

void YouMixAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < static_cast<int>(sizeof(YouMixState)))
        return;

    YouMixState state;
    std::memcpy(&state, data, sizeof(state));
    if (state.magic != kStateMagic || state.version != kStateVersion)
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
    }
    core.setBypass(state.bypass != 0);
}

void YouMixAudioProcessor::updateTrackProperties(const TrackProperties& properties)
{
    if (slot_ < 0 || ! properties.name.has_value())
        return;
    youmix::youMixShared().setName(slot_, properties.name->toRawUTF8());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new YouMixAudioProcessor();
}
