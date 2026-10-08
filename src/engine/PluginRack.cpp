#include "PluginRack.h"
#include "AppSettings.h"
#include "LatencyCompensation.h"
#include "PluginCatalogue.h"
#include "ui/Theme.h"

#include <algorithm>
#include <tuple>

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

bool configureBuses(juce::AudioPluginInstance& instance, int& processChannels)
{
    auto tryLayout = [&instance](bool wantStereo)
    {
        juce::AudioProcessor::BusesLayout layout;
        const int inputs = instance.getBusCount(true);
        const int outputs = instance.getBusCount(false);
        layout.inputBuses.resize(inputs);
        layout.outputBuses.resize(outputs);
        const auto set = wantStereo ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono();
        for (int bus = 0; bus < inputs; ++bus)
            layout.inputBuses.getReference(bus) = bus == 0 ? set : juce::AudioChannelSet::disabled();
        for (int bus = 0; bus < outputs; ++bus)
            layout.outputBuses.getReference(bus) = bus == 0 ? set : juce::AudioChannelSet::disabled();
        return instance.checkBusesLayoutSupported(layout) && instance.setBusesLayout(layout);
    };

    processChannels = 0;
    if (tryLayout(false) && layoutStaysSafe(instance, processChannels))
        return true;
    if (tryLayout(true) && layoutStaysSafe(instance, processChannels))
        return true;
    processChannels = 0;
    return false;
}

} // namespace

struct PluginRack::LiveGraph
{
    struct Slot
    {
        juce::AudioPluginInstance* instance = nullptr;
        int processChannels = 1;
        bool prepared = false;
    };

    static constexpr int kScratchCap = 8;

    Slot slots[kMaxChannels][kSlotsPerChannel] {};
    std::array<int, kMaxChannels> delayLength {};
    std::array<int, kMaxChannels> delayWrite {};
    std::array<std::vector<float>, kMaxChannels> delay {};
    std::vector<float> scratch;
    std::array<float*, kScratchCap> scratchPtrs {};
    int scratchChannels = 1;
    juce::MidiBuffer midi;
    int maxBlock = 0;
    std::vector<std::shared_ptr<HostedPlugin>> keepAlive;
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
    startTimerHz(5);
}

PluginRack::~PluginRack()
{
    releaseForQuit();
}

