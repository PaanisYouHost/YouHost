#pragma once

namespace youhost
{

// English help. Stored as UTF-8. The window must use juce::String::fromUTF8.
inline constexpr const char* kHelpManual = R"YH(YOUHOST – HELP
==============

YouHost is a live plugin host and multitrack recorder for Mac. Two main pages: REC (recording, key 1) and HOST (plugins, key 2).

Two basic rules
- HOST never affects the recording. Recording is always the raw input, before the plugins.
- Nothing you do on HOST (plugins, bypass, gain, groups, names, colours) stops recording.

What's new
----------
- 1.0.4: a sidechain plugin is processed on its full layout, with the sidechain fed silence. If that probe is silent, the plugin stays off instead of being activated. A plugin editor can take a long time to open without stopping audio or recording.
- 1.0.3: after a crash, the next launch offers Open session without this plugin. The plugin that was active or loading stays out. Its slot stays in the session file. Recording is not stopped.
- A sidechain plugin keeps the stereo bus it opened with. Its editor opens after the load has finished.
- Bus setup and prepare stay off the message thread, so a slow plugin does not freeze the app.

1. Quick start – first recording in 5 steps
-------------------------------------------
1. In the startup window, choose your audio card (YouHost always starts in Offline).
2. Check the sample rate and buffer in Audio setup.
3. Leave the channels you want to record in REC mode (the default).
4. Press Record (the button blinks red: REC ARMED).
5. Press Play or Space – recording starts. (Cmd+Space records immediately.) Stop with Space or Stop.

2. Before a gig – best performance
----------------------------------
- Quit other apps.
- Pause cloud sync.
- Turn Wi-Fi and Bluetooth off.
- Turn notifications off, or turn Focus on.
- Plug in the power adapter. Disable sleep.
- Close unused plugin servers.
- Set the buffer to 32. Raise it to 64 if dropouts appear.
- Waves LV1: lower the SoundGrid driver buffer and the network buffer (e.g. 12 ms → 3.6 ms).
- Don't scan plugins during a show.

3. Audio card and settings
--------------------------
- The device menu is in the startup window and in Audio setup (next to sample rate and buffer).
- Real hardware is listed first (USB, Thunderbolt, AVB, Dante, SoundGrid), then Virtual / aggregate devices, then the MacBook built-in, and Offline last.
- YouHost never opens a card by itself. Opening a session restores its card, sample rate, bit depth and buffer. If the card is not found, YouHost stays Offline.
- Switching cards keeps channel names, plugins and groups.
- A session always has 128 channels. Channels beyond the card's count are hidden.
- The channel mode button cycles: REC (plugins + recording) → INPUT (plugins, not recorded) → OFF (channel fully off).
- ALL REC / ALL INPUT / ALL OFF set every visible channel.

4. Recording (REC)
------------------
- Record arms, Play/Space starts, Cmd+Space records immediately.
- Cmd+Space only works if Spotlight is not using that shortcut.
- Each channel is recorded as its own mono WAV: channel_name_take.wav, e.g. 1_BD_1.wav.
- Record lock (padlock next to Record): click to arm it, even before the take. While locked, Stop, Space, Record, New, Open, Import, device/sample rate/buffer changes and REC/INPUT/OFF are blocked. Only another click on the padlock unlocks it. Red = locked (RECORDING LOCKED).
- Changing a channel mode while recording asks twice.
- If recording doesn't start, you get an alert and the reason is written to youhost.log.

5. HOST and plugins
-------------------
- Open the scanner: S. Scanning may cause dropouts.
- VST3 folders: /Library/Audio/Plug-Ins/VST3 and ~/Library/Audio/Plug-Ins/VST3.
- Audio Units are found through the macOS registry.
- Waves shells and instruments are off by default ("Scan Waves shells" and "Show instruments in inserts" in the scanner). "Rescan selected" scans the selected plugins again.
- The plugin list marks each plugin M, ST or M/ST.
- Right-click a plugin slot: Open, Bypass, Remove, and for a stereo plugin on a mono channel: Use L / Use R / Sum (L+R)/2.
- If a stereo plugin won't open on a mono channel (some stereo-only AU plugins), use the VST3 or mono version.
- Stereo link: right-click > Stereo link with ch N+1, or click LINK on the odd channel (1–2, 3–4…). Recording stays one mono WAV per channel.
- Drag = move a plugin. Option+drag = copy it with its settings.
- Output gain (dB box): −9 to +9 dB. Option-click resets to 0. Not written to the WAV.
- ALL PLUGIN BYPASS bypasses every plugin (lights amber).
- LATENCY (5): Global (default) or Per group.

6. Groups
---------
- Select channels (click, Shift+click, Cmd+click, Cmd+A).
- Cmd+G or the Group button: name + colour, Enter creates and folds the group.
- Click the group bar to open it. Double-click it to rename.
- Groups are visual by default and do not affect audio or recording. Exception: with LATENCY set to Per group, delay is aligned per group.

7. Markers
----------
- M drops a marker at the playhead – recording keeps going.
- 6 opens the MARKERS list. Select a marker and press Space to play from it.
- Delete: Cmd+Backspace, or right-click > Delete.

8. Virtual soundcheck / playback
--------------------------------
1. Don't arm Record.
2. Press Play: the recorded tracks run through the HOST plugin chain instead of the live input.
3. Stop returns the live input.
- The switch is a short crossfade, no click.
- The recorder never records the playback.

9. Import
---------
File > Import Recording Folder...
1. Save the session first.
2. Put the mono WAVs in one folder.
3. Name them channel_name_take.wav, e.g. 1_BD_1.wav, 2_Sn_1.wav.
4. All files must share one sample rate.
5. Choose the folder.
- Not available while recording or while the record lock is on.
- Reaper: Render > stems (mono), then name the files as above.

10. Sessions
------------
- New: clean session (device settings stay). Named by date, DD.MM.YYYY.
- Open... / Open Recent: open a session.
- Save (Cmd+S): writes session.youhost. The first save asks for a name and a folder.
- Save As (Cmd+Shift+S): copies the whole session, including audio, to a new folder and continues there. Not while recording.
- A backup is written every 5 minutes to the Backups folder (10 newest kept).
- You are always asked about unsaved changes.

11. Keyboard shortcuts
----------------------
Key                   Action
1 / 2                 REC page / HOST page
3 or D                DROPOUTS
4                     CPU
5                     LATENCY
6                     MARKERS
S                     Scanner (SCAN)
Space                 Play / Stop
Cmd+Space             Record immediately
M                     Marker
Cmd+Backspace         Delete selected marker
Left / Right          Previous / next take
Shift+Left / Right    5 s back / forward
T / R                 Zoom in / out
F or Option+R         Fit the whole session
Cmd+[ / Cmd+]         Taller / shorter lanes
G                     Go to channel (number + Return)
Cmd+A or A            Select all channels
Cmd+G                 Make group from selection
Cmd+S / Cmd+Shift+S   Save / Save As
Cmd+Z / Cmd+Shift+Z   Undo / redo (takes and WAVs are kept)
Enter / Esc           Commit / cancel a name
Tab / Shift+Tab       Next / previous channel name

Shortcuts do nothing while a text field has focus.

12. Troubleshooting
-------------------
- SYNC LOST: the card in use disappeared. A large window stays until the card returns; then it shrinks to a blinking box in the bottom-left corner. Click it to see when sync was lost and for how long. It does not stop audio or recording.
- Sample rate mismatch: a red line names both rates, for example "Sample rate mismatch: session 48 kHz, audio device 44.1 kHz". youhost.log records it. Audio and recording keep going.
- Dropouts: open DROPOUTS (3). Raise the buffer to 64; see section 2.
- CPU: open CPU (4). DSP 100% = the buffer is full and the next callback drops out. You can see each plugin's share.
- Plugin won't load: scan it again (S > Rescan selected). After a crash, the next launch offers Open session without this plugin. The slot stays in the session file.
- "Damaged" / macOS blocks the app: right-click YouHost.app > Open, or System Settings > Privacy & Security > Open Anyway, or in Terminal:
  xattr -dr com.apple.quarantine /Applications/YouHost.app
- Logs: Application Support > Ambient Audio > YouHost (youhost.log, crash-journal.txt).
)YH";

inline const char* helpButtonTip() noexcept
{
    return "Open YouHost help.";
}

} // namespace youhost
