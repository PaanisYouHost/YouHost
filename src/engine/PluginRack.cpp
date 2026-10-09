#include "PluginRack.h"
#include "AppSettings.h"
#include "LatencyCompensation.h"
#include "PluginCatalogue.h"
#include "SignalPath.h"
#include "StallWatch.h"
#include "ui/Theme.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <tuple>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace youhost
{
namespace
{

struct InstanceDeleter
{
    PluginRack* rack = nullptr;

    void operator()(juce::AudioPluginInstance* instance) const
    {
        PluginRack::destroyInstance(rack, instance);
    }
};

juce::String pluginIdentifier(const juce::PluginDescription& description)
{
    if (description.fileOrIdentifier.isNotEmpty())
        return description.fileOrIdentifier;
    return description.createIdentifierString();
}

bool layoutStaysSafe(const juce::AudioPluginInstance& instance, int& processChannels)
{
    for (int bus = 1; bus < instance.getBusCount(true); ++bus)
        if (instance.getChannelCountOfBus(true, bus) != 0)
            return false;
    for (int bus = 1; bus < instance.getBusCount(false); ++bus)
        if (instance.getChannelCountOfBus(false, bus) != 0)
            return false;

    const int totalIn = instance.getTotalNumInputChannels();
    const int totalOut = instance.getTotalNumOutputChannels();
    if (totalOut < 1)
        return false;
    const int width = std::max(totalIn, totalOut);
    if (width < 1 || width > 2)
        return false;
    processChannels = width;
    return true;
}

struct BusChoice
{
    int inputChannels = 0;
    int outputChannels = 0;
    bool ok = false;
};

// Mono in/out first, then mono in with a stereo out, then stereo in/out.
// A mono channel must not be forced into a stereo input: that duplicates the
// signal, and a plugin which then subtracts or offsets the two sides high-passes it.
BusChoice configureBuses(juce::AudioPluginInstance& instance)
{
    auto tryLayout = [&instance](int inCh, int outCh)
    {
        juce::AudioProcessor::BusesLayout layout;
        const int inputs = instance.getBusCount(true);
        const int outputs = instance.getBusCount(false);
        layout.inputBuses.resize(inputs);
        layout.outputBuses.resize(outputs);
        const auto inSet = inCh > 1 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono();
        const auto outSet = outCh > 1 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono();
        for (int bus = 0; bus < inputs; ++bus)
            layout.inputBuses.getReference(bus) = bus == 0 ? inSet : juce::AudioChannelSet::disabled();
        for (int bus = 0; bus < outputs; ++bus)
            layout.outputBuses.getReference(bus) = bus == 0 ? outSet : juce::AudioChannelSet::disabled();
        if (! instance.checkBusesLayoutSupported(layout) || ! instance.setBusesLayout(layout))
            return false;
        int width = 0;
        if (! layoutStaysSafe(instance, width))
            return false;
        return instance.getTotalNumInputChannels() == inCh
               && instance.getTotalNumOutputChannels() == outCh
               && width == std::max(inCh, outCh);
    };

    if (tryLayout(1, 1))
        return { 1, 1, true };
    if (tryLayout(1, 2))
        return { 1, 2, true };
    if (tryLayout(2, 2))
        return { 2, 2, true };
    return {};
}

} // namespace

struct PluginRack::LiveGraph
{
    struct Slot
    {
        juce::AudioPluginInstance* instance = nullptr;
        PluginGate* gate = nullptr;
        int inputChannels = 1;
        int outputChannels = 1;
        int processChannels = 1;
        bool prepared = false;
    };

    static constexpr int kScratchCap = 2;

    Slot slots[kMaxChannels][kSlotsPerChannel] {};
    std::array<int, kMaxChannels> delayLength {};
    std::array<int, kMaxChannels> delayWrite {};
    std::array<std::vector<float>, kMaxChannels> delay {};
    std::vector<float> scratch;
    std::array<std::array<float*, kScratchCap>, kMaxChannels> scratchPtrs {};
    std::array<juce::MidiBuffer, kMaxChannels> midi {};
    int maxBlock = 0;
    std::vector<std::shared_ptr<HostedPlugin>> keepAlive;
};

namespace
{

std::uint64_t steadyNs() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

int dspWorkerCount()
{
    int cores = juce::SystemStats::getNumPhysicalCpus();
#if defined(__APPLE__)
    int performance = 0;
    auto length = sizeof(performance);
    if (sysctlbyname("hw.perflevel0.physicalcpu", &performance, &length, nullptr, 0) == 0 && performance > 0)
        cores = performance;
#endif
    if (cores < 1)
        cores = 1;
    return std::clamp(cores - 1, 0, 7);
}

} // namespace

struct PluginRack::RealtimePool
{
    class Worker : public juce::Thread
    {
    public:
        Worker(RealtimePool& pool, int index)
            : juce::Thread("youhost-dsp"),
              pool_(pool),
              index_(index)
        {
        }

        void run() override
        {
            pool_.runWorker(index_);
        }

        RealtimePool& pool_;
        int index_ = 0;
    };

    explicit RealtimePool(int workersIn)
    {
        const int workers = std::clamp(workersIn, 0, 7);
        threads_.reserve(static_cast<std::size_t>(workers));
        for (int index = 0; index < workers; ++index)
        {
            auto worker = std::make_unique<Worker>(*this, index);
            const auto options = juce::Thread::RealtimeOptions {}.withApproximateAudioProcessingTime(128, 48000.0);
            if (! worker->startRealtimeThread(options))
                worker->startThread();
            threads_.push_back(std::move(worker));
        }
    }

    ~RealtimePool()
    {
        stop();
    }

    void stop()
    {
        if (stopped_)
            return;
        stopped_ = true;
        stop_.store(true, std::memory_order_release);
        for (auto& wake : wakes_)
            wake.signal();
        for (auto& thread : threads_)
        {
            if (thread != nullptr)
                thread->stopThread(200);
        }
    }

    int workerCount() const noexcept
    {
        return static_cast<int>(threads_.size());
    }

    bool running() const noexcept
    {
        for (const auto& thread : threads_)
            if (thread != nullptr && thread->isThreadRunning())
                return true;
        return false;
    }

    void parallel(int count, PluginRack* rack)
    {
        if (count <= 0 || rack == nullptr)
            return;
        if (count == 1 || threads_.empty() || stop_.load(std::memory_order_acquire))
        {
            for (int index = 0; index < count; ++index)
                rack->processJob(index);
            return;
        }

        const int helpers = std::min(static_cast<int>(threads_.size()), count - 1);
        rack_ = rack;
        count_ = count;
        cursor_.store(0, std::memory_order_relaxed);
        done_.reset();
        left_.store(helpers + 1, std::memory_order_release);
        for (int index = 0; index < helpers; ++index)
            wakes_[static_cast<std::size_t>(index)].signal();

        const auto began = steadyNs();
        for (;;)
        {
            const int index = cursor_.fetch_add(1, std::memory_order_acq_rel);
            if (index >= count)
                break;
            rack->processJob(index);
        }
        rack->addCoreNs(0, steadyNs() - began);

        if (left_.fetch_sub(1, std::memory_order_acq_rel) != 1)
            done_.wait();
    }

    void runWorker(int index)
    {
        juce::WorkgroupToken token;
        while (! stop_.load(std::memory_order_acquire))
        {
            wakes_[static_cast<std::size_t>(index)].wait();
            if (stop_.load(std::memory_order_acquire))
                break;
            auto* rack = rack_;
            if (rack == nullptr)
            {
                if (left_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    done_.signal();
                continue;
            }
            rack->workgroup_.join(token);
            juce::ScopedNoDenormals noDenormals;
            const auto began = steadyNs();
            for (;;)
            {
                const int job = cursor_.fetch_add(1, std::memory_order_acq_rel);
                if (job >= count_)
                    break;
                rack->processJob(job);
            }
            rack->addCoreNs(index + 1, steadyNs() - began);
            if (left_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                done_.signal();
        }
    }

    std::vector<std::unique_ptr<Worker>> threads_;
    std::array<juce::WaitableEvent, 7> wakes_;
    juce::WaitableEvent done_;
    std::atomic<int> cursor_ { 0 };
    std::atomic<int> left_ { 0 };
    std::atomic<bool> stop_ { false };
    int count_ = 0;
    PluginRack* rack_ = nullptr;
    bool stopped_ = false;
};

static juce::String editorWindowKey(const juce::PluginDescription& description)
{
    auto name = description.name.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    if (name.isEmpty())
        name = "Plugin";
    if (name.length() > 48)
        name = name.substring(0, 48);
    return "windowEditor" + name;
}

struct PluginRack::EditorWindow : public juce::DocumentWindow
{
    EditorWindow(const juce::String& title, std::function<void()> onClose, AppSettings* settings, juce::String key)
        : juce::DocumentWindow(title, theme::background, juce::DocumentWindow::closeButton),
          onClose_(std::move(onClose)),
          settings_(settings),
          key_(std::move(key))
    {
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        setResizeLimits(320, 200, 4000, 2400);
    }

    ~EditorWindow() override
    {
        if (settings_ != nullptr && key_.isNotEmpty())
            settings_->saveNamedWindow(key_, getWindowStateAsString());
    }

    void closeButtonPressed() override
    {
        if (onClose_ != nullptr)
            onClose_();
    }

    std::function<void()> onClose_;
    AppSettings* settings_ = nullptr;
    juce::String key_;
};

PluginRack::PluginRack(PluginCatalogue& catalogue, std::atomic<int>& compensationSamples, AppSettings* settings)
    : catalogue_(catalogue),
      compensationSamples_(compensationSamples),
      settings_(settings)
{
    audible_.fill(true);
    groups_.fill(-1);
    pool_ = std::make_unique<RealtimePool>(dspWorkerCount());
    startTimerHz(5);
}

PluginRack::~PluginRack()
{
    releaseForQuit();
}

void PluginRack::timerCallback()
{
    const bool stateChanged = stateDirty_.exchange(false, std::memory_order_relaxed);
    if (latencyDirty_.exchange(false, std::memory_order_relaxed))
        refreshLatency();
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        reapUnlocked();
    }
    if (stateChanged)
        notifyDirty();
}

void PluginRack::audioProcessorParameterChanged(juce::AudioProcessor*, int, float)
{
    if (! capturing_.load(std::memory_order_relaxed))
        stateDirty_.store(true, std::memory_order_relaxed);
}

void PluginRack::audioProcessorChanged(juce::AudioProcessor*, const juce::AudioProcessorListener::ChangeDetails& details)
{
    if (details.latencyChanged)
        latencyDirty_.store(true, std::memory_order_relaxed);
    if (details.nonParameterStateChanged && ! capturing_.load(std::memory_order_relaxed))
        stateDirty_.store(true, std::memory_order_relaxed);
}

void PluginRack::process(float* const* outputs,
                         int numOutputs,
                         int numSamples,
                         const Routing& routing,
                         std::uint64_t enabledLow,
                         std::uint64_t enabledHigh)
{
    StallClock::get().audioPhase.store(kPhasePlugins, std::memory_order_relaxed);
    const auto started = steadyNs();
    callbackEpoch_.fetch_add(1, std::memory_order_acq_rel);
    auto* graph = published_.load(std::memory_order_acquire);
    inUse_.store(graph, std::memory_order_release);

    const bool blocked = blockProcessing_.load(std::memory_order_acquire);
    // Bypass-all is the null test: leave the dry copy alone. No plugin, and no
    // compensation delay, so the output stays the input (plus the device's own delay).
    const bool bypass = bypassAll_.load(std::memory_order_relaxed) != 0;
    if (graph != nullptr && ! blocked && ! bypass && outputs != nullptr && numSamples > 0)
    {
        if (! audioJoined_)
        {
            workgroup_.join(audioToken_);
            audioJoined_ = static_cast<bool>(workgroup_);
        }

        jobGraph_ = graph;
        jobSamples_ = numSamples;
        int count = 0;
        for (int channel = 0; channel < kMaxChannels; ++channel)
        {
            const int packed = routing.outputPacked[static_cast<std::size_t>(channel)];
            if (packed < 0 || packed >= numOutputs)
                continue;
            float* output = outputs[packed];
            if (output == nullptr)
                continue;
            if (! channelIsOn(enabledLow, enabledHigh, channel))
                continue;

            bool anyPlugin = false;
            for (int slot = 0; slot < kSlotsPerChannel; ++slot)
                anyPlugin = anyPlugin || graph->slots[channel][slot].instance != nullptr;
            const bool delayed = graph->delayLength[static_cast<std::size_t>(channel)] > 0;
            if (! anyPlugin && ! delayed)
                continue;

            jobChannels_[count] = channel;
            jobOutputs_[count] = output;
            ++count;
        }

        if (count > 0 && pool_ != nullptr)
            pool_->parallel(count, this);
        else
            for (int index = 0; index < count; ++index)
                processJob(index);
    }

    inUse_.store(nullptr, std::memory_order_release);
    callbackEpoch_.fetch_add(1, std::memory_order_release);
    lastCallbackNs_.store(steadyNs() - started, std::memory_order_relaxed);
    StallClock::get().audioPhase.store(kPhaseAudio, std::memory_order_relaxed);
}

void PluginRack::processJob(int index)
{
    if (jobGraph_ == nullptr || index < 0 || index >= kMaxChannels)
        return;
    processOneChannel(*jobGraph_, jobChannels_[index], jobOutputs_[index], jobSamples_);
}

void PluginRack::processOneChannel(LiveGraph& graph, int channel, float* output, int numSamples)
{
    if (output == nullptr || channel < 0 || channel >= kMaxChannels)
        return;

    const bool canProcess = numSamples > 0 && numSamples <= graph.maxBlock;
    if (canProcess)
    {
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            const auto& live = graph.slots[channel][slot];
            auto* instance = live.instance;
            const int inputs = live.inputChannels;
            const int outputsN = live.outputChannels;
            const int width = std::max(inputs, outputsN);
            if (instance == nullptr || ! live.prepared || width < 1 || width > LiveGraph::kScratchCap)
                continue;

            auto* gate = live.gate;
            if (gate != nullptr && gate->pause.load(std::memory_order_acquire) != 0)
                continue;
            if (gate != nullptr)
                gate->depth.fetch_add(1, std::memory_order_acq_rel);
            if (gate != nullptr && gate->pause.load(std::memory_order_acquire) != 0)
            {
                gate->depth.fetch_sub(1, std::memory_order_release);
                continue;
            }

            float* pointers[LiveGraph::kScratchCap] {};
            if (width == 1)
            {
                pointers[0] = output;
            }
            else
            {
                bool ready = true;
                for (int index = 0; index < width; ++index)
                {
                    pointers[index] = graph.scratchPtrs[static_cast<std::size_t>(channel)][static_cast<std::size_t>(index)];
                    if (pointers[index] == nullptr)
                        ready = false;
                }
                if (! ready)
                {
                    if (gate != nullptr)
                        gate->depth.fetch_sub(1, std::memory_order_release);
                    continue;
                }
                // Stereo-in gets two identical copies (no sample offset).
                // Mono-in/stereo-out gets one copy and a silent second channel.
                // The second output is never added or subtracted afterwards.
                stagePluginChannels(pointers, inputs, outputsN, output, numSamples);
            }

            const auto began = steadyNs();
            juce::AudioBuffer<float> view(pointers, width, numSamples);
            auto& midi = graph.midi[static_cast<std::size_t>(channel)];
            midi.clear();
            instance->processBlock(view, midi);
            if (width != 1)
                takePluginChannel(output, pointers, outputsN, numSamples);
            if (gate != nullptr)
            {
                gate->cpuNs.fetch_add(steadyNs() - began, std::memory_order_relaxed);
                gate->blocks.fetch_add(1, std::memory_order_relaxed);
                gate->depth.fetch_sub(1, std::memory_order_release);
            }
        }
    }

    auto& line = graph.delay[static_cast<std::size_t>(channel)];
    const int length = graph.delayLength[static_cast<std::size_t>(channel)];
    if (length > 0 && static_cast<int>(line.size()) >= length)
        delayInPlace(line.data(),
                     length,
                     graph.delayWrite[static_cast<std::size_t>(channel)],
                     output,
                     numSamples);
}

bool PluginRack::pauseGate(PluginGate& gate)
{
    gate.pause.store(1, std::memory_order_release);
    const auto start = steadyNs();
    while (gate.depth.load(std::memory_order_acquire) > 0)
    {
        if (steadyNs() - start > 50000000ULL)
        {
            gate.pause.store(0, std::memory_order_release);
            return false;
        }
        juce::Thread::yield();
    }
    return true;
}

void PluginRack::resumeGate(PluginGate& gate) noexcept
{
    gate.pause.store(0, std::memory_order_release);
}

void PluginRack::refreshLatency()
{
    PhaseScope phase(kPhasePrepare);
    std::vector<std::shared_ptr<HostedPlugin>> plugins;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        for (auto& row : model_)
            for (auto& slot : row)
                if (slot.plugin != nullptr && slot.plugin->instance != nullptr)
                    plugins.push_back(slot.plugin);
    }

    for (auto& plugin : plugins)
    {
        if (plugin == nullptr || plugin->instance == nullptr || plugin->gate == nullptr)
            continue;
        if (! pauseGate(*plugin->gate))
            continue;
        plugin->latencySamples = clampLatencySamples(plugin->instance->getLatencySamples());
        resumeGate(*plugin->gate);
    }

    std::lock_guard<std::mutex> lock(lifeLock_);
    publishUnlocked();
}

