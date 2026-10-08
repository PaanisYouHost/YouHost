#include "PluginCatalogue.h"
#include "AppSettings.h"

namespace youhost
{
namespace
{

constexpr int kScanTimeoutMs = 45000;

juce::String textFromBlock(const juce::MemoryBlock& block)
{
    if (block.getSize() == 0 || block.getData() == nullptr)
        return {};
    return juce::String(static_cast<const char*>(block.getData()), block.getSize());
}

juce::MemoryBlock blockFromXml(const juce::XmlElement& xml)
{
    const auto text = xml.toString();
    return juce::MemoryBlock(text.toRawUTF8(), static_cast<size_t>(text.getNumBytesAsUTF8()));
}

enum class ScanOutcome
{
    ok,
    failed,
    unavailable
};

} // namespace

bool isScanWorkerCommandLine(const juce::String& commandLine)
{
    return commandLine.contains("--YouHostScan:");
}

ScanWorker::ScanWorker()
    : juce::Thread("YouHostScan")
{
    juce::addDefaultFormatsToManager(formats_);
    startThread();
}

ScanWorker::~ScanWorker()
{
    signalThreadShouldExit();
    jobEvent_.signal();
    stopThread(4000);
}

void ScanWorker::handleMessageFromCoordinator(const juce::MemoryBlock& block)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    job_ = block;
    jobPending_ = true;
    jobEvent_.signal();
}

void ScanWorker::handleConnectionLost()
{
    signalThreadShouldExit();
    jobEvent_.signal();
    if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating())
        messageManager->stopDispatchLoop();
}

juce::AudioPluginFormat* ScanWorker::formatByName(const juce::String& name)
{
    for (int index = 0; index < formats_.getNumFormats(); ++index)
        if (auto* format = formats_.getFormat(index))
            if (format->getName() == name)
                return format;
    return nullptr;
}

void ScanWorker::run()
{
    while (! threadShouldExit())
    {
        if (! jobEvent_.wait(200))
            continue;

        juce::MemoryBlock job;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (! jobPending_)
                continue;
            job.swapWith(job_);
            jobPending_ = false;
        }

        auto request = juce::parseXML(textFromBlock(job));
        juce::XmlElement reply("YouHostScanResult");
        reply.setAttribute("ok", 0);

        if (request != nullptr && request->hasTagName("YouHostScan"))
        {
            const auto file = request->getStringAttribute("file");
            if (auto* format = formatByName(request->getStringAttribute("format")))
            {
                juce::OwnedArray<juce::PluginDescription> found;
                bool ok = false;
                try
                {
                    format->findAllTypesForFile(found, file);
                    ok = true;
                }
                catch (...)
                {
                    ok = false;
                }

                reply.setAttribute("ok", ok ? 1 : 0);
                for (auto* description : found)
                    if (description != nullptr)
                        if (auto xml = description->createXml())
                            reply.addChildElement(xml.release());
            }
        }

        sendMessageToCoordinator(blockFromXml(reply));
    }
}

// Coordinator and scanner stay in this file. The known-list owns the scanner.
class PluginCatalogue::Coordinator : public juce::ChildProcessCoordinator
{
public:
    ~Coordinator() override
    {
        shutdown();
    }

    void shutdown()
    {
        std::shared_ptr<Pending> pending;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            workerAlive_ = false;
            pending = std::move(pending_);
        }
        killWorkerProcess();
        if (pending != nullptr)
        {
            pending->failed = true;
            pending->event.signal();
        }
    }

    ScanOutcome scanFile(juce::AudioPluginFormat& format,
                         juce::OwnedArray<juce::PluginDescription>& result,
                         const juce::String& file,
                         const std::atomic<bool>& cancel)
    {
        if (cancel.load(std::memory_order_relaxed))
            return ScanOutcome::ok;

        if (! ensureWorker())
            return ScanOutcome::unavailable;

        auto pending = std::make_shared<Pending>();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            pending_ = pending;
        }

        juce::XmlElement request("YouHostScan");
        request.setAttribute("format", format.getName());
        request.setAttribute("file", file);
        if (! sendMessageToWorker(blockFromXml(request)))
        {
            noteDead();
            return ScanOutcome::unavailable;
        }

        if (! pending->event.wait(kScanTimeoutMs) || cancel.load(std::memory_order_relaxed))
        {
            const bool cancelled = cancel.load(std::memory_order_relaxed);
            noteDead();
            return cancelled ? ScanOutcome::ok : ScanOutcome::failed;
        }

        if (pending->failed)
            return ScanOutcome::failed;

        for (auto* description : pending->found)
            if (description != nullptr)
                result.add(new juce::PluginDescription(*description));
        return pending->ok ? ScanOutcome::ok : ScanOutcome::failed;
    }

    void handleMessageFromWorker(const juce::MemoryBlock& block) override
    {
        std::shared_ptr<Pending> pending;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            pending = pending_;
        }
        if (pending == nullptr)
            return;

        if (auto xml = juce::parseXML(textFromBlock(block)))
        {
            pending->ok = xml->getBoolAttribute("ok", false);
            for (auto* child : xml->getChildIterator())
            {
                auto description = std::make_unique<juce::PluginDescription>();
                if (description->loadFromXml(*child))
                    pending->found.add(description.release());
            }
        }
        else
        {
            pending->failed = true;
        }

        pending->event.signal();
    }

    void handleConnectionLost() override
    {
        std::shared_ptr<Pending> pending;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            workerAlive_ = false;
            pending = std::move(pending_);
        }
        if (pending != nullptr)
        {
            pending->failed = true;
            pending->event.signal();
        }
    }

