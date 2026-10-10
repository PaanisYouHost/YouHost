#include "Recorder.h"

#include <algorithm>
#include <cstring>
#include <cmath>

namespace youhost
{
namespace
{

constexpr int kPeakSamples = 4096;

bool ratesMatch(double left, double right) noexcept
{
    return left > 0.0 && right > 0.0 && std::abs(left - right) < 1.0;
}

void zeroRange(float* data, int count) noexcept
{
    if (data != nullptr && count > 0)
        std::memset(data, 0, sizeof(float) * static_cast<std::size_t>(count));
}

} // namespace

void Recorder::Ring::prepare(int size)
{
    data.assign(static_cast<std::size_t>(size), 0.0f);
    fifo = std::make_unique<juce::AbstractFifo>(size);
}

void Recorder::Ring::clear() noexcept
{
    if (fifo != nullptr)
        fifo->reset();
}

Recorder::Recorder()
    : juce::Thread("YouHost Disk")
{
    formats_.registerBasicFormats();
    ioScratch_.assign(static_cast<std::size_t>(kMaxChannels * kIoSamples), 0.0f);
    for (auto& ring : recordRings_)
        ring.prepare(kRingSamples);
    for (auto& ring : playRings_)
        ring.prepare(kRingSamples);
    armed_.fill(true);
    startThread(juce::Thread::Priority::high);
}

Recorder::~Recorder()
{
    callbacksLive_.store(false, std::memory_order_release);
    stop();
    signalThreadShouldExit();
    notify();
    stopThread(4000);
}

void Recorder::setAudioFolder(const juce::File& folder)
{
    const std::lock_guard<std::mutex> lock(stateLock_);
    audioFolder_ = folder;
}

void Recorder::setDevice(double sampleRate, bool callbacksLive)
{
    deviceRate_.store(sampleRate, std::memory_order_relaxed);
    setCallbacksLive(callbacksLive);

    const std::lock_guard<std::mutex> lock(stateLock_);
    if (takes_.empty() && mode_.load(std::memory_order_acquire) == static_cast<int>(TransportMode::stopped))
        timelineRate_.store(sampleRate, std::memory_order_relaxed);
}

void Recorder::setCallbacksLive(bool live) noexcept
{
    callbacksLive_.store(live, std::memory_order_release);
    audioHolding_.store(! live, std::memory_order_release);
}

double Recorder::timelineSampleRate() const noexcept
{
    const double timeline = timelineRate_.load(std::memory_order_relaxed);
    if (timeline > 0.0)
        return timeline;
    return deviceRate_.load(std::memory_order_relaxed);
}

void Recorder::setTimelineSampleRate(double sampleRate) noexcept
{
    if (sampleRate > 0.0)
        timelineRate_.store(sampleRate, std::memory_order_relaxed);
}

void Recorder::visitRecordedTakes(const std::function<void(const RecordedTakeView* takes, int count, const RecordedTakeView* live)>& fn) const
{
    if (fn == nullptr)
        return;

    const std::lock_guard<std::mutex> lock(stateLock_);
    std::vector<RecordedTakeView> views;
    views.reserve(takes_.size());
    int number = 1;
    for (const auto& take : takes_)
    {
        RecordedTakeView view;
        view.number = number++;
        view.start = take.start;
        view.length = take.length;
        for (int channel = 0; channel < kMaxChannels; ++channel)
        {
            const auto index = static_cast<std::size_t>(channel);
            view.recorded[index] = take.files[index].isNotEmpty();
            if (! take.channelPeaks[index].empty())
                view.peaks[index] = &take.channelPeaks[index];
        }
        views.push_back(view);
    }

    RecordedTakeView live;
    const RecordedTakeView* livePtr = nullptr;
    const auto liveLength = std::max(diskSamples_.load(std::memory_order_relaxed),
                                     audioSamples_.load(std::memory_order_relaxed));
    if (mode_.load(std::memory_order_relaxed) == static_cast<int>(TransportMode::recording) && liveLength > 0)
    {
        live.number = number;
        live.start = takeStart_.load(std::memory_order_relaxed);
        live.length = liveLength;
        for (int channel = 0; channel < kMaxChannels; ++channel)
        {
            const auto index = static_cast<std::size_t>(channel);
            live.recorded[index] = takeFiles_[index].isNotEmpty();
            if (! liveChannelPeaks_[index].empty())
                live.peaks[index] = &liveChannelPeaks_[index];
        }
        livePtr = &live;
    }

    fn(views.empty() ? nullptr : views.data(), static_cast<int>(views.size()), livePtr);
}

void Recorder::setDirtyHandler(std::function<void()> handler)
{
    dirty_ = std::move(handler);
}

void Recorder::markDirty()
{
    if (dirty_ != nullptr)
        dirty_();
}

int Recorder::pushRing(Ring& ring, const float* source, int numSamples) noexcept
{
    if (ring.fifo == nullptr || numSamples <= 0)
        return 0;

    int start1 = 0;
    int size1 = 0;
    int start2 = 0;
    int size2 = 0;
    ring.fifo->prepareToWrite(numSamples, start1, size1, start2, size2);
    const int written = size1 + size2;
    if (written <= 0)
        return 0;

    if (source == nullptr)
    {
        if (size1 > 0)
            zeroRange(ring.data.data() + start1, size1);
        if (size2 > 0)
            zeroRange(ring.data.data() + start2, size2);
    }
    else
    {
        if (size1 > 0)
            std::memcpy(ring.data.data() + start1, source, sizeof(float) * static_cast<std::size_t>(size1));
        if (size2 > 0)
            std::memcpy(ring.data.data() + start2,
                        source + size1,
                        sizeof(float) * static_cast<std::size_t>(size2));
    }

    ring.fifo->finishedWrite(written);
    return written;
}

int Recorder::popRing(Ring& ring, float* dest, int numSamples) noexcept
{
    if (ring.fifo == nullptr || dest == nullptr || numSamples <= 0)
        return 0;

    int start1 = 0;
    int size1 = 0;
    int start2 = 0;
    int size2 = 0;
    ring.fifo->prepareToRead(numSamples, start1, size1, start2, size2);
    const int ready = size1 + size2;
    if (ready <= 0)
        return 0;

    if (size1 > 0)
        std::memcpy(dest, ring.data.data() + start1, sizeof(float) * static_cast<std::size_t>(size1));
    if (size2 > 0)
        std::memcpy(dest + size1, ring.data.data() + start2, sizeof(float) * static_cast<std::size_t>(size2));
    ring.fifo->finishedRead(ready);
    return ready;
}

void Recorder::processRecord(const float* const* inputs,
                             int numInputs,
                             const std::int16_t* inputPacked,
                             int packedCount,
                             int numSamples,
                             int activeChannels) noexcept
{
    if (numSamples <= 0 || mode_.load(std::memory_order_acquire) != static_cast<int>(TransportMode::recording))
        return;

    const int limit = std::clamp(activeChannels, 0, kMaxChannels);
    int armed = 0;
    int space = kRingSamples;
    for (int channel = 0; channel < limit; ++channel)
    {
        if (recordMask_[static_cast<std::size_t>(channel)] == 0)
            continue;
        ++armed;
        if (recordRings_[static_cast<std::size_t>(channel)].fifo != nullptr)
            space = std::min(space, recordRings_[static_cast<std::size_t>(channel)].fifo->getFreeSpace());
    }

    const int queued = armed == 0 ? 0 : std::min(numSamples, std::max(0, space));
    if (armed > 0 && queued < numSamples)
        overflows_.fetch_add(numSamples - queued, std::memory_order_relaxed);

    for (int channel = 0; channel < limit && queued > 0; ++channel)
    {
        if (recordMask_[static_cast<std::size_t>(channel)] == 0)
            continue;

        const float* source = nullptr;
        if (inputs != nullptr && inputPacked != nullptr && channel < packedCount)
        {
            const int packed = inputPacked[channel];
            if (packed >= 0 && packed < numInputs)
                source = inputs[packed];
        }

        int remaining = queued;
        const float* cursor = source;
        while (remaining > 0)
        {
            const int chunk = std::min(remaining, kIoSamples);
            pushRing(recordRings_[static_cast<std::size_t>(channel)], cursor, chunk);
            if (cursor != nullptr)
                cursor += chunk;
            remaining -= chunk;
        }
    }

    const auto clock = audioSamples_.fetch_add(numSamples, std::memory_order_relaxed) + numSamples;
    position_.store(takeStart_.load(std::memory_order_relaxed) + clock, std::memory_order_relaxed);
}

bool Recorder::processPlayback(float* const* dest, int numSamples) noexcept
{
    if (dest == nullptr || numSamples <= 0)
        return false;
    if (mode_.load(std::memory_order_acquire) != static_cast<int>(TransportMode::playing))
        return false;

    if (hold_.load(std::memory_order_acquire))
    {
        audioHolding_.store(true, std::memory_order_release);
        for (int channel = 0; channel < kMaxChannels; ++channel)
            if (dest[channel] != nullptr)
                zeroRange(dest[channel], numSamples);
        return true;
    }

    audioHolding_.store(false, std::memory_order_release);

    int ready = kRingSamples;
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        if (playRings_[static_cast<std::size_t>(channel)].fifo == nullptr || dest[channel] == nullptr)
        {
            ready = 0;
            break;
        }
        ready = std::min(ready, playRings_[static_cast<std::size_t>(channel)].fifo->getNumReady());
    }