DspLoad PluginRack::dspLoad() const
{
    DspLoad load;
    const double rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
    const int block = blockSize_ > 0 ? blockSize_ : 512;
    const double period = static_cast<double>(block) / rate;
    if (period <= 0.0)
        return load;

    const auto callbackNs = lastCallbackNs_.load(std::memory_order_relaxed);
    load.callbackPercent = static_cast<float>((static_cast<double>(callbackNs) / 1.0e9) / period * 100.0);

    std::lock_guard<std::mutex> lock(lifeLock_);
    float pluginSum = 0.0f;
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            const auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
            if (model.plugin == nullptr || model.plugin->gate == nullptr || model.plugin->instance == nullptr)
                continue;
            auto& gate = *model.plugin->gate;
            const auto ns = gate.cpuNs.exchange(0, std::memory_order_relaxed);
            const auto blocks = gate.blocks.exchange(0, std::memory_order_relaxed);
            if (blocks == 0 || ns == 0)
                continue;
            const float percent = static_cast<float>((static_cast<double>(ns) / 1.0e9)
                                                     / (period * static_cast<double>(blocks)) * 100.0);
            pluginSum += percent;
            int place = load.count;
            if (place >= static_cast<int>(load.lines.size()))
            {
                place = 0;
                for (int index = 1; index < load.count; ++index)
                    if (load.lines[static_cast<std::size_t>(index)].percent < load.lines[static_cast<std::size_t>(place)].percent)
                        place = index;
                if (load.lines[static_cast<std::size_t>(place)].percent >= percent)
                    continue;
            }
            else
            {
                ++load.count;
            }
            auto& line = load.lines[static_cast<std::size_t>(place)];
            line.channel = channel + 1;
            line.slot = slot + 1;
            line.percent = percent;
            line.name[0] = '\0';
            const auto& label = model.plugin->description.name;
            const auto raw = label.toRawUTF8();
            if (raw != nullptr)
            {
                std::size_t length = 0;
                while (raw[length] != '\0' && length + 1 < sizeof(line.name))
                    ++length;
                std::memcpy(line.name, raw, length);
                line.name[length] = '\0';
            }
        }
    }
    load.pluginPercent = pluginSum;
    return load;
}

