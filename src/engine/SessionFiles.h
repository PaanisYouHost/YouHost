#pragma once

#include <string>
#include <string_view>

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

} // namespace youhost
