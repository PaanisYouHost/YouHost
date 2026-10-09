#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

inline constexpr int kSessionBackupIntervalMs = 5 * 60 * 1000;
inline constexpr int kSessionBackupsToKeep = 10;

inline std::string europeanSessionDate(int day, int month, int year)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%02d.%02d.%04d", day, month, year);
    return text;
}

inline std::string crashRecoverySessionName(int day, int month, int year, int hour, int minute)
{
    char text[64];
    std::snprintf(text,
                  sizeof(text),
                  "%02d.%02d.%04d_crash_%02d-%02d",
                  day,
                  month,
                  year,
                  hour,
                  minute);
    return text;
}

// `base`, then `base_1`, `base_2`, ... The first name `exists` rejects is skipped.
template <typename Exists>
std::string nextFreeSessionName(std::string_view base, Exists exists)
{
    const std::string stem(base);
    if (! exists(stem))
        return stem;
    for (int index = 1; index < 10000; ++index)
    {
        const std::string candidate = stem + "_" + std::to_string(index);
        if (! exists(candidate))
            return candidate;
    }
    return stem + "_10000";
}

inline std::string nextFreeSessionName(std::string_view base, const std::vector<std::string>& taken)
{
    return nextFreeSessionName(base, [&taken](const std::string& name)
    {
        return std::find(taken.begin(), taken.end(), name) != taken.end();
    });
}

struct BackupStamp
{
    std::string name;
    std::int64_t modifiedMs = 0;
};

// Newest first. Names past `keep` are the ones to delete.
inline std::vector<std::string> backupsToRemove(std::vector<BackupStamp> entries, int keep)
{
    if (keep < 0)
        keep = 0;
    std::sort(entries.begin(), entries.end(), [](const BackupStamp& left, const BackupStamp& right)
    {
        if (left.modifiedMs != right.modifiedMs)
            return left.modifiedMs > right.modifiedMs;
        return left.name > right.name;
    });
    std::vector<std::string> drop;
    for (int index = keep; index < static_cast<int>(entries.size()); ++index)
        drop.push_back(entries[static_cast<std::size_t>(index)].name);
    return drop;
}

} // namespace youhost