void PluginRack::addCoreNs(int core, std::uint64_t ns) noexcept
{
    if (core < 0 || core >= static_cast<int>(coreNs_.size()) || ns == 0)
        return;
    coreNs_[static_cast<std::size_t>(core)].fetch_add(ns, std::memory_order_relaxed);
    coreBlocks_[static_cast<std::size_t>(core)].fetch_add(1, std::memory_order_relaxed);
}

CpuMeters PluginRack::cpuMeters() const
{
    CpuMeters meters;
    const double rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
    const int block = blockSize_ > 0 ? blockSize_ : 512;
    const double period = static_cast<double>(block) / rate;
    const auto callbackNs = lastCallbackNs_.load(std::memory_order_relaxed);
    if (period > 0.0)
        meters.callbackPercent = static_cast<float>((static_cast<double>(callbackNs) / 1.0e9) / period * 100.0);
    meters.workers = pool_ != nullptr ? pool_->workerCount() : 0;
    const int cores = std::min(1 + meters.workers, static_cast<int>(meters.percent.size()));
    for (int core = 0; core < cores; ++core)
    {
        auto& nsSlot = coreNs_[static_cast<std::size_t>(core)];
        auto& blockSlot = coreBlocks_[static_cast<std::size_t>(core)];
        const auto ns = nsSlot.exchange(0, std::memory_order_relaxed);
        const auto blocks = blockSlot.exchange(0, std::memory_order_relaxed);
        if (blocks == 0 || period <= 0.0)
            continue;
        meters.percent[static_cast<std::size_t>(core)] = static_cast<float>((static_cast<double>(ns) / 1.0e9)
                                                                            / (period * static_cast<double>(blocks)) * 100.0);
    }
    return meters;
}

