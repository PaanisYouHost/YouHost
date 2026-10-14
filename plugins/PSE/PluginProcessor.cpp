#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cstdint>
#include <cstring>

namespace
{

constexpr std::uint32_t kStateMagic = 0x50534531; // PSE1
constexpr std::uint32_t kStateVersion = 2;
constexpr int kStateBytesV1 = 36;

struct PseState
{
    std::uint32_t magic = kStateMagic;
    std::uint32_t version = kStateVersion;
    std::uint32_t bypass = 0;
    float thresholdDb = -40.0f;
    float rangeDb = 18.0f;
    float attackMs = 12.0f;
    float holdMs = 80.0f;
    float releaseMs = 140.0f;
    float hysteresisDb = 12.0f;
    std::int32_t editorWidth = 0;
    std::int32_t editorHeight = 0;
};

static_assert(sizeof(PseState) == 44, "PSE session blob must stay fixed-size");

} // namespace

PseAudioProcessor::PseAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      core_(&youmix::pseSharedBus())
{
    setLatencySamples(0);
}

void PseAudioProcessor::prepareToPlay(double, int)
{
    setLatencySamples(0);
}

void PseAudioProcessor::releaseResources() {}

bool PseAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& input = layouts.getMainInputChannelSet();
    const auto& output = layouts.getMainOutputChannelSet();
    if (input != output)
        return false;
    return output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo();
}

void PseAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = juce::jmin(2, buffer.getNumChannels());
    const int samples = buffer.getNumSamples();
    if (channels <= 0 || samples <= 0)
        return;

    float* pointers[2] = { nullptr, nullptr };
    for (int channel = 0; channel < channels; ++channel)
        pointers[channel] = buffer.getWritePointer(channel);
    core_.process(pointers, channels, samples, getSampleRate());
}

void PseAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&)
{
    // The dry signal is already in the buffer. Leave it untouched.
}

juce::AudioProcessorEditor* PseAudioProcessor::createEditor()
{
    return new PseAudioProcessorEditor(*this);
}

void PseAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const youmix::PseSettings settings = core_.settings();
    PseState state;
    state.bypass = settings.bypass ? 1u : 0u;
    state.thresholdDb = settings.thresholdDb;
    state.rangeDb = settings.rangeDb;
    state.attackMs = settings.attackMs;
    state.holdMs = settings.hangMs;
    state.releaseMs = settings.releaseMs;
    state.hysteresisDb = settings.kneeDb;
    state.editorWidth = editorWidth_;
    state.editorHeight = editorHeight_;
    destData.append(&state, sizeof(state));
}

void PseAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < kStateBytesV1)
        return;

    PseState state {};
    const int bytes = juce::jmin(sizeInBytes, static_cast<int>(sizeof(state)));
    std::memcpy(&state, data, static_cast<std::size_t>(bytes));
    if (state.magic != kStateMagic || state.version < 1 || state.version > kStateVersion)
        return;

    core_.setThresholdDb(state.thresholdDb);
    core_.setRangeDb(state.rangeDb);
    core_.setAttackMs(state.attackMs);
    core_.setHangMs(state.holdMs);
    core_.setReleaseMs(state.releaseMs);
    core_.setKneeDb(state.hysteresisDb);
    core_.setBypass(state.bypass != 0);
    if (state.version >= 2 && bytes >= static_cast<int>(sizeof(PseState))
        && state.editorWidth >= 320 && state.editorHeight >= 200)
    {
        editorWidth_ = state.editorWidth;
        editorHeight_ = state.editorHeight;
    }
}

void PseAudioProcessor::rememberEditorSize(int width, int height) noexcept
{
    if (width >= 320 && height >= 200)
    {
        editorWidth_ = width;
        editorHeight_ = height;
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PseAudioProcessor();
}
