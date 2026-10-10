#pragma once

#include "SessionFormat.h"

namespace youhost
{

enum class UnsavedChoice
{
    save,
    discard,
    cancel
};

// New, Open, and Open Recent ask before throwing away edits.
inline bool sessionReplaceAsks(bool dirty) noexcept
{
    return dirty;
}

inline bool sessionReplaceProceeds(bool dirty, UnsavedChoice choice) noexcept
{
    if (! dirty)
        return true;
    return choice != UnsavedChoice::cancel;
}

inline bool sessionReplaceSavesFirst(bool dirty, UnsavedChoice choice) noexcept
{
    return dirty && choice == UnsavedChoice::save;
}

// A brand new session has no plugins, names, colours, groups, or takes.
// Every channel is REC, output gain is 0 dB, and alignment is All aligned.
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
