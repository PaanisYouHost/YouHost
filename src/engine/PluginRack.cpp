#include "PluginRack.h"
#include "AppSettings.h"
#include "CpuMeter.h"
#include "HostLog.h"
#include "PluginBuses.h"
#include "HostPath.h"
#include "LatencyCompensation.h"
#include "PluginCatalogue.h"
#include "StereoLink.h"
#include "PluginMoves.h"
#include "SignalPath.h"
#include "StallWatch.h"
#include "SessionChannels.h"
#include "Vst3Path.h"
#include "CrashJournal.h"
#include "WindowFit.h"
#include "PluginFault.h"
#include "ui/Theme.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
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

// Slate's destructor dispatch_syncs the message thread from inside
// AudioComponentInstanceDispose. Doing that release here never returns.
// The instance is deleted on another thread. This thread does not wait.
void releaseInstanceOffMessageThread(juce::AudioPluginInstance* instance)
{
    if (instance == nullptr)
        return;
    const bool started = juce::Thread::launch([instance]
    {
        delete instance;
    });
    juce::ignoreUnused(started);
}

// Quit deletes on the message thread, before NSApplication terminate.
// A plugin destructor that throws is swallowed here. One that calls
// std::terminate exits without aborting inside _deallocHardCore.
void releasePluginOnMessageThread(juce::AudioPluginInstance* instance)
{
    if (instance == nullptr)
        return;
    const auto previous = std::set_terminate([] { std::_Exit(0); });
    try
    {
        delete instance;
    }
    catch (...)
    {
    }
    std::set_terminate(previous);
}

juce::String pluginIdentifier(const juce::PluginDescription& description)
{
    if (description.fileOrIdentifier.isNotEmpty())
        return description.fileOrIdentifier;
    return description.createIdentifierString();
}

bool countsMatchPlan(const juce::AudioPluginInstance& instance, const LayoutPlan& plan, juce::String& why)
{
    if (instance.getBusCount(true) != plan.inputBusCount || instance.getBusCount(false) != plan.outputBusCount)
    {
        why = "plugin changed its bus count";
        return false;
    }
    for (int bus = 0; bus < plan.inputBusCount; ++bus)
    {
        const int got = instance.getChannelCountOfBus(true, bus);
        if (got != plan.inputBuses[bus])
        {
            why = "input bus " + juce::String(bus) + " came back as " + juce::String(got)
                  + " channels, wanted " + juce::String(plan.inputBuses[bus]);
            return false;
        }
    }
    for (int bus = 0; bus < plan.outputBusCount; ++bus)
    {
        const int got = instance.getChannelCountOfBus(false, bus);
        if (got != plan.outputBuses[bus])
        {
            why = "output bus " + juce::String(bus) + " came back as " + juce::String(got)
                  + " channels, wanted " + juce::String(plan.outputBuses[bus]);
            return false;
        }
    }
    if (instance.getTotalNumInputChannels() != plan.totalIn || instance.getTotalNumOutputChannels() != plan.totalOut)
    {
        why = "totals " + juce::String(instance.getTotalNumInputChannels()) + "/"
              + juce::String(instance.getTotalNumOutputChannels())
              + " wanted " + juce::String(plan.totalIn) + "/" + juce::String(plan.totalOut);
        return false;
    }
    why.clear();
    return true;
}

struct BusChoice
{
    int inputChannels = 0;
    int outputChannels = 0;
    bool ok = false;
    bool supportMono = false;
    bool supportSide = false;
    bool supportStereo = false;
    int openInputs = 0;
    int openOutputs = 0;
    int totalInputs = 0;
    int totalOutputs = 0;
    bool hasExtraBuses = false;
    bool openedStereo = false;
    ExtraBusMode usedExtra = ExtraBusMode::disable;
    ChosenLayout chosen = ChosenLayout::none;
    juce::StringArray attempts;
};

juce::AudioChannelSet channelSetForCount(int channels, const juce::AudioChannelSet& fallback)
{
    if (channels <= 0)
        return juce::AudioChannelSet::disabled();
    if (channels == 1)
        return juce::AudioChannelSet::mono();
    if (channels == 2)
        return juce::AudioChannelSet::stereo();
    return fallback;
}

juce::AudioChannelSet channelSetForBus(int channels, const juce::AudioChannelSet& opened)
{
    // Keep the set the Audio Unit opened with when the width already matches.
    // Replacing it with stereo() or mono() can fail the equality check even
    // though it is the same number of channels.
    if (! opened.isDisabled() && opened.size() == channels)
        return opened;
    return channelSetForCount(channels, opened);
}

// One layout offer. JUCE's AudioUnit check rejects a disabled sidechain and
// also rejects Logic's stereo-main plus mono-sidechain layout. The check is
// logged and is not a veto. setBusesLayout is what the Audio Unit answers.
bool offerLayout(juce::AudioPluginInstance& instance,
                 const LayoutPlan& plan,
                 const juce::AudioProcessor::BusesLayout& original,
                 juce::String& why)
{
    if (! plan.ok)
    {
        why = plan.reason;
        return false;
    }
    // One slow AudioUnitInitialize must not be added to the previous one.
    // The loader thread also calls this. Only the message thread owns the beat.
    if (auto* messages = juce::MessageManager::getInstanceWithoutCreating())
        if (messages->isThisTheMessageThread())
            touchMessageBeat();
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.resize(plan.inputBusCount);
    layout.outputBuses.resize(plan.outputBusCount);
    for (int bus = 0; bus < plan.inputBusCount; ++bus)
    {
        const auto& opened = bus < original.inputBuses.size() ? original.inputBuses.getReference(bus) : juce::AudioChannelSet::disabled();
        layout.inputBuses.getReference(bus) = channelSetForBus(plan.inputBuses[bus], opened);
    }
    for (int bus = 0; bus < plan.outputBusCount; ++bus)
    {
        const auto& opened = bus < original.outputBuses.size() ? original.outputBuses.getReference(bus) : juce::AudioChannelSet::disabled();
        layout.outputBuses.getReference(bus) = channelSetForBus(plan.outputBuses[bus], opened);
    }
    const bool checkAccepted = instance.checkBusesLayoutSupported(layout);
    const bool setAccepted = instance.setBusesLayout(layout);
    if (! setAccepted)
    {
        why = checkAccepted ? juce::String("setBusesLayout refused")
                            : juce::String("checkBusesLayoutSupported refused; setBusesLayout refused");
        return false;
    }
    if (! countsMatchPlan(instance, plan, why))
        return false;
    why = checkAccepted ? juce::String("ok")
                        : juce::String("checkBusesLayoutSupported refused; setBusesLayout accepted");
    return true;
}

const char* layoutNameFor(int mainIn, int mainOut) noexcept
{
    if (mainIn == 1 && mainOut == 1)
        return "1/1";
    if (mainIn == 1 && mainOut == 2)
        return "1/2";
    if (mainIn == 2 && mainOut == 2)
        return "2/2";
    return "other";
}

ChosenLayout chosenFromMain(int mainIn, int mainOut) noexcept
{
    if (mainIn == 1 && mainOut == 1)
        return ChosenLayout::mono;
    if (mainIn == 1 && mainOut == 2)
        return ChosenLayout::side;
    return ChosenLayout::stereo;
}

// The layout the plugin opened with is tried first, and a mono sidechain is
// tried before a disabled one. The first layout the Audio Unit accepts is kept.
BusChoice configureBuses(juce::AudioPluginInstance& instance, bool unlinkedMono)
{
    BusChoice choice;
    choice.openInputs = instance.getTotalNumInputChannels();
    choice.openOutputs = instance.getTotalNumOutputChannels();
    choice.openedStereo = choice.openOutputs >= 2 || choice.openInputs >= 2;
    const int rawInputs = instance.getBusCount(true);
    const int rawOutputs = instance.getBusCount(false);
    choice.hasExtraBuses = rawInputs > 1 || rawOutputs > 1;
    if (rawInputs > kPluginBusCap || rawOutputs > kPluginBusCap)
    {
        choice.attempts.add("attempt none extra=disable supported=no reason=more buses than YouHost can host");
        choice.attempts.add("chose none");
        return choice;
    }

    const auto remember = instance.getBusesLayout();
    int inputDefaults[kPluginBusCap] {};
    int outputDefaults[kPluginBusCap] {};
    const int inputBusCount = std::min(instance.getBusCount(true), kPluginBusCap);
    const int outputBusCount = std::min(instance.getBusCount(false), kPluginBusCap);
    for (int bus = 0; bus < inputBusCount; ++bus)
        inputDefaults[bus] = instance.getChannelCountOfBus(true, bus);
    for (int bus = 0; bus < outputBusCount; ++bus)
        outputDefaults[bus] = instance.getChannelCountOfBus(false, bus);

    int wantIn[3] {};
    int wantOut[3] {};
    const ChosenLayout preferred = layoutForChannel(choice.openedStereo ? ChosenLayout::stereo : ChosenLayout::mono,
                                                    unlinkedMono,
                                                    true);
    const bool vst3 = instance.getPluginDescription().pluginFormatName == "VST3";
    const bool monoFirst = preferred == ChosenLayout::mono
                           && ! sidechainKeepsStereoMain(choice.hasExtraBuses, choice.openedStereo)
                           && ! vst3MonoKeepsOpenedStereo(vst3, choice.openedStereo);
    int layoutCount = 3;
    if (vst3MonoKeepsOpenedStereo(vst3, choice.openedStereo))
    {
        wantIn[0] = 2;
        wantOut[0] = 2;
        layoutCount = 1;
    }
    else
    {
        mainLayoutAttemptOrder(monoFirst, wantIn, wantOut);
    }
    ExtraBusMode modes[5] {};
    const int modeCount = copyExtraBusAttemptOrder(choice.hasExtraBuses, modes, 5);
    // True while the plugin still has the layout it opened with. A planner
    // rejection leaves it there. setBusesLayout(remember) is only needed after
    // a real offer changed it.
    bool pluginLeftOpened = true;

    for (int layout = 0; layout < layoutCount; ++layout)
    {
        for (int modeIndex = 0; modeIndex < modeCount; ++modeIndex)
        {
            const auto mode = modes[modeIndex];
            const auto plan = planPluginLayout(inputDefaults, inputBusCount, outputDefaults, outputBusCount,
                                               wantIn[layout], wantOut[layout], mode);
            if (plan.ok)
                pluginLeftOpened = false;
            juce::String why;
            const bool ok = offerLayout(instance, plan, remember, why);
            juce::String line;
            line << "attempt " << layoutNameFor(wantIn[layout], wantOut[layout])
                 << " extra=" << extraBusModeName(mode)
                 << " supported=" << (ok ? "yes" : "no")
                 << " reason=" << (why.isNotEmpty() ? why : juce::String("none"));
            if (ok)
                line << " total " << plan.totalIn << "/" << plan.totalOut;
            choice.attempts.add(line);
            if (! ok)
            {
                // The planner can reject "opened" before the plugin is touched.
                // Restoring that layout would be a second setBusesLayout.
                if (failedLayoutNeedsRestore(plan.ok))
                {
                    instance.setBusesLayout(remember);
                    pluginLeftOpened = true;
                }
                continue;
            }

            if (wantIn[layout] == 1 && wantOut[layout] == 1)
                choice.supportMono = true;
            else if (wantIn[layout] == 1 && wantOut[layout] == 2)
                choice.supportSide = true;
            else if (wantIn[layout] == 2 && wantOut[layout] == 2)
                choice.supportStereo = true;
            choice.inputChannels = plan.mainIn;
            choice.outputChannels = plan.mainOut;
            choice.totalInputs = plan.totalIn;
            choice.totalOutputs = plan.totalOut;
            choice.usedExtra = mode;
            choice.chosen = chosenFromMain(plan.mainIn, plan.mainOut);
            choice.ok = true;
            choice.attempts.add("chose " + juce::String(layoutNameFor(plan.mainIn, plan.mainOut))
                                + " extra=" + juce::String(extraBusModeName(mode))
                                + " main " + juce::String(choice.inputChannels) + "/" + juce::String(choice.outputChannels)
                                + " total " + juce::String(choice.totalInputs) + "/" + juce::String(choice.totalOutputs));
            return choice;
        }
    }

    if (! pluginLeftOpened)
        instance.setBusesLayout(remember);
    int liveIn[kPluginBusCap] {};
    int liveOut[kPluginBusCap] {};
    const int liveInputBuses = std::min(instance.getBusCount(true), kPluginBusCap);
    const int liveOutputBuses = std::min(instance.getBusCount(false), kPluginBusCap);
    for (int bus = 0; bus < liveInputBuses; ++bus)
        liveIn[bus] = instance.getChannelCountOfBus(true, bus);
    for (int bus = 0; bus < liveOutputBuses; ++bus)
        liveOut[bus] = instance.getChannelCountOfBus(false, bus);
    const auto kept = keepOpenedLayout(liveIn, liveInputBuses, liveOut, liveOutputBuses);
    if (kept.ok)
    {
        choice.inputChannels = kept.mainIn;
        choice.outputChannels = kept.mainOut;
        choice.totalInputs = kept.totalIn;
        choice.totalOutputs = kept.totalOut;
        choice.usedExtra = ExtraBusMode::opened;
        choice.chosen = chosenFromMain(kept.mainIn, kept.mainOut);
        if (kept.mainIn == 1 && kept.mainOut == 1)
            choice.supportMono = true;
        else if (kept.mainIn == 1 && kept.mainOut == 2)
            choice.supportSide = true;
        else if (kept.mainIn == 2 && kept.mainOut == 2)
            choice.supportStereo = true;
        choice.ok = true;
        choice.attempts.add("kept opened layout main " + juce::String(kept.mainIn) + "/" + juce::String(kept.mainOut)
                            + " total " + juce::String(kept.totalIn) + "/" + juce::String(kept.totalOut)
                            + " reason=setBusesLayout did not accept another layout");
        return choice;
    }

    choice.attempts.add("chose none");
    return choice;
}

void logHostedPlugin(AppSettings* settings,
                     const juce::PluginDescription& description,
                     const BusChoice& buses,
                     StereoFold fold,
                     double rate,
                     int block,
                     int latencySamples)
{
    if (settings == nullptr || block <= 0)
        return;

    juce::String line;
    line << "plugin " << description.name
         << " id " << (description.fileOrIdentifier.isNotEmpty() ? description.fileOrIdentifier : description.createIdentifierString())
         << " opened " << buses.openInputs << "/" << buses.openOutputs
         << " supports 1/1=" << (buses.supportMono ? "yes" : "no")
         << " 1/2=" << (buses.supportSide ? "yes" : "no")
         << " 2/2=" << (buses.supportStereo ? "yes" : "no")
         << " chose " << chosenLayoutName(buses.chosen)
         << " extra=" << extraBusModeName(buses.usedExtra)
         << " main " << buses.inputChannels << "/" << buses.outputChannels
         << " total " << buses.totalInputs << "/" << buses.totalOutputs
         << " fold " << stereoFoldToken(fold)
         << " rate " << juce::String(rate, 0)
         << " block " << block
         << " latency " << latencySamples
         << " audio-thread process, channels " << buses.totalInputs << "/" << buses.totalOutputs;
    appendHostLog(*settings, line);
}

} // namespace

struct PluginRack::MessageThreadJob
{
    std::mutex mutex;
    std::function<void()> fn;
    std::atomic<bool> started { false };
    std::atomic<bool> done { false };
};

void PluginRack::completeMessageJob(const std::shared_ptr<MessageThreadJob>& job)
{
    if (job == nullptr)
        return;
    if (job->started.exchange(true, std::memory_order_acq_rel))
        return;
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        fn = std::move(job->fn);
    }
    if (fn)
        fn();
    job->done.store(true, std::memory_order_release);
}

