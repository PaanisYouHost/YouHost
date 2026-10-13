#include "SessionDisk.h"
#include "SessionCopy.h"
#include "SessionFiles.h"
#include "SessionNames.h"

namespace youhost
{

SessionDisk::SessionDisk()
    : juce::Thread("youhost-disk")
{
    startThread(juce::Thread::Priority::background);
}

SessionDisk::~SessionDisk()
{
    signalThreadShouldExit();
    wake_.signal();
    stopThread(8000);
}

bool SessionDisk::post(Request request)
{
    const juce::ScopedLock hold(lock_);
    if (busy_.load(std::memory_order_acquire) || hasPending_)
        return false;
    pending_ = std::move(request);
    hasPending_ = true;
    wake_.signal();
    return true;
}

bool SessionDisk::startBackup(SessionData data, juce::File sessionFolder, juce::String dateStem)
{
    Request request;
    request.job = Job::backup;
    request.data = std::move(data);
    request.source = std::move(sessionFolder);
    request.dateStem = std::move(dateStem);
    return post(std::move(request));
}

bool SessionDisk::startCopy(SessionData data,
                            juce::File source,
                            juce::File destination,
                            std::function<void(bool, juce::String)> finished)
{
    Request request;
    request.job = Job::copy;
    request.data = std::move(data);
    request.source = std::move(source);
    request.destination = std::move(destination);
    request.finished = std::move(finished);
    progress_.store(0.0f, std::memory_order_release);
    if (! post(std::move(request)))
    {
        progress_.store(-1.0f, std::memory_order_release);
        return false;
    }
    return true;
}

bool SessionDisk::startCrashCopy(juce::File sessionFile, juce::String folderName)
{
    Request request;
    request.job = Job::crash;
    request.source = std::move(sessionFile);
    request.dateStem = std::move(folderName);
    return post(std::move(request));
}

void SessionDisk::run()
{
    while (! threadShouldExit())
    {
        wake_.wait(500);
        if (threadShouldExit())
            return;

        Request request;
        {
            const juce::ScopedLock hold(lock_);
            if (! hasPending_)
                continue;
            request = std::move(pending_);
            pending_ = {};
            hasPending_ = false;
        }

        busy_.store(true, std::memory_order_release);
        if (request.job == Job::backup)
            runBackup(request);
        else if (request.job == Job::copy)
            runCopy(request);
        else if (request.job == Job::crash)
            runCrash(request);
        busy_.store(false, std::memory_order_release);
    }
}

void SessionDisk::runBackup(Request& request)
{
    if (threadShouldExit() || request.source == juce::File())
        return;

    const auto backups = request.source.getChildFile("Backups");
    backups.createDirectory();
    std::vector<std::string> taken;
    for (const auto& child : backups.findChildFiles(juce::File::findDirectories, false))
        taken.push_back(child.getFileName().toStdString());

    const auto folderName = nextFreeSessionName(request.dateStem.toStdString(), taken);
    const auto folder = backups.getChildFile(juce::String(folderName));
    folder.createDirectory();
    if (! writeSessionFile(folder.getChildFile(kSessionFileName), request.data))
        return;

    std::vector<BackupStamp> stamps;
    for (const auto& child : backups.findChildFiles(juce::File::findDirectories, false))
    {
        const auto session = child.getChildFile(kSessionFileName);
        if (! session.existsAsFile())
            continue;
        BackupStamp stamp;
        stamp.name = child.getFileName().toStdString();
        stamp.modifiedMs = session.getLastModificationTime().toMilliseconds();
        stamps.push_back(std::move(stamp));
    }
    for (const auto& name : backupsToRemove(std::move(stamps), kSessionBackupsToKeep))
        backups.getChildFile(juce::String(name)).deleteRecursively();

    const auto now = juce::Time::getCurrentTime();
    backupHour_.store(now.getHours(), std::memory_order_release);
    backupMinute_.store(now.getMinutes(), std::memory_order_release);
}

void SessionDisk::runCopy(Request& request)
{
    auto finish = [this, &request](bool ok, juce::String message)
    {
        progress_.store(-1.0f, std::memory_order_release);
        if (threadShouldExit() || request.finished == nullptr)
            return;
        auto callback = std::move(request.finished);
        juce::MessageManager::callAsync([callback = std::move(callback), ok, message]() mutable
        {
            callback(ok, message);
        });
    };

    if (threadShouldExit())
    {
        finish(false, "The copy was interrupted.");
        return;
    }

    const bool existed = request.destination.isDirectory();
    if (! request.destination.createDirectory())
    {
        finish(false, "Could not create " + request.destination.getFullPathName());
        return;
    }

    // Fast copy: clonefile (APFS copy-on-write) when the volume matches, otherwise
    // sendfile or a large read/write. No sleep and no per-file fsync.
    const auto audioSource = request.source.getChildFile(kAudioFolderName);
    const auto audioDest = request.destination.getChildFile(kAudioFolderName);
    if (! audioDest.createDirectory())
    {
        if (! existed)
            request.destination.deleteRecursively();
        finish(false, "Could not create " + audioDest.getFullPathName());
        return;
    }

    struct Item
    {
        juce::File source;
        juce::File destination;
        std::int64_t bytes = 0;
    };
    std::vector<Item> items;
    if (audioSource.isDirectory())
    {
        for (const auto& wav : audioSource.findChildFiles(juce::File::findFiles, false))
        {
            if (! wav.existsAsFile())
                continue;
            items.push_back({ wav,
                              audioDest.getChildFile(wav.getFileName()),
                              std::max<std::int64_t>(0, wav.getSize()) });
        }
    }

    std::int64_t total = 0;
    for (const auto& item : items)
        total += item.bytes;
    if (total < 1)
        total = 1;

    std::int64_t done = 0;
    progress_.store(0.0f, std::memory_order_release);
    for (const auto& item : items)
    {
        if (threadShouldExit())
        {
            if (! existed)
                request.destination.deleteRecursively();
            finish(false, "The copy was interrupted.");
            return;
        }
        const auto result = copyFileFast(
            item.source.getFullPathName().toStdString(),
            item.destination.getFullPathName().toStdString(),
            [this, &done, total](std::uint64_t copied)
            {
                const float fraction = static_cast<float>(done + static_cast<std::int64_t>(copied))
                                       / static_cast<float>(total);
                progress_.store(juce::jlimit(0.0f, 1.0f, fraction), std::memory_order_release);
            });
        if (! result.ok)
        {
            if (! existed)
                request.destination.deleteRecursively();
            finish(false, "Could not copy " + item.source.getFileName());
            return;
        }
        done += static_cast<std::int64_t>(result.bytes);
    }

    if (! writeSessionFile(request.destination.getChildFile(kSessionFileName), request.data))
    {
        if (! existed)
            request.destination.deleteRecursively();
        finish(false, "Could not write the session file.");
        return;
    }

    fsyncDirectory(audioDest.getFullPathName().toStdString());
    fsyncDirectory(request.destination.getFullPathName().toStdString());
    progress_.store(1.0f, std::memory_order_release);
    finish(true, {});
}

void SessionDisk::runCrash(Request& request)
{
    if (threadShouldExit() || ! request.source.existsAsFile())
        return;

    const auto sessionFolder = request.source.getParentDirectory();
    const auto parent = sessionFolder.getParentDirectory();
    if (! parent.isDirectory())
        return;

    std::vector<std::string> taken;
    for (const auto& child : parent.findChildFiles(juce::File::findDirectories, false))
        taken.push_back(child.getFileName().toStdString());
    const auto folderName = nextFreeSessionName(request.dateStem.toStdString(), taken);
    const auto folder = parent.getChildFile(juce::String(folderName));
    folder.createDirectory();
    request.source.copyFileTo(folder.getChildFile(request.source.getFileName()));
}

} // namespace youhost