    const int pulled = std::max(0, std::min(numSamples, ready));
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        if (dest[channel] == nullptr)
            continue;
        if (pulled > 0)
            popRing(playRings_[static_cast<std::size_t>(channel)], dest[channel], pulled);
        if (pulled < numSamples)
            zeroRange(dest[channel] + pulled, numSamples - pulled);
    }

    if (pulled > 0)
        position_.fetch_add(pulled, std::memory_order_relaxed);

    const auto position = position_.load(std::memory_order_relaxed);
    const auto end = contentEnd_.load(std::memory_order_relaxed);
    if (position >= end && ready == 0)
        naturalEnd_.store(true, std::memory_order_release);

    return true;
}

void Recorder::noteCallback() noexcept
{
    callbackCount_.fetch_add(1, std::memory_order_release);
}

void Recorder::setArmed(int channel, bool armed)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        armed_[static_cast<std::size_t>(channel)] = armed;
    }
    markDirty();
}

bool Recorder::isArmed(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return false;
    const std::lock_guard<std::mutex> lock(stateLock_);
    return armed_[static_cast<std::size_t>(channel)];
}

void Recorder::setChannelName(int channel, const juce::String& name)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        names_[static_cast<std::size_t>(channel)] = name.trim().substring(0, 40);
    }
    markDirty();
}

