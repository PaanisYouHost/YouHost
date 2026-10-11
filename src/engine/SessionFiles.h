#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

inline constexpr const char* kSessionFileName = "session.youhost";
inline constexpr const char* kAudioFolderName = "audio";

struct SessionLayout
{
    std::string folder;
    std::string sessionFile;
    std::string audioFolder;
};

// A session is a folder: the session file next to an audio/ directory the recorder
// will use later. `folder` may have a trailing slash.
inline SessionLayout sessionLayoutFor(std::string_view folder)
{
    std::string root(folder);
    while (! root.empty() && (root.back() == '/' || root.back() == '\\'))
        root.pop_back();

    SessionLayout layout;
    layout.folder = root;
    layout.sessionFile = root + "/" + kSessionFileName;
    layout.audioFolder = root + "/" + kAudioFolderName;
    return layout;
}

// A folder name the user typed. Slashes and other path characters are dropped.
// An empty result becomes "Session" so the chooser always has a name.
inline std::string sanitiseSessionName(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    for (const char ch : name)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 32 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?'
            || ch == '"' || ch == '<' || ch == '>' || ch == '|')
            continue;
        out.push_back(ch);
    }
    while (! out.empty() && (out.front() == ' ' || out.front() == '.'))
        out.erase(out.begin());
    while (! out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    if (out.size() > 80)
        out.resize(80);
    while (! out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    if (out.empty())
        return "Session";
    return out;
}

struct SessionCopyEntry
{
    std::string relativePath;
};

// Save As writes session.youhost once and copies every WAV in audio/.
// Backups and other root files (dropouts, temp) are not copied.
inline std::vector<SessionCopyEntry> planSessionCopy(const std::vector<std::string>& rootFiles,
                                                     const std::vector<std::string>& wavNames,
                                                     const std::vector<std::string>& backupFolders)
{
    (void) rootFiles;
    (void) backupFolders;
    std::vector<SessionCopyEntry> plan;
    plan.push_back(SessionCopyEntry { kSessionFileName });
    for (const auto& wav : wavNames)
        plan.push_back(SessionCopyEntry { std::string(kAudioFolderName) + "/" + wav });
    return plan;
}

// After a successful Save As the session continues in the destination folder.
inline std::string folderAfterSaveAs(std::string_view current, std::string_view destination, bool copyOk)
{
    if (! copyOk || destination.empty())
        return std::string(current);
    return std::string(destination);
}

} // namespace youhost
