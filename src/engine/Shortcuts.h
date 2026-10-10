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
    goToChannel,
    makeGroup,
    selectAll,
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
    { ShortcutId::selectAll, KeyKind::character, 'a', 0, 1, 0, "Cmd+A",
      "Select every visible channel on the REC page", false, 0 },
    { ShortcutId::selectAll, KeyKind::character, 'a', 0, 0, 0, "A",
      "Select every visible channel on the REC page", false, 0 },
    { ShortcutId::makeGroup, KeyKind::character, 'g', 0, 1, 0, "Cmd+G",
      "On the REC page, name a group from the selected channels, pick a colour, and fold it", false, 0 },
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
    "YouHost — ASENNUSOHJE\n"
    "=====================\n"
    "\n"
    "1. Pura zip kaksoisklikkauksella.\n"
    "\n"
    "2. Siirrä YouHost.app kansioon Ohjelmat (Applications).\n"
    "\n"
    "3. Ensimmäisellä käynnistyksellä macOS voi estää ohjelman. Klikkaa YouHost.appia oikealla ja valitse Avaa. Tai avaa Järjestelmäasetukset > Tietosuoja ja suojaus > Avaa silti. Vaihtoehto päätteessä:\n"
    "   xattr -dr com.apple.quarantine /Applications/YouHost.app\n"
    "\n"
    "4. Salli mikrofoni ja äänitulo, kun macOS kysyy.\n"
    "\n"
    "5. Käynnistysikkunassa valitse äänikortti.\n"
    "\n"
    "\n"
    "YouHost — INSTALL\n"
    "=================\n"
    "\n"
    "1. Unzip the file (double-click).\n"
    "\n"
    "2. Move YouHost.app to Applications.\n"
    "\n"
    "3. On the first launch macOS may block the app. Right-click YouHost.app and choose Open. Or open System Settings > Privacy & Security > Open Anyway. Optional Terminal command:\n"
    "   xattr -dr com.apple.quarantine /Applications/YouHost.app\n"
    "\n"
    "4. Allow the microphone and audio input when macOS asks.\n"
    "\n"
    "5. In the startup window, choose the audio card.\n"
    "Click a channel to select it. Shift+click selects the channels from the anchor through the one you click. "
    "Cmd+click, or Ctrl+click, adds or removes that one channel. "
    "Cmd+A, or A, selects every visible channel on the REC page, so you can colour or group them together. "
    "It does not select on the HOST page.\n"
    "On the REC page, Cmd+G does the same thing as the Group button and right-click Make group from selection: "
    "one dialog for the selected channels. "
    "Cmd+A, then Cmd+G, groups every visible channel. "
    "Cmd+G does nothing on the HOST page, and nothing while a text field has focus. "
    "The name is selected so you can type, and the colour swatches sit under it. "
    "Enter or OK creates the group, applies the name and colour, and folds it. Esc cancels. "
    "The group bar stays visible. Click it to open the channels again.\n"
    "On the REC page a group bar shows the group name in the centre. On the HOST page the name stays on the left. "
    "The fold mark is a fixed column, \u25BE 8 ch when open and \u25B8 8 ch when folded.\n"
    "Double-click a channel name, or choose Rename. The editor opens with the name selected. Enter commits. Esc cancels. "
    "Tab commits and edits the next visible channel. Shift+Tab edits the previous one.\n"
    "Rename on a group, or double-click the group bar, opens the name with a row of colour swatches under it. "
    "A swatch sets the colour immediately. Enter keeps the name and the colour. Esc puts both back. "
    "The group menu still has Color and Rename.\n"
    "Each channel has a mode button. REC means the audio passes through the plugins and is recorded. "
    "INPUT means the audio passes through the plugins to the output and is not recorded. "
    "OFF cuts the channel fully: no audio, no plugins, and no recording. "
    "Click the button to cycle REC, INPUT, OFF. The default is REC.\n"
    "The interface always opens with all channels, up to 128. Channel use is chosen only with these buttons. "
    "Audio setup has the device, sample rate, and buffer size. It does not turn channels on or off.\n"
    "The device list starts on the connected interface, including a MacBook built-in input, and the mixer shows that card's channels. "
    "Real hardware is listed first, including USB, Thunderbolt, AVB, Dante, and SoundGrid. "
    "Then the heading Virtual / aggregate devices, then the MacBook built-in, then Offline. "
    "Each entry shows channel counts, for example WING 2 - 48 in / 48 out. "
    "YouHost does not auto-select a virtual device such as Pro Tools Audio Bridge, Teams, an aggregate, or a loopback unless that device was chosen last time. "
    "The same device menu is in the startup window and in Audio setup, beside the sample rate and the buffer. "
    "It stays there for the whole session. Switching cards keeps channel names, plugins, and groups. "
    "Offline (no audio) - 128 channels is the last entry in that menu and is not selected unless you choose it. "
    "Choosing it opens no audio device and shows all 128 channels. Sample rate, bit depth, and buffer stay selectable and are saved. "
    "The session always keeps 128 channels. A stored channels count is ignored. "
    "Opening a session uses the device selected in the startup window, not the device stored in the session. "
    "Session channel N is device channel N. Channels past the card stay hidden on REC, HOST, and the timeline. "
    "They stay in the session and are not processed. A card with enough channels shows them again. "
    "Hide unused channels with groups and Hide. "
    "The channel list header has ALL REC, ALL INPUT, and ALL OFF on REC and on HOST. They set every visible channel. A selection does not limit them. "
    "Leaving REC while recording asks first. The record lock still blocks them. "
    "If the card rate is higher than the session, YouHost shows Session moves to 48 kHz. A lower rate changes with no prompt. "
    "Plugins are prepared again off the audio thread. Takes keep the rate in their WAV files.\n"
    "The padlock beside the transport is the record lock. Click it to arm the lock, including before the take starts. "
    "While recording and locked, Stop, Space, Cmd+Space, Record, New, Open, Open Recent, Clear, Import, "
    "device, sample rate, buffer, Offline, and REC, INPUT, and OFF are blocked. "
    "Quit asks you to unlock first. Only another click on the padlock unlocks it. There is no keyboard shortcut. "
    "The padlock is red and the banner says RECORDING LOCKED. Unlocked, the padlock is a calm teal. "
    "While recording, HOST stays usable: plugins, editors, bypass, output gain, groups, names, and colours. "
    "The recording copies the raw input before the plugins.\n"
    "New, Open, Open Recent, and quit ask Save, Save As, Don't Save, or Cancel when the session has unsaved changes. "
    "A take that is still recording has to be stopped before the session can close.\n"
    "Record arms the take and the button blinks red. While REC is armed and not recording, the transport shows REC ARMED - press PLAY (or Cmd+Space). "
    "Play or Space starts the take. Cmd+Space records immediately. "
    "Play while not armed is the virtual soundcheck. Stop turns record arm off. "
    "If recording does not start, YouHost writes the reason to youhost.log and shows an alert. "
    "The reasons include no session, the disk, the device not running, and no REC channel with an input.\n"
    "Cmd+Space reaches YouHost only when Spotlight is not using that shortcut.\n"
    "The + and - buttons zoom time the same way as T and R. The W+ and W- buttons change waveform height. "
    "That does not change the audio. Cmd or Option plus the wheel over the lanes does the same.\n"
    "FIT is the green button at the left of the timeline controls, on REC and on HOST. "
    "It does the same as Option+R. With no takes yet the timeline shows 60 seconds. "
    "FIT shows every take, from 0 to just past the last take, and every lane. "
    "Lanes stay tall enough to read. The bar on the right scrolls the rest and can be dragged.\n"
    "Click a channel on REC or HOST and the timeline scrolls that lane into view. "
    "G focuses Go. Type the channel number and press Return. "
    "v+ and v- keep the lane under the mouse, or the selected channel, on screen. "
    "Each lane shows its channel number and name in the top-left corner. That label stays put when you scroll time.\n"
    "The v+ and v- buttons change lane height the same way as Cmd+[ and Cmd+].\n"
    "While recording or playing, the cursor sits about three quarters of the way across the view. "
    "A scroll holds the view until the cursor leaves it.\n"
    "The plain wheel scrolls lanes. Shift plus the wheel, or a sideways trackpad swipe, scrolls time. "
    "The bars on the right and along the bottom do the same. Drag the timeline's bottom edge to change its height.\n"
    "On HOST, the dB box after the plugins is output gain, -9 to +9. Drag for 0.5 dB steps, double-click to type, "
    "Option-click resets to 0. The wheel does not change it. Amber means it is not 0 dB. It is not written to the WAV.\n"
    "Option-drag copies a plugin and its settings. A plain drag moves the same plugin. "
    "A drag is ignored while a plugin on that channel is still loading.\n"
    "ALL PLUGIN BYPASS is the button on HOST. It lights amber while every plugin and the compensation delay are bypassed. "
    "Loaded plugin slots turn amber and blink slowly until you click the button again. The recorded WAV is always the raw input.\n"
    "LATENCY chooses Global or Per group. Global is the default: every included channel lines up on the slowest plugin. "
    "Per group lines each group up on its own slowest plugin and lists that delay in the window. Ungrouped channels get no extra delay. "
    "The dropout graph is the DROPOUTS window. LATENCY does not have a Timeline button. "
    "The window opens large enough to show the compensation value, Global, Per group, the group list, and the alignment note. "
    "A saved size smaller than that content is replaced.\n"
    "FX and PDC on a row are that channel's plugin latency and the extra delay that lines it up.\n"
    "New starts a clean session: no plugins, no channel names, no colours, groups back to defaults, every channel REC, "
    "output gain 0 dB, bypass off, and an empty timeline. The device, sample rate, bit depth, and buffer stay as they are. "
    "New, Open, and Open Recent ask before discarding unsaved changes. Save writes this session. The first save asks for a name and a folder. "
    "Save As copies the whole folder, including the WAV files, and continues in the copy.\n"
    "Windows open at least as large as their content, and never smaller than that unless the screen itself is smaller. "
    "A larger size is remembered. An older smaller size is replaced so the text stays visible.\n"
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
