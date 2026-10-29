#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace youhost
{

// Creating a plugin on the message thread holds the message lock for the
// whole instantiate. A slow Audio Unit (a couple of seconds) then blocks
// anything on the audio thread that needs that lock. Every plugin is created
// on the loader thread. The message thread stays in the runloop and only
// takes the instance back once it exists, for the bus layout and the editor.
enum class PluginInstantiateWhere
{
    messageAsync,
    background
};

inline bool isAudioUnitFormat(const char* formatName) noexcept
{
    return formatName != nullptr && std::strcmp(formatName, "AudioUnit") == 0;
}

inline PluginInstantiateWhere pluginInstantiateWhere(bool audioUnit, bool requiresUnblockedMessageThread) noexcept
{
    (void) audioUnit;
    (void) requiresUnblockedMessageThread;
    return PluginInstantiateWhere::background;
}

// JUCE omits an Audio Unit from searchPathsForPlugins when this is false and
// the component is AUv3. That plugin then never appears, and the log has no
// load line for it. The loader already waits for the async instantiate.
inline bool audioUnitScanIncludesAsync() noexcept
{
    return true;
}

inline const char* pluginFormatLabel(const char* format, const char* identifier) noexcept
{
    if (format != nullptr && format[0] != '\0')
        return format;
    if (identifier != nullptr)
    {
        if (std::strncmp(identifier, "AudioUnit:", 10) == 0)
            return "AudioUnit";
        if (std::strstr(identifier, ".vst3") != nullptr || std::strstr(identifier, ".VST3") != nullptr)
            return "VST3";
    }
    return "unknown-format";
}

// Slot is stored 0-based and written 1-based, the same way as "loading ch 1 slot 2".
// Every attempt names the format, so an Audio Unit is visible even when it fails.
inline std::string formatPluginAttemptLine(const char* phase,
                                           int channel,
                                           int slot,
                                           const char* format,
                                           const char* name,
                                           const char* identifier,
                                           const char* reason)
{
    std::string line = phase != nullptr && phase[0] != '\0' ? phase : "plugin";
    line += "  ch ";
    line += std::to_string(channel + 1);
    line += " slot ";
    line += std::to_string(slot + 1);
    line += "  ";
    line += pluginFormatLabel(format, identifier);
    line += "  ";
    line += name != nullptr && name[0] != '\0' ? name : "Plugin";
    if (identifier != nullptr && identifier[0] != '\0')
    {
        line += "  id=";
        line += identifier;
    }
    if (reason != nullptr && reason[0] != '\0')
    {
        line += "  reason=";
        line += reason;
    }
    return line;
}

inline std::string formatScanFoundLine(const char* format, const char* name, const char* identifier)
{
    std::string line = "scan found  ";
    line += pluginFormatLabel(format, identifier);
    line += "  ";
    line += name != nullptr && name[0] != '\0' ? name : "Plugin";
    if (identifier != nullptr && identifier[0] != '\0')
    {
        line += "  id=";
        line += identifier;
    }
    return line;
}

inline std::string formatScanRejectedLine(const char* format, const char* identifier, const char* reason)
{
    std::string line = "scan rejected  ";
    line += pluginFormatLabel(format, identifier);
    line += "  ";
    line += identifier != nullptr && identifier[0] != '\0' ? identifier : "unknown";
    line += "  reason=";
    line += reason != nullptr && reason[0] != '\0' ? reason : "no reason from the host";
    return line;
}

inline std::string formatScanFailureLine(const char* format, const char* identifier, const char* reason)
{
    std::string line = "did-not-load  scan  ";
    line += pluginFormatLabel(format, identifier);
    line += "  ";
    line += identifier != nullptr && identifier[0] != '\0' ? identifier : "unknown";
    line += "  reason=";
    line += reason != nullptr && reason[0] != '\0' ? reason : "no reason from the host";
    return line;
}

// Milliseconds for one load stage. setActive is inside JUCE prepareToPlay,
// so that stage is logged with the same span and a note.
inline std::string formatLoadStageLine(const char* stage, int milliseconds, const char* note = nullptr)
{
    std::string line = "load stage ";
    line += stage != nullptr && stage[0] != '\0' ? stage : "unknown";
    line += " ";
    line += std::to_string(milliseconds < 0 ? 0 : milliseconds);
    line += " ms";
    if (note != nullptr && note[0] != '\0')
    {
        line += " ";
        line += note;
    }
    return line;
}

// One session plugin is in flight at a time. The next one starts on a later
// message-loop turn, after this one has finished.
struct SessionLoadCursor
{
    int total = 0;
    int finished = 0;
    bool inFlight = false;

    bool startOne() noexcept
    {
        if (inFlight || total <= 0 || finished >= total)
            return false;
        inFlight = true;
        return true;
    }

    void completeOne() noexcept
    {
        if (! inFlight)
            return;
        inFlight = false;
        if (finished < total)
            ++finished;
    }

    bool done() const noexcept
    {
        return total <= 0 || (finished >= total && ! inFlight);
    }
};

// "Loading plugins 3/5…" while the third plugin is the one in flight.
// Returns false when the line should be hidden.
inline bool formatPluginLoadProgress(int finished, int total, bool inFlight, char* dest, std::size_t destSize) noexcept
{
    if (dest == nullptr || destSize == 0)
        return false;
    dest[0] = '\0';
    if (total <= 0 || finished < 0)
        return false;
    if (! inFlight && finished >= total)
        return false;

    int shown = finished + (inFlight ? 1 : 0);
    if (shown < 1)
        shown = 1;
    if (shown > total)
        shown = total;

    const int wrote = std::snprintf(dest, destSize, "Loading plugins %d/%d\xE2\x80\xA6", shown, total);
    if (wrote <= 0 || static_cast<std::size_t>(wrote) >= destSize)
    {
        dest[0] = '\0';
        return false;
    }
    return true;
}

} // namespace youhost