void PluginRack::timerCallback()
{
    const bool stateChanged = stateDirty_.exchange(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        reapUnlocked();
        if (latencyDirty_.exchange(false, std::memory_order_relaxed))
            publishUnlocked();
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
    callbackEpoch_.fetch_add(1, std::memory_order_acq_rel);
    auto* graph = published_.load(std::memory_order_acquire);
    inUse_.store(graph, std::memory_order_release);

    const bool blocked = blockProcessing_.load(std::memory_order_acquire);
    if (graph != nullptr && ! blocked && outputs != nullptr && numSamples > 0)
    {
        for (int channel = 0; channel < kMaxChannels; ++channel)
        {
            const int packed = routing.outputPacked[static_cast<std::size_t>(channel)];
            if (packed < 0 || packed >= numOutputs)
                continue;
            float* output = outputs[packed];
            if (output == nullptr)
                continue;

            // Off: the passthrough already wrote silence. Skip the plugins and the
            // delay line so a parked channel does not cost CPU or play stale audio.
            if (! channelIsOn(enabledLow, enabledHigh, channel))
                continue;

            bool anyPlugin = false;
            for (int slot = 0; slot < kSlotsPerChannel; ++slot)
                anyPlugin = anyPlugin || graph->slots[channel][slot].instance != nullptr;

            // No plugin: the passthrough copy stays. That is the dry virtual-soundcheck
            // path, and the delay below still lines it up with the processed channels.
            if (anyPlugin && numSamples <= graph->maxBlock && graph->scratchChannels > 0)
            {
                for (int slot = 0; slot < kSlotsPerChannel; ++slot)
                {
                    const auto& live = graph->slots[channel][slot];
                    auto* instance = live.instance;
                    const int width = live.processChannels;
                    if (instance == nullptr || ! live.prepared || width < 1 || width > graph->scratchChannels)
                        continue;

                    float* pointers[LiveGraph::kScratchCap];
                    for (int index = 0; index < width; ++index)
                    {
                        pointers[index] = graph->scratchPtrs[static_cast<std::size_t>(index)];
                        if (pointers[index] == nullptr)
                            continue;
                        juce::FloatVectorOperations::copy(pointers[index], output, numSamples);
                    }
                    bool pointersReady = true;
                    for (int index = 0; index < width; ++index)
                        pointersReady = pointersReady && pointers[index] != nullptr;
                    if (! pointersReady)
                        continue;

                    juce::AudioBuffer<float> view(pointers, width, numSamples);
                    graph->midi.clear();
                    instance->processBlock(view, graph->midi);
                    juce::FloatVectorOperations::copy(output, pointers[0], numSamples);
                }
            }

            auto& line = graph->delay[static_cast<std::size_t>(channel)];
            const int length = graph->delayLength[static_cast<std::size_t>(channel)];
            if (length > 0 && static_cast<int>(line.size()) >= length)
                delayInPlace(line.data(),
                             length,
                             graph->delayWrite[static_cast<std::size_t>(channel)],
                             output,
                             numSamples);
        }
    }

    inUse_.store(nullptr, std::memory_order_release);
    callbackEpoch_.fetch_add(1, std::memory_order_release);
}

void PluginRack::prepare(double sampleRate, int blockSize, const Routing& routing)
{
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int block = blockSize > 0 ? blockSize : 512;
    blockProcessing_.store(true, std::memory_order_release);
    waitUntilOutsideCallback();

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

    int processChannels = 0;
    juce::String problem = error;
    if (instance != nullptr && ! configureBuses(*instance, processChannels))
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
        instance->prepareToPlay(rate, block);

        hosted = std::make_shared<HostedPlugin>();
        hosted->instance.reset(instance.release(), InstanceDeleter { this });
        hosted->description = std::move(description);
        hosted->state = std::move(state);
        hosted->bypassed = bypassed;
        hosted->processChannels = processChannels;
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

    auto* editor = plugin->instance->createEditorAndMakeActive();
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
    window->clearContentComponent();
    window.reset();
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
    capturing_.store(true, std::memory_order_release);
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
                source.plugin->instance->getStateInformation(source.plugin->state);
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
    reapUnlocked();
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
    graph->midi.ensureSize(1024);

    std::array<ChannelLatencyInput, kMaxChannels> inputs {};
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        int chain = 0;
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
            if (model.plugin == nullptr || model.plugin->instance == nullptr)
                continue;

            model.plugin->latencySamples = clampLatencySamples(model.plugin->instance->getLatencySamples());
            if (model.plugin->bypassed)
                continue;

            chain += model.plugin->latencySamples;
            auto& live = graph->slots[channel][slot];
            live.instance = model.plugin->instance.get();
            live.processChannels = std::max(1, model.plugin->processChannels);
            live.prepared = model.plugin->prepared;
            graph->keepAlive.push_back(model.plugin);
        }

        chainSamples_[static_cast<std::size_t>(channel)] = chain;
        const bool outputOpen = routing_.outputPacked[static_cast<std::size_t>(channel)] >= 0;
        const bool counts = outputOpen && ! excluded_[static_cast<std::size_t>(channel)] && audible_[static_cast<std::size_t>(channel)];
        inputs[static_cast<std::size_t>(channel)] = ChannelLatencyInput { chain, counts };
    }

    const auto plan = planCompensation(inputs.data(), kMaxChannels);
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

    int width = 1;
    for (int channel = 0; channel < kMaxChannels; ++channel)
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
            if (graph->slots[channel][slot].instance != nullptr)
                width = std::max(width, graph->slots[channel][slot].processChannels);
    graph->scratchChannels = std::clamp(width, 1, LiveGraph::kScratchCap);
    graph->scratch.assign(static_cast<std::size_t>(graph->scratchChannels * block), 0.0f);
    for (int index = 0; index < graph->scratchChannels; ++index)
        graph->scratchPtrs[static_cast<std::size_t>(index)] = graph->scratch.data()
                                                              + static_cast<std::size_t>(index * block);

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
    closeAllEditors();

    std::lock_guard<std::mutex> lock(lifeLock_);
    for (auto& row : model_)
    {
        for (auto& slot : row)
        {
            slot.ticket = ++ticketSource_;
            slot.loading = false;
            slot.plugin.reset();
        }
    }
    published_.store(nullptr, std::memory_order_release);
    current_.reset();
    retired_.clear();
}

void PluginRack::waitUntilOutsideCallback()
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        const auto epoch = callbackEpoch_.load(std::memory_order_acquire);
        if ((epoch & 1u) == 0u)
            return;
        juce::Thread::sleep(1);
    }
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