void PluginRack::setAlignMode(int perGroup)
{
    const int mode = perGroup == 1 ? 1 : 0;
    std::lock_guard<std::mutex> lock(lifeLock_);
    if (alignGroup_ == mode)
        return;
    alignGroup_ = mode;
    publishUnlocked();
}

void PluginRack::setChannelGroups(const int* groups, int count)
{
    std::lock_guard<std::mutex> lock(lifeLock_);
    bool changed = false;
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        int group = -1;
        if (groups != nullptr && channel < count)
            group = groups[channel];
        if (group < 0)
            group = -1;
        if (groups_[static_cast<std::size_t>(channel)] != group)
        {
            groups_[static_cast<std::size_t>(channel)] = group;
            changed = true;
        }
    }
    if (changed)
        publishUnlocked();
}

void PluginRack::prepare(double sampleRate, int blockSize, const Routing& routing, const juce::AudioWorkgroup& workgroup)
{
    PhaseScope phase(kPhasePrepare);
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int block = blockSize > 0 ? blockSize : 512;
    blockProcessing_.store(true, std::memory_order_release);
    if (! waitUntilOutsideCallback())
    {
        blockProcessing_.store(false, std::memory_order_release);
        return;
    }
    workgroup_ = workgroup;
    audioJoined_ = false;

    std::vector<std::shared_ptr<HostedPlugin>> hosted;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        sampleRate_ = rate;
        blockSize_ = block;
        routing_ = routing;
        callbacksRunning_.store(true, std::memory_order_release);
        for (auto& row : model_)
            for (auto& slot : row)
                if (slot.plugin != nullptr && slot.plugin->instance != nullptr)
                    hosted.push_back(slot.plugin);
    }

    for (auto& plugin : hosted)
        if (plugin != nullptr && plugin->instance != nullptr)
            plugin->instance->prepareToPlay(rate, block);

    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        for (auto& plugin : hosted)
            if (plugin != nullptr)
                plugin->prepared = plugin->instance != nullptr;
        publishUnlocked();
    }
    blockProcessing_.store(false, std::memory_order_release);
}

