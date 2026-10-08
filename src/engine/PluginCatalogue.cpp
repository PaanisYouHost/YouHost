#include "PluginCatalogue.h"
#include "AppSettings.h"
#include "ScanJobs.h"

#include <algorithm>
#include <thread>

#if defined(__APPLE__) || defined(__linux__)
 #include <signal.h>
 #include <unistd.h>
#endif

namespace youhost
{
namespace
{

constexpr int kWorkers = 4;
constexpr int kNormalTimeoutMs = 20000;
constexpr int kShellTimeoutMs = 45000;
constexpr int kReuseLimit = 8;

juce::String labelFor(const juce::String& identifier)
{
    const auto file = juce::File(identifier);
    if (file.exists())
        return file.getFileName();
    const auto slash = identifier.lastIndexOfChar('/');
    if (slash >= 0)
        return identifier.substring(slash + 1);
    return identifier;
}

struct Slot
{
    std::mutex mutex;
    juce::ChildProcess process;
    juce::File directory;
    int served = 0;
};

struct Job
{
    juce::String format;
    juce::String identifier;
    juce::String label;
    bool shell = false;
    bool waves = false;
    bool forced = false;
};

struct Outcome
{
    bool ok = false;
    bool cancelled = false;
    bool killed = false;
    juce::String reason;
    juce::OwnedArray<juce::PluginDescription> found;
};

void killSlot(Slot& slot)
{
    int pid = 0;
    {
        const std::lock_guard<std::mutex> lock(slot.mutex);
        pid = slot.directory.getChildFile("pid").loadFileAsString().trim().getIntValue();
    }

#if defined(__APPLE__) || defined(__linux__)
    if (pid > 1)
        ::kill(-pid, SIGKILL);
#else
    juce::ignoreUnused(pid);
#endif

    const std::lock_guard<std::mutex> lock(slot.mutex);
    slot.process.kill();
    slot.process.waitForProcessToFinish(1500);
    slot.served = 0;
}

bool slotRunning(Slot& slot)
{
    const std::lock_guard<std::mutex> lock(slot.mutex);
    return slot.process.isRunning();
}

} // namespace

bool isScanWorkerCommandLine(const juce::String& commandLine)
{
    if (commandLine.contains("--YouHostScan:"))
        return true;

    const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
    for (const auto& argument : args)
        if (argument.contains("YouHostScan:"))
            return true;
    return false;
}

ScanWorker::ScanWorker()
    : juce::Thread("YouHostScan")
{
    juce::addDefaultFormatsToManager(formats_);
}

ScanWorker::~ScanWorker()
{
    signalThreadShouldExit();
    stopThread(2000);
}

juce::AudioPluginFormat* ScanWorker::formatByName(const juce::String& name)
{
    for (int index = 0; index < formats_.getNumFormats(); ++index)
        if (auto* format = formats_.getFormat(index))
            if (format->getName() == name)
                return format;
    return nullptr;
}

void ScanWorker::startFromCommandLine(const juce::String& commandLine)
{
    const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
    for (const auto& argument : args)
        if (argument.startsWith("--YouHostScan:dir:"))
            directory_ = juce::File(argument.fromFirstOccurrenceOf("--YouHostScan:dir:", false, false));

    if (directory_.getFullPathName().isEmpty())
    {
        const auto marker = commandLine.fromFirstOccurrenceOf("--YouHostScan:dir:", false, false).trim();
        directory_ = juce::File(marker.upToFirstOccurrenceOf(" ", false, false));
    }

    directory_.createDirectory();

#if defined(__APPLE__) || defined(__linux__)
    setpgid(0, 0);
    directory_.getChildFile("pid").replaceWithText(juce::String(static_cast<int>(::getpid())));
#endif

    directory_.getChildFile("ready").replaceWithText("1");
    startThread();
}

void ScanWorker::run()
{
    if (directory_.getFullPathName().isEmpty())
    {
        if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating())
            messageManager->stopDispatchLoop();
        return;
    }

