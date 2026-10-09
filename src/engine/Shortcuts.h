#pragma once

#include <optional>
#include <string>

namespace youhost
{

// Keyboard shortcuts live here. The key handler matches this table, and the
// help text is built from the same rows, so a listed key cannot point at a
// different action than the one the handler runs.

enum class ShortcutId
{
    recPage,
    hostPage,
    dropouts,
    cpu,
    latency,
    scanner,
    zoomIn,
    zoomOut,
    fit,
    lanesTaller,
    lanesShorter,
    playOrStop,
    recordNow,
    previousTake,
    nextTake,
    nudgeBack,
    nudgeForward,
    save,
    saveAs,
    count
};

enum class KeyKind
{
    character,
    space,
    left,
    right,
    bracketLeft,
    bracketRight,
    letterR
};

struct KeyQuery
{
    KeyKind kind = KeyKind::character;
    char character = 0;
    bool shift = false;
    bool command = false;
    bool alt = false;
};

struct Binding
{
    ShortcutId id;
    KeyKind kind;
    char character;
    int shift;    // -1 ignore, 0 up, 1 down
    int command;
    int alt;
    const char* label;
    const char* meaning;
    bool commandBinding;
    char commandCharacter;
};

inline constexpr Binding kBindings[] = {
    { ShortcutId::saveAs, KeyKind::character, 's', 1, 1, 0, "Cmd+Shift+S",
      "Save a copy of the whole session, including audio, then continue in the new folder", true, 's' },
    { ShortcutId::save, KeyKind::character, 's', 0, 1, 0, "Cmd+S",
      "Save the session file", true, 's' },
    { ShortcutId::recordNow, KeyKind::space, 0, 0, 1, -1, "Cmd+Space",
      "Record immediately", false, 0 },
    { ShortcutId::playOrStop, KeyKind::space, 0, -1, -1, -1, "Space",
      "Play, or Stop when already playing or recording", false, 0 },
    { ShortcutId::nudgeBack, KeyKind::left, 0, 1, -1, -1, "Shift+Left",
      "Move 5 seconds back", false, 0 },
    { ShortcutId::previousTake, KeyKind::left, 0, 0, -1, -1, "Left",
      "Previous take", false, 0 },
    { ShortcutId::nudgeForward, KeyKind::right, 0, 1, -1, -1, "Shift+Right",
      "Move 5 seconds forward", false, 0 },
    { ShortcutId::nextTake, KeyKind::right, 0, 0, -1, -1, "Right",
      "Next take", false, 0 },
    { ShortcutId::fit, KeyKind::letterR, 0, 0, 0, 1, "Option+R",
      "Fit the whole session across and down", false, 0 },
    { ShortcutId::lanesTaller, KeyKind::bracketLeft, 0, 0, 1, -1, "Cmd+[",
      "Taller timeline lanes", false, 0 },
    { ShortcutId::lanesShorter, KeyKind::bracketRight, 0, 0, 1, -1, "Cmd+]",
      "Shorter timeline lanes", false, 0 },
    { ShortcutId::zoomIn, KeyKind::character, 't', 0, 0, 0, "T",
      "Zoom the timeline in", false, 0 },
    { ShortcutId::zoomOut, KeyKind::letterR, 0, 0, 0, 0, "R",
      "Zoom the timeline out until the whole session fits", false, 0 },
    { ShortcutId::recPage, KeyKind::character, '1', 0, 0, -1, "1",
      "REC page", false, 0 },
    { ShortcutId::hostPage, KeyKind::character, '2', 0, 0, -1, "2",
      "HOST page", false, 0 },
    { ShortcutId::dropouts, KeyKind::character, '3', 0, 0, -1, "3",
      "Open or close DROPOUTS", false, 0 },
    { ShortcutId::dropouts, KeyKind::character, 'd', 0, 0, -1, "D",
      "Open or close DROPOUTS", false, 0 },
    { ShortcutId::cpu, KeyKind::character, '4', 0, 0, -1, "4",
      "Open or close CPU", false, 0 },
    { ShortcutId::latency, KeyKind::character, '5', 0, 0, -1, "5",
      "Open or close LATENCY", false, 0 },
    { ShortcutId::scanner, KeyKind::character, 's', 0, 0, 0, "S",
      "Open or close SCAN", false, 0 },
};

enum class NameKey
{
    none,
    commit,
    cancel,
    next,
    previous
};

inline NameKey matchNameKey(bool tab, bool enter, bool escape, bool shift, bool command, bool alt, bool ctrl)
{
    if (command || alt || ctrl)
        return NameKey::none;
    if (escape)
        return NameKey::cancel;
    if (enter)
        return NameKey::commit;
    if (tab)
        return shift ? NameKey::previous : NameKey::next;
    return NameKey::none;
}

struct NameKeyHelp
{
    NameKey key;
    const char* line;
};

// Same actions as matchNameKey. Help prints these lines in this order.
inline constexpr NameKeyHelp kNameKeyHelp[] = {
    { NameKey::commit, "Enter  Commit a name and leave the editor" },
    { NameKey::cancel, "Esc  Cancel a name edit" },
    { NameKey::next, "Tab  Commit the channel name and edit the next visible channel" },
    { NameKey::previous, "Shift+Tab  Commit the channel name and edit the previous visible channel" },
};

inline bool bindingModsMatch(const Binding& binding, const KeyQuery& query)
{
    const auto ok = [](int want, bool down)
    {
        if (want < 0)
            return true;
        return down == (want != 0);
    };
    return ok(binding.shift, query.shift) && ok(binding.command, query.command) && ok(binding.alt, query.alt);
}

inline std::optional<ShortcutId> matchShortcut(const KeyQuery& query)
{
    for (const auto& binding : kBindings)
    {
        if (binding.kind != query.kind)
            continue;
        if (binding.kind == KeyKind::character && binding.character != query.character)
            continue;
        if (! bindingModsMatch(binding, query))
            continue;
        return binding.id;
    }
    return std::nullopt;
}

inline std::string shortcutChord(ShortcutId id)
{
    std::string keys;
    for (const auto& binding : kBindings)
    {
        if (binding.id != id || binding.label == nullptr || binding.label[0] == '\0')
            continue;
        if (! keys.empty())
            keys += " or ";
        keys += binding.label;
    }
    return keys;
}

inline const char* shortcutMeaning(ShortcutId id)
{
    for (const auto& binding : kBindings)
        if (binding.id == id)
            return binding.meaning;
    return "";
}

inline constexpr const char* kShortcutNotes =
    "Click a channel to select it. Shift+click selects the channels from the anchor through the one you click. "
    "Cmd+click, or Ctrl+click, adds or removes that one channel.\n"
    "Make group from selection assigns those channels to one of the 10 groups and folds the group. "
    "The group bar stays visible. Click it to open the channels again. Use the channel menu or the Group button.\n"
    "On the REC page a group bar shows the group name in the centre. On the HOST page the name stays on the left. "
    "The fold mark is a fixed column, \u25BE 8 ch when open and \u25B8 8 ch when folded.\n"
    "Double-click a channel name, or choose Rename. The editor opens with the name selected. Enter commits. Esc cancels. "
    "Tab commits and edits the next visible channel. Shift+Tab edits the previous one.\n"
    "Rename on a group, or double-click the group bar, opens the name with a row of colour swatches under it. "
    "A swatch sets the colour immediately. Enter keeps the name and the colour. Esc puts both back. "
    "The group menu still has Color and Rename.\n"
    "REC plays and records through the plugins. INPUT plays through and is not recorded. OFF is silent. "
    "Click the channel button to cycle REC, INPUT, OFF. The default is REC.\n"
    "Audio setup ticks follow that. A tick means the channel is live (REC or INPUT). Untick turns it OFF. "
    "Ticking an OFF channel sets REC. The device stays open.\n"
    "Record arms the take and the button blinks red. Play while armed starts recording. "
    "Play while not armed is the virtual soundcheck. Stop turns record arm off.\n"
    "Cmd+Space reaches YouHost only when Spotlight is not using that shortcut.\n"
    "The + and - buttons zoom time the same way as T and R. The W+ and W- buttons change waveform height. "
    "That does not change the audio. Cmd or Option plus the wheel over the lanes does the same.\n"
    "The Fit button does the same as Option+R. With no takes yet the timeline shows 60 seconds. "
    "Fit shows every take, from 0 to just past the last take, and every visible lane.\n"
    "The v+ and v- buttons change lane height the same way as Cmd+[ and Cmd+].\n"
    "While recording or playing, the cursor sits about three quarters of the way across the view.\n"
    "The plain wheel scrolls lanes. Shift plus the wheel scrolls time. Drag the timeline's bottom edge to change its height.\n"
    "On HOST, the dB box after the plugins is output gain, -9 to +9. Drag for 0.5 dB steps, double-click to type, "
    "Option-click resets to 0. The wheel does not change it. Amber means it is not 0 dB. It is not written to the WAV.\n"
    "Option-drag copies a plugin and its settings. A plain drag moves the same plugin. "
    "A drag is ignored while a plugin on that channel is still loading.\n"
    "Null test bypasses every plugin and the compensation delay so you can hear the clean input. "
    "Click again to restore the plugins. The recorded WAV is always the raw input.\n"
    "LATENCY chooses All aligned or Per group. All aligned lines every included channel up on the slowest plugin. "
    "Per group lines each group up on its own slowest plugin. Ungrouped channels are not delayed.\n"
    "FX and PDC on a row are that channel's plugin latency and the extra delay that lines it up.\n"
    "New sessions are named with the date, DD.MM.YYYY. If that folder already exists, the name becomes DD.MM.YYYY_1, then _2. "
    "A crash copy is named DD.MM.YYYY_crash_HH-MM.\n"
    "Every 5 minutes YouHost writes a backup of the session file into the session's Backups folder and keeps the 10 newest. "
    "Audio files are not copied into the backup. The status line shows Backup and the time. "
    "The backup waits if the disk is already busy or a take is recording.\n"
    "Save writes session.youhost. The Save As button, and Cmd+Shift+S, copy the whole session folder including the audio "
    "onto a new name and location, then continue in that copy. Save As does not run while recording; YouHost asks to stop first. "
    "A progress bar shows the copy.\n"
    "3, D, 4, 5, and S open that window, or close it when it is already open, including when that window is in front.\n"
    "DROPOUTS lists CPU only for channels that have a plugin, and the list scrolls.\n"
    "If YouHost quits unexpectedly, the next launch names the plugin that was loading. "
    "Notes are in youhost.log and crash-journal.txt under Application Support, Ambient Audio, YouHost.\n"
    "Other shortcuts do nothing while a text field has focus.";

inline std::string shortcutHelpText()
{
    std::string text;
    bool seen[static_cast<int>(ShortcutId::count)] = {};
    for (const auto& binding : kBindings)
    {
        const int index = static_cast<int>(binding.id);
        if (seen[index])
            continue;
        seen[index] = true;
        text += shortcutChord(binding.id);
        text += "  ";
        text += binding.meaning;
        text += '\n';
    }
    for (const auto& nameKey : kNameKeyHelp)
    {
        text += nameKey.line;
        text += '\n';
    }
    text += '\n';
    text += kShortcutNotes;
    return text;
}

} // namespace youhost