void PluginRack::deviceStopped()
{
    callbacksRunning_.store(false, std::memory_order_release);
    inUse_.store(nullptr, std::memory_order_release);
}

void PluginRack::updateRouting(const Routing& routing)
{
    std::lock_guard<std::mutex> lock(lifeLock_);
    routing_ = routing;
    publishUnlocked();
}

void PluginRack::loadPlugin(int channel,
                            int slot,
                            const juce::PluginDescription& description,
                            const juce::MemoryBlock& state,
                            bool bypassed,
                            bool markDirty,
                            bool openWhenReady)
{
    if (! validSlot(channel, slot))
        return;

    closeEditor(channel, slot);

    if (isBlocked(description))
    {
        const auto name = description.name.isNotEmpty() ? description.name : juce::String("Plugin");
        {
            std::lock_guard<std::mutex> lock(lifeLock_);
            auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
            model.ticket = ++ticketSource_;
            model.loading = false;
            model.error = name + " may have crashed YouHost. It is turned off.";
            model.plugin.reset();
        }
        return;
    }

    std::uint64_t ticket = 0;
    double rate = 48000.0;
    int block = 512;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        ticket = ++ticketSource_;
        model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)].ticket = ticket;
        model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)].loading = true;
        model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)].error.clear();
        rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
        block = blockSize_ > 0 ? blockSize_ : 512;
    }

    StallClock::get().messagePhase.store(kPhaseLoad, std::memory_order_relaxed);
    tracePlugin(channel, slot, "loading", description.name, pluginIdentifier(description));

    auto alive = alive_;
    catalogue_.createInstanceAsync(
        description,
        rate,
        block,
        [this, alive, channel, slot, ticket, markDirty, bypassed, openWhenReady, description, state](
            std::unique_ptr<juce::AudioPluginInstance> instance,
            const juce::String& error)
        {
            if (! alive->load(std::memory_order_acquire))
                return;
            finishLoad(channel, slot, ticket, markDirty, bypassed, openWhenReady, description, state, std::move(instance), error);
        });
}

void PluginRack::finishLoad(int channel,
                            int slot,
                            std::uint64_t ticket,
                            bool markDirty,
                            bool bypassed,
                            bool openWhenReady,
                            juce::PluginDescription description,
                            juce::MemoryBlock state,
                            std::unique_ptr<juce::AudioPluginInstance> instance,
                            const juce::String& error)
{
    if (! validSlot(channel, slot))
        return;

    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        if (model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)].ticket != ticket)
            return;
    }

    BusChoice buses;
    juce::String problem = error;
    if (instance != nullptr)
        buses = configureBuses(*instance);
    if (instance != nullptr && ! buses.ok)
    {
        if (problem.isEmpty())
            problem = "This plugin needs a sidechain or a channel layout YouHost cannot host safely.";
        instance.reset();
    }

    std::shared_ptr<HostedPlugin> hosted;
    if (instance != nullptr)
    {
        instance->addListener(this);
        if (state.getSize() > 0)
            instance->setStateInformation(state.getData(), static_cast<int>(state.getSize()));

        double rate = 48000.0;
        int block = 512;
        {
            std::lock_guard<std::mutex> lock(lifeLock_);
            rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
            block = blockSize_ > 0 ? blockSize_ : 512;
        }
        {
            PhaseScope preparing(kPhasePrepare);
            instance->prepareToPlay(rate, block);
        }

        hosted = std::make_shared<HostedPlugin>();
        hosted->instance.reset(instance.release(), InstanceDeleter { this });
        hosted->description = std::move(description);
        hosted->state = std::move(state);
        hosted->bypassed = bypassed;
        hosted->inputChannels = buses.inputChannels;
        hosted->outputChannels = buses.outputChannels;
        hosted->processChannels = std::max(buses.inputChannels, buses.outputChannels);
        hosted->prepared = true;
        hosted->latencySamples = clampLatencySamples(hosted->instance->getLatencySamples());
    }
    else if (problem.isEmpty())
    {
        problem = "Could not load the plugin.";
    }

    bool notify = false;
    bool openIt = false;
    bool loaded = false;
    juce::String loadedName = description.name;
    juce::String loadedId = pluginIdentifier(description);
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
        if (model.ticket != ticket)
            return;

        model.loading = false;
        model.error = hosted == nullptr ? problem : juce::String();
        model.plugin = std::move(hosted);
        publishUnlocked();
        notify = markDirty;
        openIt = openWhenReady && model.plugin != nullptr;
        loaded = model.plugin != nullptr;
        if (loaded)
        {
            loadedName = model.plugin->description.name;
            loadedId = pluginIdentifier(model.plugin->description);
        }
    }

    tracePlugin(channel, slot, loaded ? "active" : "unload", loadedName, loadedId);

    if (notify)
        notifyDirty();
    if (openIt)
        openEditor(channel, slot);
}

void PluginRack::clearAll(bool markDirty)
{
    closeAllEditors();

    std::vector<std::tuple<int, int, juce::String, juce::String>> unloaded;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        for (int channel = 0; channel < kMaxChannels; ++channel)
        {
            for (int slot = 0; slot < kSlotsPerChannel; ++slot)
            {
                auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
                if (model.plugin != nullptr)
                    unloaded.emplace_back(channel, slot, model.plugin->description.name, pluginIdentifier(model.plugin->description));
                model.ticket = ++ticketSource_;
                model.loading = false;
                model.error.clear();
                model.plugin.reset();
            }
        }
        publishUnlocked();
    }

    for (const auto& item : unloaded)
        tracePlugin(std::get<0>(item), std::get<1>(item), "unload", std::get<2>(item), std::get<3>(item));

    if (markDirty)
        notifyDirty();
}