    while (! threadShouldExit())
    {
        if (directory_.getChildFile("stop").existsAsFile())
            break;

        const auto requestFile = directory_.getChildFile("request.xml");
        if (! requestFile.existsAsFile())
        {
            wait(15);
            continue;
        }

        const auto request = juce::XmlDocument::parse(requestFile);
        requestFile.deleteFile();

        juce::XmlElement reply("YouHostScanResult");
        reply.setAttribute("ok", 0);

        if (request != nullptr && request->hasTagName("YouHostScan"))
        {
            const auto file = request->getStringAttribute("file");
            if (auto* format = formatByName(request->getStringAttribute("format")))
            {
                juce::OwnedArray<juce::PluginDescription> found;
                auto reason = juce::String();
                auto ok = false;
                try
                {
                    format->findAllTypesForFile(found, file);
                    ok = true;
                }
                catch (...)
                {
                    ok = false;
                    reason = "The plugin threw while being scanned";
                }

                reply.setAttribute("ok", ok ? 1 : 0);
                reply.setAttribute("reason", reason);
                for (auto* description : found)
                    if (description != nullptr)
                        if (auto xml = description->createXml())
                            reply.addChildElement(xml.release());
            }
            else
            {
                reply.setAttribute("reason", "Unknown plugin format");
            }
        }

        const auto temporary = directory_.getChildFile("result.tmp");
        reply.writeTo(temporary);
        temporary.moveFileTo(directory_.getChildFile("result.xml"));
    }

    if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating())
        messageManager->stopDispatchLoop();
}

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
    : settings_(settings)
{
    juce::addDefaultFormatsToManager(formats_);

    pedalFile_ = settings_.supportDirectory().getChildFile("plugin-scan-pedal.txt");
    pedalFile_.getParentDirectory().createDirectory();

    if (auto saved = settings_.loadKnownPlugins())
        list_.recreateFromXml(*saved);

    loadStoredFailures();
    for (const auto& failure : failures_)
        if (! list_.getBlacklistedFiles().contains(failure.identifier))
            list_.addToBlacklist(failure.identifier);

    if (pedalFile_.existsAsFile())
    {
        for (const auto& line : juce::StringArray::fromLines(pedalFile_.loadFileAsString()))
        {
            const auto identifier = line.trim();
            if (identifier.isNotEmpty())
                rememberFailure(identifier, "Scanner crashed on this plugin last time");
        }
        pedalFile_.deleteFile();
        requestSave();
    }

    knownCount_.store(list_.getNumTypes(), std::memory_order_relaxed);
    failedCount_.store(static_cast<int>(failures_.size()), std::memory_order_relaxed);
    if (knownCount_.load(std::memory_order_relaxed) > 0 || failedCount_.load(std::memory_order_relaxed) > 0)
    {
        juce::String text = juce::String(knownCount_.load(std::memory_order_relaxed)) + " plugins remembered";
        if (failedCount_.load(std::memory_order_relaxed) > 0)
            text << ", " << failedCount_.load(std::memory_order_relaxed) << " failed";
        setStatus(text, false);
    }
}

PluginCatalogue::~PluginCatalogue()
{
    cancel_.store(true, std::memory_order_relaxed);
    if (scanThread_ != nullptr)
    {
        scanThread_->signalThreadShouldExit();
        scanThread_->stopThread(8000);
    }
    flushSave();
    saveStoredFailures();
}

void PluginCatalogue::startIfEmpty()
{
    if (list_.getNumTypes() == 0 && failures_.empty() && ! scanning_.load(std::memory_order_relaxed))
        scanNew();
}

