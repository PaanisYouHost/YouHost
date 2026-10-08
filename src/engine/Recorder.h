#pragma once

#include "SessionDocument.h"
#include "TakePlan.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace youhost
{

enum class TransportMode
{
    stopped = 0,
    recording,
    playing
};

struct TakeDraw
{
    int number = 0;
    TakeSpan span;
    std::vector<WavePeak> peaks;
    std::array<juce::String, kMaxChannels> files {};
};

struct TransportView
{
    TransportMode mode = TransportMode::stopped;
    std::int64_t position = 0;
    std::int64_t length = 0;
    double sampleRate = 0.0;
    int overflows = 0;
    bool failed = false;
    bool naturalEnd = false;
    juce::String status;
    std::vector<TakeDraw> takes;
    TakeDraw live;
    bool liveValid = false;
};

// Multitrack WAV recorder. The audio thread only pushes or pulls lock-free
// rings. A disk thread owns the files, flushes the WAV header about once a
// second, and lets JUCE switch the file to RF64 once it passes 4 GB.
class Recorder : private juce::Thread
{
public:
    Recorder();
    ~Recorder() override;

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void setAudioFolder(const juce::File& folder);
    void setDevice(double sampleRate, bool callbacksLive);
    void setDirtyHandler(std::function<void()> handler);

    void processRecord(const float* const* inputs,
                       int numInputs,
                       const std::int16_t* inputPacked,
                       int packedCount,
                       int numSamples) noexcept;
    bool processPlayback(float* const* dest, int numSamples) noexcept;
    void noteCallback() noexcept;

    bool isRecording() const noexcept { return mode_.load(std::memory_order_acquire) == static_cast<int>(TransportMode::recording); }
    bool isPlaying() const noexcept { return mode_.load(std::memory_order_acquire) == static_cast<int>(TransportMode::playing); }
    std::int64_t playhead() const noexcept { return position_.load(std::memory_order_relaxed); }
    void addImportedTake(std::int64_t length, const std::array<juce::String, kMaxChannels>& files, double sampleRate);

    void setArmed(int channel, bool armed);
    bool isArmed(int channel) const;
    void setChannelName(int channel, const juce::String& name);
    juce::String channelName(int channel) const;
    void setWavBitDepth(int bits);
    int wavBitDepth() const;

    void record(const std::int16_t* inputPacked, int packedCount);
    void stop();
    void play();
    void locate(std::int64_t sample);
    void jumpMarker(int direction);
    void nudgeSeconds(double seconds);
    void clearTakes();

    TransportView view() const;
    void captureSession(SessionData& data) const;
    void restoreSession(const SessionData& data, const juce::File& audioFolder);

private:
    struct Ring
    {
        std::unique_ptr<juce::AbstractFifo> fifo;
        std::vector<float> data;

        void prepare(int size);
        void clear() noexcept;
    };

    struct StoredTake
    {
        std::int64_t start = 0;
        std::int64_t length = 0;
        std::array<juce::String, kMaxChannels> files {};
        std::vector<WavePeak> peaks;
    };

    struct OpenWriter
    {
        int channel = 0;
        std::unique_ptr<juce::AudioFormatWriter> writer;
    };

    struct OpenReader
    {
        std::int64_t start = 0;
        std::int64_t length = 0;
        std::array<std::unique_ptr<juce::AudioFormatReader>, kMaxChannels> readers {};
    };

    void run() override;
    bool drainOnce();
    bool fillOnce();
    void finishWriters();
    void commitTake();
    void deleteEmptyTakeFiles();
    bool waitUntilIdle();
    void holdStreams();
    void releaseStreams();
    std::int64_t contentEndUnlocked() const;
    void publishEnd();
    void markDirty();
    int pushRing(Ring& ring, const float* source, int numSamples) noexcept;
    int popRing(Ring& ring, float* dest, int numSamples) noexcept;
    void readTimeline(std::int64_t position, int numSamples);

    static constexpr int kRingSamples = 32768;
    static constexpr int kIoSamples = 2048;

    juce::AudioFormatManager formats_;
    std::array<Ring, kMaxChannels> recordRings_ {};
    std::array<Ring, kMaxChannels> playRings_ {};
    std::vector<float> ioScratch_;

    mutable std::mutex stateLock_;
    std::vector<StoredTake> takes_;
    std::array<bool, kMaxChannels> armed_ {};
    std::array<juce::String, kMaxChannels> names_ {};
    std::vector<WavePeak> livePeaks_;
    juce::String status_;
    std::function<void()> dirty_;

    std::array<std::uint8_t, kMaxChannels> recordMask_ {};
    std::array<juce::String, kMaxChannels> takeFiles_ {};
    std::vector<OpenWriter> writers_;
    std::vector<OpenReader> readers_;
    juce::File audioFolder_;
    int wavBitDepth_ = kDefaultWavBitDepth;

    std::atomic<int> mode_ { static_cast<int>(TransportMode::stopped) };
    std::atomic<bool> callbacksLive_ { false };
    std::atomic<bool> drainRequest_ { false };
    std::atomic<bool> idle_ { true };
    std::atomic<bool> hold_ { false };
    std::atomic<bool> audioHolding_ { false };
    std::atomic<bool> failed_ { false };
    std::atomic<bool> naturalEnd_ { false };
    std::atomic<std::int64_t> position_ { 0 };
    std::atomic<std::int64_t> takeStart_ { 0 };
    std::atomic<std::int64_t> audioSamples_ { 0 };
    std::atomic<std::int64_t> diskSamples_ { 0 };
    std::atomic<std::int64_t> contentEnd_ { 0 };
    std::atomic<std::int64_t> readerCursor_ { 0 };
    std::atomic<int> overflows_ { 0 };
    std::atomic<std::uint64_t> callbackCount_ { 0 };
    std::atomic<double> deviceRate_ { 0.0 };
    std::atomic<double> timelineRate_ { 0.0 };

    int peakBucketCount_ = 0;
    float peakBucketLow_ = 0.0f;
    float peakBucketHigh_ = 0.0f;
    std::uint32_t lastFlushMs_ = 0;
    std::atomic<bool> writersOpen_ { false };

    juce::WaitableEvent drainDone_;
};

} // namespace youhost
