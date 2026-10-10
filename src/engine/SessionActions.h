#pragma once

#include "SessionFormat.h"

namespace youhost
{

enum class UnsavedChoice
{
    save,
    saveAs,
    discard,
    cancel
};

enum class SessionCloseReason
{
    newSession,
    open,
    openRecent,
    quit,
    deviceChange
};

// Changing device or Offline keeps the session. It does not reload it.
inline bool closePathDiscardsSession(SessionCloseReason reason) noexcept
{
    return reason != SessionCloseReason::deviceChange;
}

inline bool recordingBlocksClose(bool recording, SessionCloseReason reason) noexcept
{
    return recording && closePathDiscardsSession(reason);
}

// New, Open, Open Recent, and quit ask before throwing away edits.
inline bool sessionReplaceAsks(bool dirty) noexcept
{
    return dirty;
}

inline bool sessionCloseAsks(bool dirty, bool recording, SessionCloseReason reason) noexcept
{
    if (recordingBlocksClose(recording, reason))
        return false;
    if (! closePathDiscardsSession(reason))
        return false;
    return dirty;
}

inline bool sessionReplaceProceeds(bool dirty, UnsavedChoice choice) noexcept
{
    if (! dirty)
        return true;
    return choice != UnsavedChoice::cancel;
}

inline bool sessionCloseProceeds(bool dirty, bool recording, SessionCloseReason reason, UnsavedChoice choice) noexcept
{
    if (recordingBlocksClose(recording, reason))
        return false;
    if (! closePathDiscardsSession(reason))
        return true;
    return sessionReplaceProceeds(dirty, choice);
}

inline bool sessionReplaceSavesFirst(bool dirty, UnsavedChoice choice) noexcept
{
    return dirty && choice == UnsavedChoice::save;
}

inline bool sessionCloseSavesFirst(bool dirty, bool recording, SessionCloseReason reason, UnsavedChoice choice) noexcept
{
    return sessionCloseProceeds(dirty, recording, reason, choice)
           && dirty
           && choice == UnsavedChoice::save
           && closePathDiscardsSession(reason);
}

inline bool sessionCloseSaveAsFirst(bool dirty, bool recording, SessionCloseReason reason, UnsavedChoice choice) noexcept
{
    return sessionCloseProceeds(dirty, recording, reason, choice)
           && dirty
           && choice == UnsavedChoice::saveAs
           && closePathDiscardsSession(reason);
}

// A brand new session has no plugins, names, colours, groups, or takes.
// Every channel is REC, output gain is 0 dB, and alignment is Global.
inline SessionDocumentModel cleanSessionModel()
{
    SessionDocumentModel model;
    model.version = kSessionFormatVersion;
    model.bits = kDefaultWavBitDepth;
    model.page = 1;
    model.wave = 1.0;
    model.align = "all";
    model.peak = false;
    model.reference = kDefaultRmsReferenceDb;
    model.hasMeters = true;
    model.hasTimeline = true;
    return model;
}

inline bool sessionModelIsClean(const SessionDocumentModel& model) noexcept
{
    if (model.align != "all" && ! model.align.empty())
        return false;
    if (model.page != 1 && model.page != 0)
        return false;
    if (! model.channels.empty() || ! model.groups.empty() || ! model.takes.empty())
        return false;
    if (model.timeline.zoom != 0 || model.timeline.vertical != 0 || model.timeline.scroll != 0
        || model.timeline.laneScroll != 0)
        return false;
    return true;
}

// Open replaces the whole document. Channels that are not in the file do not
// keep values from the session that was open before.
inline SessionDocumentModel replaceSessionModel(const SessionDocumentModel& loaded)
{
    return loaded;
}

} // namespace youhost
