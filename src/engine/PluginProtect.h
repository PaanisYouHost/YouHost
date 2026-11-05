#pragma once

#include "SafetyFade.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

// One device block at the live buffer. The channel delay is this many
// samples and no more, and only after a plugin has crashed or quit.
inline constexpr int kProtectedLatencySamples = 32;
inline constexpr int kProtectRingSamples = 4096;
inline constexpr int kProofAudioMs = 8000;

enum class ProofResult
{
    passed,
    crashed,
    quit,
    timedOut,
    workerFailed
};

enum class HostingMode
{
    inProcess,
    protectedMode,
    notAdded
};

struct ProtectedRecord
{
    std::string identifier;
    std::string version;
    std::string reason;
};

inline std::string pluginBuildKey(std::string_view version, std::int64_t fileTimeMs)
{
    std::string key(version);
    key.push_back('@');
    key += std::to_string(fileTimeMs);
    return key;
}

inline const ProtectedRecord* findProtected(const std::vector<ProtectedRecord>& list, std::string_view identifier)
{
    for (const auto& record : list)
        if (record.identifier == identifier)
            return &record;
    return nullptr;
}

// First load, and any load whose version key changed, is tested out of process.
// A scanner timeout is not proof. A crash or a quit is.
inline bool proofRequired(const std::vector<ProtectedRecord>& list,
                          std::string_view identifier,
                          std::string_view version,
                          bool alreadyPassedThisVersion) noexcept
{
    if (alreadyPassedThisVersion)
        return false;
    const auto* found = findProtected(list, identifier);
    if (found == nullptr)
        return true;
    return found->version != version;
}

inline HostingMode hostingAfterProof(ProofResult result) noexcept
{
    if (result == ProofResult::crashed || result == ProofResult::quit)
        return HostingMode::protectedMode;
    return HostingMode::inProcess;
}

inline int protectedChannelLatency(int protectedPluginsOnChannel) noexcept
{
    return protectedPluginsOnChannel > 0 ? kProtectedLatencySamples : 0;
}

inline bool protectsOnProof(ProofResult result) noexcept
{
    return hostingAfterProof(result) == HostingMode::protectedMode;
}

struct ProtectRing
{
    std::atomic<std::uint32_t> inCount { 0 };
    std::atomic<std::uint32_t> outCount { 0 };
    std::atomic<std::uint32_t> generation { 1 };
    std::atomic<std::uint32_t> alive { 1 };
    float inL[kProtectRingSamples] {};
    float inR[kProtectRingSamples] {};
    float outL[kProtectRingSamples] {};
    float outR[kProtectRingSamples] {};
};

struct ProtectPull
{
    int processed = 0;
    int silenced = 0;
    bool passedDry = false;
};

// Host side. Writes this block, then reads the block that is `latency`
// samples behind. Missing helper audio becomes silence, never the dry input.
inline ProtectPull protectExchange(ProtectRing& ring,
                                   const float* inL,
                                   const float* inR,
                                   float* outL,
                                   float* outR,
                                   int samples,
                                   int latency,
                                   std::uint32_t& seenGeneration,
                                   bool helperAlive) noexcept
{
    ProtectPull pull;
    if (samples <= 0)
        return pull;
    if (latency < 0)
        latency = 0;
    if (latency > kProtectRingSamples / 4)
        latency = kProtectRingSamples / 4;

    const auto generation = ring.generation.load(std::memory_order_acquire);
    if (generation != seenGeneration)
    {
        seenGeneration = generation;
        ring.inCount.store(ring.outCount.load(std::memory_order_acquire), std::memory_order_relaxed);
    }

    auto written = ring.inCount.load(std::memory_order_relaxed);
    for (int index = 0; index < samples; ++index)
    {
        const auto slot = (written + static_cast<std::uint32_t>(index)) % static_cast<std::uint32_t>(kProtectRingSamples);
        ring.inL[slot] = inL != nullptr ? inL[index] : 0.0f;
        ring.inR[slot] = inR != nullptr ? inR[index] : 0.0f;
    }
    written += static_cast<std::uint32_t>(samples);
    ring.inCount.store(written, std::memory_order_release);

    const auto published = ring.outCount.load(std::memory_order_acquire);
    const bool running = helperAlive && ring.alive.load(std::memory_order_acquire) != 0;
    for (int index = 0; index < samples; ++index)
    {
        const auto absolute = written - static_cast<std::uint32_t>(samples) - static_cast<std::uint32_t>(latency)
                              + static_cast<std::uint32_t>(index);
        const bool ready = running && published > absolute;
        if (! ready)
        {
            if (outL != nullptr)
                outL[index] = 0.0f;
            if (outR != nullptr)
                outR[index] = 0.0f;
            ++pull.silenced;
            continue;
        }
        const auto slot = absolute % static_cast<std::uint32_t>(kProtectRingSamples);
        if (outL != nullptr)
            outL[index] = ring.outL[slot];
        if (outR != nullptr)
            outR[index] = ring.outR[slot];
        ++pull.processed;
    }
    pull.passedDry = false;
    return pull;
}