void PluginRack::removePlugin(int channel, int slot)
{
    if (! validSlot(channel, slot))
        return;

    closeEditor(channel, slot);
    juce::String name;
    juce::String identifier;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
        if (model.plugin != nullptr)
        {
            name = model.plugin->description.name;
            identifier = pluginIdentifier(model.plugin->description);
        }
        model.ticket = ++ticketSource_;
        model.loading = false;
        model.error.clear();
        model.plugin.reset();
        publishUnlocked();
    }
    if (identifier.isNotEmpty() || name.isNotEmpty())
        tracePlugin(channel, slot, "unload", name, identifier);
    notifyDirty();
}

void PluginRack::transferPlugin(int fromChannel, int fromSlot, int toChannel, int toSlot, bool copy)
{
    if (! validSlot(fromChannel, fromSlot) || ! validSlot(toChannel, toSlot))
        return;
    if (fromChannel == toChannel && fromSlot == toSlot)
        return;

    juce::PluginDescription description;
    juce::MemoryBlock state;
    bool bypassed = false;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        auto& source = model_[static_cast<std::size_t>(fromChannel)][static_cast<std::size_t>(fromSlot)];
        if (source.loading || source.plugin == nullptr || source.plugin->instance == nullptr)
            return;
        source.plugin->instance->getStateInformation(source.plugin->state);
        description = source.plugin->description;
        state = source.plugin->state;
        bypassed = source.plugin->bypassed;
    }

    loadPlugin(toChannel, toSlot, description, state, bypassed, true, false);
    if (! copy)
        removePlugin(fromChannel, fromSlot);
}

void PluginRack::setBypassAll(bool bypass) noexcept
{
    bypassAll_.store(bypass ? 1 : 0, std::memory_order_relaxed);
}

bool PluginRack::bypassAll() const noexcept
{
    return bypassAll_.load(std::memory_order_relaxed) != 0;
}

void PluginRack::setBypassed(int channel, int slot, bool bypassed)
{
    if (! validSlot(channel, slot))
        return;

    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
        if (model.plugin == nullptr)
            return;
        model.plugin->bypassed = bypassed;
        publishUnlocked();
    }
    notifyDirty();
}

void PluginRack::setExcluded(int channel, bool excluded)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;

    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        excluded_[static_cast<std::size_t>(channel)] = excluded;
        publishUnlocked();
    }
    notifyDirty();
}

void PluginRack::setAudible(int channel, bool audible)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;

    std::lock_guard<std::mutex> lock(lifeLock_);
    if (audible_[static_cast<std::size_t>(channel)] == audible)
        return;
    audible_[static_cast<std::size_t>(channel)] = audible;
    publishUnlocked();
}

void PluginRack::setAudibleAll(const std::array<bool, kMaxChannels>& audible)
{
    std::lock_guard<std::mutex> lock(lifeLock_);
    audible_ = audible;
    publishUnlocked();
}

void PluginRack::toggleEditor(int channel, int slot)
{
    if (! validSlot(channel, slot))
        return;
    if (editors_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)] != nullptr)
        closeEditor(channel, slot);
    else
        openEditor(channel, slot);
}

bool PluginRack::isEditorOpen(int channel, int slot) const
{
    if (! validSlot(channel, slot))
        return false;
    return editors_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)] != nullptr;
}

void PluginRack::openEditor(int channel, int slot)
{
    if (! validSlot(channel, slot))
        return;

    std::shared_ptr<HostedPlugin> plugin;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        plugin = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)].plugin;
    }
    if (plugin == nullptr || plugin->instance == nullptr)
        return;

    if (editors_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)] != nullptr)
    {
        editors_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)]->toFront(true);
        return;
    }

    PhaseScope phase(kPhaseEditor);
    if (plugin->gate == nullptr || ! pauseGate(*plugin->gate))
    {
        tracePlugin(channel, slot, "active", plugin->description.name, pluginIdentifier(plugin->description));
        return;
    }

    auto* editor = plugin->instance->createEditorAndMakeActive();
    resumeGate(*plugin->gate);
    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor(*plugin->instance);

    auto alive = alive_;
    const auto key = editorWindowKey(plugin->description);
    auto window = std::make_unique<EditorWindow>(
        plugin->description.name.isNotEmpty() ? plugin->description.name : "Plugin",
        [this, alive, channel, slot]
        {
            juce::MessageManager::callAsync([this, alive, channel, slot]
            {
                if (! alive->load(std::memory_order_acquire))
                    return;
                closeEditor(channel, slot);
            });
        },
        settings_,
        key);
    window->setContentOwned(editor, true);
    const auto stored = settings_ != nullptr ? settings_->loadNamedWindow(key) : juce::String();
    if (stored.isEmpty() || ! window->restoreWindowStateFromString(stored))
        window->centreWithSize(juce::jmax(360, window->getWidth()), juce::jmax(240, window->getHeight()));
    if (commandKeys_ != nullptr)
        window->addKeyListener(commandKeys_);
    window->setVisible(true);
    editors_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)] = std::move(window);
}

void PluginRack::setGlobalKeyListener(juce::KeyListener* listener)
{
    if (commandKeys_ == listener)
        return;

    for (auto& row : editors_)
        for (auto& editor : row)
            if (editor != nullptr && commandKeys_ != nullptr)
                editor->removeKeyListener(commandKeys_);

    commandKeys_ = listener;

    for (auto& row : editors_)
        for (auto& editor : row)
            if (editor != nullptr && commandKeys_ != nullptr)
                editor->addKeyListener(commandKeys_);
}

void PluginRack::closeEditor(int channel, int slot)
{
    if (! validSlot(channel, slot))
        return;
    auto& window = editors_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
    if (window == nullptr)
        return;

    std::shared_ptr<PluginGate> gate;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        const auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
        if (model.plugin != nullptr)
            gate = model.plugin->gate;
    }

    PhaseScope phase(kPhaseCloseEditor);
    if (gate != nullptr && ! pauseGate(*gate))
    {
        auto& tries = editorCloseTries_[channel][slot];
        if (tries < 5)
        {
            ++tries;
            auto alive = alive_;
            juce::MessageManager::callAsync([this, alive, channel, slot]
            {
                if (! alive->load(std::memory_order_acquire))
                    return;
                closeEditor(channel, slot);
            });
        }
        return;
    }

    editorCloseTries_[channel][slot] = 0;
    window->clearContentComponent();
    window.reset();
    if (gate != nullptr)
        resumeGate(*gate);
}

