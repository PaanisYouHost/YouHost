#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

struct CrashMark
{
    std::string phase;
    int channel = -1;
    int slot = -1;
    std::string name;
    std::string identifier;
};

struct CrashJournal
{
    bool clean = true;
    std::vector<CrashMark> marks;
};

inline std::string sanitiseJournalField(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char raw : text)
    {
        const auto character = static_cast<unsigned char>(raw);
        if (character < 32 || character == '\t')
            out.push_back(' ');
        else
            out.push_back(static_cast<char>(character));
    }
    return out;
}

inline std::string writeCrashJournal(const CrashJournal& journal)
{
    std::string text = journal.clean ? "clean=1\n" : "clean=0\n";
    for (const auto& mark : journal.marks)
    {
        text += "mark\t";
        text += sanitiseJournalField(mark.phase.empty() ? "active" : mark.phase);
        text += '\t';
        text += std::to_string(mark.channel);
        text += '\t';
        text += std::to_string(mark.slot);
        text += '\t';
        text += sanitiseJournalField(mark.name);
        text += '\t';
        text += sanitiseJournalField(mark.identifier);
        text += '\n';
    }
    return text;
}

inline std::vector<std::string_view> splitJournalFields(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= line.size(); ++index)
    {
        if (index == line.size() || line[index] == '\t')
        {
            fields.emplace_back(line.substr(start, index - start));
            start = index + 1;
        }
    }
    return fields;
}