bool PluginRack::runOnMessageThread(const std::function<void()>& fn)
{
    // VST3 setupProcessing, setActive, and setState are illegal off the message
    // thread. callAsync runs this on the message thread and the loader waits.
    // It does not render the plugin. Quit runs the same job inline, because
    // modal loops are disabled and cannot pump the queue.
    if (! fn)
        return false;
    auto* messages = juce::MessageManager::getInstanceWithoutCreating();
    if (messages == nullptr || messages->isThisTheMessageThread())
    {
        fn();
        return true;
    }

    auto job = std::make_shared<MessageThreadJob>();
    job->fn = fn;
    {
        std::lock_guard<std::mutex> lock(offThreadMutex_);
        messageJob_ = job;
    }
    const bool posted = juce::MessageManager::callAsync([job]
    {
        PluginRack::completeMessageJob(job);
    });
    if (! posted)
    {
        std::lock_guard<std::mutex> lock(offThreadMutex_);
        if (messageJob_ == job)
            messageJob_.reset();
        return false;
    }
    while (! job->done.load(std::memory_order_acquire))
        juce::Thread::sleep(2);
    {
        std::lock_guard<std::mutex> lock(offThreadMutex_);
        if (messageJob_ == job)
            messageJob_.reset();
    }
    return true;
}

struct PluginRack::LiveGraph
{
    struct Slot
    {
        juce::AudioPluginInstance* instance = nullptr;
        PluginGate* gate = nullptr;
        int inputChannels = 1;
        int outputChannels = 1;
        int totalInputs = 1;
        int totalOutputs = 1;
        int processChannels = 1;
        int stereoFold = 0;
        bool prepared = false;
        bool vst3 = false;
        char name[40] {};
    };

    // Two channels for a normal insert. Eight leaves room for a sidechain
    // or aux bus. Those extra inputs are silence. The audio thread only
    // touches the channels that plugin uses.
    static constexpr int kScratchCap = 8;

    Slot slots[kMaxChannels][kSlotsPerChannel] {};
    std::array<int, kMaxChannels> delayLength {};
    std::array<int, kMaxChannels> delayWrite {};
    std::array<std::vector<float>, kMaxChannels> delay {};
    std::vector<float> scratch;
    // One pair per slot. Slots must not share a right channel: the previous
    // plugin's output would be the next plugin's input.
    std::array<std::array<std::array<float*, kScratchCap>, kSlotsPerChannel>, kMaxChannels> scratchPtrs {};
    std::array<juce::MidiBuffer, kMaxChannels> midi {};
    int maxBlock = 0;
    int feedCapacity = 0;
    std::vector<float> feedIn;
    std::vector<float> feedOut;
    std::vector<float> blockWork;
    std::array<int, kMaxChannels> inCount {};
    std::array<int, kMaxChannels> inRead {};
    std::array<int, kMaxChannels> inWrite {};
    std::array<int, kMaxChannels> outCount {};
    std::array<int, kMaxChannels> outRead {};
    std::array<int, kMaxChannels> outWrite {};
    std::vector<std::shared_ptr<HostedPlugin>> keepAlive;
};

namespace
{

std::uint64_t steadyNs() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void copyPluginName(char* dest, int cap, const char* source) noexcept
{
    if (dest == nullptr || cap < 2)
        return;
    dest[0] = '\0';
    if (source == nullptr)
        return;
    int index = 0;
    for (; index < cap - 1 && source[index] != '\0'; ++index)
        dest[index] = source[index];
    dest[index] = '\0';
}

void copyPluginName(char* dest, int cap, const juce::String& name) noexcept
{
    copyPluginName(dest, cap, name.toRawUTF8());
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

    explicit RealtimePool(int workersIn, double sampleRateIn, int blockSize)
        : rate_(sampleRateIn),
          block_(blockSize)
    {
        const int workers = std::clamp(workersIn, 0, 7);
        threads_.reserve(static_cast<std::size_t>(workers));
        for (int index = 0; index < workers; ++index)
        {
            auto worker = std::make_unique<Worker>(*this, index);
            // The period is one device buffer, the same deadline Ardour gives
            // its process thread. A fixed 128-sample budget leaves the period
            // at 0, and macOS then rejects the time-constraint policy.
            const int samplesPerFrame = block_ > 0 ? block_ : 32;
            const double sampleRate = rate_ > 0.0 ? rate_ : 48000.0;
            const double periodMs = (static_cast<double>(samplesPerFrame) / sampleRate) * 1000.0;
            const auto options = juce::Thread::RealtimeOptions {}
                                     .withPeriodMs(periodMs)
                                     .withProcessingTimeMs(periodMs)
                                     .withApproximateAudioProcessingTime(samplesPerFrame, sampleRate);
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
            lastWaitNs_ = 0;
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

        const auto waitBegan = steadyNs();
        if (left_.fetch_sub(1, std::memory_order_acq_rel) != 1)
            done_.wait();
        lastWaitNs_ = static_cast<int>(steadyNs() - waitBegan);
    }

    int lastWaitNs() const noexcept
    {
        return lastWaitNs_;
    }

    bool sameQuantum(double sampleRate, int blockSize) const noexcept
    {
        return block_ == blockSize && rate_ > sampleRate - 1.0 && rate_ < sampleRate + 1.0;
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
            noteAudioThread();
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
    int lastWaitNs_ = 0;
    PluginRack* rack_ = nullptr;
    bool stopped_ = false;
    double rate_ = 48000.0;
    int block_ = 32;
};

static juce::String editorWindowKey(const juce::PluginDescription& description)
{
    auto keep = [](juce::String text)
    {
        text = text.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
        if (text.length() > 24)
            text = text.substring(0, 24);
        return text;
    };
    auto format = keep(description.pluginFormatName);
    if (format.isEmpty())
        format = "Plugin";
    auto name = keep(description.name);
    if (name.isEmpty())
        name = "Plugin";
    return "windowEditor" + format + name;
}

juce::String pluginLogLine(const char* phase,
                           int channel,
                           int slot,
                           const juce::String& format,
                           const juce::String& name,
                           const juce::String& identifier,
                           const juce::String& reason)
{
    const auto line = formatPluginAttemptLine(phase,
                                              channel,
                                              slot,
                                              format.toRawUTF8(),
                                              name.toRawUTF8(),
                                              identifier.toRawUTF8(),
                                              reason.isNotEmpty() ? reason.toRawUTF8() : nullptr);
    return juce::String::fromUTF8(line.c_str());
}

struct PluginRack::EditorWindow : public juce::DocumentWindow, private juce::ComponentListener
{
    EditorWindow(const juce::String& title, std::function<void(int, int)> onClose, AppSettings* settings, juce::String key)
        : juce::DocumentWindow(title, theme::background, juce::DocumentWindow::closeButton),
          onClose_(std::move(onClose)),
          settings_(settings),
          key_(std::move(key))
    {
        setUsingNativeTitleBar(true);
    }

    ~EditorWindow() override
    {
        if (auto* content = getContentComponent())
            content->removeComponentListener(this);
        if (settings_ != nullptr && key_.isNotEmpty())
            settings_->saveNamedWindow(key_, getWindowStateAsString());
    }

    // HiDPI stays on the plugin. A second scale on this window crops it.
    float getDesktopScaleFactor() const override
    {
        return editorHostScale(1.0f);
    }

    void place(int channel, int slot) noexcept
    {
        channel_ = channel;
        slot_ = slot;
    }

    void watchContent()
    {
        if (auto* content = getContentComponent())
        {
            content->addComponentListener(this);
            const int borderW = getWidth() - content->getWidth();
            const int borderH = getHeight() - content->getHeight();
            if (borderW >= 0)
                frameW_ = borderW;
            if (borderH >= 0)
                frameH_ = borderH;
        }
    }

    // An Audio Unit chooses its own size. setResizeLimits asks the view to
    // change frame, which Melodyne rejects by crashing.
    void usePluginSize() noexcept
    {
        usePluginSize_ = true;
    }

    void quiesce()
    {
        if (auto* content = getContentComponent())
        {
            if (auto* timed = dynamic_cast<juce::Timer*>(content))
                timed->stopTimer();
            content->setVisible(false);
        }
    }

    void closeButtonPressed() override
    {
        if (onClose_ != nullptr)
            onClose_(channel_, slot_);
    }

    void componentMovedOrResized(juce::Component& component, bool, bool wasResized) override
    {
        // VST3 and Audio Unit views on the Mac are both in points. The window
        // follows that view. It does not lock a later size that JUCE produced
        // by dividing the VST3 view by the Retina backing scale. The hosted
        // NSView stays 1:1 with this window.
        if (! wasResized || fitting_ || &component != getContentComponent())
            return;
        const int contentW = component.getWidth();
        const int contentH = component.getHeight();
        if (contentW < 1 || contentH < 1)
            return;
        const int targetW = contentW + frameW_;
        const int targetH = contentH + frameH_;
        if (targetW == getWidth() && targetH == getHeight())
            return;
        fitting_ = true;
        if (usePluginSize_)
        {
            setSize(targetW, targetH);
            fitting_ = false;
            return;
        }
        if (! isResizable())
            setResizeLimits(targetW, targetH, targetW, targetH);
        else if (auto* limits = getConstrainer())
            setResizeLimits(std::min(limits->getMinimumWidth(), targetW),
                            std::min(limits->getMinimumHeight(), targetH),
                            std::max(limits->getMaximumWidth(), targetW),
                            std::max(limits->getMaximumHeight(), targetH));
        setSize(targetW, targetH);
        fitting_ = false;
    }

    std::function<void(int, int)> onClose_;
    AppSettings* settings_ = nullptr;
    juce::String key_;
    int channel_ = -1;
    int slot_ = -1;
    int frameW_ = 0;
    int frameH_ = 0;
    bool fitting_ = false;
    bool usePluginSize_ = false;
};

struct PluginRack::DeferredPluginRelease
{
    struct Item
    {
        std::shared_ptr<HostedPlugin> plugin;
        std::unique_ptr<EditorWindow> window;
        int hops = pluginGraveHops();
    };

    std::vector<Item> items;
};

class PluginRack::BackgroundInstantiate : public juce::Thread
{
public:
    explicit BackgroundInstantiate(PluginRack& owner)
        : juce::Thread("youhost-plugin-load"),
          owner_(owner)
    {
    }

    void run() override
    {
        std::unique_ptr<juce::AudioPluginInstance> instance;
        juce::String error;
        const bool vst3 = owner_.jobDescription_.pluginFormatName == "VST3";
        // An Audio Unit is created on the loader thread. A VST3 is created
        // on the message thread, in the same turn as setActive.
        if (! vst3)
            owner_.catalogue_.createInstanceBlocking(owner_.jobDescription_, owner_.jobRate_, owner_.jobBlock_, instance, error);

        // VST3 setupProcessing is on the message thread. The loader waits.
        // The audio callback is the first render, with the same channel count
        // and block size. No lock is held across the plugin calls. The message
        // thread publishes the instance only after this returns.
        PluginRack::OffThreadBuses setup;
        const int left = owner_.jobChannel_ & ~1;
        const bool unlinkedMono = ! owner_.stereoLink(left);
        auto* plugin = instance.get();
        BusChoice measured;
        auto setupPlugin = [&, plugin, unlinkedMono]()
        {
            // The captured pointer is the loader-thread instance. A VST3 is
            // filled in on this same turn, after this lambda was built.
            auto* live = plugin != nullptr ? plugin : instance.get();
            if (live == nullptr)
                return;
            const auto buses = configureBuses(*live, unlinkedMono);
            measured = buses;
            setup.ready = true;
            setup.ok = buses.ok;
            setup.inputChannels = buses.inputChannels;
            setup.outputChannels = buses.outputChannels;
            setup.totalInputs = buses.totalInputs;
            setup.totalOutputs = buses.totalOutputs;
            setup.openInputs = buses.openInputs;
            setup.openOutputs = buses.openOutputs;
            setup.supportMono = buses.supportMono;
            setup.supportSide = buses.supportSide;
            setup.supportStereo = buses.supportStereo;
            setup.openedStereo = buses.openedStereo;
            setup.hasExtraBuses = buses.hasExtraBuses;
            setup.attempts = buses.attempts;
            if (! buses.ok)
                return;
            if (owner_.jobState_.getSize() > 0)
                live->setStateInformation(owner_.jobState_.getData(), static_cast<int>(owner_.jobState_.getSize()));
            int liveIn = live->getTotalNumInputChannels();
            int liveOut = live->getTotalNumOutputChannels();
            // A saved state can reopen the stereo bus after the mono layout
            // was accepted. Negotiating again keeps that width off the audio callback.
            if (stateRestoredDifferentBuses(measured.totalInputs, measured.totalOutputs, liveIn, liveOut))
            {
                const auto again = configureBuses(*live, unlinkedMono);
                setup.attempts.add("state changed buses, negotiated again");
                setup.attempts.addArray(again.attempts);
                if (again.ok)
                {
                    measured = again;
                    setup.ok = true;
                    setup.inputChannels = again.inputChannels;
                    setup.outputChannels = again.outputChannels;
                    setup.totalInputs = again.totalInputs;
                    setup.totalOutputs = again.totalOutputs;
                    setup.supportMono = again.supportMono;
                    setup.supportSide = again.supportSide;
                    setup.supportStereo = again.supportStereo;
                    setup.openedStereo = again.openedStereo;
                    setup.hasExtraBuses = again.hasExtraBuses;
                    liveIn = again.totalInputs;
                    liveOut = again.totalOutputs;
                }
            }
            const int mainIn = live->getMainBusNumInputChannels();
            const int mainOut = live->getMainBusNumOutputChannels();
            if (liveIn > 0)
                measured.totalInputs = liveIn;
            if (liveOut > 0)
                measured.totalOutputs = liveOut;
            if (mainIn > 0)
                measured.inputChannels = mainIn;
            if (mainOut > 0)
                measured.outputChannels = mainOut;
            setup.inputChannels = measured.inputChannels;
            setup.outputChannels = measured.outputChannels;
            setup.totalInputs = measured.totalInputs;
            setup.totalOutputs = measured.totalOutputs;
            const int width = pluginProcessWidth(measured.totalInputs, measured.totalOutputs);
            if (width < 1)
            {
                setup.ok = false;
                if (owner_.settings_ != nullptr)
                    appendHostLog(*owner_.settings_,
                                  pluginLogLine("did-not-load",
                                                owner_.jobChannel_,
                                                owner_.jobSlot_,
                                                owner_.jobDescription_.pluginFormatName,
                                                owner_.jobDescription_.name,
                                                pluginIdentifier(owner_.jobDescription_),
                                                "layout " + juce::String(measured.totalInputs) + "/"
                                                    + juce::String(measured.totalOutputs)
                                                    + " is wider than the process buffer, not activated"));
                return;
            }
            live->setNonRealtime(false);
            // The device workgroup is part of prepare, then again on the
            // audio thread before process. AudioPluginHost does the same.
            if (static_cast<bool>(owner_.workgroup_))
                live->audioWorkgroupContextChanged(owner_.workgroup_);
            live->prepareToPlay(owner_.jobRate_, owner_.jobBlock_);
            setup.latencySamples = live->getLatencySamples();
        };
        bool preparedOnMessage = false;
        if (vst3)
        {
            // VST3 setupProcessing is on the message thread.
            preparedOnMessage = owner_.runOnMessageThread([&]()
            {
                // Factory, initialize, setupProcessing, setActive, and
                // prepareToPlay are this one turn. A CFRunLoop timer the
                // plugin arms during initialize cannot run until this
                // returns, so it cannot fire into a component that is
                // not active yet.
                owner_.catalogue_.createInstanceBlocking(owner_.jobDescription_,
                                                         owner_.jobRate_,
                                                         owner_.jobBlock_,
                                                         instance,
                                                         error,
                                                         true);
                if (instance != nullptr)
                    setupPlugin();
            });
        }
        else if (instance != nullptr)
        {
            // An Audio Unit is prepared on the loader thread. Doing it on
            // the message thread holds that thread, and the audio callback
            // waits inside the Audio Units already running. The message
            // thread stays in the runloop. blockProcessing_ stays false,
            // so the callback does not pass the dry input.
            setupPlugin();
            preparedOnMessage = true;
        }
        if (vst3 || instance != nullptr)
        {
            if (! preparedOnMessage)
            {
                setup.ready = true;
                setup.ok = false;
                if (owner_.settings_ != nullptr)
                    appendHostLog(*owner_.settings_,
                                  pluginLogLine("did-not-load",
                                                owner_.jobChannel_,
                                                owner_.jobSlot_,
                                                owner_.jobDescription_.pluginFormatName,
                                                owner_.jobDescription_.name,
                                                pluginIdentifier(owner_.jobDescription_),
                                                "prepare did not run on the message thread, not activated"));
            }
            else if (setup.ok)
            {
                logHostedPlugin(owner_.settings_, owner_.jobDescription_, measured,
                                stereoFoldFromInt(owner_.jobStereo_), owner_.jobRate_, owner_.jobBlock_,
                                setup.latencySamples);
            }
        }

        {
            std::lock_guard<std::mutex> lock(owner_.offThreadMutex_);
            owner_.offThreadInstance_ = std::move(instance);
            owner_.offThreadError_ = error;
            owner_.offThreadBuses_ = std::move(setup);
        }
        auto alive = owner_.alive_;
        auto* rack = &owner_;
        juce::MessageManager::callAsync([alive, rack]
        {
            if (! alive->load(std::memory_order_acquire))
                return;
            rack->deliverOffThreadInstance();
        });
    }

private:
    PluginRack& owner_;
};

PluginRack::PluginRack(PluginCatalogue& catalogue, std::atomic<int>& compensationSamples, AppSettings* settings)
    : catalogue_(catalogue),
      compensationSamples_(compensationSamples),
      settings_(settings)
{
    audible_.fill(true);
    groups_.fill(-1);
    deferred_ = std::make_unique<DeferredPluginRelease>();
    pool_ = std::make_unique<RealtimePool>(dspWorkerCount(), 48000.0, 32);
    startTimerHz(5);
}

PluginRack::~PluginRack()
{
    releaseForQuit();
}

void PluginRack::timerCallback()
{
    if (const int fault = takePluginFault())
    {
        const bool messageThread = fault == kPluginFaultMessageThread;
        juce::String line = messageThread ? "plugin crashed on the message thread"
                                          : "plugin window crashed on its own thread; editor closed, audio kept";
        if (faultName_.isNotEmpty())
            line << ": " << faultName_ << " ch " << (faultChannel_ + 1) << " slot " << (faultSlot_ + 1);
        if (settings_ != nullptr)
            appendHostLog(*settings_, line);
        closeAllEditors();
        if (messageThread && validSlot(faultChannel_, faultSlot_))
            removePlugin(faultChannel_, faultSlot_);
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Plugin",
                                               messageThread
                                                   ? "A plugin crashed. It was removed. Audio keeps running."
                                                   : "A plugin window crashed. The plugin stays in the audio path.");
    }
    const bool stateChanged = stateDirty_.exchange(false, std::memory_order_relaxed);
    if (latencyDirty_.exchange(false, std::memory_order_relaxed))
        refreshLatency();
    const int wantBlock = reprepareBlock_.exchange(0, std::memory_order_relaxed);
    if (wantBlock > 0 && wantBlock != blockSize_ && wantBlock <= 8192)
    {
        if (settings_ != nullptr)
            appendHostLog(*settings_, "callback " + juce::String(wantBlock) + " samples, prepared " + juce::String(blockSize_) + ", re-preparing plugins");
        prepare(sampleRate_, wantBlock, routing_, workgroup_);
    }
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        reapUnlocked();
    }
    if (stateChanged)
        notifyDirty();

    int paintChannel = -1;
    int paintSlot = -1;
    int paintTrigger = 0;
    const auto nowMs = static_cast<std::int64_t>(steadyNs() / 1000000ULL);
    if (uiRepaint_.take(nowMs, paintChannel, paintSlot, paintTrigger) && settings_ != nullptr)
    {
        const auto notes = uiRepaint_.notes();
        const auto now = steadyNs();
        if (triggerLogNs_ == 0 || now - triggerLogNs_ > 2000000000ULL)
        {
            juce::String line = "ui trigger ";
            line << repaintTriggerName(paintTrigger)
                 << " x" << (notes - triggerLoggedNotes_)
                 << " coalesced, slot repaint only";
            if (paintChannel >= 0)
                line << " ch " << (paintChannel + 1) << " slot " << (paintSlot + 1);
            appendHostLog(*settings_, line);
            triggerLogNs_ = now;
            triggerLoggedNotes_ = notes;
        }
    }

    int faultChannel = -1;
    int faultSlot = -1;
    if (takeNaNFault(faultChannel, faultSlot) && settings_ != nullptr)
    {
        juce::String name;
        if (faultChannel >= 0 && faultChannel < kMaxChannels && faultSlot >= 0 && faultSlot < kSlotsPerChannel)
        {
            std::lock_guard<std::mutex> lock(lifeLock_);
            const auto& model = model_[static_cast<std::size_t>(faultChannel)][static_cast<std::size_t>(faultSlot)];
            if (model.plugin != nullptr)
                name = model.plugin->description.name;
        }
        if (name.isEmpty())
            name = "ch " + juce::String(faultChannel + 1) + " slot " + juce::String(faultSlot + 1);
        appendHostLog(*settings_, "plugin " + name + " produced NaN, muted");
    }
}

void PluginRack::audioProcessorParameterChanged(juce::AudioProcessor*, int, float)
{
    // Note only. The message thread paints one slot, at most 30 times a second.
    uiRepaint_.note(-1, -1, kRepaintParameter);
    if (! capturing_.load(std::memory_order_relaxed))
        stateDirty_.store(true, std::memory_order_relaxed);
}

void PluginRack::audioProcessorChanged(juce::AudioProcessor*, const juce::AudioProcessorListener::ChangeDetails& details)
{
    if (details.latencyChanged)
    {
        uiRepaint_.note(-1, -1, kRepaintLatency);
        latencyDirty_.store(true, std::memory_order_relaxed);
    }
    if (details.nonParameterStateChanged && ! capturing_.load(std::memory_order_relaxed))
        stateDirty_.store(true, std::memory_order_relaxed);
}

void PluginRack::setActiveChannels(int count)
{
    if (count < 0)
        count = 0;
    if (count > kMaxChannels)
        count = kMaxChannels;
    const int previous = activeChannels_.exchange(count, std::memory_order_relaxed);
    if (previous == count)
        return;

    struct IdleNote
    {
        int channel = 0;
        int slot = 0;
        juce::String name;
        juce::String identifier;
        bool running = false;
    };
    std::vector<IdleNote> idle;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        publishUnlocked();
        for (int channel = 0; channel < kMaxChannels; ++channel)
        {
            const int packed = routing_.inputPacked[static_cast<std::size_t>(channel)];
            const bool runs = channelProcessesPlugins(channel, count, packed);
            const bool ran = channelProcessesPlugins(channel, previous, packed);
            for (int slot = 0; slot < kSlotsPerChannel; ++slot)
            {
                auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
                if (model.plugin == nullptr || model.plugin->description.name.isEmpty())
                    continue;
                // A hidden channel can still hold a smoothed percent from when it
                // was open. That leftover is what kept the meter and the journal
                // saying the plugin was running after Offline -> 1 channel.
                int leftover = 0;
                if (! runs && model.plugin->gate != nullptr)
                    leftover = model.plugin->gate->shownPercent.exchange(0, std::memory_order_relaxed);
                if (runs == ran && leftover == 0)
                    continue;
                IdleNote note;
                note.channel = channel;
                note.slot = slot;
                note.name = model.plugin->description.name;
                note.identifier = pluginIdentifier(model.plugin->description);
                note.running = runs;
                idle.push_back(std::move(note));
            }
        }
    }
    for (const auto& note : idle)
    {
        if (note.running)
            tracePlugin(note.channel, note.slot, "active", note.name, note.identifier, "input is open");
        else
            tracePlugin(note.channel, note.slot, "idle", note.name, note.identifier, "no input on this channel");
    }
}

