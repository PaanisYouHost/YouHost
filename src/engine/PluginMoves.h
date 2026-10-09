#pragma once

#include <utility>

namespace youhost
{

// A plain drag moves the loaded instance. It must not unload and create another one.
// Option-drag is the only path that creates a new instance. A drag while this channel
// is already loading, or while either slot is loading, is ignored.
struct SlotMoveState
{
    int instance = 0;
    bool loading = false;
};

inline bool slotDragAccepted(const SlotMoveState& from, const SlotMoveState& to, bool channelBusy) noexcept
{
    if (channelBusy || from.loading || to.loading)
        return false;
    return from.instance != 0;
}

inline bool moveSlotInstance(SlotMoveState& from, SlotMoveState& to) noexcept
{
    if (! slotDragAccepted(from, to, false))
        return false;
    const int moved = from.instance;
    std::swap(from.instance, to.instance);
    std::swap(from.loading, to.loading);
    return to.instance == moved;
}

struct SlotCopyResult
{
    bool ok = false;
    int created = 0;
    int retired = 0;
};

// Copy leaves the source instance where it is and marks the destination loading
// under a new id. The previous destination id is retired by the caller.
inline SlotCopyResult copySlotInstance(SlotMoveState& from, SlotMoveState& to, int& nextInstance) noexcept
{
    SlotCopyResult result;
    if (! slotDragAccepted(from, to, false))
        return result;
    result.ok = true;
    result.retired = to.instance;
    result.created = ++nextInstance;
    to.instance = result.created;
    to.loading = true;
    return result;
}

// One load at a time on a channel. A second begin is rejected until finish.
struct ChannelOpQueue
{
    bool inFlight = false;
    int started = 0;
    int finished = 0;
};

inline bool beginQueuedLoad(ChannelOpQueue& queue) noexcept
{
    if (queue.inFlight)
        return false;
    queue.inFlight = true;
    ++queue.started;
    return true;
}

inline void finishQueuedLoad(ChannelOpQueue& queue) noexcept
{
    if (! queue.inFlight)
        return;
    queue.inFlight = false;
    ++queue.finished;
}

// The editor stays alive for a few message-loop turns so a deferred AU remote-view
// callback can finish. The editor is destroyed on its own turn. The instance goes
// on a later turn, never on the same turn as the editor.
enum class GraveStep
{
    keep,
    dropEditor,
    dropInstance
};

inline int pluginGraveHops() noexcept
{
    return 6;
}

inline GraveStep graveStep(int hops) noexcept
{
    if (hops > 1)
        return GraveStep::keep;
    if (hops == 1)
        return GraveStep::dropEditor;
    return GraveStep::dropInstance;
}

inline int graveNextHops(int hops) noexcept
{
    if (hops > 1)
        return hops - 1;
    return 0;
}

} // namespace youhost