inline CrashJournal readCrashJournal(std::string_view text)
{
    CrashJournal journal;
    std::size_t cursor = 0;
    bool sawClean = false;
    while (cursor < text.size())
    {
        const auto end = text.find('\n', cursor);
        auto line = text.substr(cursor, end == std::string_view::npos ? text.size() - cursor : end - cursor);
        if (! line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        cursor = end == std::string_view::npos ? text.size() : end + 1;
        if (line.empty())
            continue;

        if (line.rfind("clean=", 0) == 0)
        {
            sawClean = true;
            journal.clean = line.size() > 6 && line[6] == '1';
            continue;
        }

        const auto fields = splitJournalFields(line);
        if (fields.size() < 6 || fields[0] != "mark")
            continue;

        CrashMark mark;
        mark.phase = std::string(fields[1]);
        try
        {
            mark.channel = std::stoi(std::string(fields[2]));
            mark.slot = std::stoi(std::string(fields[3]));
        }
        catch (...)
        {
            mark.channel = -1;
            mark.slot = -1;
        }
        mark.name = std::string(fields[4]);
        mark.identifier = std::string(fields[5]);
        journal.marks.push_back(std::move(mark));
    }

    if (! sawClean && ! journal.marks.empty())
        journal.clean = false;
    return journal;
}

// The most recently touched slot is last. "Last active" and "last loaded"
// are the marks at the end of the journal, not the first one that loaded.
inline void upsertCrashMark(CrashJournal& journal, CrashMark mark)
{
    journal.clean = false;
    for (std::size_t index = 0; index < journal.marks.size(); ++index)
    {
        if (journal.marks[index].channel == mark.channel && journal.marks[index].slot == mark.slot)
        {
            journal.marks.erase(journal.marks.begin() + static_cast<std::ptrdiff_t>(index));
            break;
        }
    }
    journal.marks.push_back(std::move(mark));
}

inline void eraseCrashMark(CrashJournal& journal, int channel, int slot)
{
    std::vector<CrashMark> kept;
    kept.reserve(journal.marks.size());
    for (auto& mark : journal.marks)
        if (mark.channel != channel || mark.slot != slot)
            kept.push_back(std::move(mark));
    journal.marks.swap(kept);
}

// Exact button on the unclean-start dialog. No trailing period.
inline constexpr const char* kOpenSessionWithoutPlugin = "Open session without this plugin";

// The last plugin that reached "active", and the last one still "loading".
// Other loaded plugins stay. If neither phase is present, the last mark is used.
inline std::vector<CrashMark> crashPluginsToSkip(const CrashJournal& journal)
{
    const CrashMark* lastActive = nullptr;
    const CrashMark* lastLoading = nullptr;
    for (const auto& mark : journal.marks)
    {
        if (mark.phase == "active")
            lastActive = &mark;
        else if (mark.phase == "loading" || mark.phase == "load")
            lastLoading = &mark;
    }

    std::vector<const CrashMark*> chosen;
    const auto consider = [&](const CrashMark* mark)
    {
        if (mark == nullptr)
            return;
        for (const auto* existing : chosen)
            if (existing->channel == mark->channel && existing->slot == mark->slot)
                return;
        chosen.push_back(mark);
    };

    if (lastLoading != nullptr && (lastActive == nullptr || lastLoading > lastActive))
    {
        consider(lastLoading);
        consider(lastActive);
    }
    else
    {
        consider(lastActive);
        consider(lastLoading);
    }

    if (chosen.empty() && ! journal.marks.empty())
        consider(&journal.marks.back());

    std::vector<CrashMark> out;
    out.reserve(chosen.size());
    for (const auto* mark : chosen)
        out.push_back(*mark);
    return out;
}

// One generation per save. "---" starts the next one. Startup appends a new
// generation; it does not erase the ones already in the file.
inline std::string appendCrashGeneration(std::string_view existing, const CrashJournal& journal)
{
    std::string text(existing);
    if (! text.empty() && text.back() != '\n')
        text.push_back('\n');
    if (! text.empty())
        text += "---\n";
    text += writeCrashJournal(journal);
    return text;
}

inline CrashJournal readLatestCrashJournal(std::string_view text)
{
    std::string_view latest = text;
    std::size_t cursor = 0;
    while (cursor < text.size())
    {
        const auto end = text.find('\n', cursor);
        auto line = text.substr(cursor, end == std::string_view::npos ? text.size() - cursor : end - cursor);
        if (! line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line == "---")
            latest = text.substr(end == std::string_view::npos ? text.size() : end + 1);
        cursor = end == std::string_view::npos ? text.size() : end + 1;
    }
    return readCrashJournal(latest);
}

// Drop whole generations from the front until the file fits. The latest
// generation stays even if it is the only one left.
inline std::string rotateCrashHistory(std::string_view text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return std::string(text);
    if (maxBytes < 64)
        maxBytes = 64;
    std::size_t start = text.size() > maxBytes ? text.size() - maxBytes : 0;
    const auto mark = text.find("---\n", start);
    if (mark == std::string_view::npos)
        return std::string(text.substr(text.size() > maxBytes ? text.size() - maxBytes : 0));
    return std::string(text.substr(mark + 4));
}

// Choosing a plugin always loads it. A previous crash is only a warning.
struct SlotBlock
{
    int channel = -1;
    int slot = -1;
    std::string identifier;
};

struct SlotAdmit
{
    bool load = false;
    bool clearSlot = false;
    const char* reason = nullptr;
};

inline constexpr const char* kPluginCrashedLastTime = "this plugin crashed last time";
inline constexpr const char* kLeftOutOfThisLaunch = "left out of this launch";
inline constexpr const char* kSlotFreedReason = "the slot is free";

// Small non-blocking note. nullptr when this plugin has no crash warning.
inline const char* crashWarningNote(bool crashedBefore) noexcept
{
    return crashedBefore ? kPluginCrashedLastTime : nullptr;
}

// "Open session without this plugin" skips the automatic restore only.
// A manual choice (automaticRestore == false) always loads.
inline bool leaveOutOfThisLaunch(bool automaticRestore, bool listedForThisLaunch) noexcept
{
    return automaticRestore && listedForThisLaunch;
}

// The user already chose a plugin for this slot. The automatic restore must
// not clear it. Channel 1 slot 1 is the same rule as every other slot.
inline bool restoreSkipsClaimedSlot(bool automaticRestore, bool userClaimedSlot) noexcept
{
    return automaticRestore && userClaimedSlot;
}

// An empty slot opens the plugin list. A live plugin toggles its editor.
// A slot left off after a crash has no live instance, so the click opens the list.
inline bool slotClickOpensPluginList(bool liveInstance, bool loading) noexcept
{
    return ! loading && ! liveInstance;
}

// True when this channel and slot were named by the dialog for this launch.
// The identifier must be the same plugin. An empty crash id does not match
// every plugin, so it cannot lock the slot.
inline bool pluginListedForThisLaunch(int channel,
                                      int slot,
                                      std::string_view identifier,
                                      const SlotBlock* listed,
                                      int count) noexcept
{
    if (listed == nullptr || count <= 0)
        return false;
    for (int index = 0; index < count; ++index)
    {
        if (listed[index].channel != channel || listed[index].slot != slot)
            continue;
        if (listed[index].identifier == identifier)
            return true;
    }
    return false;
}

// Drops one remembered slot. The identifier remains while another slot still names it.
inline int releaseCrashSlot(SlotBlock* blocks,
                            int count,
                            int channel,
                            int slot,
                            std::string* releasedIdentifier)
{
    if (releasedIdentifier != nullptr)
        releasedIdentifier->clear();
    if (blocks == nullptr || count <= 0)
        return count < 0 ? 0 : count;
    int kept = 0;
    for (int index = 0; index < count; ++index)
    {
        if (blocks[index].channel == channel && blocks[index].slot == slot)
        {
            if (releasedIdentifier != nullptr)
                *releasedIdentifier = blocks[index].identifier;
            continue;
        }
        if (kept != index)
            blocks[kept] = blocks[index];
        ++kept;
    }
    return kept;
}

inline bool identifierHeldBySlot(const SlotBlock* blocks, int count, std::string_view identifier) noexcept
{
    if (blocks == nullptr || identifier.empty())
        return false;
    for (int index = 0; index < count; ++index)
        if (blocks[index].identifier == identifier)
            return true;
    return false;
}

// Any plugin, any slot. load is always true. A leftover hold is cleared.
// reason is the warning note when this plugin crashed before, otherwise nullptr.
inline SlotAdmit admitPluginIntoSlot(int channel,
                                     int slot,
                                     std::string_view identifier,
                                     const SlotBlock* blocks,
                                     int blockCount,
                                     bool identifierBlocked) noexcept
{
    const SlotBlock* slotBlock = nullptr;
    if (blocks != nullptr)
    {
        for (int index = 0; index < blockCount; ++index)
        {
            if (blocks[index].channel == channel && blocks[index].slot == slot)
                slotBlock = &blocks[index];
        }
    }

    const bool samePlugin = slotBlock != nullptr && ! slotBlock->identifier.empty()
                            && slotBlock->identifier == identifier;
    const bool unknownPlugin = slotBlock != nullptr && slotBlock->identifier.empty() && identifier.empty();

    SlotAdmit admitted;
    admitted.load = true;
    admitted.clearSlot = slotBlock != nullptr;
    admitted.reason = crashWarningNote(identifierBlocked || samePlugin || unknownPlugin);
    return admitted;
}

inline std::string crashDialogMessage(const CrashJournal& journal)
{
    const auto skip = crashPluginsToSkip(journal);
    std::string text;
    if (skip.empty())
        text += "A plugin may have crashed YouHost.\n";
    for (const auto& mark : skip)
    {
        text += mark.name.empty() ? "A plugin" : mark.name;
        text += " may have crashed YouHost.\n";
    }
    text += "\n";
    text += kOpenSessionWithoutPlugin;
    text += " leaves that plugin out of this launch and keeps its slot in the session file.\n";
    text += "Load them anyway if you still want to try it. Recording is not stopped.\n";
    return text;
}

} // namespace youhost