private:
    struct Pending
    {
        juce::WaitableEvent event;
        juce::OwnedArray<juce::PluginDescription> found;
        bool ok = false;
        bool failed = false;
    };

    bool ensureWorker()
    {
        if (workerAlive_)
            return true;

        const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        workerAlive_ = launchWorkerProcess(executable, "YouHostScan", 10000);
        return workerAlive_;
    }

    void noteDead()
    {
        std::shared_ptr<Pending> pending;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            workerAlive_ = false;
            pending = std::move(pending_);
        }
        killWorkerProcess();
        if (pending != nullptr)
        {
            pending->failed = true;
            pending->event.signal();
        }
    }

    std::mutex mutex_;
    std::shared_ptr<Pending> pending_;
    bool workerAlive_ = false;
};

class PluginCatalogue::Scanner : public juce::KnownPluginList::CustomScanner
{
public:
    Scanner(Coordinator& coordinator, std::atomic<bool>& cancel, std::atomic<bool>& broken)
        : coordinator_(coordinator),
          cancel_(cancel),
          broken_(broken)
    {
    }

    bool findPluginTypesFor(juce::AudioPluginFormat& format,
                            juce::OwnedArray<juce::PluginDescription>& result,
                            const juce::String& fileOrIdentifier) override
    {
        if (cancel_.load(std::memory_order_relaxed) || shouldExit())
            return true;

        const auto outcome = coordinator_.scanFile(format, result, fileOrIdentifier, cancel_);
        if (outcome == ScanOutcome::unavailable)
        {
            broken_.store(true, std::memory_order_relaxed);
            return true;
        }
        if (cancel_.load(std::memory_order_relaxed))
            return true;
        return outcome == ScanOutcome::ok;
    }

    void scanFinished() override
    {
        coordinator_.shutdown();
    }

private:
    Coordinator& coordinator_;
    std::atomic<bool>& cancel_;
    std::atomic<bool>& broken_;
};

class PluginCatalogue::DirectoryScanThread : public juce::Thread
{
public:
    explicit DirectoryScanThread(PluginCatalogue& owner)
        : juce::Thread("YouHostPluginScan"),
          owner_(owner)
    {
    }

    void run() override
    {
        owner_.runScan();
    }

private:
    PluginCatalogue& owner_;
};

PluginCatalogue::PluginCatalogue(AppSettings& settings)
    : settings_(settings),
      coordinator_(std::make_unique<Coordinator>())
{
    juce::addDefaultFormatsToManager(formats_);
    list_.setCustomScanner(std::make_unique<Scanner>(*coordinator_, cancel_, workerBroken_));

    pedalFile_ = settings_.supportDirectory().getChildFile("plugin-scan-pedal.txt");
    pedalFile_.getParentDirectory().createDirectory();

    if (auto saved = settings_.loadKnownPlugins())
        list_.recreateFromXml(*saved);

    knownCount_.store(list_.getNumTypes(), std::memory_order_relaxed);
    failedCount_.store(list_.getBlacklistedFiles().size(), std::memory_order_relaxed);
    if (knownCount_.load(std::memory_order_relaxed) > 0)
        setStatus(juce::String(knownCount_.load(std::memory_order_relaxed)) + " plugins remembered", false);
}

PluginCatalogue::~PluginCatalogue()
{
    cancel_.store(true, std::memory_order_relaxed);
    if (coordinator_ != nullptr)
        coordinator_->shutdown();
    if (scanThread_ != nullptr)
    {
        scanThread_->signalThreadShouldExit();
        scanThread_->stopThread(8000);
    }
    flushSave();
}

void PluginCatalogue::startIfEmpty()
{
    if (list_.getNumTypes() == 0 && ! scanning_.load(std::memory_order_relaxed))
        scanNew();
}

void PluginCatalogue::scanNew()
{
    if (scanning_.load(std::memory_order_relaxed))
        return;

    cancel_.store(false, std::memory_order_relaxed);
    workerBroken_.store(false, std::memory_order_relaxed);
    rescanKnown_.store(false, std::memory_order_relaxed);
    scanning_.store(true, std::memory_order_relaxed);
    setStatus("Starting scan", true);

    if (scanThread_ != nullptr)
        scanThread_->stopThread(1000);
    scanThread_ = std::make_unique<DirectoryScanThread>(*this);
    scanThread_->startThread();
}