juce::String Recorder::channelName(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return {};
    const std::lock_guard<std::mutex> lock(stateLock_);
    return names_[static_cast<std::size_t>(channel)];
}

void Recorder::setWavBitDepth(int bits)
{
    const std::lock_guard<std::mutex> lock(stateLock_);
    wavBitDepth_ = normaliseWavBitDepth(bits);
}

int Recorder::wavBitDepth() const
{
    const std::lock_guard<std::mutex> lock(stateLock_);
    return wavBitDepth_;
}

bool Recorder::waitUntilIdle()
{
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        if (idle_.load(std::memory_order_acquire))
            return true;
        juce::Thread::sleep(2);
    }
    return idle_.load(std::memory_order_acquire);
}

void Recorder::holdStreams()
{
    hold_.store(true, std::memory_order_release);
    notify();
    for (int attempt = 0; attempt < 80; ++attempt)
    {
        const bool playing = mode_.load(std::memory_order_acquire) == static_cast<int>(TransportMode::playing);
        const bool audioReady = ! callbacksLive_.load(std::memory_order_acquire) || ! playing
                                 || audioHolding_.load(std::memory_order_acquire);
        const bool diskReady = idle_.load(std::memory_order_acquire);
        if (audioReady && diskReady)
            return;
        juce::Thread::sleep(2);
    }
}

void Recorder::releaseStreams()
{
    hold_.store(false, std::memory_order_release);
    audioHolding_.store(false, std::memory_order_release);
    notify();
}