void PluginRack::closeAllEditors()
{
    for (int channel = 0; channel < kMaxChannels; ++channel)
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
            closeEditor(channel, slot);
}

ChannelSnapshot PluginRack::snapshot(int channel) const
{
    ChannelSnapshot snap;
    if (channel < 0 || channel >= kMaxChannels)
        return snap;

    std::lock_guard<std::mutex> lock(lifeLock_);
    snap.excluded = excluded_[static_cast<std::size_t>(channel)];
    snap.chainSamples = chainSamples_[static_cast<std::size_t>(channel)];
    snap.delaySamples = delaySamples_[static_cast<std::size_t>(channel)];
    for (int slot = 0; slot < kSlotsPerChannel; ++slot)
    {
        const auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
        auto& destination = snap.slots[static_cast<std::size_t>(slot)];
        destination.loading = model.loading;
        destination.error = model.error;
        if (model.plugin == nullptr)
            continue;
        destination.occupied = model.plugin->instance != nullptr;
        destination.bypassed = model.plugin->bypassed;
        destination.latencySamples = model.plugin->latencySamples;
        destination.name = model.plugin->description.name;
    }
    return snap;
}

int PluginRack::alignmentSamples() const
{
    std::lock_guard<std::mutex> lock(lifeLock_);
    return alignmentSamples_;
}

void PluginRack::captureSession(SessionData& data)
{
    PhaseScope phase(kPhaseCapture);
    capturing_.store(true, std::memory_order_release);
    std::vector<std::shared_ptr<HostedPlugin>> plugins;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        for (auto& row : model_)
            for (auto& slot : row)
                if (slot.plugin != nullptr && slot.plugin->instance != nullptr)
                    plugins.push_back(slot.plugin);
    }
    for (auto& plugin : plugins)
    {
        if (plugin == nullptr || plugin->instance == nullptr || plugin->gate == nullptr)
            continue;
        if (! pauseGate(*plugin->gate))
            continue;
        plugin->instance->getStateInformation(plugin->state);
        resumeGate(*plugin->gate);
    }
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        for (int channel = 0; channel < kMaxChannels; ++channel)
        {
            auto& destination = data.channels[static_cast<std::size_t>(channel)];
            destination.excludeFromCompensation = excluded_[static_cast<std::size_t>(channel)];
            for (int slot = 0; slot < kSlotsPerChannel; ++slot)
            {
                auto& source = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
                auto& slotOut = destination.slots[static_cast<std::size_t>(slot)];
                slotOut = {};
                if (source.plugin == nullptr || source.plugin->instance == nullptr)
                    continue;
                slotOut.occupied = true;
                slotOut.bypassed = source.plugin->bypassed;
                slotOut.description = source.plugin->description;
                slotOut.state = source.plugin->state;
            }
        }
    }
    capturing_.store(false, std::memory_order_release);
}

void PluginRack::restoreSession(const SessionData& data)
{
    restoring_ = true;
    clearAll(false);
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        const auto& source = data.channels[static_cast<std::size_t>(channel)];
        {
            std::lock_guard<std::mutex> lock(lifeLock_);
            excluded_[static_cast<std::size_t>(channel)] = source.excludeFromCompensation;
        }
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            const auto& sourceSlot = source.slots[static_cast<std::size_t>(slot)];
            if (! sourceSlot.occupied)
                continue;
            loadPlugin(channel, slot, sourceSlot.description, sourceSlot.state, sourceSlot.bypassed, false);
        }
    }
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        publishUnlocked();
    }
    restoring_ = false;
}

void PluginRack::setDirtyHandler(std::function<void()> handler)
{
    dirtyHandler_ = std::move(handler);
}

void PluginRack::publishUnlocked()
{
    auto graph = buildGraph();
    if (current_ != nullptr)
        retired_.push_back(std::move(current_));
    current_ = std::move(graph);
    published_.store(current_.get(), std::memory_order_release);
    rememberActiveNames();
    reapUnlocked();
}

void PluginRack::rememberActiveNames()
{
    char text[160] {};
    std::size_t used = 0;
    int count = 0;
    auto append = [&](const char* chunk)
    {
        if (chunk == nullptr)
            return;
        while (*chunk != '\0' && used + 1 < sizeof(text))
            text[used++] = *chunk++;
    };

    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            const auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
            if (model.plugin == nullptr || model.plugin->instance == nullptr)
                continue;
            if (count > 0)
                append(", ");
            append(model.plugin->description.name.toRawUTF8());
            ++count;
            if (count >= 8)
                break;
        }
        if (count >= 8)
            break;
    }
    text[used] = '\0';
    StallClock::get().activePlugins.store(count, std::memory_order_relaxed);
    StallClock::get().setNames(text);
}

void PluginRack::reapUnlocked()
{
    const auto first = callbackEpoch_.load(std::memory_order_acquire);
    if ((first & 1u) != 0u)
        return;

    auto* published = published_.load(std::memory_order_acquire);
    auto* used = inUse_.load(std::memory_order_acquire);
    const auto second = callbackEpoch_.load(std::memory_order_acquire);
    if (first != second)
        return;

    std::vector<std::unique_ptr<LiveGraph>> keep;
    keep.reserve(retired_.size());
    for (auto& graph : retired_)
    {
        if (graph != nullptr && (graph.get() == published || (used != nullptr && graph.get() == used)))
            keep.push_back(std::move(graph));
    }
    retired_.swap(keep);
}

