#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

struct ParsedRecordingName
{
    int takeNumber = 0;
    int channelNumber = 0;
};

struct ImportedChannel
{
    int channel = -1;
    std::string fileName;
};

struct ImportedTake
{
    int number = 0;
    std::vector<ImportedChannel> channels;
};

inline std::string_view fileNameOnly(std::string_view path)
{
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    return path;
}

inline std::string_view stripExtension(std::string_view name)
{
    const auto dot = name.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0)
        name.remove_suffix(name.size() - dot);
    return name;
}

inline int digitsAfter(std::string_view text, std::size_t index)
{
    while (index < text.size())
    {
        const auto character = static_cast<unsigned char>(text[index]);
        if (std::isdigit(character) != 0)
            break;
        if (character != ' ' && character != '_' && character != '-')
            return 0;
        ++index;
    }

    int value = 0;
    int digits = 0;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0 && digits < 6)
    {
        value = value * 10 + (text[index] - '0');
        ++index;
        ++digits;
    }
    return digits > 0 ? value : 0;
}

inline bool prefixAt(std::string_view text, std::size_t index, std::string_view prefix)
{
    if (index + prefix.size() > text.size())
        return false;
    for (std::size_t offset = 0; offset < prefix.size(); ++offset)
    {
        const auto left = static_cast<char>(std::tolower(static_cast<unsigned char>(text[index + offset])));
        const auto right = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[offset])));
        if (left != right)
            return false;
    }
    return true;
}

// Understands YouHost names (Take01_Ch03_Kick.wav) and loose "track 4" / "ch3" names.
inline ParsedRecordingName parseRecordingName(std::string_view path)
{
    const auto base = stripExtension(fileNameOnly(path));
    ParsedRecordingName parsed;
    for (std::size_t index = 0; index < base.size(); ++index)
    {
        const bool boundary = index == 0
                              || std::isalnum(static_cast<unsigned char>(base[index - 1])) == 0;
        if (! boundary)
            continue;

        if (parsed.takeNumber == 0 && prefixAt(base, index, "take"))
        {
            const int number = digitsAfter(base, index + 4);
            if (number > 0)
                parsed.takeNumber = number;
        }

        int channel = 0;
        if (prefixAt(base, index, "channel"))
            channel = digitsAfter(base, index + 7);
        else if (prefixAt(base, index, "track"))
            channel = digitsAfter(base, index + 5);
        else if (prefixAt(base, index, "ch"))
            channel = digitsAfter(base, index + 2);

        if (channel > 0 && parsed.channelNumber == 0)
            parsed.channelNumber = channel;
    }
    return parsed;
}

// Groups file names into timeline takes. Channel numbers in the name win.
// Names with no channel become the next free channel inside that take.
inline std::vector<ImportedTake> groupImportedRecordings(const std::vector<std::string>& fileNames)
{
    struct Row
    {
        ParsedRecordingName parsed;
        std::string fileName;
    };

    std::vector<Row> rows;
    rows.reserve(fileNames.size());
    for (const auto& fileName : fileNames)
        rows.push_back({ parseRecordingName(fileName), fileName });

    std::stable_sort(rows.begin(), rows.end(), [](const Row& left, const Row& right)
    {
        const int leftTake = left.parsed.takeNumber == 0 ? 1000000 : left.parsed.takeNumber;
        const int rightTake = right.parsed.takeNumber == 0 ? 1000000 : right.parsed.takeNumber;
        if (leftTake != rightTake)
            return leftTake < rightTake;
        if (left.parsed.channelNumber != right.parsed.channelNumber)
            return left.parsed.channelNumber < right.parsed.channelNumber;
        return left.fileName < right.fileName;
    });

    std::vector<ImportedTake> takes;
    for (const auto& row : rows)
    {
        const int number = row.parsed.takeNumber;
        if (takes.empty() || takes.back().number != number)
            takes.push_back(ImportedTake { number, {} });

        ImportedChannel channel;
        channel.fileName = row.fileName;
        channel.channel = row.parsed.channelNumber > 0 ? row.parsed.channelNumber - 1 : -1;
        takes.back().channels.push_back(std::move(channel));
    }

    for (auto& take : takes)
    {
        std::vector<bool> used(128, false);
        for (const auto& channel : take.channels)
            if (channel.channel >= 0 && channel.channel < static_cast<int>(used.size()))
                used[static_cast<std::size_t>(channel.channel)] = true;

        int next = 0;
        for (auto& channel : take.channels)
        {
            if (channel.channel >= 0)
                continue;
            while (next < static_cast<int>(used.size()) && used[static_cast<std::size_t>(next)])
                ++next;
            channel.channel = next;
            if (next < static_cast<int>(used.size()))
                used[static_cast<std::size_t>(next)] = true;
            ++next;
        }
    }

    return takes;
}

} // namespace youhost