namespace
{
// AudioPluginHost publishes the device host time on the playhead for the
// duration of the callback. An Audio Unit copies that into AudioTimeStamp.
// VST3 copies it into ProcessContext::systemTime.
class DevicePlayHead final : public juce::AudioPlayHead
{
public:
    DevicePlayHead(std::int64_t hostTimeNs, std::int64_t samplePosition, double sampleRate) noexcept
        : hostTimeNs_(hostTimeNs),
          samplePosition_(samplePosition),
          sampleRate_(sampleRate)
    {
    }

    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
    {
        juce::AudioPlayHead::PositionInfo info;
        info.setTimeInSamples(samplePosition_);
        if (sampleRate_ > 0.0)
            info.setTimeInSeconds(static_cast<double>(samplePosition_) / sampleRate_);
        if (hostTimeNs_ > 0)
            info.setHostTimeNs(static_cast<std::uint64_t>(hostTimeNs_));
        return info;
    }

private:
    std::int64_t hostTimeNs_ = -1;
    std::int64_t samplePosition_ = 0;
    double sampleRate_ = 0.0;
};
} // namespace

void PluginRack::process(float* const* outputs,
                         int numOutputs,
                         int numSamples,
                         const Routing& routing,
                         std::uint64_t enabledLow,
                         std::uint64_t enabledHigh,
                         int activeChannels,
                         std::int64_t hostTimeNs)
{
    // A shorter callback is legal: the plugin was prepared for the maximum.
    // Re-preparing every time the device jitters (64, then 32, then 64)
    // restarts the plugin and is the dropout burst. Only a callback larger
    // than the prepared maximum needs a new prepare: numSamples > blockSize_.
    int askedReprepare = 0;
    if (blockAsksReprepare(numSamples, blockSize_))
    {
        const int previous = seenCallback_.load(std::memory_order_relaxed);
        if (previous == numSamples)
            seenStreak_.fetch_add(1, std::memory_order_relaxed);
        else
        {
            seenCallback_.store(numSamples, std::memory_order_relaxed);
            seenStreak_.store(1, std::memory_order_relaxed);
        }
        if (seenStreak_.load(std::memory_order_relaxed) >= 6)
        {
            reprepareBlock_.store(numSamples, std::memory_order_relaxed);
            askedReprepare = 1;
        }
    }
    else if (numSamples > 0 && numSamples <= blockSize_)
    {
        seenStreak_.store(0, std::memory_order_relaxed);
    }

    StallClock::get().audioPhase.store(kPhasePlugins, std::memory_order_relaxed);
    pluginPercentScratch_.store(0, std::memory_order_relaxed);
    slowestNs_.store(0, std::memory_order_relaxed);
    slowestBlockNs_.store(0, std::memory_order_relaxed);
    slowestChannel_.store(-1, std::memory_order_relaxed);
    slowestSlot_.store(-1, std::memory_order_relaxed);
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
        joinAudioThread();
        DevicePlayHead devicePlayHead(hostTimeNs, hostSamplePosition_, sampleRate_);
        blockPlayHead_ = &devicePlayHead;
        if (announceWorkgroup_.exchange(false, std::memory_order_acq_rel) && static_cast<bool>(workgroup_))
        {
            for (int channel = 0; channel < kMaxChannels; ++channel)
            {
                for (int slot = 0; slot < kSlotsPerChannel; ++slot)
                {
                    if (auto* instance = graph->slots[channel][slot].instance)
                        instance->audioWorkgroupContextChanged(workgroup_);
                }
            }
        }

        jobGraph_ = graph;
        jobSamples_ = numSamples;
        int count = 0;
        const int limit = std::clamp(activeChannels, 0, kMaxChannels);
        for (int channel = 0; channel < limit; ++channel)
        {
            if (channelReset_[static_cast<std::size_t>(channel)].exchange(0, std::memory_order_acq_rel) != 0)
                clearChannelMemory(*graph, channel);

            // No input, hidden, or past the card: the plugin stays loaded and does not run.
            // An output on its own is not an input. The right side of a one-input,
            // two-output device stays in this skip.
            if (! channelProcessesPlugins(channel, limit, routing.inputPacked[static_cast<std::size_t>(channel)]))
                continue;

            const bool linkedRight = (channel & 1) == 1
                                     && stereoLink_[static_cast<std::size_t>(channel - 1)].load(std::memory_order_relaxed) != 0;
            if (linkedRight && channelIsOn(enabledLow, enabledHigh, channel - 1))
                continue;

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
            float* partner = nullptr;
            if ((channel & 1) == 0
                && stereoLink_[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed) != 0
                && channelProcessesPlugins(channel + 1, limit, routing.inputPacked[static_cast<std::size_t>(channel + 1)])
                && channelIsOn(enabledLow, enabledHigh, channel + 1))
            {
                const int packedRight = routing.outputPacked[static_cast<std::size_t>(channel + 1)];
                if (packedRight >= 0 && packedRight < numOutputs)
                    partner = outputs[packedRight];
            }
            if (! anyPlugin && ! delayed && partner == nullptr)
                continue;

            jobChannels_[count] = channel;
            jobOutputs_[count] = output;
            jobPartners_[count] = partner;
            ++count;
        }

        // One channel stays on this thread. Further channels use the workgroup
        // pool, VST3 and Audio Unit the same way. VST3 process does not take
        // the wrapper lock, so a worker can call it.
        int waitNs = 0;
        if (count <= 1 || pool_ == nullptr || ! vst3ProcessOnAudioThread())
        {
            for (int index = 0; index < count; ++index)
                processJob(index);
        }
        else if (count > 0)
        {
            pool_->parallel(count, this);
            waitNs = pool_->lastWaitNs();
        }
        blockNote_.vst3Inline = 0;
        blockNote_.waitNs = waitNs;
        blockPlayHead_ = nullptr;
    }
    else
    {
        blockNote_.vst3Inline = 0;
        blockNote_.waitNs = 0;
        blockPlayHead_ = nullptr;
    }
    if (numSamples > 0)
        hostSamplePosition_ += numSamples;

    // Name is copied while this graph is still the in-use one. Releasing it
    // first lets the message thread free the graph under the audio thread.
    blockNote_.prepared = blockSize_;
    blockNote_.reprepare = askedReprepare;
    blockNote_.channel = slowestChannel_.load(std::memory_order_relaxed);
    blockNote_.slot = slowestSlot_.load(std::memory_order_relaxed);
    blockNote_.nanoseconds = slowestNs_.load(std::memory_order_relaxed);
    blockNote_.blockNs = slowestBlockNs_.load(std::memory_order_relaxed);
    blockNote_.name[0] = '\0';
    if (graph != nullptr && blockNote_.channel >= 0 && blockNote_.slot >= 0
        && blockNote_.channel < kMaxChannels && blockNote_.slot < kSlotsPerChannel)
        copyPluginName(blockNote_.name,
                       static_cast<int>(sizeof(blockNote_.name)),
                       graph->slots[blockNote_.channel][blockNote_.slot].name);

    inUse_.store(nullptr, std::memory_order_release);
    callbackEpoch_.fetch_add(1, std::memory_order_release);
    if (numSamples > 0)
        lastCallbackSamples_.store(numSamples, std::memory_order_relaxed);
    lastCallbackNs_.store(steadyNs() - started, std::memory_order_relaxed);
    livePluginPercent_.store(clampCpuPercent(pluginPercentScratch_.exchange(0, std::memory_order_relaxed)),
                             std::memory_order_relaxed);
    StallClock::get().audioPhase.store(kPhaseAudio, std::memory_order_relaxed);
}

void PluginRack::joinAudioThread()
{
    // Join once. Core Audio's IO thread is already in this workgroup, and so
    // is a worker after its first job. os_workgroup_join then returns
    // EALREADY. Joining on every buffer must not leave the workgroup. Ardour
    // joins its process thread once.
    if (! static_cast<bool>(workgroup_))
        return;
    if (audioJoined_ && joinedWorkgroup_ == workgroup_)
        return;
    workgroup_.join(audioToken_);
    joinedWorkgroup_ = workgroup_;
    audioJoined_ = true;
}

void PluginRack::processJob(int index)
{
    if (jobGraph_ == nullptr || index < 0 || index >= kMaxChannels)
        return;
    processOneChannel(*jobGraph_, jobChannels_[index], jobOutputs_[index], jobPartners_[index], jobSamples_);
}