// Helper side. Processes newly written samples and publishes them in order.
template <typename Process>
inline int protectConsume(ProtectRing& ring, int maxSamples, Process&& process) noexcept
{
    const auto written = ring.inCount.load(std::memory_order_acquire);
    auto read = ring.outCount.load(std::memory_order_relaxed);
    if (written < read)
        return 0;
    int todo = static_cast<int>(written - read);
    if (todo > maxSamples)
        todo = maxSamples;
    if (todo <= 0)
        return 0;

    float inL[256];
    float inR[256];
    float outL[256];
    float outR[256];
    int done = 0;
    while (done < todo)
    {
        int chunk = todo - done;
        if (chunk > 256)
            chunk = 256;
        for (int index = 0; index < chunk; ++index)
        {
            const auto slot = (read + static_cast<std::uint32_t>(index)) % static_cast<std::uint32_t>(kProtectRingSamples);
            inL[index] = ring.inL[slot];
            inR[index] = ring.inR[slot];
            outL[index] = 0.0f;
            outR[index] = 0.0f;
        }
        process(inL, inR, outL, outR, chunk);
        for (int index = 0; index < chunk; ++index)
        {
            const auto slot = (read + static_cast<std::uint32_t>(index)) % static_cast<std::uint32_t>(kProtectRingSamples);
            ring.outL[slot] = outL[index];
            ring.outR[slot] = outR[index];
        }
        read += static_cast<std::uint32_t>(chunk);
        done += chunk;
    }
    ring.outCount.store(read, std::memory_order_release);
    return done;
}

// The audio-thread block for one protected plugin. Dry samples are kept only
// so an explicit bypass can fade them in. Every other path writes silence
// or the helper's audio.
inline ProtectPull renderProtectedBlock(ProtectRing& ring,
                                        SafetyState& safety,
                                        const float* inL,
                                        const float* inR,
                                        float* outL,
                                        float* outR,
                                        int samples,
                                        std::uint32_t& seenGeneration,
                                        bool helperAlive,
                                        bool userBypass) noexcept
{
    ProtectPull pull;
    if (samples <= 0)
        return pull;
    int offset = 0;
    while (offset < samples)
    {
        const int chunk = samples - offset > 256 ? 256 : samples - offset;
        float dryL[256];
        float dryR[256];
        const float* chunkL = inL != nullptr ? inL + offset : nullptr;
        const float* chunkR = inR != nullptr ? inR + offset : nullptr;
        float* outChunkL = outL != nullptr ? outL + offset : nullptr;
        float* outChunkR = outR != nullptr ? outR + offset : nullptr;
        for (int index = 0; index < chunk; ++index)
        {
            dryL[index] = chunkL != nullptr ? chunkL[index] : 0.0f;
            dryR[index] = chunkR != nullptr ? chunkR[index] : 0.0f;
        }
        auto part = protectExchange(ring, chunkL, chunkR, outChunkL, outChunkR, chunk, kProtectedLatencySamples, seenGeneration, helperAlive);
        const bool ready = helperAlive && part.processed == chunk;
        const bool running = helperAlive && ring.alive.load(std::memory_order_acquire) != 0;
        const auto step = stepChannelSafety(safety, ready, running, userBypass, chunk);
        applySafety(outChunkL, outChunkR, chunk, step.gain, step.source, dryL, dryR);
        pull.processed += step.source == SafetySource::processed ? chunk : 0;
        pull.silenced += step.source == SafetySource::silence ? chunk : 0;
        pull.passedDry = pull.passedDry || step.passedUnprocessed;
        offset += chunk;
    }
    return pull;
}

inline std::string writeProtectedList(const std::vector<ProtectedRecord>& list)
{
    std::string text;
    for (const auto& record : list)
    {
        if (record.identifier.empty())
            continue;
        text += record.identifier;
        text.push_back('\t');
        text += record.version;
        text.push_back('\t');
        text += record.reason.empty() ? "crash" : record.reason;
        text.push_back('\n');
    }
    return text;
}

inline std::vector<ProtectedRecord> readProtectedList(std::string_view text)
{
    std::vector<ProtectedRecord> list;
    std::size_t cursor = 0;
    while (cursor < text.size())
    {
        auto end = text.find('\n', cursor);
        auto line = text.substr(cursor, end == std::string_view::npos ? text.size() - cursor : end - cursor);
        cursor = end == std::string_view::npos ? text.size() : end + 1;
        if (! line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty())
            continue;
        const auto first = line.find('\t');
        const auto second = first == std::string_view::npos ? std::string_view::npos : line.find('\t', first + 1);
        ProtectedRecord record;
        if (first == std::string_view::npos)
        {
            record.identifier = std::string(line);
        }
        else
        {
            record.identifier = std::string(line.substr(0, first));
            if (second == std::string_view::npos)
                record.version = std::string(line.substr(first + 1));
            else
            {
                record.version = std::string(line.substr(first + 1, second - first - 1));
                record.reason = std::string(line.substr(second + 1));
            }
        }
        if (! record.identifier.empty())
            list.push_back(std::move(record));
    }
    return list;
}

inline void upsertProtected(std::vector<ProtectedRecord>& list, ProtectedRecord record)
{
    for (auto& existing : list)
    {
        if (existing.identifier == record.identifier)
        {
            existing = std::move(record);
            return;
        }
    }
    list.push_back(std::move(record));
}

} // namespace youhost
