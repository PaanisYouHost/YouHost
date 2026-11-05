#pragma once

#include "PluginProtect.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace youhost
{

class ProtectBridge
{
public:
    static std::shared_ptr<ProtectBridge> open(const juce::PluginDescription& description,
                                               const juce::MemoryBlock& state,
                                               double sampleRate,
                                               int blockSize,
                                               int channels);

    ~ProtectBridge();

    ProtectBridge(const ProtectBridge&) = delete;
    ProtectBridge& operator=(const ProtectBridge&) = delete;

    // Audio thread. No allocation, no lock, no log.
    void render(float* left, float* right, int samples) noexcept;

    void requestEditor() noexcept;
    void setBypass(bool bypass) noexcept;
    bool isAlive() const noexcept;
    bool isReady() const noexcept;
    bool gaveUp() const noexcept { return restarts_ > 4 && ! isAlive(); }
    double mutedSeconds(double sampleRate) const noexcept;
    juce::MemoryBlock latestState() const;
    void poll();

    const juce::PluginDescription& description() const noexcept { return description_; }

private:
    ProtectBridge(const juce::PluginDescription& description,
                  const juce::MemoryBlock& state,
                  double sampleRate,
                  int blockSize,
                  int channels);

    bool launch(bool proof);
    void markDead() noexcept;
    void restart();

    juce::PluginDescription description_;
    juce::MemoryBlock state_;
    double sampleRate_ = 48000.0;
    int blockSize_ = 32;
    int channels_ = 2;
    juce::File directory_;
    juce::ChildProcess process_;
    ProtectRing* ring_ = nullptr;
    int mapFd_ = -1;
    void* mapped_ = nullptr;
    std::size_t mapBytes_ = 0;
    std::uint32_t seenGeneration_ = 0;
    SafetyState safety_ {};
    std::atomic<int> bypass_ { 0 };
    std::atomic<std::uint64_t> mutedSamples_ { 0 };
    std::atomic<int> alive_ { 0 };
    std::atomic<int> ready_ { 0 };
    void* wake_ = nullptr;
    int restarts_ = 0;
    bool restarting_ = false;
    bool sawRunning_ = false;
};

ProofResult runPluginProof(const juce::PluginDescription& description,
                           double sampleRate,
                           int blockSize,
                           int channels);

bool isProtectWorkerCommandLine(const juce::String& commandLine);
void runProtectWorker(const juce::String& commandLine);

} // namespace youhost