void Recorder::record(const std::int16_t* inputPacked, int packedCount)
{
    if (isRecording())
        return;
    if (isPlaying())
        stop();

    const double rate = deviceRate_.load(std::memory_order_relaxed);
    if (rate <= 0.0 || ! callbacksLive_.load(std::memory_order_acquire))
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        status_ = "Choose an audio device before recording.";
        return;
    }

    const double timeline = timelineRate_.load(std::memory_order_relaxed);
    bool hasTakes = false;
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        hasTakes = ! takes_.empty();
    }
    if (timeline > 0.0 && hasTakes && ! ratesMatch(timeline, rate))
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        status_ = "This session was recorded at a different sample rate.";
        return;
    }

    juce::File folder;
    std::array<bool, kMaxChannels> armSnapshot {};
    std::array<juce::String, kMaxChannels> nameSnapshot {};
    int takeNumber = 1;
    int bits = kDefaultWavBitDepth;
    std::int64_t start = 0;
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        folder = audioFolder_;
        armSnapshot = armed_;
        nameSnapshot = names_;
        takeNumber = static_cast<int>(takes_.size()) + 1;
        bits = wavBitDepth_;
        start = contentEndUnlocked();
        livePeaks_.clear();
    }

    if (folder.getFullPathName().isEmpty())
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        status_ = "No session folder yet.";
        return;
    }
    folder.createDirectory();

    std::array<std::uint8_t, kMaxChannels> mask {};
    std::array<juce::String, kMaxChannels> files {};
    std::vector<OpenWriter> writers;
    int armedChannels = 0;
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        const bool hasInput = inputPacked != nullptr && channel < packedCount && inputPacked[channel] >= 0;
        if (! armSnapshot[static_cast<std::size_t>(channel)] || ! hasInput)
            continue;

        const auto fileName = juce::String(takeWaveName(takeNumber,
                                                        channel + 1,
                                                        nameSnapshot[static_cast<std::size_t>(channel)].toStdString()));
        const auto file = folder.getChildFile(fileName);
        auto fileStream = std::make_unique<juce::FileOutputStream>(file);
        if (fileStream == nullptr || ! fileStream->openedOk())
            continue;
        std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);

        juce::WavAudioFormat wav;
        const auto options = juce::AudioFormatWriterOptions {}
                                 .withSampleRate(rate)
                                 .withNumChannels(1)
                                 .withBitsPerSample(bits)
                                 .withSampleFormat(wavBitDepthIsFloat(bits)
                                                       ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                       : juce::AudioFormatWriterOptions::SampleFormat::integral);
        auto writer = wav.createWriterFor(stream, options);
        if (writer == nullptr)
            continue;

        OpenWriter open;
        open.channel = channel;
        open.writer = std::move(writer);
        writers.push_back(std::move(open));
        mask[static_cast<std::size_t>(channel)] = 1;
        files[static_cast<std::size_t>(channel)] = fileName;
        ++armedChannels;
    }

    if (armedChannels == 0)
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        status_ = "Turn on a channel that has an input.";
        return;
    }

    waitUntilIdle();
    for (auto& ring : recordRings_)
        ring.clear();

    writers_ = std::move(writers);
    writersOpen_.store(true, std::memory_order_release);
    takeFiles_ = files;
    recordMask_ = mask;
    peakBucketCount_ = 0;
    peakBucketLow_ = 0.0f;
    peakBucketHigh_ = 0.0f;
    channelBucketLow_.fill(0.0f);
    channelBucketHigh_.fill(0.0f);
    channelBucketCount_.fill(0);
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        for (auto& peaks : liveChannelPeaks_)
            peaks.clear();
    }
    lastFlushMs_ = juce::Time::getMillisecondCounter();
    diskSamples_.store(0, std::memory_order_relaxed);
    audioSamples_.store(0, std::memory_order_relaxed);
    overflows_.store(0, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    naturalEnd_.store(false, std::memory_order_relaxed);
    takeStart_.store(start, std::memory_order_relaxed);
    position_.store(start, std::memory_order_relaxed);
    timelineRate_.store(rate, std::memory_order_relaxed);

    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        status_ = "Recording take " + juce::String(takeNumber);
    }

    mode_.store(static_cast<int>(TransportMode::recording), std::memory_order_release);
    notify();
    markDirty();
}

void Recorder::finishWriters()
{
    for (auto& open : writers_)
        if (open.writer != nullptr)
            open.writer->flush();
    writers_.clear();
    writersOpen_.store(false, std::memory_order_release);
}

void Recorder::deleteEmptyTakeFiles()
{
    juce::File folder;
    std::array<juce::String, kMaxChannels> files {};
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        folder = audioFolder_;
        files = takeFiles_;
    }
    for (const auto& name : files)
        if (name.isNotEmpty())
            folder.getChildFile(name).deleteFile();
}

void Recorder::commitTake()
{
    const auto written = diskSamples_.load(std::memory_order_relaxed);
    const auto heard = audioSamples_.load(std::memory_order_relaxed);
    std::vector<WavePeak> peaks;
    std::array<std::vector<WavePeak>, kMaxChannels> channelPeaks;
    std::array<juce::String, kMaxChannels> files {};
    std::int64_t start = 0;
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        peaks.swap(livePeaks_);
        channelPeaks.swap(liveChannelPeaks_);
        files = takeFiles_;
        start = takeStart_.load(std::memory_order_relaxed);
    }

    // A take the audio thread actually heard must stay on the timeline.
    // Only a rec/stop with no callbacks removes the empty header files.
    if (written <= 0 && heard <= 0)
    {
        deleteEmptyTakeFiles();
        const std::lock_guard<std::mutex> lock(stateLock_);
        status_ = "Recording stopped.";
        publishEnd();
        return;
    }

    const auto length = written > 0 ? written : heard;
    StoredTake take;
    take.start = start;
    take.length = length;
    take.files = files;
    take.peaks = std::move(peaks);
    take.channelPeaks = std::move(channelPeaks);
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        takes_.push_back(std::move(take));
        status_ = "Take " + juce::String(static_cast<int>(takes_.size())) + " saved.";
        if (written <= 0)
            status_ = "Take kept on the timeline. The disk had not flushed audio yet.";
        publishEnd();
    }
    position_.store(start + length, std::memory_order_relaxed);
    markDirty();
}

void Recorder::publishEnd()
{
    contentEnd_.store(contentEndUnlocked(), std::memory_order_relaxed);
}

std::int64_t Recorder::contentEndUnlocked() const
{
    std::vector<TakeSpan> spans;
    spans.reserve(takes_.size());
    for (const auto& take : takes_)
        spans.push_back({ take.start, take.length });
    if (spans.empty())
        return 0;
    return timelineEnd(spans.data(), static_cast<int>(spans.size()));
}

