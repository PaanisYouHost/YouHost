#include "PluginBridge.h"
#include "QuitGuard.h"

#include <cstring>
#include <thread>

#if defined(__APPLE__) || defined(__linux__)
 #include <fcntl.h>
 #include <sys/mman.h>
 #include <unistd.h>
#endif

namespace youhost
{
namespace
{

juce::File protectRoot()
{
    return juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("youhost-protect-" + juce::String(juce::Time::currentTimeMillis()) + "-"
                      + juce::String(juce::Random::getSystemRandom().nextInt()));
}

bool writeRequest(const juce::File& directory,
                  const juce::PluginDescription& description,
                  double sampleRate,
                  int blockSize,
                  int channels,
                  bool proof)
{
    juce::XmlElement request("YouHostProtect");
    request.setAttribute("file", description.fileOrIdentifier);
    request.setAttribute("format", description.pluginFormatName);
    request.setAttribute("name", description.name);
    request.setAttribute("rate", sampleRate);
    request.setAttribute("block", blockSize > 0 ? blockSize : 32);
    request.setAttribute("channels", channels > 0 ? channels : 2);
    request.setAttribute("proof", proof ? 1 : 0);
    request.setAttribute("ms", kProofAudioMs);
    return request.writeTo(directory.getChildFile("request.xml"));
}

struct MappedRing
{
    ProtectRing* ring = nullptr;
    int fd = -1;
    void* memory = nullptr;

    ~MappedRing()
    {
#if defined(__APPLE__) || defined(__linux__)
        if (memory != nullptr)
            ::munmap(memory, sizeof(ProtectRing));
        if (fd >= 0)
            ::close(fd);
#else
        delete ring;
#endif
    }
};

std::unique_ptr<MappedRing> mapRing(const juce::File& file, bool initialise)
{
    auto mapped = std::make_unique<MappedRing>();
#if defined(__APPLE__) || defined(__linux__)
    mapped->fd = ::open(file.getFullPathName().toRawUTF8(), O_RDWR | O_CREAT, 0600);
    if (mapped->fd < 0)
        return nullptr;
    if (::ftruncate(mapped->fd, static_cast<off_t>(sizeof(ProtectRing))) != 0)
        return nullptr;
    mapped->memory = ::mmap(nullptr, sizeof(ProtectRing), PROT_READ | PROT_WRITE, MAP_SHARED, mapped->fd, 0);
    if (mapped->memory == MAP_FAILED)
    {
        mapped->memory = nullptr;
        return nullptr;
    }
    if (initialise)
        mapped->ring = new (mapped->memory) ProtectRing;
    else
        mapped->ring = static_cast<ProtectRing*>(mapped->memory);
#else
    juce::ignoreUnused(file, initialise);
    mapped->ring = new ProtectRing;
    mapped->memory = mapped->ring;
#endif
    return mapped;
}

bool spawnHelper(juce::ChildProcess& process, const juce::File& directory)
{
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::StringArray args;
    args.add(executable.getFullPathName());
    args.add("--YouHostProtect:dir:" + directory.getFullPathName());
    return process.start(args);
}

bool waitForFile(const juce::File& file, juce::ChildProcess& process, int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    while (juce::Time::getMillisecondCounter() < deadline)
    {
        if (file.existsAsFile())
            return true;
        if (! process.isRunning())
            return false;
        juce::Thread::sleep(20);
    }
    return file.existsAsFile();
}

ProofResult proofFromProcess(juce::ChildProcess& process, const juce::File& directory, bool resultOk)
{
    if (resultOk)
        return ProofResult::passed;
    if (directory.getChildFile("quit-request").existsAsFile())
        return ProofResult::quit;
    if (! process.isRunning())
        return ProofResult::crashed;
    return ProofResult::timedOut;
}

class ProtectServer : private juce::Thread
{
public:
    explicit ProtectServer(juce::File directory)
        : juce::Thread("YouHostProtect"),
          directory_(std::move(directory))
    {
    }

