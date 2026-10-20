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