void Recorder::stop()
{
    const int previous = mode_.exchange(static_cast<int>(TransportMode::stopped), std::memory_order_acq_rel);
    naturalEnd_.store(false, std::memory_order_relaxed);
    const bool diskFailed = failed_.exchange(false, std::memory_order_relaxed);
    if (previous == static_cast<int>(TransportMode::stopped) && ! writersOpen_.load(std::memory_order_acquire))
        return;

    if (callbacksLive_.load(std::memory_order_acquire))
    {
        const auto seen = callbackCount_.load(std::memory_order_acquire);
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            if (callbackCount_.load(std::memory_order_acquire) != seen)
                break;
            juce::Thread::sleep(2);
        }
    }

    if (previous == static_cast<int>(TransportMode::recording) || writersOpen_.load(std::memory_order_acquire))
    {
        drainDone_.reset();
        drainRequest_.store(true, std::memory_order_release);
        notify();
        drainDone_.wait(4000);
        if (drainRequest_.load(std::memory_order_acquire))
        {
            // The disk thread did not finish. Close here only if it is idle.
            if (waitUntilIdle())
                finishWriters();
            drainRequest_.store(false, std::memory_order_relaxed);
        }
        commitTake();
        if (diskFailed)
        {
            const std::lock_guard<std::mutex> lock(stateLock_);
            status_ = "The disk could not keep up. The take was saved up to the last flush.";
        }
    }
    else
    {
        holdStreams();
        readers_.clear();
        for (auto& ring : playRings_)
            ring.clear();
        releaseStreams();
        const std::lock_guard<std::mutex> lock(stateLock_);
        status_ = "Stopped.";
    }
}

void Recorder::play()
{
    if (isRecording())
        stop();
    if (isPlaying())
        return;

    const double rate = deviceRate_.load(std::memory_order_relaxed);
    const double timeline = timelineRate_.load(std::memory_order_relaxed);
    if (rate <= 0.0 || ! callbacksLive_.load(std::memory_order_acquire))
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        status_ = "Choose an audio device before playback.";
        return;
    }
    if (timeline > 0.0 && ! ratesMatch(timeline, rate))
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        status_ = "Set the device to the session sample rate before playback.";
        return;
    }

    std::vector<StoredTake> takes;
    juce::File folder;
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        takes = takes_;
        folder = audioFolder_;
    }
    if (takes.empty())
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        status_ = "Nothing recorded yet.";
        return;
    }

    waitUntilIdle();
    readers_.clear();
    for (const auto& take : takes)
    {
        OpenReader open;
        open.start = take.start;
        open.length = take.length;
        for (int channel = 0; channel < kMaxChannels; ++channel)
        {
            const auto& name = take.files[static_cast<std::size_t>(channel)];
            if (name.isEmpty())
                continue;
            open.readers[static_cast<std::size_t>(channel)].reset(formats_.createReaderFor(folder.getChildFile(name)));
        }
        readers_.push_back(std::move(open));
    }

    auto position = position_.load(std::memory_order_relaxed);
    const auto end = contentEnd_.load(std::memory_order_relaxed);
    if (position >= end)
        position = 0;
    position_.store(position, std::memory_order_relaxed);
    readerCursor_.store(position, std::memory_order_relaxed);
    naturalEnd_.store(false, std::memory_order_relaxed);
    for (auto& ring : playRings_)
        ring.clear();

    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        status_ = "Playback - virtual soundcheck.";
    }
    mode_.store(static_cast<int>(TransportMode::playing), std::memory_order_release);
    notify();
}

void Recorder::locate(std::int64_t sample)
{
    if (isRecording())
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        status_ = "Stop recording before moving the playhead.";
        return;
    }

    const auto end = contentEnd_.load(std::memory_order_relaxed);
    const auto clamped = clampTimeline(sample, end);
    if (! isPlaying())
    {
        position_.store(clamped, std::memory_order_relaxed);
        return;
    }

    holdStreams();
    for (auto& ring : playRings_)
        ring.clear();
    position_.store(clamped, std::memory_order_relaxed);
    readerCursor_.store(clamped, std::memory_order_relaxed);
    naturalEnd_.store(false, std::memory_order_relaxed);
    releaseStreams();
    notify();
}

void Recorder::jumpMarker(int direction)
{
    std::vector<TakeSpan> spans;
    {
        const std::lock_guard<std::mutex> lock(stateLock_);
        spans.reserve(takes_.size());
        for (const auto& take : takes_)
            spans.push_back({ take.start, take.length });
    }
    const auto marks = takeMarkers(spans.data(), static_cast<int>(spans.size()));
    const auto position = position_.load(std::memory_order_relaxed);
    locate(direction < 0 ? previousMarker(position, marks) : nextMarker(position, marks));
}

