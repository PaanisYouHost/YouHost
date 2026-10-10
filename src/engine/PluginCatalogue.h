#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace youhost
{

class AppSettings;

// Child of this executable. Jobs arrive as files in a directory so the parent
// can SIGKILL a stuck plugin and never leave the process running.
class ScanWorker : private juce::Thread
{
public:
    ScanWorker();
    ~ScanWorker() override;

    void startFromCommandLine(const juce::String& commandLine);

private:
    void run() override;
    juce::AudioPluginFormat* formatByName(const juce::String& name);

    juce::File directory_;
    juce::AudioPluginFormatManager formats_;
};

bool isScanWorkerCommandLine(const juce::String& commandLine);

struct FailedPlugin
{
    juce::String identifier;
    juce::String reason;
};

struct CatalogueStatus
{
    juce::String text;
    juce::String current;
    bool scanning = false;
    int known = 0;
    int failed = 0;
    int skippedWaves = 0;
    int done = 0;
    int total = 0;
    int elapsedMs = 0;
    int token = 0;
};

// Known AU and VST3 list. Scanning runs out of process, several plugins at a time.
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
    void stopScan();
    void clearFailedAndScan();
    void scanFile(const juce::File& file);
    void rescanIdentifier(const juce::String& identifier);

    bool scanWavesShells() const;
    void setScanWavesShells(bool enabled);
    bool showInstrumentsInInserts() const;
    void setShowInstrumentsInInserts(bool enabled);
    void setDeviceOpen(bool open) noexcept;

    juce::Array<juce::PluginDescription> types() const;
    juce::Array<juce::PluginDescription> insertTypes() const;
    std::vector<FailedPlugin> failures() const;
    CatalogueStatus status() const;
    void flushSave();

    void createInstanceAsync(const juce::PluginDescription& description,
                             double sampleRate,
                             int blockSize,
                             juce::AudioPluginFormat::PluginCreationCallback callback);

    // True when this format may be constructed off the message thread.
    // AudioUnit is never background: it uses createPluginInstanceAsync.
    bool prefersBackgroundInstance(const juce::PluginDescription& description) const;

    // Called on a background thread. The callback inside the format runs
    // before this returns.
    bool createInstanceBlocking(const juce::PluginDescription& description,
                                double sampleRate,
                                int blockSize,
                                std::unique_ptr<juce::AudioPluginInstance>& instance,
                                juce::String& error) const;

private:
    class DirectoryScanThread;
    friend class DirectoryScanThread;

    struct ForcedJob
    {
        juce::String format;
        juce::String identifier;
    };

    void runScan();
    void setStatus(juce::String text, bool scanning);
    void requestSave();
    void rememberFailure(const juce::String& identifier, const juce::String& reason);
    void loadStoredFailures();
    void saveStoredFailures();
    bool identifierNeedsScan(const juce::String& formatName, const juce::String& identifier) const;

    AppSettings& settings_;
    juce::AudioPluginFormatManager formats_;
    juce::KnownPluginList list_;
    std::unique_ptr<DirectoryScanThread> scanThread_;
    juce::File pedalFile_;

    mutable std::mutex listMutex_;
    std::vector<FailedPlugin> failures_;
    std::vector<ForcedJob> forced_;

    std::atomic<bool> cancel_ { false };
    std::atomic<bool> deviceOpen_ { false };
    std::atomic<bool> scanning_ { false };
    std::atomic<bool> rescanKnown_ { false };
    std::atomic<bool> listDirty_ { false };
    std::atomic<int> knownCount_ { 0 };
    std::atomic<int> failedCount_ { 0 };
    std::atomic<int> skippedWaves_ { 0 };
    std::atomic<int> doneCount_ { 0 };
    std::atomic<int> totalCount_ { 0 };
    std::atomic<int> token_ { 0 };
    std::atomic<juce::uint32> scanStartedMs_ { 0 };

    mutable std::mutex statusMutex_;
    juce::String statusText_ { "Plugins not scanned" };
    juce::String currentText_;
};

} // namespace youhost