void PluginRack::noteNaN(int channel, int slot) noexcept
{
    if (nanFaultPending_.load(std::memory_order_relaxed) != 0)
        return;
    nanFaultChannel_.store(channel, std::memory_order_relaxed);
    nanFaultSlot_.store(slot, std::memory_order_relaxed);
    nanFaultPending_.store(1, std::memory_order_release);
}

bool PluginRack::takeNaNFault(int& channel, int& slot) noexcept
{
    if (nanFaultPending_.exchange(0, std::memory_order_acq_rel) == 0)
        return false;
    channel = nanFaultChannel_.load(std::memory_order_relaxed);
    slot = nanFaultSlot_.load(std::memory_order_relaxed);
    return true;
}

void PluginRack::requestChannelReset(int channel) noexcept
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    channelReset_[static_cast<std::size_t>(channel)].store(1, std::memory_order_release);
}

void PluginRack::clearChannelMemory(LiveGraph& graph, int channel) noexcept
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    auto& line = delayLines_[static_cast<std::size_t>(channel)];
    if (! line.empty())
        std::memset(line.data(), 0, sizeof(float) * line.size());
    delayCursor_[static_cast<std::size_t>(channel)] = 0;
    graph.delayWrite[static_cast<std::size_t>(channel)] = 0;

    const int block = graph.maxBlock;
    if (block > 0)
    {
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            for (int index = 0; index < LiveGraph::kScratchCap; ++index)
            {
                float* scratch = graph.scratchPtrs[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)][static_cast<std::size_t>(index)];
                if (scratch != nullptr)
                    std::memset(scratch, 0, sizeof(float) * static_cast<std::size_t>(block));
            }
        }
        if (! graph.blockWork.empty())
        {
            float* work = graph.blockWork.data() + static_cast<std::size_t>(channel * block);
            std::memset(work, 0, sizeof(float) * static_cast<std::size_t>(block));
        }
    }
    if (graph.feedCapacity > 0 && ! graph.feedIn.empty() && ! graph.feedOut.empty())
    {
        const auto bytes = sizeof(float) * static_cast<std::size_t>(graph.feedCapacity);
        std::memset(graph.feedIn.data() + static_cast<std::size_t>(channel * graph.feedCapacity), 0, bytes);
        std::memset(graph.feedOut.data() + static_cast<std::size_t>(channel * graph.feedCapacity), 0, bytes);
    }
    graph.inCount[static_cast<std::size_t>(channel)] = 0;
    graph.inRead[static_cast<std::size_t>(channel)] = 0;
    graph.inWrite[static_cast<std::size_t>(channel)] = 0;
    graph.outCount[static_cast<std::size_t>(channel)] = 0;
    graph.outRead[static_cast<std::size_t>(channel)] = 0;
    graph.outWrite[static_cast<std::size_t>(channel)] = 0;
}

void PluginRack::setStereoLink(int leftChannel, bool linked)
{
    if (leftChannel < 0 || (leftChannel & 1) != 0 || leftChannel + 1 >= kMaxChannels)
        return;
    const auto next = static_cast<std::uint8_t>(linked ? 1 : 0);
    if (stereoLink_[static_cast<std::size_t>(leftChannel)].exchange(next, std::memory_order_relaxed) == next)
        return;
    // Keep the delay line. Zeroing it on a link change is the mute with no dropout.
    std::lock_guard<std::mutex> lock(lifeLock_);
    publishUnlocked();
}

bool PluginRack::stereoLink(int leftChannel) const noexcept
{
    if (leftChannel < 0 || leftChannel >= kMaxChannels)
        return false;
    return stereoLink_[static_cast<std::size_t>(leftChannel)].load(std::memory_order_relaxed) != 0;
}

void PluginRack::processOneChannel(LiveGraph& graph, int channel, float* output, float* partner, int numSamples)
{
    if (output == nullptr || channel < 0 || channel >= kMaxChannels)
        return;

    const int prepared = graph.maxBlock;
    const int partnerChannel = partner != nullptr ? channel + 1 : -1;
    const int meterSamples = numSamples;
    auto runPlugins = [&graph, channel, meterSamples, this](float* buffer, float* right, int count)
    {
        if (buffer == nullptr || count <= 0)
            return;
        bool sawPause = false;
        bool ranPlugin = false;
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            const auto& live = graph.slots[channel][slot];
            auto* instance = live.instance;
            const int inputs = live.inputChannels;
            const int outputsN = live.outputChannels;
            const int totalIn = std::max(inputs, live.totalInputs);
            const int totalOut = std::max(outputsN, live.totalOutputs);
            // The buffer covers the main bus and the sidechain. A host-channel
            // buffer (1 or 2) is too small once an extra bus is enabled.
            const int width = pluginProcessWidth(totalIn, totalOut);
            const bool extraBuses = totalIn > inputs || totalOut > outputsN;
            if (instance == nullptr || ! live.prepared || width < 1 || width > LiveGraph::kScratchCap)
                continue;
            if (count > graph.maxBlock)
                continue;

            auto* gate = live.gate;
            if (gate != nullptr && gate->pause.load(std::memory_order_acquire) != 0)
            {
                sawPause = true;
                continue;
            }
            if (gate != nullptr)
                gate->depth.fetch_add(1, std::memory_order_acq_rel);
            if (gate != nullptr && gate->pause.load(std::memory_order_acquire) != 0)
            {
                gate->depth.fetch_sub(1, std::memory_order_release);
                sawPause = true;
                continue;
            }

            float* scratch[LiveGraph::kScratchCap] {};
            for (int index = 0; index < LiveGraph::kScratchCap; ++index)
                scratch[index] = graph.scratchPtrs[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)][static_cast<std::size_t>(index)];
            const bool trueStereo = ! extraBuses && right != nullptr && inputs >= 2 && outputsN >= 2;
            // A 1-in/1-out plugin writes the channel buffer. Another host does
            // that in place. The scratch copy is a second pass over this block.
            const bool monoInPlace = ! extraBuses && ! trueStereo && std::max(inputs, outputsN) <= 1;
            bool scratchMissing = scratch[0] == nullptr;
            if (! scratchMissing)
            {
                const int need = extraBuses ? width : (trueStereo || width > 1 ? 2 : 1);
                for (int index = 0; index < need; ++index)
                    scratchMissing = scratchMissing || scratch[index] == nullptr;
            }
            if (! trueStereo && ! monoInPlace && scratchMissing)
            {
                if (gate != nullptr)
                    gate->depth.fetch_sub(1, std::memory_order_release);
                continue;
            }

            ranPlugin = true;
            int blockOnlyNs = 0;
            const auto began = steadyNs();
            auto& midi = graph.midi[static_cast<std::size_t>(channel)];
            auto render = [&](juce::AudioBuffer<float>& view)
            {
                if (blockPlayHead_ != nullptr)
                    instance->setPlayHead(blockPlayHead_);
                instance->processBlock(view, midi);
                instance->setPlayHead(nullptr);
            };
            const auto fold = foldKeepingRightInput(stereoFoldFromInt(live.stereoFold),
                                                     (channel & 1) == 1 && right == nullptr);
            auto runOne = [&](float* mono)
            {
                if (mono == nullptr)
                    return;
                const int viewWidth = std::max(1, std::min(std::max(inputs, outputsN), 2));
                if (viewWidth <= 1)
                {
                    float* plugin[1] = { mono };
                    midi.clear();
                    juce::AudioBuffer<float> view(plugin, 1, count);
                    const auto blockBegan = steadyNs();
                    render(view);
                    blockOnlyNs += static_cast<int>(steadyNs() - blockBegan);
                    return;
                }
                if (scratch[0] == nullptr || scratch[1] == nullptr)
                    return;
                float* plugin[2] = { scratch[0], scratch[1] };
                preparePluginBlock(plugin, inputs, outputsN, mono, count);
                midi.clear();
                juce::AudioBuffer<float> view(plugin, viewWidth, count);
                const auto blockBegan = steadyNs();
                render(view);
                blockOnlyNs += static_cast<int>(steadyNs() - blockBegan);
                takeFoldedChannel(mono, plugin, outputsN, count, fold);
            };
            SanitizeReport leftReport;
            SanitizeReport rightReport;
            if (extraBuses)
            {
                // Main channels carry this block. Every extra input is silence.
                // Extra outputs are ignored, so a sidechain cannot reach the mix.
                auto runExtra = [&](float* left, float* partner)
                {
                    const int fed = std::min(std::max(inputs, 1), width);
                    // A mono channel through a stereo main bus (sidechain kept
                    // on) feeds both sides the same signal, then folds.
                    const float* rightFeed = partner != nullptr ? partner : (inputs >= 2 ? left : nullptr);
                    feedPluginChannels(scratch, width, fed, count, left, rightFeed);
                    midi.clear();
                    juce::AudioBuffer<float> view(scratch, width, count);
                    const auto blockBegan = steadyNs();
                    render(view);
                    blockOnlyNs += static_cast<int>(steadyNs() - blockBegan);
                    if (partner == nullptr && outputsN >= 2)
                    {
                        takeFoldedChannel(left, scratch, outputsN, count, fold);
                        return;
                    }
                    if (scratch[0] != nullptr && left != nullptr && scratch[0] != left)
                        std::memcpy(left, scratch[0], sizeof(float) * static_cast<std::size_t>(count));
                    if (outputsN >= 2 && partner != nullptr && scratch[1] != nullptr && scratch[1] != partner)
                        std::memcpy(partner, scratch[1], sizeof(float) * static_cast<std::size_t>(count));
                };
                runExtra(buffer, outputsN >= 2 ? right : nullptr);
                leftReport = sanitizeAudioBlock(buffer, count);
                if (outputsN < 2 && right != nullptr)
                {
                    runExtra(right, nullptr);
                    rightReport = sanitizeAudioBlock(right, count);
                }
                else if (right != nullptr)
                {
                    rightReport = sanitizeAudioBlock(right, count);
                }
            }
            else if (trueStereo)
            {
                float* pair[2] = { buffer, right };
                midi.clear();
                juce::AudioBuffer<float> view(pair, 2, count);
                const auto blockBegan = steadyNs();
                render(view);
                blockOnlyNs += static_cast<int>(steadyNs() - blockBegan);
                leftReport = sanitizeAudioBlock(buffer, count);
                rightReport = sanitizeAudioBlock(right, count);
            }
            else
            {
                // Private scratch, zeroed, then filled from this block's mono
                // input. The channel buffer is not an input the plugin can
                // alias with last block's output.
                runOne(buffer);
                leftReport = sanitizeAudioBlock(buffer, count);
                if (right != nullptr && width == 1)
                {
                    runOne(right);
                    rightReport = sanitizeAudioBlock(right, count);
                }
            }
            if (leftReport.nonFinite || rightReport.nonFinite)
                noteNaN(channel, slot);
            if (gate != nullptr)
            {
                const auto elapsed = steadyNs() - began;
                gate->cpuNs.fetch_add(elapsed, std::memory_order_relaxed);
                gate->blocks.fetch_add(1, std::memory_order_relaxed);
                const int instant = blockCpuPercent(elapsed, meterSamples > 0 ? meterSamples : count, sampleRate_);
                const int previous = gate->shownPercent.load(std::memory_order_relaxed);
                const int shown = smoothCpuPercent(previous, instant);
                gate->shownPercent.store(shown, std::memory_order_relaxed);
                pluginPercentScratch_.fetch_add(shown, std::memory_order_relaxed);
                const int ns = static_cast<int>(elapsed > static_cast<std::uint64_t>(2147483647) ? 2147483647 : elapsed);
                int prior = slowestNs_.load(std::memory_order_relaxed);
                while (ns > prior)
                {
                    if (! slowestNs_.compare_exchange_weak(prior, ns, std::memory_order_relaxed))
                        continue;
                    slowestBlockNs_.store(blockOnlyNs, std::memory_order_relaxed);
                    slowestChannel_.store(channel, std::memory_order_relaxed);
                    slowestSlot_.store(slot, std::memory_order_relaxed);
                    break;
                }
                gate->depth.fetch_sub(1, std::memory_order_release);
            }
        }
        // A paused plugin must not leave the dry mic on the output.
        if (replaceDryWhenPluginPaused(sawPause, ranPlugin))
        {
            std::memset(buffer, 0, sizeof(float) * static_cast<std::size_t>(count));
            if (right != nullptr)
                std::memset(right, 0, sizeof(float) * static_cast<std::size_t>(count));
        }
    };

    auto scrub = [&](float* buffer, int count)
    {
        if (buffer == nullptr || count <= 0)
            return;
        if (sanitizeAudioBlock(buffer, count).nonFinite)
            noteNaN(channel, -1);
    };

    // One processBlock of the callback when it fits. Holding samples until a
    // full prepared block, then running that block inside one short callback,
    // spends the whole plugin cost against a smaller device period. That is
    // the buffer-64 dropout with a plugin prepared for a larger block.
    const int chunk = pluginCallbackSamples(numSamples, prepared);
    if (chunk > 0 && numSamples <= prepared)
    {
        runPlugins(output, partner, numSamples);
    }
    else if (chunk > 0 && numSamples > prepared)
    {
        int done = 0;
        while (done < numSamples)
        {
            const int count = std::min(prepared, numSamples - done);
            const auto offset = static_cast<std::ptrdiff_t>(done);
            runPlugins(output + offset, partner != nullptr ? partner + offset : nullptr, count);
            done += count;
        }
    }

    scrub(output, numSamples);
    scrub(partner, numSamples);

    auto delayChannel = [&graph, numSamples, this](int index, float* data)
    {
        if (data == nullptr || index < 0 || index >= kMaxChannels)
            return;
        const int length = graph.delayLength[static_cast<std::size_t>(index)];
        auto& line = delayLines_[static_cast<std::size_t>(index)];
        auto& cursor = delayCursor_[static_cast<std::size_t>(index)];
        if (length <= 0)
            return;
        if (static_cast<int>(line.size()) != length)
        {
            std::vector<float> next(static_cast<std::size_t>(length), 0.0f);
            int nextWrite = 0;
            adoptDelayLine(next.data(), length, nextWrite, line.data(), static_cast<int>(line.size()), cursor);
            line.swap(next);
            cursor = nextWrite;
        }
        delayInPlace(line.data(), length, cursor, data, numSamples);
    };
    delayChannel(channel, output);
    delayChannel(partnerChannel, partner);
    scrub(output, numSamples);
    scrub(partner, numSamples);
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
    const int callbackSamples = lastCallbackSamples_.load(std::memory_order_relaxed);
    const int block = callbackSamples > 0 ? callbackSamples : (blockSize_ > 0 ? blockSize_ : 512);
    const double period = static_cast<double>(block) / rate;
    if (period <= 0.0)
        return load;

    const auto callbackNs = lastCallbackNs_.load(std::memory_order_relaxed);
    const float rawCallback = static_cast<float>((static_cast<double>(callbackNs) / 1.0e9) / period * 100.0);
    load.callbackPercent = static_cast<float>(displayCpuPercent(rawCallback));

    std::lock_guard<std::mutex> lock(lifeLock_);
    float pluginSum = 0.0f;
    const int shown = activeChannels_.load(std::memory_order_relaxed);
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        if (! channelProcessesPlugins(channel, shown, routing_.inputPacked[static_cast<std::size_t>(channel)]))
            continue;
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            const auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
            if (model.plugin == nullptr || model.plugin->gate == nullptr || model.plugin->instance == nullptr)
                continue;
            auto& gate = *model.plugin->gate;
            const float percent = static_cast<float>(clampCpuPercent(gate.shownPercent.load(std::memory_order_relaxed)));
            if (percent <= 0.0f)
                continue;
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
    load.pluginPercent = static_cast<float>(displayCpuPercent(pluginSum));
    return load;
}

