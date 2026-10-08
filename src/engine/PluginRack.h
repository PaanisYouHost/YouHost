#pragma once

#include "ChannelEnable.h"
#include "Passthrough.h"
#include "SessionDocument.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace youhost
{

class AppSettings;
class PluginCatalogue;

struct SlotSnapshot
{
    bool occupied = false;
    bool bypassed = false;
    bool loading = false;
    int latencySamples = 0;
    juce::String name;
    juce::String error;
};

struct ChannelSnapshot
{
    bool excluded = false;
    int chainSamples = 0;
    int delaySamples = 0;
    std::array<SlotSnapshot, kSlotsPerChannel> slots {};
};

// Message thread sets pause before it calls into a plugin. The audio thread
// sees pause and skips that plugin instead of waiting. depth counts threads
// currently inside processBlock so the message thread can wait until they leave.
struct PluginGate
{
    std::atomic<int> depth { 0 };
    std::atomic<int> pause { 0 };
    std::atomic<std::uint64_t> cpuNs { 0 };
    std::atomic<std::uint32_t> blocks { 0 };
};

struct DspLoadLine
{
    int channel = 0;
    int slot = 0;
    float percent = 0.0f;
    char name[40] {};
};

struct DspLoad
{
    float callbackPercent = 0.0f;
    float pluginPercent = 0.0f;
    int count = 0;
    std::array<DspLoadLine, 8> lines {};
};

// Four in-process slots per channel. The audio thread only reads a published
// graph of raw processors and never takes a lock. A later sandbox can publish
// the same graph shape.
class PluginRack : private juce::Timer,
                   private juce::AudioProcessorListener
{
public:
    PluginRack(PluginCatalogue& catalogue, std::atomic<int>& compensationSamples, AppSettings* settings);
    ~PluginRack() override;

    PluginRack(const PluginRack&) = delete;
    PluginRack& operator=(const PluginRack&) = delete;

    void process(float* const* outputs,
                 int numOutputs,
                 int numSamples,
                 const Routing& routing,
                 std::uint64_t enabledLow,
                 std::uint64_t enabledHigh);
    void prepare(double sampleRate, int blockSize, const Routing& routing, const juce::AudioWorkgroup& workgroup);
    DspLoad dspLoad() const;
    void deviceStopped();
    void updateRouting(const Routing& routing);

    void loadPlugin(int channel,
                    int slot,
                    const juce::PluginDescription& description,
                    const juce::MemoryBlock& state,
                    bool bypassed,
                    bool markDirty,
                    bool openWhenReady = false);
    void clearAll(bool markDirty);
    void removePlugin(int channel, int slot);
    void transferPlugin(int fromChannel, int fromSlot, int toChannel, int toSlot, bool copy);
    void setBypassed(int channel, int slot, bool bypassed);
    void setExcluded(int channel, bool excluded);
    void setAudible(int channel, bool audible);
    void setAudibleAll(const std::array<bool, kMaxChannels>& audible);
    void openEditor(int channel, int slot);
    void toggleEditor(int channel, int slot);
    bool isEditorOpen(int channel, int slot) const;

    ChannelSnapshot snapshot(int channel) const;
    int alignmentSamples() const;
    void captureSession(SessionData& data);
    void restoreSession(const SessionData& data);
    void setDirtyHandler(std::function<void()> handler);
    void setGlobalKeyListener(juce::KeyListener* listener);
    void setBlockedIdentifiers(const juce::StringArray& identifiers);
    void setPluginTrace(std::function<void(int channel, int slot, const juce::String& phase, const juce::String& name, const juce::String& identifier)> trace);
    void releaseForQuit();
    static void destroyInstance(PluginRack* rack, juce::AudioPluginInstance* instance);

private:
    struct EditorWindow;
    struct LiveGraph;
    struct RealtimePool;

    struct HostedPlugin
    {
        std::shared_ptr<juce::AudioPluginInstance> instance;
        std::shared_ptr<PluginGate> gate = std::make_shared<PluginGate>();
        juce::PluginDescription description;
        juce::MemoryBlock state;
        bool bypassed = false;
        int processChannels = 1;
        bool prepared = false;
        int latencySamples = 0;
    };

    struct SlotModel
    {
        std::shared_ptr<HostedPlugin> plugin;
        std::uint64_t ticket = 0;
        bool loading = false;
        juce::String error;
    };

    void timerCallback() override;
    void audioProcessorParameterChanged(juce::AudioProcessor* processor, int parameterIndex, float newValue) override;
    void audioProcessorChanged(juce::AudioProcessor* processor, const juce::AudioProcessorListener::ChangeDetails& details) override;

    void finishLoad(int channel,
                    int slot,
                    std::uint64_t ticket,
                    bool markDirty,
                    bool bypassed,
                    bool openWhenReady,
                    juce::PluginDescription description,
                    juce::MemoryBlock state,
                    std::unique_ptr<juce::AudioPluginInstance> instance,
                    const juce::String& error);
    void publishUnlocked();
    void reapUnlocked();
    std::unique_ptr<LiveGraph> buildGraph();
    void refreshLatency();
    bool pauseGate(PluginGate& gate);
    void resumeGate(PluginGate& gate) noexcept;
    void processJob(int index);
    void processOneChannel(LiveGraph& graph, int channel, float* output, int numSamples);
    void rememberActiveNames();
    bool waitUntilOutsideCallback();
    std::vector<std::shared_ptr<juce::AudioPluginInstance>> collectInstances() const;
    void closeEditor(int channel, int slot);
    void closeAllEditors();
    void notifyDirty();
    bool isBlocked(const juce::PluginDescription& description) const;
    void tracePlugin(int channel, int slot, const juce::String& phase, const juce::String& name, const juce::String& identifier);
    bool validSlot(int channel, int slot) const noexcept;

    PluginCatalogue& catalogue_;
    std::atomic<int>& compensationSamples_;
    AppSettings* settings_ = nullptr;
    std::function<void()> dirtyHandler_;
    juce::KeyListener* commandKeys_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    mutable std::mutex lifeLock_;
    std::array<std::array<SlotModel, kSlotsPerChannel>, kMaxChannels> model_ {};
    std::array<bool, kMaxChannels> excluded_ {};
    std::array<bool, kMaxChannels> audible_ {};
    std::array<int, kMaxChannels> chainSamples_ {};
    std::array<int, kMaxChannels> delaySamples_ {};
    int alignmentSamples_ = 0;
    Routing routing_ {};
    double sampleRate_ = 48000.0;
    int blockSize_ = 512;
    std::uint64_t ticketSource_ = 0;
    bool restoring_ = false;

    std::unique_ptr<LiveGraph> current_;
    std::vector<std::unique_ptr<LiveGraph>> retired_;
    std::atomic<LiveGraph*> published_ { nullptr };
    std::atomic<LiveGraph*> inUse_ { nullptr };
    std::atomic<std::uint32_t> callbackEpoch_ { 0 };
    std::atomic<bool> callbacksRunning_ { false };
    std::atomic<bool> blockProcessing_ { true };
    std::atomic<bool> latencyDirty_ { false };
    std::atomic<bool> stateDirty_ { false };
    std::atomic<bool> capturing_ { false };
    std::atomic<std::uint64_t> lastCallbackNs_ { 0 };

    std::unique_ptr<RealtimePool> pool_;
    juce::AudioWorkgroup workgroup_;
    juce::WorkgroupToken audioToken_;
    bool audioJoined_ = false;
    int jobChannels_[kMaxChannels] {};
    float* jobOutputs_[kMaxChannels] {};
    LiveGraph* jobGraph_ = nullptr;
    int jobSamples_ = 0;
    int editorCloseTries_[kMaxChannels][kSlotsPerChannel] {};

    std::array<std::array<std::unique_ptr<EditorWindow>, kSlotsPerChannel>, kMaxChannels> editors_ {};
    juce::StringArray blocked_;
    std::function<void(int, int, const juce::String&, const juce::String&, const juce::String&)> trace_;
};

} // namespace youhost