std::unique_ptr<PluginRack::LiveGraph> PluginRack::buildGraph()
{
    auto graph = std::make_unique<LiveGraph>();
    const int block = blockSize_ > 0 ? blockSize_ : 512;
    graph->maxBlock = block;
    for (auto& buffer : graph->midi)
        buffer.ensureSize(256);

    std::array<ChannelLatencyInput, kMaxChannels> inputs {};
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        int chain = 0;
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
            if (model.plugin == nullptr || model.plugin->instance == nullptr)
                continue;

            if (model.plugin->bypassed)
                continue;

            chain += model.plugin->latencySamples;
            auto& live = graph->slots[channel][slot];
            live.instance = model.plugin->instance.get();
            live.gate = model.plugin->gate.get();
            live.inputChannels = std::clamp(model.plugin->inputChannels, 1, LiveGraph::kScratchCap);
            live.outputChannels = std::clamp(model.plugin->outputChannels, 1, LiveGraph::kScratchCap);
            live.processChannels = std::max(live.inputChannels, live.outputChannels);
            live.prepared = model.plugin->prepared;
            graph->keepAlive.push_back(model.plugin);
        }

        chainSamples_[static_cast<std::size_t>(channel)] = chain;
        const bool outputOpen = routing_.outputPacked[static_cast<std::size_t>(channel)] >= 0;
        const bool counts = outputOpen && ! excluded_[static_cast<std::size_t>(channel)] && audible_[static_cast<std::size_t>(channel)];
        inputs[static_cast<std::size_t>(channel)] = ChannelLatencyInput { chain, counts, groups_[static_cast<std::size_t>(channel)] };
    }

    const auto plan = planCompensation(inputs.data(), kMaxChannels, alignGroup_ == 1 ? AlignMode::group : AlignMode::all);
    alignmentSamples_ = plan.alignmentSamples;
    compensationSamples_.store(plan.alignmentSamples, std::memory_order_relaxed);

    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        const int delay = plan.delaySamples[static_cast<std::size_t>(channel)];
        delaySamples_[static_cast<std::size_t>(channel)] = delay;
        graph->delayLength[static_cast<std::size_t>(channel)] = delay;
        if (delay > 0)
            graph->delay[static_cast<std::size_t>(channel)].assign(static_cast<std::size_t>(delay), 0.0f);
    }

    graph->scratch.assign(static_cast<std::size_t>(kMaxChannels * LiveGraph::kScratchCap * block), 0.0f);
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        graph->midi[static_cast<std::size_t>(channel)].ensureSize(256);
        for (int index = 0; index < LiveGraph::kScratchCap; ++index)
            graph->scratchPtrs[static_cast<std::size_t>(channel)][static_cast<std::size_t>(index)]
                = graph->scratch.data() + static_cast<std::size_t>((channel * LiveGraph::kScratchCap + index) * block);
    }

    return graph;
}

std::vector<std::shared_ptr<juce::AudioPluginInstance>> PluginRack::collectInstances() const
{
    std::vector<std::shared_ptr<juce::AudioPluginInstance>> instances;
    for (const auto& row : model_)
        for (const auto& slot : row)
            if (slot.plugin != nullptr && slot.plugin->instance != nullptr)
                instances.push_back(slot.plugin->instance);
    return instances;
}

void PluginRack::notifyDirty()
{
    if (! restoring_ && dirtyHandler_ != nullptr)
        dirtyHandler_();
}

bool PluginRack::validSlot(int channel, int slot) const noexcept
{
    return channel >= 0 && channel < kMaxChannels && slot >= 0 && slot < kSlotsPerChannel;
}

void PluginRack::setBlockedIdentifiers(const juce::StringArray& identifiers)
{
    blocked_ = identifiers;
}

void PluginRack::setPluginTrace(std::function<void(int, int, const juce::String&, const juce::String&, const juce::String&)> trace)
{
    trace_ = std::move(trace);
}

void PluginRack::releaseForQuit()
{
    alive_->store(false, std::memory_order_release);
    stopTimer();
    blockProcessing_.store(true, std::memory_order_release);
    const bool idle = waitUntilOutsideCallback();
    if (idle)
        closeAllEditors();
    if (pool_ != nullptr)
    {
        pool_->stop();
        if (pool_->running())
            pool_.release();
        else
            pool_.reset();
    }

    std::lock_guard<std::mutex> lock(lifeLock_);
    for (auto& row : model_)
    {
        for (auto& slot : row)
        {
            slot.ticket = ++ticketSource_;
            slot.loading = false;
            if (idle)
                slot.plugin.reset();
        }
    }
    published_.store(nullptr, std::memory_order_release);
    if (idle)
    {
        current_.reset();
        retired_.clear();
    }
    else
    {
        current_.release();
        for (auto& graph : retired_)
            graph.release();
        retired_.clear();
    }
}

bool PluginRack::waitUntilOutsideCallback()
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        const auto epoch = callbackEpoch_.load(std::memory_order_acquire);
        if ((epoch & 1u) == 0u)
            return true;
        juce::Thread::sleep(1);
    }
    return false;
}

bool PluginRack::isBlocked(const juce::PluginDescription& description) const
{
    if (description.fileOrIdentifier.isNotEmpty() && blocked_.contains(description.fileOrIdentifier))
        return true;
    const auto created = description.createIdentifierString();
    return created.isNotEmpty() && blocked_.contains(created);
}

void PluginRack::tracePlugin(int channel, int slot, const juce::String& phase, const juce::String& name, const juce::String& identifier)
{
    if (trace_ != nullptr)
        trace_(channel, slot, phase, name, identifier);
}

void PluginRack::destroyInstance(PluginRack* rack, juce::AudioPluginInstance* instance)
{
    if (instance == nullptr)
        return;

    if (auto* messages = juce::MessageManager::getInstanceWithoutCreating())
    {
        if (! messages->isThisTheMessageThread())
        {
            juce::MessageManager::callAsync([rack, instance]
            {
                destroyInstance(rack, instance);
            });
            return;
        }
    }

    if (rack != nullptr)
        instance->removeListener(rack);
    instance->releaseResources();
    delete instance;
}

} // namespace youhost