void PluginRack::addCoreNs(int core, std::uint64_t ns) noexcept
{
    if (core < 0 || core >= static_cast<int>(coreNs_.size()) || ns == 0)
        return;
    coreNs_[static_cast<std::size_t>(core)].fetch_add(ns, std::memory_order_relaxed);
    coreBlocks_[static_cast<std::size_t>(core)].fetch_add(1, std::memory_order_relaxed);
    const int instant = blockCpuPercent(ns, blockSize_ > 0 ? blockSize_ : 512, sampleRate_ > 0.0 ? sampleRate_ : 48000.0);
    const int previous = coreShown_[static_cast<std::size_t>(core)].load(std::memory_order_relaxed);
    coreShown_[static_cast<std::size_t>(core)].store(smoothCpuPercent(previous, instant), std::memory_order_relaxed);
}

CpuMeters PluginRack::cpuMeters() const
{
    CpuMeters meters;
    const double rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
    const int callbackSamples = lastCallbackSamples_.load(std::memory_order_relaxed);
    const int block = callbackSamples > 0 ? callbackSamples : (blockSize_ > 0 ? blockSize_ : 512);
    const double period = static_cast<double>(block) / rate;
    const auto callbackNs = lastCallbackNs_.load(std::memory_order_relaxed);
    if (period > 0.0)
        meters.callbackPercent = static_cast<float>(displayCpuPercent(
            static_cast<float>((static_cast<double>(callbackNs) / 1.0e9) / period * 100.0)));
    meters.workers = pool_ != nullptr ? pool_->workerCount() : 0;
    const int cores = std::min(1 + meters.workers, static_cast<int>(meters.percent.size()));
    for (int core = 0; core < cores; ++core)
        meters.percent[static_cast<std::size_t>(core)] = static_cast<float>(
            clampCpuPercent(coreShown_[static_cast<std::size_t>(core)].load(std::memory_order_relaxed)));
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

void PluginRack::copyLatencyInputs(ChannelLatencyInput* out, int count) const
{
    if (out == nullptr || count <= 0)
        return;
    std::lock_guard<std::mutex> lock(lifeLock_);
    const int n = std::min(count, kMaxChannels);
    for (int channel = 0; channel < n; ++channel)
        out[channel] = latencyInputs_[static_cast<std::size_t>(channel)];
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
    const double previousRate = sampleRate_;
    const int previousBlock = blockSize_;
    const bool sameQuantum = previousBlock > 0 && previousBlock == block
                             && previousRate > rate - 1.0 && previousRate < rate + 1.0;
    // Do not set blockProcessing_. That flag makes the callback skip every
    // plugin after it has copied the dry input. A message-thread stall would
    // then be the mic with no De-Feedback. signalDuringMessageStall stays plugins.
    if (! waitUntilOutsideCallback())
        return;
    workgroup_ = workgroup;
    audioJoined_ = false;
    announceWorkgroup_.store(true, std::memory_order_release);
    // Replace workers only before the device is running. The period has to be
    // this buffer. Tearing the pool down from a live callback is a use after free.
    if (pool_ == nullptr
        || (! callbacksRunning_.load(std::memory_order_acquire) && ! pool_->sameQuantum(rate, block)))
        pool_ = std::make_unique<RealtimePool>(dspWorkerCount(), rate, block);

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

    // Same rate and block: the instances are already prepared. Leave them in
    // the callback. A device-mask dip must not re-prepare the whole rack.
    if (! sameQuantum)
    {
        for (auto& plugin : hosted)
        {
            if (plugin == nullptr || plugin->instance == nullptr || plugin->gate == nullptr)
                continue;
            if (! pauseGate(*plugin->gate))
                continue;
            auto* instance = plugin->instance.get();
            const auto deviceWorkgroup = workgroup_;
            runOnMessageThread([instance, plugin, rate, block, deviceWorkgroup]
            {
                if (instance == nullptr)
                    return;
                instance->setNonRealtime(false);
                if (static_cast<bool>(deviceWorkgroup))
                    instance->audioWorkgroupContextChanged(deviceWorkgroup);
                instance->prepareToPlay(rate, block);
                instance->reset();
                plugin->latencySamples = clampLatencySamples(instance->getLatencySamples());
            });
            plugin->prepared = true;
            resumeGate(*plugin->gate);
        }
    }

    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        for (auto& plugin : hosted)
        {
            if (plugin == nullptr)
                continue;
            plugin->prepared = plugin->instance != nullptr;
        }
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
                            bool openWhenReady,
                            int stereoFold)
{
    if (! validSlot(channel, slot))
        return;

    if (auto* messages = juce::MessageManager::getInstanceWithoutCreating())
    {
        if (! messages->isThisTheMessageThread())
        {
            auto alive = alive_;
            juce::MessageManager::callAsync(
                [this, alive, channel, slot, description, state, bypassed, markDirty, openWhenReady, stereoFold]
                {
                    if (! alive->load(std::memory_order_acquire))
                        return;
                    loadPlugin(channel, slot, description, state, bypassed, markDirty, openWhenReady, stereoFold);
                });
            return;
        }
    }

    // Loads on one channel run one at a time. A second load waits behind this one.
    enqueueChannel(channel,
                   [this, channel, slot, description, state, bypassed, markDirty, openWhenReady, stereoFold]
                   {
                       beginLoad(channel, slot, description, state, bypassed, markDirty, openWhenReady, stereoFold, 0);
                   },
                   true);
}

void PluginRack::beginLoad(int channel,
                           int slot,
                           juce::PluginDescription description,
                           juce::MemoryBlock state,
                           bool bypassed,
                           bool markDirty,
                           bool openWhenReady,
                           int stereoFold,
                           std::uint64_t restoreGeneration)
{
    touchMessageBeat();
    faultChannel_ = channel;
    faultSlot_ = slot;
    faultName_ = description.name;
    if (! validSlot(channel, slot))
    {
        finishChannelWork(channel, channelWork_[static_cast<std::size_t>(channel)].ticket);
        completeRestoreStep(restoreGeneration);
        return;
    }

    // A click that chooses a plugin owns this slot, including ch 1 slot 1 after
    // "left crashed plugins off". A restore still waiting in the queue must not
    // clear that plugin. The check is before the editor is detached.
    if (restoreGeneration == 0)
        claimSlotForManualLoad(channel, slot);
    else if (restoreSkipsClaimedSlot(true, userClaimedSlot_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)]))
    {
        finishChannelWork(channel, channelWork_[static_cast<std::size_t>(channel)].ticket);
        completeRestoreStep(restoreGeneration);
        return;
    }

    // Hide the old editor now. It stays allocated until the deferred AU view
    // callbacks have had a few turns on the message loop.
    detachEditor(channel, slot);

    std::uint64_t ticket = 0;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        ticket = ++ticketSource_;
        auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
        model.ticket = ticket;
        model.loading = true;
        model.error.clear();
    }
    channelWork_[static_cast<std::size_t>(channel)].ticket = ticket;

    std::vector<SlotBlock> slotBlocks;
    slotBlocks.reserve(blockedSlots_.size());
    for (const auto& block : blockedSlots_)
    {
        SlotBlock item;
        item.channel = block.channel;
        item.slot = block.slot;
        item.identifier = block.identifier.toStdString();
        slotBlocks.push_back(std::move(item));
    }
    const auto incomingId = pluginIdentifier(description).toStdString();
    const auto formatName = description.pluginFormatName;
    // The attempt is logged before anything else. Choosing a plugin always loads it.
    tracePlugin(channel, slot, "loading", description.name, pluginIdentifier(description), {}, formatName);
    // The dialog "Open session without this plugin" skips this automatic restore
    // only. The slot stays empty. A later manual choice uses generation 0 and loads.
    if (leaveOutOfThisLaunch(restoreGeneration != 0, omittedFromThisLaunch(channel, slot, incomingId)))
    {
        const auto name = description.name.isNotEmpty() ? description.name : juce::String("Plugin");
        std::shared_ptr<HostedPlugin> previous;
        {
            std::lock_guard<std::mutex> lock(lifeLock_);
            auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
            model.loading = false;
            model.error.clear();
            model.note = kPluginCrashedLastTime;
            previous = std::move(model.plugin);
            publishUnlocked();
        }
        bury(std::move(previous), nullptr);
        keepPluginInSessionFile(channel, slot, description, state, bypassed, stereoFold);
        tracePlugin(channel, slot, "did-not-load", name, pluginIdentifier(description), kLeftOutOfThisLaunch, formatName);
        finishChannelWork(channel, ticket);
        noteSlot(channel);
        completeRestoreStep(restoreGeneration);
        return;
    }
    dropKeptPlugin(channel, slot);
    const auto admit = admitPluginIntoSlot(channel,
                                           slot,
                                           incomingId,
                                           slotBlocks.data(),
                                           static_cast<int>(slotBlocks.size()),
                                           crashedBefore(description));
    if (admit.clearSlot)
        clearCrashBlock(channel, slot);
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
        model.note = admit.reason != nullptr ? juce::String(admit.reason) : juce::String();
    }
    if (admit.reason != nullptr)
        tracePlugin(channel, slot, "warn", description.name, pluginIdentifier(description), juce::String(admit.reason), formatName);

    double rate = 48000.0;
    int block = 512;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
        block = blockSize_ > 0 ? blockSize_ : 512;
    }

    // The loader thread creates the instance for an Audio Unit. The message
    // thread must stay in the runloop: a slow AudioComponentInstanceNew on
    // this thread holds the message lock, and the audio callback can block
    // on that lock.
    // Two AudioComponentInstanceNew calls at once deadlock. The loader waits
    // for the main thread, and the main thread is inside its own instantiate.
    if (catalogue_.prefersBackgroundInstance(description))
    {
        StallClock::get().messagePhase.store(kPhaseMessage, std::memory_order_relaxed);
        BackgroundJob job;
        job.channel = channel;
        job.slot = slot;
        job.ticket = ticket;
        job.generation = restoreGeneration;
        job.markDirty = markDirty;
        job.bypassed = bypassed;
        job.open = openWhenReady;
        job.stereoFold = stereoFold;
        job.description = std::move(description);
        job.state = std::move(state);
        job.rate = rate;
        job.block = block;
        backgroundQueue_.push_back(std::move(job));
        pumpBackgroundQueue();
        return;
    }

    // The format creates the instance when this message is delivered, on the
    // message thread. Phase stays "load" until that callback starts.
    StallClock::get().messagePhase.store(kPhaseLoad, std::memory_order_relaxed);
    auto alive = alive_;
    catalogue_.createInstanceAsync(
        description,
        rate,
        block,
        [this, alive, channel, slot, ticket, markDirty, bypassed, openWhenReady, stereoFold, description, state, restoreGeneration](
            std::unique_ptr<juce::AudioPluginInstance> instance,
            const juce::String& error)
        {
            if (! alive->load(std::memory_order_acquire))
            {
                if (instance != nullptr)
                    destroyInstance(nullptr, instance.release());
                return;
            }

            // AUv2 finishes inside this call, on the message thread. AUv3's
            // AudioComponentInstantiate completion can arrive on another
            // thread. Move that callback onto the message thread. Bus setup
            // and prepare stay on the loader thread.
            if (auto* messages = juce::MessageManager::getInstanceWithoutCreating())
            {
                if (! messages->isThisTheMessageThread())
                {
                    struct Held
                    {
                        std::unique_ptr<juce::AudioPluginInstance> instance;
                        juce::String error;
                    };
                    auto held = std::make_shared<Held>();
                    held->instance = std::move(instance);
                    held->error = error;
                    juce::MessageManager::callAsync(
                        [this, alive, channel, slot, ticket, markDirty, bypassed, openWhenReady, stereoFold, description, state, restoreGeneration, held]
                        {
                            if (! alive->load(std::memory_order_acquire))
                            {
                                if (held->instance != nullptr)
                                    destroyInstance(nullptr, held->instance.release());
                                return;
                            }
                            if (settings_ != nullptr)
                                appendHostLog(*settings_, pluginLogLine("plugin", channel, slot, description.pluginFormatName, description.name, pluginIdentifier(description),
                                                                        "instantiate callback moved onto the message thread"));
                            finishLoad(channel, slot, ticket, markDirty, bypassed, openWhenReady, stereoFold,
                                       description, state, std::move(held->instance), held->error, restoreGeneration);
                        });
                    return;
                }
            }

            finishLoad(channel, slot, ticket, markDirty, bypassed, openWhenReady, stereoFold, description, state, std::move(instance), error, restoreGeneration);
        });
}

void PluginRack::finishLoad(int channel,
                            int slot,
                            std::uint64_t ticket,
                            bool markDirty,
                            bool bypassed,
                            bool openWhenReady,
                            int stereoFold,
                            juce::PluginDescription description,
                            juce::MemoryBlock state,
                            std::unique_ptr<juce::AudioPluginInstance> instance,
                            const juce::String& error,
                            std::uint64_t restoreGeneration)
{
    const auto refusedName = description.name;
    const auto refusedId = pluginIdentifier(description);
    const auto refusedFormat = description.pluginFormatName;
    // createPluginInstance has returned. Later paints are not part of the load.
    touchMessageBeat();
    StallClock::get().messagePhase.store(kPhaseMessage, std::memory_order_relaxed);
    struct RestoreFinish
    {
        PluginRack& rack;
        int channel;
        std::uint64_t generation;
        ~RestoreFinish()
        {
            rack.noteSlot(channel);
            rack.completeRestoreStep(generation);
        }
    } restoreFinish { *this, channel, restoreGeneration };

    if (! validSlot(channel, slot))
        return;

    bool stale = false;
    bool ownsGate = false;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        stale = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)].ticket != ticket;
        ownsGate = channelWork_[static_cast<std::size_t>(channel)].ticket == ticket;
    }
    if (stale)
    {
        if (instance != nullptr)
        {
            auto retired = std::make_shared<HostedPlugin>();
            retired->instance.reset(instance.release(), InstanceDeleter { this });
            bury(std::move(retired), nullptr);
        }
        if (ownsGate)
            finishChannelWork(channel, ticket);
        if (settings_ != nullptr)
            appendHostLog(*settings_, pluginLogLine("did-not-load", channel, slot, refusedFormat, refusedName, refusedId,
                                                    "the slot changed before the plugin was ready"));
        return;
    }

    OffThreadBuses buses;
    {
        std::lock_guard<std::mutex> lock(offThreadMutex_);
        buses = std::move(offThreadBuses_);
        offThreadBuses_ = {};
    }
    juce::String problem = error;
    const int left = channel & ~1;
    const bool unlinkedMono = ! stereoLink(left);
    // Bus setup and prepare already finished. Doing them again here would run
    // setActive while this thread is also drawing.
    if (instance != nullptr && ! buses.ready)
        problem = "The plugin setup did not finish. See youhost.log.";
    juce::String attemptText;
    for (int index = 0; index < buses.attempts.size(); ++index)
    {
        if (index > 0)
            attemptText += "; ";
        attemptText += buses.attempts[index];
    }
    const auto failureReason = juce::String(pluginFailureLogReason(instance != nullptr,
                                                                   error.toRawUTF8(),
                                                                   attemptText.toRawUTF8()));
    if (settings_ != nullptr)
    {
        if (instance == nullptr)
            appendHostLog(*settings_, pluginLogLine("did-not-load", channel, slot, refusedFormat, refusedName, refusedId, failureReason));
        for (const auto& attempt : buses.attempts)
            appendHostLog(*settings_, pluginLogLine("plugin", channel, slot, refusedFormat, refusedName, refusedId, attempt));
    }
    if (instance != nullptr && (! buses.ready || ! buses.ok))
    {
        if (problem.isEmpty())
            problem = pluginLoadMessage(true, unlinkedMono, buses.supportMono, buses.supportStereo, buses.supportSide,
                                        buses.openedStereo, buses.hasExtraBuses, nullptr);
        if (settings_ != nullptr)
            appendHostLog(*settings_, pluginLogLine("did-not-load", channel, slot, refusedFormat, refusedName, refusedId, problem));
        auto retired = std::make_shared<HostedPlugin>();
        retired->instance.reset(instance.release(), InstanceDeleter { this });
        bury(std::move(retired), nullptr);
    }

    std::shared_ptr<HostedPlugin> hosted;
    if (instance != nullptr)
    {
        instance->addListener(this);

        hosted = std::make_shared<HostedPlugin>();
        hosted->instance.reset(instance.release(), InstanceDeleter { this });
        hosted->description = std::move(description);
        hosted->state = std::move(state);
        hosted->bypassed = bypassed;
        hosted->inputChannels = buses.inputChannels;
        hosted->outputChannels = buses.outputChannels;
        hosted->totalInputs = std::max(buses.inputChannels, buses.totalInputs);
        hosted->totalOutputs = std::max(buses.outputChannels, buses.totalOutputs);
        hosted->processChannels = std::max(hosted->totalInputs, hosted->totalOutputs);
        hosted->prepared = true;
        hosted->stereoFold = static_cast<int>(stereoFoldFromInt(stereoFold));
        hosted->latencySamples = clampLatencySamples(buses.latencySamples);
    }
    else if (problem.isEmpty())
    {
        problem = pluginLoadMessage(false, true, false, false, false, false, false, nullptr);
    }

    bool notify = false;
    bool openIt = false;
    bool loaded = false;
    bool replaced = false;
    bool channelFeeds = false;
    juce::String loadedName = refusedName;
    juce::String loadedId = refusedId;
    std::shared_ptr<HostedPlugin> previous;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
        ownsGate = channelWork_[static_cast<std::size_t>(channel)].ticket == ticket;
        if (model.ticket == ticket)
        {
            model.loading = false;
            model.error = hosted == nullptr ? problem : juce::String();
            previous = std::move(model.plugin);
            model.plugin = std::move(hosted);
            if (model.plugin != nullptr)
                announceWorkgroup_.store(true, std::memory_order_release);
            publishUnlocked();
            notify = markDirty;
            openIt = openWhenReady && model.plugin != nullptr;
            loaded = model.plugin != nullptr;
            replaced = true;
            if (loaded)
            {
                loadedName = model.plugin->description.name;
                loadedId = pluginIdentifier(model.plugin->description);
                channelFeeds = channelProcessesPlugins(channel,
                                                       activeChannels_.load(std::memory_order_relaxed),
                                                       routing_.inputPacked[static_cast<std::size_t>(channel)]);
            }
        }
    }
    if (! replaced)
    {
        bury(std::move(hosted), nullptr);
        // The slot already belongs to a newer load. A journal upsert here would
        // replace that plugin's mark, so the reason goes to the log only.
        if (settings_ != nullptr)
            appendHostLog(*settings_, pluginLogLine("did-not-load", channel, slot, refusedFormat, refusedName, refusedId,
                                                    "the slot changed before the plugin was ready"));
        if (ownsGate)
            finishChannelWork(channel, ticket);
        return;
    }
    bury(std::move(previous), nullptr);

    if (! loaded)
        tracePlugin(channel, slot, "unload", loadedName, loadedId, failureReason, refusedFormat);
    else if (! channelFeeds)
        tracePlugin(channel, slot, "idle", loadedName, loadedId, "no input on this channel", refusedFormat);
    else
        tracePlugin(channel, slot, "active", loadedName, loadedId, {}, refusedFormat);

    if (notify)
        notifyDirty();
    // The editor opens on a later message-loop turn. Doing it inside this
    // delivery callback deadlocks: the plugin waits on the message lock, and
    // this callback still owns that lock. No lock is held across the open.
    if (openIt)
        scheduleDeferredEditor(channel, slot);
    if (channelWork_[static_cast<std::size_t>(channel)].ticket == ticket)
        finishChannelWork(channel, ticket);
}

