#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

struct TakeSpan
{
    std::int64_t start = 0;
    std::int64_t length = 0;
};

struct WavePeak
{
    float low = 0.0f;
    float high = 0.0f;
};

struct Timecode
{
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
};

inline std::int64_t timelineEnd(const TakeSpan* takes, int count) noexcept
{
    std::int64_t end = 0;
    if (takes == nullptr || count <= 0)
        return end;

    for (int index = 0; index < count; ++index)
    {
        const std::int64_t takeEnd = takes[index].start + std::max<std::int64_t>(0, takes[index].length);
        if (takeEnd > end)
            end = takeEnd;
    }
    return end;
}

inline std::int64_t clampTimeline(std::int64_t position, std::int64_t end) noexcept
{
    if (position < 0)
        return 0;
    if (end < 0)
        end = 0;
    if (position > end)
        return end;
    return position;
}

inline std::vector<std::int64_t> takeMarkers(const TakeSpan* takes, int count)
{
    std::vector<std::int64_t> marks;
    if (takes == nullptr || count <= 0)
        return marks;

    marks.reserve(static_cast<std::size_t>(count) * 2u);
    for (int index = 0; index < count; ++index)
    {
        marks.push_back(std::max<std::int64_t>(0, takes[index].start));
        marks.push_back(takes[index].start + std::max<std::int64_t>(0, takes[index].length));
    }

    std::sort(marks.begin(), marks.end());
    marks.erase(std::unique(marks.begin(), marks.end()), marks.end());
    return marks;
}

inline std::int64_t previousMarker(std::int64_t position, const std::vector<std::int64_t>& marks) noexcept
{
    std::int64_t best = position;
    bool found = false;
    for (const std::int64_t mark : marks)
    {
        if (mark < position)
        {
            best = mark;
            found = true;
        }
    }
    return found ? best : position;
}

inline std::int64_t nextMarker(std::int64_t position, const std::vector<std::int64_t>& marks) noexcept
{
    for (const std::int64_t mark : marks)
        if (mark > position)
            return mark;
    return position;
}

inline std::int64_t nudgeSamples(std::int64_t position, std::int64_t delta, std::int64_t end) noexcept
{
    if (end < 0)
        end = 0;
    if (position < 0)
        position = 0;
    if (position > end)
        position = end;

    if (delta >= 0)
    {
        if (delta > end - position)
            return end;
        return position + delta;
    }

    if (position + delta < 0)
        return 0;
    return position + delta;
}

inline Timecode timecodeFromSamples(std::int64_t samples, double sampleRate) noexcept
{
    if (sampleRate <= 0.0 || samples <= 0)
        return {};

    const auto total = static_cast<std::int64_t>(static_cast<double>(samples) / sampleRate);
    if (total <= 0)
        return {};

    Timecode code;
    code.hours = static_cast<int>(total / 3600);
    code.minutes = static_cast<int>((total / 60) % 60);
    code.seconds = static_cast<int>(total % 60);
    return code;
}

inline std::string sanitiseChannelName(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    for (const char raw : name)
    {
        const auto character = static_cast<unsigned char>(raw);
        if (character < 32 || character == '/' || character == '\\' || character == ':' || character == '*'
            || character == '?' || character == '"' || character == '<' || character == '>' || character == '|')
            continue;

        if (character == ' ')
        {
            if (out.empty() || out.back() == '_')
                continue;
            out.push_back('_');
        }
        else
        {
            out.push_back(static_cast<char>(character));
        }

        if (out.size() >= 40)
            break;
    }

    while (! out.empty() && (out.back() == '_' || out.back() == '.'))
        out.pop_back();
    return out;
}

inline std::string takeWaveName(int takeNumber, int channelNumber, std::string_view name)
{
    const int take = takeNumber < 1 ? 1 : takeNumber;
    const int channel = channelNumber < 1 ? 1 : channelNumber;
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "Take%02d_Ch%02d", take, channel);

    std::string file(prefix);
    const std::string clean = sanitiseChannelName(name);
    if (! clean.empty())
    {
        file.push_back('_');
        file += clean;
    }
    file += ".wav";
    return file;
}

} // namespace youhost