void Recorder::nudgeSeconds(double seconds)
{
    const double rate = timelineRate_.load(std::memory_order_relaxed);
    const double device = deviceRate_.load(std::memory_order_relaxed);
    const double used = rate > 0.0 ? rate : device;
    if (used <= 0.0)
        return;
    const auto delta = static_cast<std::int64_t>(std::llround(seconds * used));
    const auto position = position_.load(std::memory_order_relaxed);
    const auto end = contentEnd_.load(std::memory_order_relaxed);
    locate(nudgeSamples(position, delta, std::max(end, position)));
}

void Recorder::addImportedTake(std::int64_t length, const std::array<juce::String, kMaxChannels>& files, double sampleRate)
{
    if (length <= 0)
        return;

    if (isRecording() || isPlaying())
        stop();

    const std::lock_guard<std::mutex> lock(stateLock_);
    StoredTake take;
    take.start = contentEndUnlocked();
    take.length = length;
    take.files = files;
    takes_.push_back(std::move(take));
    if (sampleRate > 0.0 && timelineRate_.load(std::memory_order_relaxed) <= 0.0)
        timelineRate_.store(sampleRate, std::memory_order_relaxed);
    publishEnd();
    position_.store(contentEndUnlocked(), std::memory_order_relaxed);
    status_ = juce::String(static_cast<int>(takes_.size())) + " takes on the timeline.";
    markDirty();
}

void Recorder::clearChannelNames()
{
    const std::lock_guard<std::mutex> lock(stateLock_);
    for (auto& name : names_)
        name.clear();
}

bool Recorder::channelHasTake(int channel) const
{
    if (channel < 0 || channel >= kMaxChannels)
        return false;
    const std::lock_guard<std::mutex> lock(stateLock_);
    for (const auto& take : takes_)
        if (take.files[static_cast<std::size_t>(channel)].isNotEmpty())
            return true;
    return false;
}

void Recorder::clearTakes()
{
    stop();
    const std::lock_guard<std::mutex> lock(stateLock_);
    takes_.clear();
    livePeaks_.clear();
    publishEnd();
    position_.store(0, std::memory_order_relaxed);
    if (deviceRate_.load(std::memory_order_relaxed) > 0.0)
        timelineRate_.store(deviceRate_.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

TransportView Recorder::view() const
{
    TransportView result;
    result.mode = static_cast<TransportMode>(mode_.load(std::memory_order_acquire));
    result.position = position_.load(std::memory_order_relaxed);
    result.sampleRate = timelineRate_.load(std::memory_order_relaxed);
    if (result.sampleRate <= 0.0)
        result.sampleRate = deviceRate_.load(std::memory_order_relaxed);
    result.overflows = overflows_.load(std::memory_order_relaxed);
    result.failed = failed_.load(std::memory_order_acquire);
    result.naturalEnd = naturalEnd_.load(std::memory_order_acquire);

    const std::lock_guard<std::mutex> lock(stateLock_);
    result.status = status_;
    result.length = contentEndUnlocked();
    result.takes.reserve(takes_.size());
    int number = 1;
    for (const auto& take : takes_)
    {
        TakeDraw draw;
        draw.number = number++;
        draw.span = { take.start, take.length };
        draw.files = take.files;
        result.takes.push_back(std::move(draw));
    }

    if (result.mode == TransportMode::recording)
    {
        result.liveValid = true;
        result.live.number = number;
        result.live.span.start = takeStart_.load(std::memory_order_relaxed);
        result.live.span.length = std::max<std::int64_t>(0, audioSamples_.load(std::memory_order_relaxed));
        result.live.files = takeFiles_;
        result.length = std::max(result.length, result.live.span.start + result.live.span.length);
        if (result.overflows > 0)
            result.status = "Recording - the disk fell behind, audio kept running.";
    }

    return result;
}

void Recorder::captureSession(SessionData& data) const
{
    const std::lock_guard<std::mutex> lock(stateLock_);
    data.sampleRate = timelineRate_.load(std::memory_order_relaxed);
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        data.channels[static_cast<std::size_t>(channel)].recordEnabled = armed_[static_cast<std::size_t>(channel)];
        data.channels[static_cast<std::size_t>(channel)].name = names_[static_cast<std::size_t>(channel)];
    }

    data.takes.clear();
    for (const auto& take : takes_)
    {
        SessionTake stored;
        stored.startSample = take.start;
        stored.lengthSamples = take.length;
        stored.files = take.files;
        stored.peaks = take.peaks;
        stored.channelPeaks = take.channelPeaks;
        data.takes.push_back(std::move(stored));
    }

    if (mode_.load(std::memory_order_acquire) == static_cast<int>(TransportMode::recording))
    {
        SessionTake live;
        live.startSample = takeStart_.load(std::memory_order_relaxed);
        live.lengthSamples = diskSamples_.load(std::memory_order_relaxed);
        live.files = takeFiles_;
        live.peaks = livePeaks_;
        live.channelPeaks = liveChannelPeaks_;
        if (live.lengthSamples > 0)
            data.takes.push_back(std::move(live));
    }
}

void Recorder::restoreSession(const SessionData& data, const juce::File& audioFolder)
{
    stop();
    const std::lock_guard<std::mutex> lock(stateLock_);
    audioFolder_ = audioFolder;
    wavBitDepth_ = normaliseWavBitDepth(data.wavBitDepth);
    takes_.clear();
    livePeaks_.clear();
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        armed_[static_cast<std::size_t>(channel)] = data.channels[static_cast<std::size_t>(channel)].recordEnabled;
        names_[static_cast<std::size_t>(channel)] = data.channels[static_cast<std::size_t>(channel)].name;
    }
    for (const auto& source : data.takes)
    {
        StoredTake take;
        take.start = source.startSample;
        take.length = source.lengthSamples;
        take.files = source.files;
        take.peaks = source.peaks;
        take.channelPeaks = source.channelPeaks;
        takes_.push_back(std::move(take));
    }
    timelineRate_.store(data.sampleRate, std::memory_order_relaxed);
    publishEnd();
    position_.store(0, std::memory_order_relaxed);
    status_ = takes_.empty() ? juce::String() : juce::String(static_cast<int>(takes_.size())) + " takes loaded.";
}

