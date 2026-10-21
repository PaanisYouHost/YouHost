#pragma once

#include "AppSettings.h"
#include "CrashJournal.h"

#include <juce_core/juce_core.h>

namespace youhost
{

inline juce::File crashJournalFile(AppSettings& settings)
{
    return settings.supportDirectory().getChildFile("crash-journal.txt");
}

inline juce::File hostLogFile(AppSettings& settings)
{
    return settings.supportDirectory().getChildFile("youhost.log");
}

inline CrashJournal loadCrashJournal(AppSettings& settings)
{
    const auto file = crashJournalFile(settings);
    if (! file.existsAsFile())
        return {};
    const auto text = file.loadFileAsString();
    return readLatestCrashJournal(text.toRawUTF8());
}

// Appends one generation and drops the oldest when the file is past 256 KB.
// A clean start does not replace the file, so earlier crashes stay readable.
inline void storeCrashJournal(AppSettings& settings, const CrashJournal& journal)
{
    auto file = crashJournalFile(settings);
    file.getParentDirectory().createDirectory();
    const auto existing = file.existsAsFile() ? file.loadFileAsString() : juce::String();
    const auto next = rotateCrashHistory(appendCrashGeneration(existing.toRawUTF8(), journal), 262144);
    file.replaceWithText(juce::String::fromUTF8(next.c_str()));
}

inline void appendHostLog(AppSettings& settings, const juce::String& line)
{
    auto file = hostLogFile(settings);
    file.getParentDirectory().createDirectory();
    file.appendText(juce::Time::getCurrentTime().toISO8601(true) + "  " + line + "\n");
}

} // namespace youhost
