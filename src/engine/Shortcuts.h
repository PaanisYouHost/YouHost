#pragma once

#include "ChannelSelect.h"
#include "HelpManual.h"

#include <optional>
#include <string>

namespace youhost
{

// Keyboard shortcuts live here. The key handler matches this table.
// The help window shows kHelpManual, the English manual, not a dump of these rows.

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
    goToChannel,
    makeGroup,
    selectAll,
    undo,
    redo,
    marker,
    markers,
    deleteMarker,
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
    letterR,
    backspace
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
    { ShortcutId::redo, KeyKind::character, 'z', 1, 1, 0, "Cmd+Shift+Z",
      "Put back the mix edit that Cmd+Z removed", true, 'z' },
    { ShortcutId::undo, KeyKind::character, 'z', 0, 1, 0, "Cmd+Z",
      "Undo the last mix edit. A take and its WAV files stay", true, 'z' },
    { ShortcutId::marker, KeyKind::character, 'm', 0, 0, 0, "M",
      "Drop a marker at the playhead. Recording keeps going", false, 0 },
    { ShortcutId::deleteMarker, KeyKind::backspace, 0, 0, 1, 0, "Cmd+Backspace",
      "Delete the selected marker. Right-click a marker and choose Delete. Recording keeps going", false, 0 },
    { ShortcutId::markers, KeyKind::character, '6', 0, 0, -1, "6",
      "Open or close MARKERS", false, 0 },
    { ShortcutId::selectAll, KeyKind::character, 'a', 0, 1, 0, "Cmd+A",
      "Select every visible channel on the REC page and the HOST page", true, 'a' },
    { ShortcutId::selectAll, KeyKind::character, 'a', 0, 0, 0, "A",
      "Select every visible channel on the REC page and the HOST page", false, 0 },
    { ShortcutId::makeGroup, KeyKind::character, 'g', 0, 1, 0, "Cmd+G",
      "On the REC page and the HOST page, name a group from the selected channels, pick a colour, and fold it", true, 'g' },
    { ShortcutId::goToChannel, KeyKind::character, 'g', 0, 0, 0, "G",
      "Go to channel. Type the number and press Return.", false, 0 },
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
    { ShortcutId::fit, KeyKind::character, 'f', 0, 0, 0, "F",
      "Fit the whole session across and down", false, 0 },
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

// Menu command ids. Save and Save As were already commands. Select All and
// Make Group use the same path so the macOS menu and the command manager
// see Cmd+A and Cmd+G.
inline constexpr int kSaveCommandId = 0x2101;
inline constexpr int kSaveAsCommandId = 0x2102;
inline constexpr int kSelectAllCommandId = 0x2103;
inline constexpr int kMakeGroupCommandId = 0x2104;
inline constexpr int kUndoCommandId = 0x2105;
inline constexpr int kRedoCommandId = 0x2106;

// Play/Stop and Record are the only keys that ask the transport to start or
// stop. While a take is recording and the padlock is on, both are refused.
// Every other shortcut leaves the recorder running.
inline bool shortcutAsksTransport(ShortcutId id) noexcept
{
    return id == ShortcutId::playOrStop || id == ShortcutId::recordNow;
}

inline int commandIdFor(ShortcutId id) noexcept
{
    if (id == ShortcutId::save)
        return kSaveCommandId;
    if (id == ShortcutId::saveAs)
        return kSaveAsCommandId;
    if (id == ShortcutId::selectAll)
        return kSelectAllCommandId;
    if (id == ShortcutId::makeGroup)
        return kMakeGroupCommandId;
    if (id == ShortcutId::undo)
        return kUndoCommandId;
    if (id == ShortcutId::redo)
        return kRedoCommandId;
    return 0;
}

inline std::optional<ShortcutId> shortcutIdForCommand(int commandId) noexcept
{
    if (commandId == kSaveCommandId)
        return ShortcutId::save;
    if (commandId == kSaveAsCommandId)
        return ShortcutId::saveAs;
    if (commandId == kSelectAllCommandId)
        return ShortcutId::selectAll;
    if (commandId == kMakeGroupCommandId)
        return ShortcutId::makeGroup;
    if (commandId == kUndoCommandId)
        return ShortcutId::undo;
    if (commandId == kRedoCommandId)
        return ShortcutId::redo;
    return std::nullopt;
}

// A key as the platform delivers it. On macOS, Command forces the text
// character to 0 and the key code is the uppercase letter.
enum class RawToken
{
    none,
    space,
    left,
    right,
    backspace
};

struct RawKey
{
    RawToken token = RawToken::none;
    int keyCode = 0;
    int textCharacter = 0;
    bool shift = false;
    bool command = false;
    bool alt = false;
};

inline char lowerAscii(int value) noexcept
{
    if (value >= 'A' && value <= 'Z')
        return static_cast<char>(value - 'A' + 'a');
    if (value > 0 && value < 128)
        return static_cast<char>(value);
    return 0;
}

inline KeyQuery keyQueryFromRaw(const RawKey& key) noexcept
{
    KeyQuery query;
    query.shift = key.shift;
    query.command = key.command;
    query.alt = key.alt;
    const char codeLetter = lowerAscii(key.keyCode);
    const char textLetter = lowerAscii(key.textCharacter);
    query.character = key.textCharacter != 0 ? textLetter : codeLetter;
    if (key.token == RawToken::space || codeLetter == ' ' || textLetter == ' ')
        query.kind = KeyKind::space;
    else if (key.token == RawToken::left)
        query.kind = KeyKind::left;
    else if (key.token == RawToken::right)
        query.kind = KeyKind::right;
    else if (key.token == RawToken::backspace || key.keyCode == 8 || key.textCharacter == 8)
        query.kind = KeyKind::backspace;
    else if (key.keyCode == '[' || textLetter == '[')
        query.kind = KeyKind::bracketLeft;
    else if (key.keyCode == ']' || textLetter == ']')
        query.kind = KeyKind::bracketRight;
    else if (codeLetter == 'r' || textLetter == 'r')
        query.kind = KeyKind::letterR;
    else
        query.kind = KeyKind::character;
    return query;
}

// Same comparison the command manager uses: the registered key has no text
// character, and letter case does not matter.
inline bool commandKeyCodeMatches(int registered, int eventCode) noexcept
{
    if (registered == eventCode)
        return true;
    if (registered > 0 && registered < 256 && eventCode > 0 && eventCode < 256)
        return lowerAscii(registered) == lowerAscii(eventCode);
    return false;
}

inline int commandForRawKey(const RawKey& key) noexcept
{
    KeyQuery mods;
    mods.shift = key.shift;
    mods.command = key.command;
    mods.alt = key.alt;
    for (const auto& binding : kBindings)
    {
        if (! binding.commandBinding || binding.commandCharacter == 0)
            continue;
        if (! bindingModsMatch(binding, mods))
            continue;
        if (! commandKeyCodeMatches(binding.commandCharacter, key.keyCode))
            continue;
        return commandIdFor(binding.id);
    }
    return 0;
}

// F and Option+R fit the timeline on REC and HOST. A text field keeps the key.
inline bool fitShortcutApplies(int page, bool textFieldFocused) noexcept
{
    return (page == 1 || page == 2) && ! textFieldFocused;
}

// Select All and Make Group work on the REC page and the HOST page. A text field keeps the key.
// Save stays available: the command manager already ran it while a field was focused.
inline bool commandApplies(ShortcutId id, int page, bool textFieldFocused) noexcept
{
    if (id == ShortcutId::selectAll)
        return selectAllShortcutApplies(page, textFieldFocused);
    if (id == ShortcutId::makeGroup)
        return makeGroupShortcutApplies(page, textFieldFocused);
    if (id == ShortcutId::fit)
        return fitShortcutApplies(page, textFieldFocused);
    if ((id == ShortcutId::undo || id == ShortcutId::redo) && textFieldFocused)
        return false;
    return true;
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

// Command manager first, then the key table. This is the path both the menu
// command and the key listener use. A macOS Command event has no text character.
inline std::optional<ShortcutId> dispatchRawKey(const RawKey& key, int page, bool textFieldFocused)
{
    const int command = commandForRawKey(key);
    if (command != 0)
    {
        const auto id = shortcutIdForCommand(command);
        if (! id.has_value() || ! commandApplies(*id, page, textFieldFocused))
            return std::nullopt;
        return id;
    }
    if (textFieldFocused)
        return std::nullopt;
    const auto matched = matchShortcut(keyQueryFromRaw(key));
    if (! matched.has_value() || ! commandApplies(*matched, page, false))
        return std::nullopt;
    return matched;
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

inline constexpr const char* kShortcutNotes = kHelpManual;

inline std::string shortcutHelpText()
{
    return std::string(kHelpManual);
}

} // namespace youhost