    void begin()
    {
        startThread();
    }

    ~ProtectServer() override
    {
        signalThreadShouldExit();
        stopThread(2000);
    }

    void run() override
    {
        auto request = juce::XmlDocument::parse(directory_.getChildFile("request.xml"));
        if (request == nullptr)
        {
            finish(false, "missing request");
            return;
        }

        auto mapped = mapRing(directory_.getChildFile("ring.bin"), false);
        if (mapped == nullptr || mapped->ring == nullptr)
        {
            finish(false, "missing audio ring");
            return;
        }
        ring_ = mapped->ring;
        mapped_ = std::move(mapped);

        juce::AudioPluginFormatManager formats;
        juce::addDefaultFormatsToManager(formats);
        juce::PluginDescription description;
        description.fileOrIdentifier = request->getStringAttribute("file");
        description.pluginFormatName = request->getStringAttribute("format");
        description.name = request->getStringAttribute("name");
        const double rate = request->getDoubleAttribute("rate", 48000.0);
        const int block = request->getIntAttribute("block", 32);
        const bool proof = request->getIntAttribute("proof", 0) != 0;
        const int proofMs = request->getIntAttribute("ms", kProofAudioMs);

        juce::String createError;
        instance_ = formats.createPluginInstance(description, rate, block, createError);
        if (instance_ == nullptr)
        {
            finish(false, createError.isNotEmpty() ? createError : juce::String("instantiate failed"));
            return;
        }
        instance_->prepareToPlay(rate, block > 0 ? block : 32);
        const auto stateFile = directory_.getChildFile("state.bin");
        if (stateFile.existsAsFile())
        {
            juce::MemoryBlock state;
            if (stateFile.loadFileAsData(state) && state.getSize() > 0)
                instance_->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }
        sampleRate_ = rate;
        block_ = block > 0 ? block : 32;

        std::atomic<int> editorDone { 0 };
        if (auto* messages = juce::MessageManager::getInstanceWithoutCreating())
        {
            messages->callAsync([this, &editorDone]
            {
                try
                {
                    if (instance_ != nullptr && instance_->hasEditor())
                        editor_.reset(instance_->createEditorIfNeeded());
                }
                catch (...)
                {
                    editor_.reset();
                }
                if (editor_ != nullptr)
                {
                    window_ = std::make_unique<juce::DocumentWindow>(instance_->getName(), juce::Colours::black, juce::DocumentWindow::closeButton);
                    window_->setUsingNativeTitleBar(true);
                    window_->setContentOwned(editor_.release(), true);
                    window_->centreWithSize(window_->getWidth(), window_->getHeight());
                    window_->setVisible(true);
                }
                editorDone.store(1, std::memory_order_release);
            });
        }
        else
        {
            editorDone.store(1, std::memory_order_release);
        }
        const auto editorDeadline = juce::Time::getMillisecondCounter() + 8000u;
        while (editorDone.load(std::memory_order_acquire) == 0 && juce::Time::getMillisecondCounter() < editorDeadline)
            juce::Thread::sleep(10);

        if (proof)
        {
            const int total = static_cast<int>(sampleRate_ * static_cast<double>(proofMs) / 1000.0);
            juce::AudioBuffer<float> audio(2, block_);
            juce::MidiBuffer midi;
            int done = 0;
            while (done < total && ! threadShouldExit())
            {
                audio.clear();
                midi.clear();
                instance_->processBlock(audio, midi);
                done += block_;
            }
            juce::MemoryBlock state;
            instance_->getStateInformation(state);
            stateFile.replaceWithData(state.getData(), state.getSize());
            finish(true, {});
            return;
        }

        ring_->outCount.store(ring_->inCount.load(std::memory_order_acquire), std::memory_order_release);
        ring_->alive.store(1, std::memory_order_release);
        directory_.getChildFile("ready").replaceWithText("1");

        while (! threadShouldExit() && ! directory_.getChildFile("stop").existsAsFile())
        {
            if (directory_.getChildFile("show-editor").existsAsFile())
            {
                directory_.getChildFile("show-editor").deleteFile();
                if (auto* messages = juce::MessageManager::getInstanceWithoutCreating())
                    messages->callAsync([this]
                    {
                        if (window_ != nullptr)
                            window_->setVisible(true);
                    });
            }
            const int consumed = protectConsume(*ring_, block_, [this](const float* inL, const float* inR, float* outL, float* outR, int count)
            {
                if (instance_ == nullptr || count <= 0)
                    return;
                juce::AudioBuffer<float> audio(2, count);
                juce::MidiBuffer midi;
                for (int index = 0; index < count; ++index)
                {
                    audio.setSample(0, index, inL != nullptr ? inL[index] : 0.0f);
                    audio.setSample(1, index, inR != nullptr ? inR[index] : 0.0f);
                }
                instance_->processBlock(audio, midi);
                for (int index = 0; index < count; ++index)
                {
                    if (outL != nullptr)
                        outL[index] = audio.getSample(0, index);
                    if (outR != nullptr)
                        outR[index] = audio.getSample(1, index);
                }
            });
            if (consumed == 0)
            {
                juce::Thread::sleep(1);
            }
            else
            {
                servedSamples_ += consumed;
                if (servedSamples_ - lastStateSamples_ > static_cast<int>(sampleRate_ * 3.0))
                {
                    lastStateSamples_ = servedSamples_;
                    juce::MemoryBlock state;
                    instance_->getStateInformation(state);
                    auto temporary = directory_.getChildFile("state.tmp");
                    temporary.replaceWithData(state.getData(), state.getSize());
                    temporary.moveFileTo(directory_.getChildFile("state.bin"));
                }
            }
        }
        ring_->alive.store(0, std::memory_order_release);
        finish(true, {});
    }

private:
    void finish(bool ok, const juce::String& reason)
    {
        juce::XmlElement result("YouHostProtectResult");
        result.setAttribute("ok", ok ? 1 : 0);
        result.setAttribute("reason", reason);
        result.writeTo(directory_.getChildFile("result.xml"));
        allowProcessExit();
        if (auto* messages = juce::MessageManager::getInstanceWithoutCreating())
            messages->callAsync([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    }

    juce::File directory_;
    ProtectRing* ring_ = nullptr;
    std::unique_ptr<MappedRing> mapped_;
    std::unique_ptr<juce::AudioPluginInstance> instance_;
    std::unique_ptr<juce::AudioProcessorEditor> editor_;
    std::unique_ptr<juce::DocumentWindow> window_;
    double sampleRate_ = 48000.0;
    int block_ = 32;
    int servedSamples_ = 0;
    int lastStateSamples_ = 0;
};

ProtectServer* server = nullptr;

} // namespace

ProtectBridge::ProtectBridge(const juce::PluginDescription& description,
                             const juce::MemoryBlock& state,
                             double sampleRate,
                             int blockSize,
                             int channels)
    : description_(description),
      state_(state),
      sampleRate_(sampleRate),
      blockSize_(blockSize),
      channels_(channels)
{
}

ProtectBridge::~ProtectBridge()
{
    directory_.getChildFile("stop").replaceWithText("1");
    if (ring_ != nullptr)
        ring_->alive.store(0, std::memory_order_release);
    process_.kill();
#if defined(__APPLE__) || defined(__linux__)
    if (mapped_ != nullptr)
        ::munmap(mapped_, mapBytes_);
    if (mapFd_ >= 0)
        ::close(mapFd_);
#endif
}

bool ProtectBridge::launch(bool)
{
    directory_ = protectRoot();
    directory_.createDirectory();
    directory_.getChildFile("quit-request").deleteFile();
    if (state_.getSize() > 0)
        directory_.getChildFile("state.bin").replaceWithData(state_.getData(), state_.getSize());
    if (! writeRequest(directory_, description_, sampleRate_, blockSize_, channels_, false))
        return false;

    const auto ringFile = directory_.getChildFile("ring.bin");
#if defined(__APPLE__) || defined(__linux__)
    mapFd_ = ::open(ringFile.getFullPathName().toRawUTF8(), O_RDWR | O_CREAT, 0600);
    if (mapFd_ < 0)
        return false;
    mapBytes_ = sizeof(ProtectRing);
    if (::ftruncate(mapFd_, static_cast<off_t>(mapBytes_)) != 0)
        return false;
    mapped_ = ::mmap(nullptr, mapBytes_, PROT_READ | PROT_WRITE, MAP_SHARED, mapFd_, 0);
    if (mapped_ == MAP_FAILED)
    {
        mapped_ = nullptr;
        return false;
    }
    ring_ = new (mapped_) ProtectRing;
#else
    ring_ = new ProtectRing;
    mapped_ = ring_;
    mapBytes_ = sizeof(ProtectRing);
#endif
    ring_->alive.store(0, std::memory_order_release);
    if (! spawnHelper(process_, directory_))
        return false;
    return true;
}

std::shared_ptr<ProtectBridge> ProtectBridge::open(const juce::PluginDescription& description,
                                                   const juce::MemoryBlock& state,
                                                   double sampleRate,
                                                   int blockSize,
                                                   int channels)
{
    auto bridge = std::shared_ptr<ProtectBridge>(new ProtectBridge(description, state, sampleRate, blockSize, channels));
    if (! bridge->launch(false))
        return nullptr;
    const bool ready = waitForFile(bridge->directory_.getChildFile("ready"), bridge->process_, 15000);
    if (! ready)
    {
        bridge->markDead();
        return bridge;
    }
    bridge->ready_.store(1, std::memory_order_release);
    bridge->alive_.store(1, std::memory_order_release);
    if (bridge->ring_ != nullptr)
        bridge->ring_->alive.store(1, std::memory_order_release);
    return bridge;
}

void ProtectBridge::markDead() noexcept
{
    alive_.store(0, std::memory_order_release);
    ready_.store(0, std::memory_order_release);
    if (ring_ != nullptr)
        ring_->alive.store(0, std::memory_order_release);
}

void ProtectBridge::restart()
{
    if (restarting_ || restarts_ > 4)
        return;
    restarting_ = true;
    ++restarts_;
    markDead();
    process_.kill();
    directory_.getChildFile("stop").deleteFile();
    directory_.getChildFile("ready").deleteFile();
    directory_.getChildFile("result.xml").deleteFile();
    if (state_.getSize() > 0)
        directory_.getChildFile("state.bin").replaceWithData(state_.getData(), state_.getSize());
    if (ring_ != nullptr)
        ring_->outCount.store(ring_->inCount.load(std::memory_order_acquire), std::memory_order_release);
    if (! spawnHelper(process_, directory_))
    {
        restarting_ = false;
        return;
    }
    restarting_ = false;
}

void ProtectBridge::render(float* left, float* right, int samples) noexcept
{
    if (ring_ == nullptr || samples <= 0)
    {
        if (left != nullptr && samples > 0)
            std::memset(left, 0, sizeof(float) * static_cast<std::size_t>(samples));
        if (right != nullptr && samples > 0)
            std::memset(right, 0, sizeof(float) * static_cast<std::size_t>(samples));
        return;
    }
    const bool bypass = bypass_.load(std::memory_order_relaxed) != 0;
    const bool helperAlive = alive_.load(std::memory_order_acquire) != 0;
    const auto pull = renderProtectedBlock(*ring_, safety_, left, right, left, right, samples, seenGeneration_, helperAlive, bypass);
    if (pull.silenced > 0)
        mutedSamples_.fetch_add(static_cast<std::uint64_t>(pull.silenced), std::memory_order_relaxed);
}

void ProtectBridge::requestEditor() noexcept
{
    if (directory_.getFullPathName().isNotEmpty())
        directory_.getChildFile("show-editor").replaceWithText("1");
}

void ProtectBridge::setBypass(bool bypass) noexcept
{
    bypass_.store(bypass ? 1 : 0, std::memory_order_release);
}

bool ProtectBridge::isAlive() const noexcept
{
    return alive_.load(std::memory_order_acquire) != 0;
}

bool ProtectBridge::isReady() const noexcept
{
    return ready_.load(std::memory_order_acquire) != 0;
}

double ProtectBridge::mutedSeconds(double sampleRate) const noexcept
{
    if (sampleRate <= 0.0)
        return 0.0;
    return static_cast<double>(mutedSamples_.load(std::memory_order_relaxed)) / sampleRate;
}

juce::MemoryBlock ProtectBridge::latestState() const
{
    juce::MemoryBlock state;
    const auto file = directory_.getChildFile("state.bin");
    if (file.existsAsFile())
        file.loadFileAsData(state);
    if (state.getSize() == 0)
        state = state_;
    return state;
}

void ProtectBridge::poll()
{
    const auto state = latestState();
    if (state.getSize() > 0)
        state_ = state;
    if (process_.isRunning())
    {
        sawRunning_ = true;
        if (directory_.getChildFile("ready").existsAsFile())
        {
            ready_.store(1, std::memory_order_release);
            alive_.store(1, std::memory_order_release);
            if (ring_ != nullptr)
                ring_->alive.store(1, std::memory_order_release);
        }
        return;
    }
    if (! sawRunning_)
        return;
    sawRunning_ = false;
    markDead();
    if (restarts_ <= 4)
        restart();
}

ProofResult runPluginProof(const juce::PluginDescription& description, double sampleRate, int blockSize, int channels)
{
    const auto directory = protectRoot();
    directory.createDirectory();
    if (! writeRequest(directory, description, sampleRate, blockSize, channels, true))
    {
        directory.deleteRecursively();
        return ProofResult::workerFailed;
    }
    auto mapped = mapRing(directory.getChildFile("ring.bin"), true);
    if (mapped == nullptr)
    {
        directory.deleteRecursively();
        return ProofResult::workerFailed;
    }
    juce::ChildProcess process;
    if (! spawnHelper(process, directory))
    {
        directory.deleteRecursively();
        return ProofResult::workerFailed;
    }
    const int waitMs = kProofAudioMs + 15000;
    const bool sawResult = waitForFile(directory.getChildFile("result.xml"), process, waitMs);
    bool ok = false;
    if (sawResult)
        if (auto xml = juce::XmlDocument::parse(directory.getChildFile("result.xml")))
            ok = xml->getBoolAttribute("ok", false);
    const auto result = proofFromProcess(process, directory, ok);
    process.kill();
    directory.deleteRecursively();
    return result;
}

bool isProtectWorkerCommandLine(const juce::String& commandLine)
{
    if (commandLine.contains("--YouHostProtect:"))
        return true;
    const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
    for (const auto& argument : args)
        if (argument.contains("YouHostProtect:"))
            return true;
    return false;
}

void runProtectWorker(const juce::String& commandLine)
{
    juce::File directory;
    const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
    for (const auto& argument : args)
        if (argument.startsWith("--YouHostProtect:dir:"))
            directory = juce::File(argument.fromFirstOccurrenceOf("--YouHostProtect:dir:", false, false));
    if (directory.getFullPathName().isEmpty())
    {
        const auto marker = commandLine.fromFirstOccurrenceOf("--YouHostProtect:dir:", false, false).trim();
        directory = juce::File(marker.upToFirstOccurrenceOf(" ", false, false));
    }
    noteQuitMarkDirectory(directory.getFullPathName().toRawUTF8());
    server = new ProtectServer(directory);
    server->begin();
}

} // namespace youhost