void PluginCatalogue::scanNew()
{
    if (scanning_.load(std::memory_order_relaxed))
        return;

    cancel_.store(false, std::memory_order_relaxed);
    rescanKnown_.store(false, std::memory_order_relaxed);
    scanning_.store(true, std::memory_order_relaxed);
    doneCount_.store(0, std::memory_order_relaxed);
    totalCount_.store(0, std::memory_order_relaxed);
    skippedWaves_.store(0, std::memory_order_relaxed);
    scanStartedMs_.store(juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
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

    {
        const std::lock_guard<std::mutex> lock(listMutex_);
        forced_.clear();
    }
    cancel_.store(false, std::memory_order_relaxed);
    rescanKnown_.store(true, std::memory_order_relaxed);
    scanning_.store(true, std::memory_order_relaxed);
    doneCount_.store(0, std::memory_order_relaxed);
    totalCount_.store(0, std::memory_order_relaxed);
    skippedWaves_.store(0, std::memory_order_relaxed);
    scanStartedMs_.store(juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
    setStatus("Rescanning changed plugins", true);

    if (scanThread_ != nullptr)
        scanThread_->stopThread(1000);
    scanThread_ = std::make_unique<DirectoryScanThread>(*this);
    scanThread_->startThread();
}

void PluginCatalogue::stopScan()
{
    cancel_.store(true, std::memory_order_relaxed);
    setStatus("Stopping scan", true);
}

void PluginCatalogue::clearFailedAndScan()
{
    if (scanning_.load(std::memory_order_relaxed))
        return;

    {
        const std::lock_guard<std::mutex> lock(listMutex_);
        failures_.clear();
        list_.clearBlacklistedFiles();
    }
    failedCount_.store(0, std::memory_order_relaxed);
    saveStoredFailures();
    requestSave();
    scanNew();
}

void PluginCatalogue::scanFile(const juce::File& file)
{
    if (scanning_.load(std::memory_order_relaxed) || file.getFullPathName().isEmpty())
        return;

    juce::String formatName;
    const auto identifier = file.getFullPathName();
    for (int index = 0; index < formats_.getNumFormats(); ++index)
    {
        if (auto* format = formats_.getFormat(index))
        {
            if (format->fileMightContainThisPluginType(identifier))
            {
                formatName = format->getName();
                break;
            }
        }
    }

    if (formatName.isEmpty())
    {
        setStatus("That file is not an AU or VST3 plugin", false);
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(listMutex_);
        list_.removeFromBlacklist(identifier);
        failures_.erase(std::remove_if(failures_.begin(), failures_.end(),
                                       [&identifier](const FailedPlugin& failure)
                                       {
                                           return failure.identifier == identifier;
                                       }),
                        failures_.end());
        forced_.clear();
        forced_.push_back({ formatName, identifier });
    }
    scanNew();
}

void PluginCatalogue::rescanIdentifier(const juce::String& identifier)
{
    if (scanning_.load(std::memory_order_relaxed) || identifier.isEmpty())
        return;

    juce::String formatName;
    for (const auto& type : list_.getTypes())
    {
        if (type.fileOrIdentifier == identifier)
        {
            formatName = type.pluginFormatName;
            break;
        }
    }
    if (formatName.isEmpty())
    {
        for (int index = 0; index < formats_.getNumFormats(); ++index)
            if (auto* format = formats_.getFormat(index))
                if (format->fileMightContainThisPluginType(identifier))
                    formatName = format->getName();
    }
    if (formatName.isEmpty())
    {
        setStatus("Select a plugin to rescan", false);
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(listMutex_);
        list_.removeFromBlacklist(identifier);
        failures_.erase(std::remove_if(failures_.begin(), failures_.end(),
                                       [&identifier](const FailedPlugin& failure)
                                       {
                                           return failure.identifier == identifier;
                                       }),
                        failures_.end());
        forced_.clear();
        forced_.push_back({ formatName, identifier });
    }
    scanNew();
}

bool PluginCatalogue::scanWavesShells() const
{
    return settings_.loadScanWavesShells();
}

void PluginCatalogue::setScanWavesShells(bool enabled)
{
    settings_.saveScanWavesShells(enabled);
    token_.fetch_add(1, std::memory_order_relaxed);
}

bool PluginCatalogue::showAppleInInserts() const
{
    return settings_.loadShowAppleInserts();
}

void PluginCatalogue::setShowAppleInInserts(bool enabled)
{
    settings_.saveShowAppleInserts(enabled);
    token_.fetch_add(1, std::memory_order_relaxed);
}

bool PluginCatalogue::showInstrumentsInInserts() const
{
    return settings_.loadShowInstrumentInserts();
}

void PluginCatalogue::setShowInstrumentsInInserts(bool enabled)
{
    settings_.saveShowInstrumentInserts(enabled);
    token_.fetch_add(1, std::memory_order_relaxed);
}

juce::Array<juce::PluginDescription> PluginCatalogue::types() const
{
    return list_.getTypes();
}

juce::Array<juce::PluginDescription> PluginCatalogue::insertTypes() const
{
    const bool showApple = showAppleInInserts();
    const bool showInstruments = showInstrumentsInInserts();
    juce::Array<juce::PluginDescription> visible;
    for (const auto& type : list_.getTypes())
    {
        if (showInInsertList(type.manufacturerName.toStdString(),
                             type.fileOrIdentifier.toStdString(),
                             type.isInstrument,
                             type.numInputChannels,
                             showApple,
                             showInstruments))
            visible.add(type);
    }
    return visible;
}

std::vector<FailedPlugin> PluginCatalogue::failures() const
{
    const std::lock_guard<std::mutex> lock(listMutex_);
    return failures_;
}

CatalogueStatus PluginCatalogue::status() const
{
    CatalogueStatus status;
    {
        const std::lock_guard<std::mutex> lock(statusMutex_);
        status.text = statusText_;
        status.current = currentText_;
    }
    status.scanning = scanning_.load(std::memory_order_relaxed);
    status.known = knownCount_.load(std::memory_order_relaxed);
    status.failed = failedCount_.load(std::memory_order_relaxed);
    status.skippedWaves = skippedWaves_.load(std::memory_order_relaxed);
    status.done = doneCount_.load(std::memory_order_relaxed);
    status.total = totalCount_.load(std::memory_order_relaxed);
    status.token = token_.load(std::memory_order_relaxed);
    if (status.scanning)
    {
        const auto started = scanStartedMs_.load(std::memory_order_relaxed);
        status.elapsedMs = static_cast<int>(juce::Time::getMillisecondCounter() - started);
    }
    return status;
}

void PluginCatalogue::flushSave()
{
    if (! listDirty_.exchange(false, std::memory_order_relaxed))
        return;

    std::unique_ptr<juce::XmlElement> xml;
    {
        const std::lock_guard<std::mutex> lock(listMutex_);
        xml = list_.createXml();
    }
    if (xml != nullptr)
        settings_.saveKnownPlugins(xml.get());
    saveStoredFailures();
}

void PluginCatalogue::createInstanceAsync(const juce::PluginDescription& description,
                                          double sampleRate,
                                          int blockSize,
                                          juce::AudioPluginFormat::PluginCreationCallback callback)
{
    formats_.createPluginInstanceAsync(description, sampleRate, blockSize, std::move(callback));
}

bool PluginCatalogue::identifierNeedsScan(const juce::String& formatName, const juce::String& identifier) const
{
    if (list_.getBlacklistedFiles().contains(identifier))
        return false;

    for (int index = 0; index < formats_.getNumFormats(); ++index)
    {
        if (auto* format = formats_.getFormat(index))
        {
            if (format->getName() == formatName)
                return ! list_.isListingUpToDate(identifier, *format);
        }
    }
    return true;
}

void PluginCatalogue::rememberFailure(const juce::String& identifier, const juce::String& reason)
{
    if (identifier.isEmpty())
        return;

    for (auto& failure : failures_)
    {
        if (failure.identifier == identifier)
        {
            failure.reason = reason;
            if (! list_.getBlacklistedFiles().contains(identifier))
                list_.addToBlacklist(identifier);
            return;
        }
    }

    failures_.push_back({ identifier, reason });
    if (! list_.getBlacklistedFiles().contains(identifier))
        list_.addToBlacklist(identifier);
}

void PluginCatalogue::loadStoredFailures()
{
    failures_.clear();
    const auto saved = settings_.loadScanFailures();
    if (saved == nullptr)
        return;

    for (auto* child = saved->getFirstChildElement(); child != nullptr; child = child->getNextElement())
    {
        const auto identifier = child->getStringAttribute("id");
        if (identifier.isEmpty())
            continue;
        auto reason = child->getStringAttribute("reason");
        if (reason.isEmpty())
            reason = "Failed";
        failures_.push_back({ identifier, reason });
    }
}

void PluginCatalogue::saveStoredFailures()
{
    juce::XmlElement root("ScanFailures");
    std::vector<FailedPlugin> copy;
    {
        const std::lock_guard<std::mutex> lock(listMutex_);
        copy = failures_;
    }
    for (const auto& failure : copy)
    {
        auto* child = root.createNewChildElement("Failed");
        child->setAttribute("id", failure.identifier);
        child->setAttribute("reason", failure.reason);
    }
    settings_.saveScanFailures(&root);
}

void PluginCatalogue::setStatus(juce::String text, bool scanning)
{
    const std::lock_guard<std::mutex> lock(statusMutex_);
    statusText_ = std::move(text);
    if (! scanning)
        currentText_.clear();
    scanning_.store(scanning, std::memory_order_relaxed);
    token_.fetch_add(1, std::memory_order_relaxed);
}

void PluginCatalogue::requestSave()
{
    listDirty_.store(true, std::memory_order_relaxed);
    token_.fetch_add(1, std::memory_order_relaxed);
}

void PluginCatalogue::runScan()
{
    std::vector<Job> jobs;
    std::vector<ForcedJob> forced;
    {
        const std::lock_guard<std::mutex> lock(listMutex_);
        forced.swap(forced_);
    }

    int skippedWaves = 0;
    if (! forced.empty())
    {
        for (const auto& item : forced)
        {
            const auto candidate = makeScanCandidate(item.format.toStdString(), item.identifier.toStdString());
            Job job;
            job.format = item.format;
            job.identifier = item.identifier;
            job.label = labelFor(item.identifier);
            job.shell = candidate.shell;
            job.waves = candidate.waves;
            job.forced = true;
            jobs.push_back(std::move(job));
        }
    }
    else
    {
        const bool allowWaves = settings_.loadScanWavesShells();
        for (int index = 0; index < formats_.getNumFormats(); ++index)
        {
            auto* format = formats_.getFormat(index);
            if (format == nullptr)
                continue;

            auto paths = format->getDefaultLocationsToSearch();
#if JUCE_MAC
            if (format->getName() == "VST3")
            {
                paths.add(juce::File("~/Library/Audio/Plug-Ins/VST3"));
                paths.add(juce::File("/Library/Audio/Plug-Ins/VST3"));
                paths.removeRedundantPaths();
            }
#endif
            // AudioUnit ignores the path and walks the AudioComponent registry,
            // which is how /Library/Audio/Plug-Ins/Components is discovered.
            const auto identifiers = format->searchPathsForPlugins(paths, true, false);
            for (const auto& identifier : identifiers)
            {
                const auto candidate = makeScanCandidate(format->getName().toStdString(), identifier.toStdString());
                if (skipBecauseWaves(candidate, allowWaves))
                {
                    ++skippedWaves;
                    continue;
                }

                bool blacklisted = false;
                {
                    const std::lock_guard<std::mutex> lock(listMutex_);
                    blacklisted = list_.getBlacklistedFiles().contains(identifier);
                }
                if (blacklisted)
                    continue;
                if (! identifierNeedsScan(format->getName(), identifier))
                    continue;

                Job job;
                job.format = format->getName();
                job.identifier = identifier;
                job.label = labelFor(identifier);
                job.shell = candidate.shell;
                job.waves = candidate.waves;
                jobs.push_back(std::move(job));
            }
        }
    }

    std::vector<ScanCandidate> order;
    order.reserve(jobs.size());
    for (const auto& job : jobs)
        order.push_back(makeScanCandidate(job.format.toStdString(), job.identifier.toStdString()));
    std::vector<int> indexes(jobs.size());
    for (int index = 0; index < static_cast<int>(indexes.size()); ++index)
        indexes[static_cast<std::size_t>(index)] = index;
    std::stable_sort(indexes.begin(), indexes.end(), [&order](int left, int right)
    {
        return ! order[static_cast<std::size_t>(left)].shell && order[static_cast<std::size_t>(right)].shell;
    });
    std::vector<Job> ordered;
    ordered.reserve(jobs.size());
    for (const int index : indexes)
        ordered.push_back(std::move(jobs[static_cast<std::size_t>(index)]));
    jobs.swap(ordered);

    skippedWaves_.store(skippedWaves, std::memory_order_relaxed);
    totalCount_.store(static_cast<int>(jobs.size()), std::memory_order_relaxed);
    doneCount_.store(0, std::memory_order_relaxed);

    if (jobs.empty())
    {
        knownCount_.store(list_.getNumTypes(), std::memory_order_relaxed);
        failedCount_.store(static_cast<int>(failures_.size()), std::memory_order_relaxed);
        scanning_.store(false, std::memory_order_relaxed);
        juce::String done = juce::String(knownCount_.load(std::memory_order_relaxed)) + " plugins";
        if (failedCount_.load(std::memory_order_relaxed) > 0)
            done << ", " << failedCount_.load(std::memory_order_relaxed) << " failed";
        if (skippedWaves > 0)
            done << ", " << skippedWaves << " Waves shells skipped";
        setStatus(done, false);
        return;
    }

    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("youhost-scan-" + juce::String(juce::Time::currentTimeMillis()));
    root.createDirectory();

    std::vector<std::shared_ptr<Slot>> slots;
    slots.reserve(static_cast<std::size_t>(kWorkers));
    for (int index = 0; index < kWorkers; ++index)
    {
        auto slot = std::make_shared<Slot>();
        slot->directory = root.getChildFile(juce::String(index));
        slot->directory.createDirectory();
        slots.push_back(std::move(slot));
    }

    std::atomic<int> cursor { 0 };
    std::mutex pedalMutex;
    juce::StringArray inFlight;

    auto rewritePedal = [&]()
    {
        if (inFlight.isEmpty())
            pedalFile_.deleteFile();
        else
            pedalFile_.replaceWithText(inFlight.joinIntoString("\n"));
    };

    auto launch = [&](Slot& slot) -> bool
    {
        killSlot(slot);
        slot.directory.createDirectory();
        slot.directory.getChildFile("ready").deleteFile();
        slot.directory.getChildFile("stop").deleteFile();
        slot.directory.getChildFile("request.xml").deleteFile();
        slot.directory.getChildFile("result.xml").deleteFile();
        slot.directory.getChildFile("pid").deleteFile();

        juce::StringArray args;
        args.add(executable.getFullPathName());
        args.add("--YouHostScan:dir:" + slot.directory.getFullPathName());
        {
            const std::lock_guard<std::mutex> lock(slot.mutex);
            if (! slot.process.start(args))
                return false;
        }

        const auto deadline = juce::Time::getMillisecondCounter() + 8000u;
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (cancel_.load(std::memory_order_relaxed))
            {
                killSlot(slot);
                return false;
            }
            if (slot.directory.getChildFile("ready").existsAsFile())
            {
                slot.served = 0;
                return true;
            }
            if (! slotRunning(slot))
                return false;
            juce::Thread::sleep(20);
        }
        killSlot(slot);
        return false;
    };

    auto runOne = [&](Slot& slot, const Job& job) -> Outcome
    {
        Outcome outcome;
        if (cancel_.load(std::memory_order_relaxed))
        {
            outcome.cancelled = true;
            return outcome;
        }

        const bool fresh = ! slotRunning(slot) || (job.shell && slot.served > 0) || slot.served >= kReuseLimit;
        if (fresh && ! launch(slot))
        {
            outcome.killed = true;
            outcome.reason = "Scanner could not start";
            return outcome;
        }

        slot.directory.getChildFile("result.xml").deleteFile();
        juce::XmlElement request("YouHostScan");
        request.setAttribute("format", job.format);
        request.setAttribute("file", job.identifier);
        const auto temporary = slot.directory.getChildFile("request.tmp");
        if (! request.writeTo(temporary) || ! temporary.moveFileTo(slot.directory.getChildFile("request.xml")))
        {
            outcome.killed = true;
            outcome.reason = "Could not send the plugin to the scanner";
            killSlot(slot);
            return outcome;
        }

        const int timeoutMs = job.shell ? kShellTimeoutMs : kNormalTimeoutMs;
        const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
        const auto resultFile = slot.directory.getChildFile("result.xml");
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (cancel_.load(std::memory_order_relaxed))
            {
                outcome.cancelled = true;
                outcome.killed = true;
                killSlot(slot);
                return outcome;
            }
            if (resultFile.existsAsFile())
                break;
            if (! slotRunning(slot))
                break;
            juce::Thread::sleep(20);
        }

        if (resultFile.existsAsFile())
        {
            if (auto xml = juce::XmlDocument::parse(resultFile))
            {
                outcome.ok = xml->getBoolAttribute("ok", false);
                outcome.reason = xml->getStringAttribute("reason");
                for (auto* child : xml->getChildIterator())
                {
                    auto description = std::make_unique<juce::PluginDescription>();
                    if (description->loadFromXml(*child))
                        outcome.found.add(description.release());
                }
            }
            resultFile.deleteFile();
            if (! outcome.ok && outcome.reason.isEmpty())
                outcome.reason = "The plugin failed to scan";
            ++slot.served;
            if (job.shell)
                killSlot(slot);
            return outcome;
        }

        outcome.killed = true;
        outcome.reason = slotRunning(slot) ? "Timed out" : "Plugin crashed the scanner";
        killSlot(slot);
        return outcome;
    };

    auto accept = [&](const Job& job, Outcome& outcome)
    {
        if (outcome.cancelled)
            return;

        const std::lock_guard<std::mutex> lock(listMutex_);
        if (! outcome.ok)
        {
            rememberFailure(job.identifier, outcome.reason.isNotEmpty() ? outcome.reason : juce::String("Failed"));
        }
        else
        {
            list_.removeFromBlacklist(job.identifier);
            failures_.erase(std::remove_if(failures_.begin(), failures_.end(),
                                           [&job](const FailedPlugin& failure)
                                           {
                                               return failure.identifier == job.identifier;
                                           }),
                            failures_.end());
            for (auto* description : outcome.found)
                if (description != nullptr)
                    list_.addType(*description);
        }
        knownCount_.store(list_.getNumTypes(), std::memory_order_relaxed);
        failedCount_.store(static_cast<int>(failures_.size()), std::memory_order_relaxed);
        requestSave();
    };

    std::vector<std::thread> threads;
    threads.reserve(slots.size());
    for (const auto& slot : slots)
    {
        threads.emplace_back([&, slot]()
        {
            while (! cancel_.load(std::memory_order_relaxed)
                   && (scanThread_ == nullptr || ! scanThread_->threadShouldExit()))
            {
                const int index = cursor.fetch_add(1, std::memory_order_relaxed);
                if (index < 0 || index >= static_cast<int>(jobs.size()))
                    break;

                const auto& job = jobs[static_cast<std::size_t>(index)];
                {
                    const std::lock_guard<std::mutex> lock(statusMutex_);
                    currentText_ = job.label;
                }
                {
                    const std::lock_guard<std::mutex> lock(pedalMutex);
                    inFlight.addIfNotAlreadyThere(job.identifier);
                    rewritePedal();
                }

                auto outcome = runOne(*slot, job);

                {
                    const std::lock_guard<std::mutex> lock(pedalMutex);
                    inFlight.removeString(job.identifier);
                    rewritePedal();
                }

                accept(job, outcome);
                doneCount_.fetch_add(1, std::memory_order_relaxed);
                token_.fetch_add(1, std::memory_order_relaxed);
            }
            killSlot(*slot);
        });
    }

    for (auto& thread : threads)
        if (thread.joinable())
            thread.join();

    for (const auto& slot : slots)
        killSlot(*slot);
    root.deleteRecursively();
    pedalFile_.deleteFile();

    knownCount_.store(list_.getNumTypes(), std::memory_order_relaxed);
    failedCount_.store(static_cast<int>(failures_.size()), std::memory_order_relaxed);
    scanning_.store(false, std::memory_order_relaxed);
    requestSave();

    juce::String done = juce::String(knownCount_.load(std::memory_order_relaxed)) + " plugins";
    const int failed = failedCount_.load(std::memory_order_relaxed);
    if (failed > 0)
        done << ", " << failed << " failed";
    if (skippedWaves > 0)
        done << ", " << skippedWaves << " Waves shells skipped";
    if (cancel_.load(std::memory_order_relaxed))
        done = "Scan stopped. " + done;
    setStatus(done, false);
}

} // namespace youhost