void PluginRack::silencePublished()
{
    for (int channel = 0; channel < kMaxChannels; ++channel)
        requestChannelReset(channel);

    const bool outside = waitUntilOutsideCallback();
    std::vector<std::shared_ptr<HostedPlugin>> plugins;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        auto* graph = published_.load(std::memory_order_acquire);
        const bool idle = (callbackEpoch_.load(std::memory_order_acquire) & 1u) == 0u
                          && inUse_.load(std::memory_order_acquire) == nullptr;
        if (outside && idle && graph != nullptr)
        {
            for (int channel = 0; channel < kMaxChannels; ++channel)
                clearChannelMemory(*graph, channel);
        }
        if (outside)
        {
            for (auto& row : model_)
                for (auto& slot : row)
                    if (slot.plugin != nullptr && slot.plugin->prepared)
                        plugins.push_back(slot.plugin);
        }
    }
    if (! outside)
        return;

    for (auto& plugin : plugins)
    {
        if (plugin == nullptr || plugin->instance == nullptr || plugin->gate == nullptr)
            continue;
        if (! pauseGate(*plugin->gate))
            continue;
        plugin->instance->reset();
        resumeGate(*plugin->gate);
    }
}

void PluginRack::clearAll(bool markDirty)
{
    ++restoreGeneration_;
    restoreQueue_.clear();
    restorePosted_ = false;
    sessionLoadsReady_ = false;
    loadCursor_ = {};
    restoring_ = false;

    for (auto& gate : channelWork_)
    {
        gate.pending.clear();
        gate.inFlight = false;
        gate.ticket = 0;
    }
    for (auto& row : userClaimedSlot_)
        row.fill(false);

    std::vector<std::tuple<int, int, juce::String, juce::String>> unloaded;
    std::vector<std::shared_ptr<HostedPlugin>> retiredPlugins;
    std::vector<std::unique_ptr<EditorWindow>> retiredWindows;
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            auto window = std::move(editors_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)]);
            if (window != nullptr)
            {
                window->onClose_ = nullptr;
                window->setVisible(false);
                retiredWindows.push_back(std::move(window));
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        for (int channel = 0; channel < kMaxChannels; ++channel)
        {
            stereoLink_[static_cast<std::size_t>(channel)].store(0, std::memory_order_relaxed);
            for (int slot = 0; slot < kSlotsPerChannel; ++slot)
            {
                auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
                if (model.plugin != nullptr)
                    unloaded.emplace_back(channel, slot, model.plugin->description.name, pluginIdentifier(model.plugin->description));
                model.ticket = ++ticketSource_;
                model.loading = false;
                model.error.clear();
                model.note.clear();
                if (model.plugin != nullptr)
                    retiredPlugins.push_back(std::move(model.plugin));
            }
        }
        for (auto& excluded : excluded_)
            excluded = false;
        keptInSessionFile_.clear();
        bypassAll_.store(0, std::memory_order_relaxed);
        publishUnlocked();
    }

    const auto count = std::max(retiredPlugins.size(), retiredWindows.size());
    for (std::size_t index = 0; index < count; ++index)
    {
        std::shared_ptr<HostedPlugin> plugin;
        std::unique_ptr<EditorWindow> window;
        if (index < retiredPlugins.size())
            plugin = std::move(retiredPlugins[index]);
        if (index < retiredWindows.size())
            window = std::move(retiredWindows[index]);
        bury(std::move(plugin), std::move(window));
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

    if (auto* messages = juce::MessageManager::getInstanceWithoutCreating())
    {
        if (! messages->isThisTheMessageThread())
        {
            auto alive = alive_;
            juce::MessageManager::callAsync([this, alive, channel, slot]
            {
                if (! alive->load(std::memory_order_acquire))
                    return;
                removePlugin(channel, slot);
            });
            return;
        }
    }

    enqueueChannel(channel, [this, channel, slot] { removePluginNow(channel, slot); }, false);
}

void PluginRack::transferPlugin(int fromChannel, int fromSlot, int toChannel, int toSlot, bool copy)
{
    if (! validSlot(fromChannel, fromSlot) || ! validSlot(toChannel, toSlot))
        return;
    if (fromChannel == toChannel && fromSlot == toSlot)
        return;

    if (auto* messages = juce::MessageManager::getInstanceWithoutCreating())
    {
        if (! messages->isThisTheMessageThread())
        {
            auto alive = alive_;
            juce::MessageManager::callAsync([this, alive, fromChannel, fromSlot, toChannel, toSlot, copy]
            {
                if (! alive->load(std::memory_order_acquire))
                    return;
                transferPlugin(fromChannel, fromSlot, toChannel, toSlot, copy);
            });
            return;
        }
    }

    // A drag during a load is ignored. Queueing it would unload an instance
    // whose editor is still being configured.
    if (channelBusy(fromChannel) || channelBusy(toChannel))
        return;

    SlotMoveState fromState;
    SlotMoveState toState;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        const auto& source = model_[static_cast<std::size_t>(fromChannel)][static_cast<std::size_t>(fromSlot)];
        const auto& destination = model_[static_cast<std::size_t>(toChannel)][static_cast<std::size_t>(toSlot)];
        fromState.loading = source.loading;
        toState.loading = destination.loading;
        fromState.instance = source.plugin != nullptr && source.plugin->instance != nullptr ? 1 : 0;
        toState.instance = destination.plugin != nullptr && destination.plugin->instance != nullptr ? 1 : 0;
    }
    if (! slotDragAccepted(fromState, toState, false))
        return;

    if (! copy)
    {
        juce::String name;
        juce::String identifier;
        {
            std::lock_guard<std::mutex> lock(lifeLock_);
            auto& source = model_[static_cast<std::size_t>(fromChannel)][static_cast<std::size_t>(fromSlot)];
            auto& destination = model_[static_cast<std::size_t>(toChannel)][static_cast<std::size_t>(toSlot)];
            if (source.plugin != nullptr)
            {
                name = source.plugin->description.name;
                identifier = pluginIdentifier(source.plugin->description);
            }
            std::swap(source.plugin, destination.plugin);
            publishUnlocked();
        }
        std::swap(editors_[static_cast<std::size_t>(fromChannel)][static_cast<std::size_t>(fromSlot)],
                  editors_[static_cast<std::size_t>(toChannel)][static_cast<std::size_t>(toSlot)]);
        placeEditor(fromChannel, fromSlot);
        placeEditor(toChannel, toSlot);
        if (name.isNotEmpty() || identifier.isNotEmpty())
        {
            tracePlugin(fromChannel, fromSlot, "moved-from", name, identifier);
            tracePlugin(toChannel, toSlot, "move", name, identifier);
        }
        notifyDirty();
        uiRepaint_.note(toChannel, toSlot, kRepaintMove);
        return;
    }

    juce::PluginDescription description;
    juce::MemoryBlock state;
    bool bypassed = false;
    std::shared_ptr<HostedPlugin> sourcePlugin;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        auto& source = model_[static_cast<std::size_t>(fromChannel)][static_cast<std::size_t>(fromSlot)];
        if (source.plugin == nullptr || source.plugin->instance == nullptr)
            return;
        sourcePlugin = source.plugin;
        description = source.plugin->description;
        bypassed = source.plugin->bypassed;
    }
    if (sourcePlugin->gate == nullptr || ! pauseGate(*sourcePlugin->gate))
        return;
    sourcePlugin->instance->getStateInformation(sourcePlugin->state);
    state = sourcePlugin->state;
    resumeGate(*sourcePlugin->gate);
    loadPlugin(toChannel, toSlot, description, state, bypassed, true, false, sourcePlugin->stereoFold);
}