void PluginCatalogue::rescan()
{
    if (scanning_.load(std::memory_order_relaxed))
        return;

    cancel_.store(false, std::memory_order_relaxed);
    workerBroken_.store(false, std::memory_order_relaxed);
    rescanKnown_.store(true, std::memory_order_relaxed);
    scanning_.store(true, std::memory_order_relaxed);
    setStatus("Rescanning plugins", true);

    if (scanThread_ != nullptr)
        scanThread_->stopThread(1000);
    scanThread_ = std::make_unique<DirectoryScanThread>(*this);
    scanThread_->startThread();
}

void PluginCatalogue::clearFailedAndScan()
{
    if (scanning_.load(std::memory_order_relaxed))
        return;

    list_.clearBlacklistedFiles();
    failedCount_.store(0, std::memory_order_relaxed);
    requestSave();
    scanNew();
}

juce::Array<juce::PluginDescription> PluginCatalogue::types() const
{
    return list_.getTypes();
}

juce::StringArray PluginCatalogue::failedFiles() const
{
    return list_.getBlacklistedFiles();
}

CatalogueStatus PluginCatalogue::status() const
{
    CatalogueStatus status;
    {
        const std::lock_guard<std::mutex> lock(statusMutex_);
        status.text = statusText_;
    }
    status.scanning = scanning_.load(std::memory_order_relaxed);
    status.known = knownCount_.load(std::memory_order_relaxed);
    status.failed = failedCount_.load(std::memory_order_relaxed);
    status.token = token_.load(std::memory_order_relaxed);
    return status;
}

void PluginCatalogue::flushSave()
{
    if (! listDirty_.exchange(false, std::memory_order_relaxed))
        return;

    std::shared_ptr<juce::XmlElement> xml;
    {
        const std::lock_guard<std::mutex> lock(saveMutex_);
        xml = pendingXml_;
    }
    if (xml != nullptr)
        settings_.saveKnownPlugins(xml.get());
}

void PluginCatalogue::createInstanceAsync(const juce::PluginDescription& description,
                                          double sampleRate,
                                          int blockSize,
                                          juce::AudioPluginFormat::PluginCreationCallback callback)
{
    formats_.createPluginInstanceAsync(description, sampleRate, blockSize, std::move(callback));
}

void PluginCatalogue::runScan()
{
    const bool rescan = rescanKnown_.load(std::memory_order_relaxed);
    for (int index = 0; index < formats_.getNumFormats(); ++index)
    {
        if (cancel_.load(std::memory_order_relaxed) || workerBroken_.load(std::memory_order_relaxed))
            break;
        if (scanThread_ != nullptr && scanThread_->threadShouldExit())
            break;

        auto* format = formats_.getFormat(index);
        if (format == nullptr)
            continue;

        juce::PluginDirectoryScanner scanner(list_,
                                             *format,
                                             format->getDefaultLocationsToSearch(),
                                             true,
                                             pedalFile_,
                                             true);
        while (! cancel_.load(std::memory_order_relaxed)
               && ! workerBroken_.load(std::memory_order_relaxed)
               && (scanThread_ == nullptr || ! scanThread_->threadShouldExit()))
        {
            const auto next = scanner.getNextPluginFileThatWillBeScanned();
            if (next.isNotEmpty())
                setStatus("Scanning " + format->getNameOfPluginFromIdentifier(next), true);

            juce::String scanned;
            if (! scanner.scanNextFile(! rescan, scanned))
                break;

            knownCount_.store(list_.getNumTypes(), std::memory_order_relaxed);
            failedCount_.store(list_.getBlacklistedFiles().size(), std::memory_order_relaxed);
        }
    }

    list_.scanFinished();
    knownCount_.store(list_.getNumTypes(), std::memory_order_relaxed);
    failedCount_.store(list_.getBlacklistedFiles().size(), std::memory_order_relaxed);
    scanning_.store(false, std::memory_order_relaxed);
    requestSave();

    juce::String done = juce::String(knownCount_.load(std::memory_order_relaxed)) + " plugins";
    const int failed = failedCount_.load(std::memory_order_relaxed);
    if (failed > 0)
        done << ", " << failed << " failed";
    if (workerBroken_.load(std::memory_order_relaxed))
        done = "Scanner could not start. " + done;
    setStatus(done, false);
}

void PluginCatalogue::setStatus(juce::String text, bool scanning)
{
    const std::lock_guard<std::mutex> lock(statusMutex_);
    statusText_ = std::move(text);
    scanning_.store(scanning, std::memory_order_relaxed);
    token_.fetch_add(1, std::memory_order_relaxed);
}

void PluginCatalogue::requestSave()
{
    if (auto xml = list_.createXml())
    {
        const std::lock_guard<std::mutex> lock(saveMutex_);
        pendingXml_.reset(xml.release());
    }
    listDirty_.store(true, std::memory_order_relaxed);
    token_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace youhost
