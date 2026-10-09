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
    return readCrashJournal(file.loadFileAsString().toRawUTF8());
}

inline void storeCrashJournal(AppSettings& settings, const CrashJournal& journal)
{
    auto file = crashJournalFile(settings);
    file.getParentDirectory().createDirectory();
    file.replaceWithText(juce::String::fromUTF8(writeCrashJournal(journal).c_str()));
}

inline void appendHostLog(AppSettings& settings, const juce::String& line)
{
    auto file = hostLogFile(settings);
    file.getParentDirectory().createDirectory();
    file.appendText(juce::Time::getCurrentTime().toISO8601(true) + "  " + line + "\n");
}

} // namespace youhost
