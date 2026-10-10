#include "SessionDisk.h"
#include "SessionFiles.h"
#include "SessionNames.h"

namespace youhost
{
namespace
{

bool copyFileChunked(const juce::File& source,
                     const juce::File& destination,
                     std::int64_t& done,
                     std::int64_t total,
                     std::atomic<float>& progress,
                     const std::function<bool()>& shouldStop)
{
    destination.getParentDirectory().createDirectory();
    std::unique_ptr<juce::FileInputStream> input(source.createInputStream());
    if (input == nullptr)
        return false;
    destination.deleteFile();
    std::unique_ptr<juce::FileOutputStream> output(destination.createOutputStream());
    if (output == nullptr)
        return false;

    constexpr int chunk = 256 * 1024;
    juce::HeapBlock<char> buffer(static_cast<size_t>(chunk));
    int yields = 0;
    for (;;)
    {
        if (shouldStop())
            return false;
        const int read = input->read(buffer.getData(), chunk);
        if (read < 0)
            return false;
        if (read == 0)
            break;
        if (! output->write(buffer.getData(), static_cast<size_t>(read)))
            return false;
        done += read;
        const float fraction = total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 1.0f;
        progress.store(juce::jlimit(0.0f, 1.0f, fraction), std::memory_order_release);
        if (++yields >= 4)
        {
            yields = 0;
            juce::Thread::sleep(1);
        }
    }
    output->flush();
    return true;
}

} // namespace

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

bool SessionDisk::startCopy(juce::File source,
                            juce::File destination,
                            std::function<void(bool, juce::String)> finished)
{
    Request request;
    request.job = Job::copy;
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

    struct Item
    {
        juce::File source;
        juce::File destination;
        std::int64_t bytes = 0;
    };
    std::vector<Item> items;
    const auto add = [&items](const juce::File& source, const juce::File& destination)
    {
        if (! source.existsAsFile())
            return;
        items.push_back({ source, destination, std::max<std::int64_t>(0, source.getSize()) });
    };

    for (const auto& child : request.source.findChildFiles(juce::File::findFiles, false))
        add(child, request.destination.getChildFile(child.getFileName()));

    const auto audio = request.source.getChildFile(kAudioFolderName);
    if (audio.isDirectory())
    {
        for (const auto& wav : audio.findChildFiles(juce::File::findFiles, false))
            add(wav, request.destination.getChildFile(kAudioFolderName).getChildFile(wav.getFileName()));
    }

    const auto backups = request.source.getChildFile("Backups");
    if (backups.isDirectory())
    {
        for (const auto& dir : backups.findChildFiles(juce::File::findDirectories, false))
        {
            const auto session = dir.getChildFile(kSessionFileName);
            add(session,
                request.destination.getChildFile("Backups").getChildFile(dir.getFileName()).getChildFile(kSessionFileName));
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
        if (! copyFileChunked(item.source, item.destination, done, total, progress_, [this]
                              { return threadShouldExit(); }))
        {
            if (! existed)
                request.destination.deleteRecursively();
            finish(false, "Could not copy " + item.source.getFileName());
            return;
        }
    }

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
