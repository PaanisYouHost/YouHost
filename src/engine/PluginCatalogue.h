#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace youhost
{

class AppSettings;

// Child of this executable. A crash while identifying a plugin dies here,
// not in the mixer process. The command line is "--YouHostScan:<pipe>".
class ScanWorker : public juce::ChildProcessWorker,
                   private juce::Thread
{
public:
    ScanWorker();
    ~ScanWorker() override;

    void handleMessageFromCoordinator(const juce::MemoryBlock& block) override;
    void handleConnectionLost() override;

private:
    void run() override;
    juce::AudioPluginFormat* formatByName(const juce::String& name);

    juce::AudioPluginFormatManager formats_;
    std::mutex mutex_;
    juce::MemoryBlock job_;
    bool jobPending_ = false;
    juce::WaitableEvent jobEvent_;
};

bool isScanWorkerCommandLine(const juce::String& commandLine);

struct CatalogueStatus
{
    juce::String text;
    bool scanning = false;
    int known = 0;
    int failed = 0;
    int token = 0;
};

// Known AU and VST3 list. Scanning runs out of process, one plugin at a time.
class PluginCatalogue
{
public:
    explicit PluginCatalogue(AppSettings& settings);
    ~PluginCatalogue();

    PluginCatalogue(const PluginCatalogue&) = delete;
    PluginCatalogue& operator=(const PluginCatalogue&) = delete;

    void startIfEmpty();
    void scanNew();
    void rescan();
    void clearFailedAndScan();

    juce::Array<juce::PluginDescription> types() const;
    juce::StringArray failedFiles() const;
    CatalogueStatus status() const;
    void flushSave();

    void createInstanceAsync(const juce::PluginDescription& description,
                             double sampleRate,
                             int blockSize,
                             juce::AudioPluginFormat::PluginCreationCallback callback);

private:
    class DirectoryScanThread;
    friend class DirectoryScanThread;

    class Coordinator;
    class Scanner;

    void runScan();
    void setStatus(juce::String text, bool scanning);
    void requestSave();

    AppSettings& settings_;
    juce::AudioPluginFormatManager formats_;
    std::unique_ptr<Coordinator> coordinator_;
    juce::KnownPluginList list_;
    std::unique_ptr<DirectoryScanThread> scanThread_;
    juce::File pedalFile_;
    std::mutex saveMutex_;
    std::shared_ptr<juce::XmlElement> pendingXml_;

    std::atomic<bool> cancel_ { false };
    std::atomic<bool> workerBroken_ { false };
    std::atomic<bool> scanning_ { false };
    std::atomic<bool> rescanKnown_ { false };
    std::atomic<bool> listDirty_ { false };
    std::atomic<int> knownCount_ { 0 };
    std::atomic<int> failedCount_ { 0 };
    std::atomic<int> token_ { 0 };

    mutable std::mutex statusMutex_;
    juce::String statusText_ { "Plugins not scanned" };
};

} // namespace youhost
