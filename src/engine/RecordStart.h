#pragma once

#include "HostLimits.h"
#include "RecordLock.h"
#include "TakePlan.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace youhost
{

inline constexpr const char* kRecordArmedHint = "REC ARMED - press PLAY (or Cmd+Space)";

enum class TransportPress
{
    play,
    stop,
    space,
    commandSpace
};

struct RecordChannelGate
{
    bool rec = false;
    bool inputOpen = false;
};

// Everything the transport needs to decide a press. Channel gates are the REC
// buttons and the device inputs. A card-size limit is not a gate: a channel
// with an open input can record even when a stale visible-count is zero.
struct RecordAttempt
{
    bool buttonArmed = false;
    bool lockArmed = false;
    bool recording = false;
    bool playing = false;
    bool hasSession = false;
    bool folderWritable = true;
    std::string folderProblem;
    bool deviceLive = false;
    bool offline = false;
    bool copyBusy = false;
    bool hasTakes = false;
    double deviceRate = 0.0;
    double timelineRate = 0.0;
    double preferredRate = 48000.0;
    int takeNumber = 1;
    std::array<RecordChannelGate, kMaxChannels> channels {};
    std::array<std::string, kMaxChannels> names {};
};

struct RecordAttemptResult
{
    bool startRecording = false;
    bool startPlayback = false;
    bool stop = false;
    std::string log;
    std::string alert;
    double rate = 0.0;
    std::vector<int> channels;
    std::vector<std::string> files;
};

inline bool channelCanRecord(bool rec, bool inputOpen, bool offline) noexcept
{
    if (! rec)
        return false;
    return offline || inputOpen;
}

inline double recordAttemptRate(const RecordAttempt& attempt) noexcept
{
    if (attempt.deviceLive && attempt.deviceRate > 0.0)
        return attempt.deviceRate;
    if (attempt.offline && attempt.preferredRate > 0.0)
        return attempt.preferredRate;
    if (attempt.timelineRate > 0.0)
        return attempt.timelineRate;
    return 0.0;
}

inline bool recordRatesConflict(const RecordAttempt& attempt, double rate) noexcept
{
    if (! attempt.hasTakes || ! (attempt.timelineRate > 0.0) || ! (rate > 0.0))
        return false;
    return std::abs(attempt.timelineRate - rate) >= 1.0;
}

inline RecordAttemptResult failAttempt(std::string reason)
{
    RecordAttemptResult result;
    result.log = "record start failed: " + reason;
    result.alert = reason;
    return result;
}

inline RecordAttemptResult planRecordStart(const RecordAttempt& attempt)
{
    if (recordLockEngaged(attempt.lockArmed, attempt.recording))
        return failAttempt("Recording is locked. Click the padlock to unlock.");
    if (attempt.recording)
        return failAttempt("Recording is already running.");
    if (attempt.copyBusy)
        return failAttempt("Finish saving the copy before recording.");
    if (! attempt.hasSession)
        return failAttempt("This session has no folder yet. Recording did not start.");
    if (! attempt.folderWritable)
    {
        if (! attempt.folderProblem.empty())
            return failAttempt(attempt.folderProblem);
        return failAttempt("The session folder is not writable. Recording did not start.");
    }
    if (! attempt.offline && ! attempt.deviceLive)
        return failAttempt("The audio device is not running. Recording did not start.");

    const double rate = recordAttemptRate(attempt);
    if (! (rate > 0.0))
        return failAttempt("There is no sample rate. Recording did not start.");
    if (recordRatesConflict(attempt, rate))
        return failAttempt("This session was recorded at a different sample rate.");

    RecordAttemptResult result;
    result.rate = rate;
    const int take = attempt.takeNumber < 1 ? 1 : attempt.takeNumber;
    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        const auto& gate = attempt.channels[static_cast<std::size_t>(channel)];
        if (! channelCanRecord(gate.rec, gate.inputOpen, attempt.offline))
            continue;
        result.channels.push_back(channel);
        result.files.push_back(takeWaveName(take, channel + 1, attempt.names[static_cast<std::size_t>(channel)]));
    }

    if (result.channels.empty())
    {
        if (attempt.offline)
            return failAttempt("Every channel is OFF. Recording did not start.");
        return failAttempt("No REC channel has an input on this device. Recording did not start.");
    }

    result.startRecording = true;
    result.log = "record start: take " + std::to_string(take)
                 + " channels " + std::to_string(result.channels.size())
                 + " rate " + std::to_string(static_cast<int>(rate + 0.5));
    return result;
}