void Recorder::run()
{
    while (! threadShouldExit())
    {
        const int mode = mode_.load(std::memory_order_acquire);
        const bool draining = drainRequest_.load(std::memory_order_acquire);
        if (mode == static_cast<int>(TransportMode::recording) || draining)
        {
            idle_.store(false, std::memory_order_release);
            if (! drainOnce())
                wait(4);
        }
        else if (mode == static_cast<int>(TransportMode::playing) && ! hold_.load(std::memory_order_acquire))
        {
            idle_.store(false, std::memory_order_release);
            if (! fillOnce())
                wait(2);
        }
        else
        {
            idle_.store(true, std::memory_order_release);
            wait(4);
        }
    }
    idle_.store(true, std::memory_order_release);
}

bool Recorder::drainOnce()
{
    if (writers_.empty())
    {
        if (drainRequest_.exchange(false, std::memory_order_acq_rel))
            drainDone_.signal();
        return false;
    }

    int ready = kRingSamples;
    for (const auto& open : writers_)
    {
        auto* fifo = recordRings_[static_cast<std::size_t>(open.channel)].fifo.get();
        ready = std::min(ready, fifo != nullptr ? fifo->getNumReady() : 0);
    }

    if (ready <= 0)
    {
        if (drainRequest_.load(std::memory_order_acquire)
            && mode_.load(std::memory_order_acquire) != static_cast<int>(TransportMode::recording))
        {
            if (peakBucketCount_ > 0)
            {
                const std::lock_guard<std::mutex> lock(stateLock_);
                livePeaks_.push_back({ peakBucketLow_, peakBucketHigh_ });
                for (const auto& open : writers_)
                {
                    if (open.channel < 0 || open.channel >= kMaxChannels)
                        continue;
                    const auto index = static_cast<std::size_t>(open.channel);
                    liveChannelPeaks_[index].push_back({ channelBucketLow_[index], channelBucketHigh_[index] });
                    channelBucketLow_[index] = 0.0f;
                    channelBucketHigh_[index] = 0.0f;
                    channelBucketCount_[index] = 0;
                }
                peakBucketCount_ = 0;
                peakBucketLow_ = 0.0f;
                peakBucketHigh_ = 0.0f;
            }
            finishWriters();
            if (drainRequest_.exchange(false, std::memory_order_acq_rel))
                drainDone_.signal();
        }
        return false;
    }

    const int count = std::min(ready, kIoSamples);
    std::array<const float*, kMaxChannels> pointers {};
    for (int channel = 0; channel < kMaxChannels; ++channel)
        pointers[static_cast<std::size_t>(channel)] = ioScratch_.data() + static_cast<std::size_t>(channel * kIoSamples);

    for (const auto& open : writers_)
    {
        auto* dest = ioScratch_.data() + static_cast<std::size_t>(open.channel * kIoSamples);
        zeroRange(dest, count);
        popRing(recordRings_[static_cast<std::size_t>(open.channel)], dest, count);
    }

    for (int index = 0; index < count; ++index)
    {
        float low = 0.0f;
        float high = 0.0f;
        for (const auto& open : writers_)
        {
            if (open.channel < 0 || open.channel >= kMaxChannels)
                continue;
            const auto channelIndex = static_cast<std::size_t>(open.channel);
            const float sample = pointers[channelIndex][index];
            low = std::min(low, sample);
            high = std::max(high, sample);
            channelBucketLow_[channelIndex] = std::min(channelBucketLow_[channelIndex], sample);
            channelBucketHigh_[channelIndex] = std::max(channelBucketHigh_[channelIndex], sample);
            ++channelBucketCount_[channelIndex];
        }
        peakBucketLow_ = std::min(peakBucketLow_, low);
        peakBucketHigh_ = std::max(peakBucketHigh_, high);
        if (++peakBucketCount_ >= kPeakSamples)
        {
            const std::lock_guard<std::mutex> lock(stateLock_);
            livePeaks_.push_back({ peakBucketLow_, peakBucketHigh_ });
            for (const auto& open : writers_)
            {
                if (open.channel < 0 || open.channel >= kMaxChannels)
                    continue;
                const auto channelIndex = static_cast<std::size_t>(open.channel);
                if (channelBucketCount_[channelIndex] <= 0)
                    continue;
                liveChannelPeaks_[channelIndex].push_back({ channelBucketLow_[channelIndex], channelBucketHigh_[channelIndex] });
                channelBucketLow_[channelIndex] = 0.0f;
                channelBucketHigh_[channelIndex] = 0.0f;
                channelBucketCount_[channelIndex] = 0;
            }
            peakBucketCount_ = 0;
            peakBucketLow_ = 0.0f;
            peakBucketHigh_ = 0.0f;
        }
    }
    bool failed = false;
    for (auto& open : writers_)
    {
        if (open.writer == nullptr)
            continue;
        const float* channelData = pointers[static_cast<std::size_t>(open.channel)];
        if (! open.writer->writeFromFloatArrays(&channelData, 1, count))
            failed = true;
    }

    if (failed)
        failed_.store(true, std::memory_order_release);

    diskSamples_.fetch_add(count, std::memory_order_relaxed);

    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastFlushMs_ >= 1000u)
    {
        for (auto& open : writers_)
            if (open.writer != nullptr)
                open.writer->flush();
        lastFlushMs_ = now;
    }

    return true;
}