void PluginRack::setStereoFold(int channel, int slot, int fold)
{
    if (! validSlot(channel, slot))
        return;
    const int stored = static_cast<int>(stereoFoldFromInt(fold));
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
        if (model.plugin == nullptr)
            return;
        if (model.plugin->stereoFold == stored)
            return;
        model.plugin->stereoFold = stored;
        publishUnlocked();
    }
    notifyDirty();
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

    juce::PluginDescription retry;
    juce::MemoryBlock state;
    int fold = 0;
    bool reload = false;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
        if (model.plugin == nullptr)
            return;
        // A crash-parked slot has no instance. "Turn bypass off" frees it and loads.
        if (! bypassed && model.plugin->instance == nullptr && model.error.isNotEmpty())
        {
            retry = model.plugin->description;
            state = model.plugin->state;
            fold = model.plugin->stereoFold;
            reload = true;
        }
        else
        {
            model.plugin->bypassed = bypassed;
            publishUnlocked();
        }
    }
    if (reload)
    {
        clearCrashBlock(channel, slot);
        beginLoad(channel, slot, std::move(retry), std::move(state), false, true, false, fold, 0);
        return;
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

void PluginRack::scheduleDeferredEditor(int channel, int slot)
{
    if (! validSlot(channel, slot) || quitting_)
        return;
    auto alive = alive_;
    juce::MessageManager::callAsync([this, alive, channel, slot]
    {
        if (! alive->load(std::memory_order_acquire) || quitting_)
            return;
        const bool loaderRunning = backgroundInstantiate_ != nullptr
                                   && backgroundInstantiate_->isThreadRunning();
        if (backgroundBusy_ || ! backgroundQueue_.empty() || loaderRunning)
        {
            scheduleDeferredEditor(channel, slot);
            return;
        }
        openEditor(channel, slot);
    });
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
    if (! plugin->prepared)
        return;

    for (const auto& pending : editorOpens_)
        if (pending != nullptr && pending->channel == channel && pending->slot == slot
            && ! pending->attached.load(std::memory_order_acquire)
            && ! pending->noticed.load(std::memory_order_acquire))
            return;

    std::uint64_t ticket = 0;
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        ticket = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)].ticket;
    }

    // The audio thread does not wait on the message thread. createView can
    // block for seconds. That wait stays on a worker. The plugin stays in the
    // audio callback the whole time: AirEQ, Slate, Eiosis, and Dyna-mite wait
    // inside createView until the processor has rendered, and pausing it here
    // leaves that wait stuck. Recording stays the raw input, before plugins.
    auto alive = alive_;
    auto job = std::make_shared<EditorOpen>();
    job->channel = channel;
    job->slot = slot;
    job->ticket = ticket;
    job->plugin = plugin;
    std::weak_ptr<EditorOpen> weak = job;
    job->notice = [this, alive, weak]
    {
        const auto job = weak.lock();
        if (job == nullptr || ! alive->load(std::memory_order_acquire) || quitting_)
            return;
        if (! job->finished.load(std::memory_order_acquire) && ! job->noticed.exchange(true, std::memory_order_acq_rel))
        {
            const auto reason = "plugin window did not open";
            if (settings_ != nullptr)
                appendHostLog(*settings_, juce::String(reason) + ": timed out");
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Plugin",
                                                   reason);
        }
    };
    job->deliver = [this, alive, weak]
    {
        const auto job = weak.lock();
        if (job == nullptr || ! alive->load(std::memory_order_acquire) || quitting_)
            return;
        if (job->attached.exchange(true, std::memory_order_acq_rel))
            return;
        auto* editor = job->editor;
        const auto channel = job->channel;
        const auto slot = job->slot;
        const auto plugin = job->plugin;
        if (editor == nullptr || plugin == nullptr || plugin->instance == nullptr || ! validSlot(channel, slot))
        {
            if (! job->noticed.exchange(true, std::memory_order_acq_rel))
            {
                const auto reason = "plugin window did not open";
                if (settings_ != nullptr)
                    appendHostLog(*settings_, juce::String(reason) + ": editor was not created");
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Plugin",
                                                       reason);
            }
            delete editor;
            return;
        }
        std::uint64_t nowTicket = 0;
        {
            std::lock_guard<std::mutex> lock(lifeLock_);
            const auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
            nowTicket = model.ticket;
            if (model.plugin != plugin)
                nowTicket = 0;
        }
        if (nowTicket != job->ticket || editors_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)] != nullptr)
        {
            delete editor;
            return;
        }
        if (job->createMs >= 2000ULL && settings_ != nullptr)
            appendHostLog(*settings_, "editor slow " + juce::String(static_cast<int>(job->createMs)) + " ms, audio kept running");
        const auto key = editorWindowKey(plugin->description);
        auto window = std::make_unique<EditorWindow>(
            plugin->description.name.isNotEmpty() ? plugin->description.name : "Plugin",
            [this, alive](int editorChannel, int editorSlot)
            {
                juce::MessageManager::callAsync([this, alive, editorChannel, editorSlot]
                {
                    if (! alive->load(std::memory_order_acquire))
                        return;
                    closeEditor(editorChannel, editorSlot);
                });
            },
            settings_,
            key);
        window->place(channel, slot);
        // VST3 and Audio Unit views on the Mac are both sized in points.
        // The hosted NSView is added at 1:1. No AffineTransform scales it.
        // The window is sized to the plugin view. setContentScaleFactor is not
        // how the Audio Unit is fitted, and passing the Retina backing scale
        // through it makes Vastaus redraw at the wrong size.
        window->setContentOwned(editor, true);
        window->watchContent();
        const int borderW = std::max(0, window->getWidth() - editor->getWidth());
        const int borderH = std::max(0, window->getHeight() - editor->getHeight());
        const bool audioUnit = plugin->description.pluginFormatName == "AudioUnit";
        if (audioUnit)
        {
            window->usePluginSize();
            const int width = std::max(1, editor->getWidth()) + borderW;
            const int height = std::max(1, editor->getHeight()) + borderH;
            window->centreWithSize(width, height);
        }
        else
        {
            int minWidth = 0;
            int minHeight = 0;
            int maxWidth = 0;
            int maxHeight = 0;
            if (auto* constrainer = editor->getConstrainer())
            {
                minWidth = constrainer->getMinimumWidth();
                minHeight = constrainer->getMinimumHeight();
                maxWidth = constrainer->getMaximumWidth();
                maxHeight = constrainer->getMaximumHeight();
            }
            if (minWidth > 0)
                minWidth += borderW;
            if (minHeight > 0)
                minHeight += borderH;
            if (maxWidth > 0 && maxWidth < 1000000)
                maxWidth += borderW;
            else if (editor->isResizable())
                maxWidth = 8000 + borderW;
            else
                maxWidth = 0;
            if (maxHeight > 0 && maxHeight < 1000000)
                maxHeight += borderH;
            else if (editor->isResizable())
                maxHeight = 8000 + borderH;
            else
                maxHeight = 0;
            float displayScale = 1.0f;
            int screenWidth = 0;
            int screenHeight = 0;
            if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            {
                displayScale = static_cast<float>(display->scale);
                screenWidth = juce::roundToInt(display->userBounds.getWidth());
                screenHeight = juce::roundToInt(display->userBounds.getHeight());
            }
            const auto stored = settings_ != nullptr ? settings_->loadNamedWindow(key) : juce::String();
            const auto saved = parseWindowState(stored.toStdString());
            const bool vst3 = plugin->description.pluginFormatName == "VST3";
            // A VST3 host window is the plugin view, not a larger remembered frame.
            const int fittedWidth = vst3 ? editor->getWidth() + borderW
                                         : std::max(editor->getWidth() + borderW, window->getWidth());
            const int fittedHeight = vst3 ? editor->getHeight() + borderH
                                          : std::max(editor->getHeight() + borderH, window->getHeight());
            const auto plan = planEditorWindow(fittedWidth,
                                               fittedHeight,
                                               editor->isResizable(),
                                               minWidth,
                                               minHeight,
                                               maxWidth,
                                               maxHeight,
                                               displayScale,
                                               screenWidth,
                                               screenHeight,
                                               vst3 ? SavedWindowSize {} : saved);
            window->setResizable(plan.resizable, false);
            if (plan.minWidth > 0 && plan.minHeight > 0 && plan.maxWidth >= plan.minWidth && plan.maxHeight >= plan.minHeight)
                window->setResizeLimits(plan.minWidth, plan.minHeight, plan.maxWidth, plan.maxHeight);
            if (plan.width > 0 && plan.height > 0)
            {
                if (saved.valid)
                    window->setBounds(saved.x, saved.y, plan.width, plan.height);
                else
                    window->centreWithSize(plan.width, plan.height);
            }
        }
        if (commandKeys_ != nullptr)
            window->addKeyListener(commandKeys_);
        window->setVisible(true);
        editors_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)] = std::move(window);
    };
    editorOpens_.push_back(job);
    const auto editorBegan = steadyNs();
    juce::Thread::launch([job, editorBegan]
    {
        juce::AudioProcessorEditor* editor = nullptr;
        try
        {
            if (job->plugin != nullptr && job->plugin->instance != nullptr)
                editor = job->plugin->instance->createEditorAndMakeActive();
        }
        catch (...)
        {
            editor = nullptr;
        }
        job->editor = editor;
        job->createMs = (steadyNs() - editorBegan) / 1000000ULL;
        job->finished.store(true, std::memory_order_release);
        if (auto* messages = juce::MessageManager::getInstanceWithoutCreating())
            messages->callAsync([job]
            {
                if (job->deliver)
                    job->deliver();
            });
    });
    juce::Timer::callAfterDelay(5000, [job]
    {
        if (job->notice)
            job->notice();
    });
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
    detachEditor(channel, slot);
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
        destination.note = model.note;
        // A plugin with no instance is not on screen. Crash protection does not
        // park a bypassed slot, so the button stays empty and can take any plugin.
        // Channel 1 slot 1 after "left crashed plugins off" is this case.
        destination.liveInstance = model.plugin != nullptr && model.plugin->instance != nullptr;
        if (! destination.liveInstance)
            continue;
        destination.occupied = true;
        destination.bypassed = model.plugin->bypassed;
        destination.crashParked = false;
        destination.latencySamples = model.plugin->latencySamples;
        destination.inputChannels = model.plugin->inputChannels;
        destination.outputChannels = model.plugin->outputChannels;
        destination.stereoFold = model.plugin->stereoFold;
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
                {
                    for (const auto& kept : keptInSessionFile_)
                    {
                        if (kept.channel != channel || kept.slot != slot || kept.description.name.isEmpty())
                            continue;
                        slotOut.occupied = true;
                        slotOut.bypassed = kept.bypassed;
                        slotOut.stereoFold = kept.stereoFold;
                        slotOut.description = kept.description;
                        slotOut.state = kept.state;
                        break;
                    }
                    continue;
                }
                // A parked slot has no instance. It is still written so the
                // session file keeps the plugin after "Open session without this plugin".
                const bool parkedSlot = source.plugin->instance == nullptr;
                if (parkedSlot && source.plugin->description.name.isEmpty())
                    continue;
                slotOut.occupied = true;
                slotOut.bypassed = source.plugin->bypassed;
                slotOut.stereoFold = source.plugin->stereoFold;
                slotOut.description = source.plugin->description;
                slotOut.state = source.plugin->state;
            }
        }
    }
    capturing_.store(false, std::memory_order_release);
}

void PluginRack::restoreSession(const SessionData& data)
{
    clearAll(false);
    restoring_ = true;
    ++restoreGeneration_;
    restoreQueue_.clear();
    loadCursor_ = {};
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        const auto& source = data.channels[static_cast<std::size_t>(channel)];
        {
            std::lock_guard<std::mutex> lock(lifeLock_);
            excluded_[static_cast<std::size_t>(channel)] = source.excludeFromCompensation;
            if ((channel & 1) == 0)
                stereoLink_[static_cast<std::size_t>(channel)].store(source.stereoLink ? 1 : 0, std::memory_order_relaxed);
        }
        const bool linkedRight = (channel & 1) == 1
                                 && data.channels[static_cast<std::size_t>(channel - 1)].stereoLink;
        if (linkedRight)
            continue;
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            const auto& sourceSlot = source.slots[static_cast<std::size_t>(slot)];
            if (! sourceSlot.occupied)
                continue;
            RestorePlugin item;
            item.channel = channel;
            item.slot = slot;
            item.description = sourceSlot.description;
            item.state = sourceSlot.state;
            item.bypassed = sourceSlot.bypassed;
            item.stereoFold = sourceSlot.stereoFold;
            restoreQueue_.push_back(std::move(item));
        }
    }
    loadCursor_.total = static_cast<int>(restoreQueue_.size());
    {
        std::lock_guard<std::mutex> lock(lifeLock_);
        publishUnlocked();
    }
    // Plugins start after the device is open. Instantiating here would block
    // the message thread before the audio callback is installed again.
    sessionLoadsReady_ = loadCursor_.total > 0;
    if (! sessionLoadsReady_)
        restoring_ = false;
}

void PluginRack::pumpSessionLoads()
{
    if (! sessionLoadsReady_ || quitting_)
        return;
    sessionLoadsReady_ = false;
    if (loadCursor_.total <= 0 || restoreQueue_.empty())
    {
        finishRestore();
        return;
    }
    noteSlot(-2);
    scheduleNextRestore();
}

juce::String PluginRack::pluginLoadProgress() const
{
    char text[64];
    if (! formatPluginLoadProgress(loadCursor_.finished, loadCursor_.total, loadCursor_.inFlight, text, sizeof(text)))
        return {};
    return juce::String(text);
}

void PluginRack::setPluginSlotHandler(std::function<void(int)> handler)
{
    slotHandler_ = std::move(handler);
}

void PluginRack::noteSlot(int channel)
{
    if (slotHandler_ != nullptr)
        slotHandler_(channel);
}

void PluginRack::scheduleNextRestore()
{
    if (quitting_ || restorePosted_)
        return;
    restorePosted_ = true;
    auto alive = alive_;
    const auto generation = restoreGeneration_;
    juce::Timer::callAfterDelay(1, [alive, generation, this]
    {
        if (! alive->load(std::memory_order_acquire))
            return;
        if (generation != restoreGeneration_)
            return;
        restorePosted_ = false;
        startNextRestore();
    });
}

void PluginRack::startNextRestore()
{
    if (quitting_)
        return;
    if (! loadCursor_.startOne())
    {
        if (restoreQueue_.empty())
            finishRestore();
        return;
    }
    if (restoreQueue_.empty())
    {
        loadCursor_.inFlight = false;
        finishRestore();
        return;
    }

    auto item = std::move(restoreQueue_.front());
    restoreQueue_.erase(restoreQueue_.begin());
    const auto generation = restoreGeneration_;
    const int channel = item.channel;
    enqueueChannel(channel,
                   [this, generation, item = std::move(item)]() mutable
                   {
                       beginLoad(item.channel,
                                 item.slot,
                                 item.description,
                                 item.state,
                                 item.bypassed,
                                 false,
                                 false,
                                 item.stereoFold,
                                 generation);
                   },
                   true);
}

void PluginRack::completeRestoreStep(std::uint64_t generation)
{
    if (generation == 0 || generation != restoreGeneration_)
        return;
    loadCursor_.completeOne();
    if (loadCursor_.done() || restoreQueue_.empty())
        finishRestore();
    else
        scheduleNextRestore();
}

void PluginRack::finishRestore()
{
    restoreQueue_.clear();
    loadCursor_ = {};
    sessionLoadsReady_ = false;
    restoring_ = false;
    StallClock::get().messagePhase.store(kPhaseMessage, std::memory_order_relaxed);
    noteSlot(-1);
}

void PluginRack::pumpBackgroundQueue()
{
    if (quitting_ || backgroundBusy_ || backgroundQueue_.empty())
        return;
    if (backgroundInstantiate_ != nullptr && backgroundInstantiate_->isThreadRunning())
    {
        // run() has posted its result but the thread has not exited yet.
        // The next message turn can start the following plugin.
        auto alive = alive_;
        juce::MessageManager::callAsync([this, alive]
        {
            if (! alive->load(std::memory_order_acquire))
                return;
            pumpBackgroundQueue();
        });
        return;
    }

    auto job = std::move(backgroundQueue_.front());
    backgroundQueue_.erase(backgroundQueue_.begin());
    backgroundBusy_ = true;
    jobChannel_ = job.channel;
    jobSlot_ = job.slot;
    jobTicket_ = job.ticket;
    jobGeneration_ = job.generation;
    jobMarkDirty_ = job.markDirty;
    jobBypassed_ = job.bypassed;
    jobOpen_ = job.open;
    jobStereo_ = job.stereoFold;
    jobDescription_ = std::move(job.description);
    jobState_ = std::move(job.state);
    jobRate_ = job.rate;
    jobBlock_ = job.block;
    launchBackgroundLoad();
}

void PluginRack::launchBackgroundLoad()
{
    if (backgroundInstantiate_ == nullptr)
        backgroundInstantiate_ = std::make_unique<BackgroundInstantiate>(*this);
    if (backgroundInstantiate_->startThread(juce::Thread::Priority::background))
        return;

    backgroundBusy_ = false;
    finishLoad(jobChannel_,
               jobSlot_,
               jobTicket_,
               jobMarkDirty_,
               jobBypassed_,
               jobOpen_,
               jobStereo_,
               jobDescription_,
               jobState_,
               nullptr,
               "The plugin loader did not start. See youhost.log.",
               jobGeneration_);
    pumpBackgroundQueue();
}

void PluginRack::deliverOffThreadInstance()
{
    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::String error;
    {
        std::lock_guard<std::mutex> lock(offThreadMutex_);
        instance = std::move(offThreadInstance_);
        error = std::move(offThreadError_);
    }
    finishLoad(jobChannel_,
               jobSlot_,
               jobTicket_,
               jobMarkDirty_,
               jobBypassed_,
               jobOpen_,
               jobStereo_,
               jobDescription_,
               jobState_,
               std::move(instance),
               error,
               jobGeneration_);
    backgroundBusy_ = false;
    pumpBackgroundQueue();
}

void PluginRack::setDirtyHandler(std::function<void()> handler)
{
    dirtyHandler_ = std::move(handler);
}

void PluginRack::publishUnlocked()
{
    graphPublishSerial_.fetch_add(1, std::memory_order_relaxed);
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
    int named = 0;
    auto append = [&](const char* chunk)
    {
        if (chunk == nullptr)
            return;
        while (*chunk != '\0' && used + 1 < sizeof(text))
            text[used++] = *chunk++;
    };

    const int shown = std::clamp(activeChannels_.load(std::memory_order_relaxed), 0, kMaxChannels);
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        if (! channelProcessesPlugins(channel, shown, routing_.inputPacked[static_cast<std::size_t>(channel)]))
            continue;
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            const auto& model = model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
            if (model.plugin == nullptr || model.plugin->instance == nullptr)
                continue;
            ++count;
            if (named >= 8)
                continue;
            if (named > 0)
                append(", ");
            append(model.plugin->description.name.toRawUTF8());
            ++named;
        }
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
    const int shown = std::clamp(activeChannels_.load(std::memory_order_relaxed), 0, kMaxChannels);
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        if (! channelProcessesPlugins(channel, shown, routing_.inputPacked[static_cast<std::size_t>(channel)]))
        {
            chainSamples_[static_cast<std::size_t>(channel)] = 0;
            inputs[static_cast<std::size_t>(channel)] = ChannelLatencyInput { 0, false, -1 };
            continue;
        }

        const bool linkedRight = (channel & 1) == 1
                                 && stereoLink_[static_cast<std::size_t>(channel - 1)].load(std::memory_order_relaxed) != 0;
        int chain = 0;
        for (int slot = 0; slot < kSlotsPerChannel && ! linkedRight; ++slot)
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
            live.inputChannels = std::clamp(model.plugin->inputChannels, 1, 2);
            live.outputChannels = std::clamp(model.plugin->outputChannels, 1, 2);
            live.totalInputs = std::clamp(std::max(model.plugin->totalInputs, live.inputChannels), 1, LiveGraph::kScratchCap);
            live.totalOutputs = std::clamp(std::max(model.plugin->totalOutputs, live.outputChannels), 1, LiveGraph::kScratchCap);
            live.processChannels = std::max(live.totalInputs, live.totalOutputs);
            live.stereoFold = model.plugin->stereoFold;
            live.prepared = model.plugin->prepared;
            live.vst3 = model.plugin->description.pluginFormatName == "VST3";
            copyPluginName(live.name, static_cast<int>(sizeof(live.name)), model.plugin->description.name);
            graph->keepAlive.push_back(model.plugin);
        }

        const int leftChain = channel > 0 ? chainSamples_[static_cast<std::size_t>(channel - 1)] : 0;
        chain = pairChainSamples(linkedRight, chain, leftChain);
        chainSamples_[static_cast<std::size_t>(channel)] = chain;
        const bool outputOpen = routing_.outputPacked[static_cast<std::size_t>(channel)] >= 0;
        const bool counts = outputOpen && ! excluded_[static_cast<std::size_t>(channel)] && audible_[static_cast<std::size_t>(channel)];
        inputs[static_cast<std::size_t>(channel)] = ChannelLatencyInput { chain, counts, groups_[static_cast<std::size_t>(channel)] };
        latencyInputs_[static_cast<std::size_t>(channel)] = inputs[static_cast<std::size_t>(channel)];
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

    graph->feedCapacity = block * 4;
    graph->feedIn.assign(static_cast<std::size_t>(kMaxChannels * graph->feedCapacity), 0.0f);
    graph->feedOut.assign(static_cast<std::size_t>(kMaxChannels * graph->feedCapacity), 0.0f);
    graph->blockWork.assign(static_cast<std::size_t>(kMaxChannels * block), 0.0f);
    graph->scratch.assign(static_cast<std::size_t>(kMaxChannels * kSlotsPerChannel * LiveGraph::kScratchCap * block), 0.0f);
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        graph->midi[static_cast<std::size_t>(channel)].ensureSize(256);
        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
            for (int index = 0; index < LiveGraph::kScratchCap; ++index)
                graph->scratchPtrs[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)][static_cast<std::size_t>(index)]
                    = graph->scratch.data()
                      + static_cast<std::size_t>(((channel * kSlotsPerChannel + slot) * LiveGraph::kScratchCap + index) * block);
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

