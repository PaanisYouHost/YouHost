#pragma once

#include "SessionDocument.h"

#include <atomic>
#include <functional>
#include <juce_core/juce_core.h>

namespace youhost
{

// One low-priority thread for session file copies. The audio thread never calls it.
class SessionDisk : private juce::Thread
{
public:
    SessionDisk();
    ~SessionDisk() override;

    bool busy() const noexcept { return busy_.load(std::memory_order_acquire); }
    float copyProgress() const noexcept { return progress_.load(std::memory_order_acquire); }
    int backupHour() const noexcept { return backupHour_.load(std::memory_order_acquire); }
    int backupMinute() const noexcept { return backupMinute_.load(std::memory_order_acquire); }

    bool startBackup(SessionData data, juce::File sessionFolder, juce::String dateStem);
    bool startCopy(juce::File source,
                   juce::File destination,
                   std::function<void(bool ok, juce::String message)> finished);
    bool startCrashCopy(juce::File sessionFile, juce::String folderName);

private:
    enum class Job
    {
        none,
        backup,
        copy,
        crash
    };

    struct Request
    {
        Job job = Job::none;
        SessionData data;
        juce::File source;
        juce::File destination;
        juce::String dateStem;
        std::function<void(bool, juce::String)> finished;
    };

    void run() override;
    bool post(Request request);
    void runBackup(Request& request);
    void runCopy(Request& request);
    void runCrash(Request& request);

    juce::CriticalSection lock_;
    juce::WaitableEvent wake_;
    Request pending_;
    bool hasPending_ = false;
    std::atomic<bool> busy_ { false };
    std::atomic<float> progress_ { -1.0f };
    std::atomic<int> backupHour_ { -1 };
    std::atomic<int> backupMinute_ { -1 };
};

} // namespace youhost