void Recorder::readTimeline(std::int64_t position, int numSamples)
{
    std::array<float*, kMaxChannels> pointers {};
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        pointers[static_cast<std::size_t>(channel)] = ioScratch_.data() + static_cast<std::size_t>(channel * kIoSamples);
        zeroRange(pointers[static_cast<std::size_t>(channel)], numSamples);
    }

    int written = 0;
    auto cursor = position;
    while (written < numSamples)
    {
        OpenReader* take = nullptr;
        for (auto& candidate : readers_)
        {
            if (cursor >= candidate.start && cursor < candidate.start + candidate.length)
            {
                take = &candidate;
                break;
            }
        }

        if (take == nullptr)
        {
            std::int64_t gap = numSamples - written;
            for (const auto& candidate : readers_)
            {
                if (candidate.start > cursor)
                    gap = std::min(gap, candidate.start - cursor);
            }
            const int count = static_cast<int>(std::min<std::int64_t>(gap, numSamples - written));
            written += count;
            cursor += count;
            continue;
        }

        const auto into = cursor - take->start;
        const auto available = take->length - into;
        const int count = static_cast<int>(std::min<std::int64_t>(available, numSamples - written));
        for (int channel = 0; channel < kMaxChannels; ++channel)
        {
            auto& reader = take->readers[static_cast<std::size_t>(channel)];
            if (reader == nullptr)
                continue;
            float* dest = pointers[static_cast<std::size_t>(channel)] + written;
            reader->read(&dest, 1, into, count);
        }
        written += count;
        cursor += count;
    }
}

bool Recorder::fillOnce()
{
    if (hold_.load(std::memory_order_acquire) || readers_.empty())
        return false;

    int space = kRingSamples;
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        auto* fifo = playRings_[static_cast<std::size_t>(channel)].fifo.get();
        space = std::min(space, fifo != nullptr ? fifo->getFreeSpace() : 0);
    }
    if (space < 512)
        return false;

    const auto end = contentEnd_.load(std::memory_order_relaxed);
    auto cursor = readerCursor_.load(std::memory_order_relaxed);
    if (cursor >= end)
        return false;

    const int count = static_cast<int>(std::min<std::int64_t>(std::min(space, kIoSamples), end - cursor));
    if (count <= 0)
        return false;

    readTimeline(cursor, count);
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        const float* source = ioScratch_.data() + static_cast<std::size_t>(channel * kIoSamples);
        pushRing(playRings_[static_cast<std::size_t>(channel)], source, count);
    }
    readerCursor_.store(cursor + count, std::memory_order_relaxed);
    return true;
}

} // namespace youhost