inline RecordAttemptResult blockedByLock()
{
    RecordAttemptResult blocked;
    blocked.log = "transport blocked: recording is locked";
    blocked.alert = "Recording is locked. Click the padlock to unlock.";
    return blocked;
}

inline RecordAttemptResult resolveStop(const RecordAttempt& attempt)
{
    if (! attempt.recording && ! attempt.playing)
    {
        RecordAttemptResult result;
        result.log = "transport stop ignored: already stopped";
        return result;
    }
    if (recordLockEngaged(attempt.lockArmed, attempt.recording))
        return blockedByLock();
    RecordAttemptResult result;
    result.stop = true;
    result.log = attempt.recording ? "transport stop: recording" : "transport stop: playback";
    return result;
}

inline RecordAttemptResult resolvePlay(const RecordAttempt& attempt)
{
    if (! attempt.hasTakes)
    {
        RecordAttemptResult result;
        result.log = "play failed: nothing recorded yet";
        result.alert = "Nothing recorded yet.";
        return result;
    }
    RecordAttemptResult result;
    result.startPlayback = true;
    result.log = "play start";
    return result;
}

// Play and Space start a take only when REC is armed and the transport is stopped.
// Cmd+Space always tries to start a take. Stop and Space stop unless the lock is engaged.
inline RecordAttemptResult resolveTransport(const RecordAttempt& attempt, TransportPress press)
{
    const bool stopped = ! attempt.recording && ! attempt.playing;
    const bool armStart = stopped && attempt.buttonArmed
                          && (press == TransportPress::play || press == TransportPress::space);
    if (press == TransportPress::commandSpace || armStart)
    {
        RecordAttempt armed = attempt;
        armed.buttonArmed = true;
        return planRecordStart(armed);
    }

    if (press == TransportPress::stop || (press == TransportPress::space && ! stopped))
        return resolveStop(attempt);

    if (press == TransportPress::play && ! stopped)
    {
        RecordAttemptResult busy;
        busy.log = attempt.recording ? "play ignored: already recording" : "play ignored: already playing";
        return busy;
    }

    if (stopped && (press == TransportPress::play || press == TransportPress::space))
        return resolvePlay(attempt);

    RecordAttemptResult result;
    result.log = "transport ignored";
    return result;
}

// Minimal PCM file so tests can prove a start plan really writes audio.
inline bool writeMonoWav(const std::string& path, int sampleRate, const float* samples, int count)
{
    if (path.empty() || sampleRate < 1 || samples == nullptr || count < 1)
        return false;
    std::ofstream out(path, std::ios::binary);
    if (! out)
        return false;

    const std::uint32_t dataBytes = static_cast<std::uint32_t>(count) * 2u;
    const std::uint32_t riffBytes = 36u + dataBytes;
    const auto write4 = [&out](const char* text)
    {
        out.write(text, 4);
    };
    const auto write32 = [&out](std::uint32_t value)
    {
        const unsigned char bytes[4] = {
            static_cast<unsigned char>(value & 0xffu),
            static_cast<unsigned char>((value >> 8) & 0xffu),
            static_cast<unsigned char>((value >> 16) & 0xffu),
            static_cast<unsigned char>((value >> 24) & 0xffu),
        };
        out.write(reinterpret_cast<const char*>(bytes), 4);
    };
    const auto write16 = [&out](std::uint16_t value)
    {
        const unsigned char bytes[2] = {
            static_cast<unsigned char>(value & 0xffu),
            static_cast<unsigned char>((value >> 8) & 0xffu),
        };
        out.write(reinterpret_cast<const char*>(bytes), 2);
    };

    write4("RIFF");
    write32(riffBytes);
    write4("WAVE");
    write4("fmt ");
    write32(16);
    write16(1);
    write16(1);
    write32(static_cast<std::uint32_t>(sampleRate));
    write32(static_cast<std::uint32_t>(sampleRate) * 2u);
    write16(2);
    write16(16);
    write4("data");
    write32(dataBytes);
    for (int index = 0; index < count; ++index)
    {
        const float clamped = samples[index] < -1.0f ? -1.0f : (samples[index] > 1.0f ? 1.0f : samples[index]);
        const int scaled = static_cast<int>(clamped * 32767.0f);
        write16(static_cast<std::uint16_t>(static_cast<int>(scaled)));
    }
    return static_cast<bool>(out);
}

} // namespace youhost
