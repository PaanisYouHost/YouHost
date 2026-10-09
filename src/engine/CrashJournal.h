#pragma once

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

inline void upsertCrashMark(CrashJournal& journal, CrashMark mark)
{
    journal.clean = false;
    for (auto& existing : journal.marks)
    {
        if (existing.channel == mark.channel && existing.slot == mark.slot)
        {
            existing = std::move(mark);
            return;
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

} // namespace youhost
