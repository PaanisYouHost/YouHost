#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace youhost
{

// AudioUnit (including AUv3) stays on the message thread and is created with
// createPluginInstanceAsync. Anything else that does not need the message
// thread unblocked can be constructed on a background thread.
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
    if (audioUnit || requiresUnblockedMessageThread)
        return PluginInstantiateWhere::messageAsync;
    return PluginInstantiateWhere::background;
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