void PluginRack::setBlockedSlots(const std::vector<CrashBlock>& slots)
{
    blockedSlots_ = slots;
}

void PluginRack::rememberCrashedPlugins(const juce::StringArray& identifiers)
{
    crashedWarned_ = identifiers;
}

void PluginRack::omitFromNextRestore(const std::vector<CrashBlock>& slots)
{
    restoreOmit_ = slots;
}

void PluginRack::claimSlotForManualLoad(int channel, int slot)
{
    if (! validSlot(channel, slot))
        return;
    userClaimedSlot_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)] = true;
    std::vector<CrashBlock> keptOmit;
    keptOmit.reserve(restoreOmit_.size());
    for (const auto& block : restoreOmit_)
        if (block.channel != channel || block.slot != slot)
            keptOmit.push_back(block);
    restoreOmit_.swap(keptOmit);

    int dropped = 0;
    std::vector<RestorePlugin> keptQueue;
    keptQueue.reserve(restoreQueue_.size());
    for (auto& item : restoreQueue_)
    {
        if (item.channel == channel && item.slot == slot)
        {
            ++dropped;
            continue;
        }
        keptQueue.push_back(std::move(item));
    }
    restoreQueue_.swap(keptQueue);
    if (dropped > 0)
    {
        loadCursor_.total -= dropped;
        if (loadCursor_.total < loadCursor_.finished)
            loadCursor_.total = loadCursor_.finished;
        if (loadCursor_.total < 0)
            loadCursor_.total = 0;
    }
}

bool PluginRack::crashedBefore(const juce::PluginDescription& description) const
{
    if (description.fileOrIdentifier.isNotEmpty() && crashedWarned_.contains(description.fileOrIdentifier))
        return true;
    const auto created = description.createIdentifierString();
    return created.isNotEmpty() && crashedWarned_.contains(created);
}

bool PluginRack::omittedFromThisLaunch(int channel, int slot, std::string_view identifier) const
{
    std::vector<SlotBlock> listed;
    listed.reserve(restoreOmit_.size());
    for (const auto& block : restoreOmit_)
    {
        SlotBlock item;
        item.channel = block.channel;
        item.slot = block.slot;
        item.identifier = block.identifier.toStdString();
        listed.push_back(std::move(item));
    }
    return pluginListedForThisLaunch(channel, slot, identifier, listed.data(), static_cast<int>(listed.size()));
}

void PluginRack::keepPluginInSessionFile(int channel,
                                         int slot,
                                         const juce::PluginDescription& description,
                                         const juce::MemoryBlock& state,
                                         bool bypassed,
                                         int stereoFold)
{
    std::lock_guard<std::mutex> lock(lifeLock_);
    for (auto& kept : keptInSessionFile_)
    {
        if (kept.channel != channel || kept.slot != slot)
            continue;
        kept.description = description;
        kept.state = state;
        kept.bypassed = bypassed;
        kept.stereoFold = stereoFold;
        return;
    }
    KeptSessionPlugin kept;
    kept.channel = channel;
    kept.slot = slot;
    kept.description = description;
    kept.state = state;
    kept.bypassed = bypassed;
    kept.stereoFold = stereoFold;
    keptInSessionFile_.push_back(std::move(kept));
}

void PluginRack::dropKeptPlugin(int channel, int slot)
{
    std::lock_guard<std::mutex> lock(lifeLock_);
    std::vector<KeptSessionPlugin> kept;
    kept.reserve(keptInSessionFile_.size());
    for (auto& item : keptInSessionFile_)
        if (item.channel != channel || item.slot != slot)
            kept.push_back(std::move(item));
    keptInSessionFile_.swap(kept);
}

void PluginRack::setPluginTrace(std::function<void(int, int, const juce::String&, const juce::String&, const juce::String&, const juce::String&, const juce::String&)> trace)
{
    trace_ = std::move(trace);
}

void PluginRack::releaseForQuit()
{
    quitting_ = true;
    alive_->store(false, std::memory_order_release);
    stopTimer();
    ++restoreGeneration_;
    restoreQueue_.clear();
    backgroundQueue_.clear();
    backgroundBusy_ = false;
    sessionLoadsReady_ = false;
    restorePosted_ = false;
    if (backgroundInstantiate_ != nullptr)
    {
        // The loader may be blocked in callAsync, waiting for this thread.
        // Run that job here, then the loader can exit. Modal loops are off,
        // so the message queue is not pumped from this wait.
        const auto start = juce::Time::getMillisecondCounter();
        while (backgroundInstantiate_->isThreadRunning()
               && juce::Time::getMillisecondCounter() - start < 8000)
        {
            std::shared_ptr<MessageThreadJob> job;
            {
                std::lock_guard<std::mutex> lock(offThreadMutex_);
                job = messageJob_;
            }
            completeMessageJob(job);
            if (! backgroundInstantiate_->isThreadRunning())
                break;
            juce::Thread::sleep(2);
        }
        if (backgroundInstantiate_->stopThread(200))
        {
            backgroundInstantiate_.reset();
            std::unique_ptr<juce::AudioPluginInstance> orphan;
            {
                std::lock_guard<std::mutex> lock(offThreadMutex_);
                orphan = std::move(offThreadInstance_);
            }
            releasePluginOnMessageThread(orphan.release());
        }
        else
        {
            backgroundInstantiate_.release();
        }
    }
    blockProcessing_.store(true, std::memory_order_release);
    waitUntilOutsideCallback();
    // Audio is already stopped. Editors and plugins are released here, on
    // this message thread, before the process terminates. A throw from a
    // plugin destructor must not abort the app.
    const auto previousTerminate = std::set_terminate([] { std::_Exit(0); });
    try
    {
        closeAllEditors();
    }
    catch (...)
    {
    }
    flushDeferred();
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
            try
            {
                slot.plugin.reset();
            }
            catch (...)
            {
            }
        }
    }
    published_.store(nullptr, std::memory_order_release);
    try
    {
        current_.reset();
        retired_.clear();
    }
    catch (...)
    {
    }
    std::set_terminate(previousTerminate);
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

bool PluginRack::isSlotBlocked(int channel, int slot) const
{
    for (const auto& blocked : blockedSlots_)
        if (blocked.channel == channel && blocked.slot == slot)
            return true;
    return false;
}

void PluginRack::clearCrashBlock(int channel, int slot)
{
    juce::String identifier;
    std::vector<CrashBlock> kept;
    kept.reserve(blockedSlots_.size());
    for (const auto& blocked : blockedSlots_)
    {
        if (blocked.channel == channel && blocked.slot == slot)
        {
            identifier = blocked.identifier;
            continue;
        }
        kept.push_back(blocked);
    }
    blockedSlots_.swap(kept);
    if (identifier.isEmpty())
        return;
    for (const auto& blocked : blockedSlots_)
        if (blocked.identifier == identifier)
            return;
    blocked_.removeString(identifier);
}

PluginRack::BlockPluginNote PluginRack::blockPluginNote() const noexcept
{
    return blockNote_;
}

void PluginRack::tracePlugin(int channel, int slot, const juce::String& phase, const juce::String& name, const juce::String& identifier, const juce::String& reason, const juce::String& format)
{
    if (trace_ != nullptr)
        trace_(channel, slot, phase, name, identifier, reason, format);
}

void PluginRack::removePluginNow(int channel, int slot)
{
    if (! validSlot(channel, slot))
        return;

    auto window = std::move(editors_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)]);
    if (window != nullptr)
    {
        window->quiesce();
        window->onClose_ = nullptr;
        window->setVisible(false);
    }

    juce::String name;
    juce::String identifier;
    const bool held = isSlotBlocked(channel, slot);
    std::shared_ptr<HostedPlugin> plugin;
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
        model.note.clear();
        for (auto it = keptInSessionFile_.begin(); it != keptInSessionFile_.end();)
        {
            if (it->channel == channel && it->slot == slot)
                it = keptInSessionFile_.erase(it);
            else
                ++it;
        }
        plugin = std::move(model.plugin);
        publishUnlocked();
    }
    bury(std::move(plugin), std::move(window));
    // Remove drops a remembered hold. The slot stays empty and can take any plugin.
    clearCrashBlock(channel, slot);
    if (identifier.isNotEmpty() || name.isNotEmpty())
        tracePlugin(channel, slot, "unload", name, identifier, held ? juce::String(kSlotFreedReason) : juce::String());
    notifyDirty();
}

void PluginRack::enqueueChannel(int channel, std::function<void()> work, bool asynchronous)
{
    if (channel < 0 || channel >= kMaxChannels || quitting_)
        return;
    auto& gate = channelWork_[static_cast<std::size_t>(channel)];
    gate.pending.push_back([this, channel, work = std::move(work), asynchronous]() mutable
    {
        work();
        if (! asynchronous)
            finishChannelWork(channel, 0);
    });
    pumpChannel(channel);
}

void PluginRack::pumpChannel(int channel)
{
    if (channel < 0 || channel >= kMaxChannels || quitting_)
        return;
    auto& gate = channelWork_[static_cast<std::size_t>(channel)];
    if (gate.inFlight || gate.pending.empty())
        return;
    gate.inFlight = true;
    auto work = std::move(gate.pending.front());
    gate.pending.erase(gate.pending.begin());
    work();
}

void PluginRack::finishChannelWork(int channel, std::uint64_t ticket)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    auto& gate = channelWork_[static_cast<std::size_t>(channel)];
    if (ticket != 0 && gate.ticket != ticket)
        return;
    if (! gate.inFlight)
        return;
    gate.inFlight = false;
    if (ticket != 0)
        gate.ticket = 0;
    pumpChannel(channel);
}

bool PluginRack::channelBusy(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return true;
    const auto& gate = channelWork_[static_cast<std::size_t>(channel)];
    if (gate.inFlight || ! gate.pending.empty())
        return true;
    for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        if (model_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)].loading)
            return true;
    return false;
}

void PluginRack::detachEditor(int channel, int slot)
{
    if (! validSlot(channel, slot))
        return;
    auto window = std::move(editors_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)]);
    if (window == nullptr)
        return;
    editorCloseTries_[channel][slot] = 0;
    window->onClose_ = nullptr;
    window->setVisible(false);
    bury(nullptr, std::move(window));
}

void PluginRack::placeEditor(int channel, int slot)
{
    if (! validSlot(channel, slot))
        return;
    auto& window = editors_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
    if (window != nullptr)
        window->place(channel, slot);
}

void PluginRack::bury(std::shared_ptr<HostedPlugin> plugin, std::unique_ptr<EditorWindow> window)
{
    if (plugin == nullptr && window == nullptr)
        return;
    if (deferred_ == nullptr)
        return;
    if (window != nullptr)
    {
        window->quiesce();
        window->onClose_ = nullptr;
        window->setVisible(false);
    }
    DeferredPluginRelease::Item item;
    item.plugin = std::move(plugin);
    item.window = std::move(window);
    item.hops = pluginGraveHops();
    deferred_->items.push_back(std::move(item));
    if (! quitting_)
        scheduleGraves();
}

void PluginRack::scheduleGraves()
{
    if (gravesPosted_ || quitting_)
        return;
    gravesPosted_ = true;
    auto alive = alive_;
    juce::MessageManager::callAsync([this, alive]
    {
        gravesPosted_ = false;
        if (! alive->load(std::memory_order_acquire))
            return;
        pumpGraves();
    });
}

void PluginRack::pumpGraves()
{
    if (deferred_ == nullptr)
        return;

    std::vector<DeferredPluginRelease::Item> keep;
    keep.reserve(deferred_->items.size());
    for (auto& item : deferred_->items)
    {
        const auto step = graveStep(item.hops);
        if (step == GraveStep::keep)
        {
            item.hops = graveNextHops(item.hops);
            keep.push_back(std::move(item));
            continue;
        }

        if (step == GraveStep::dropEditor)
        {
            std::shared_ptr<PluginGate> gate = item.plugin != nullptr ? item.plugin->gate : nullptr;
            if (item.window != nullptr && gate != nullptr && ! pauseGate(*gate))
            {
                keep.push_back(std::move(item));
                continue;
            }
            if (item.window != nullptr)
                item.window->quiesce();
            item.window.reset();
            if (gate != nullptr)
                resumeGate(*gate);
            item.hops = 0;
            keep.push_back(std::move(item));
            continue;
        }

        item.window.reset();
        item.plugin.reset();
    }
    deferred_->items.swap(keep);
    if (! deferred_->items.empty())
        scheduleGraves();
}

void PluginRack::scheduleCondemn()
{
    if (condemnPosted_ || quitting_)
        return;
    condemnPosted_ = true;
    auto alive = alive_;
    juce::MessageManager::callAsync([this, alive]
    {
        condemnPosted_ = false;
        if (! alive->load(std::memory_order_acquire))
            return;
        pumpCondemned();
    });
}

void PluginRack::pumpCondemned()
{
    std::vector<CondemnedInstance> keep;
    keep.reserve(condemned_.size());
    for (auto& item : condemned_)
    {
        if (item.instance == nullptr)
            continue;
        if (item.hops > 0)
        {
            item.hops = graveNextHops(item.hops);
            if (item.hops > 0)
            {
                keep.push_back(item);
                continue;
            }
        }
        auto* dying = item.instance;
        item.instance = nullptr;
        dying->removeListener(this);
        // An Audio Unit is released on this message thread after the grave
        // hops, so a plugin timer cannot run into freed memory.
        // releaseInstanceOffMessageThread is not used for that release.
        releasePluginOnMessageThread(dying);
    }
    condemned_.swap(keep);
    if (! condemned_.empty())
        scheduleCondemn();
}

void PluginRack::flushDeferred()
{
    quitting_ = true;
    if (deferred_ != nullptr)
    {
        for (auto& item : deferred_->items)
            item.window.reset();
        deferred_->items.clear();
    }
    for (auto& item : condemned_)
    {
        if (item.instance == nullptr)
            continue;
        auto* dying = item.instance;
        item.instance = nullptr;
        dying->removeListener(this);
        // Session unload uses releaseInstanceOffMessageThread. Quit deletes
        // here, on the message thread, before the process terminates.
        releasePluginOnMessageThread(dying);
    }
    condemned_.clear();
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

    if (rack != nullptr && rack->quitting_)
    {
        instance->removeListener(rack);
        // Quit stays on this message thread. A session unload still goes
        // through releaseInstanceOffMessageThread, because an Audio Unit
        // destructor can dispatch_sync back here and must not run here.
        releasePluginOnMessageThread(instance);
        return;
    }

    if (rack == nullptr)
    {
        // Already on the message thread. releaseInstanceOffMessageThread would
        // free an Audio Unit while its timer is still queued here.
        juce::ignoreUnused(&releaseInstanceOffMessageThread);
        releasePluginOnMessageThread(instance);
        return;
    }

    CondemnedInstance condemned;
    condemned.instance = instance;
    condemned.hops = pluginGraveHops();
    rack->condemned_.push_back(condemned);
    rack->scheduleCondemn();
}

} // namespace youhost
