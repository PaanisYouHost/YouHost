# YouHost release checklist

Every release runs the engine tests. They print one line per rule. The run passes only when every line is `[pass]`.

```
g++ -std=c++20 -Wall -Wextra -Wpedantic -I src tests/engine_tests.cpp -o /tmp/YouHostTests && /tmp/YouHostTests
```

The macOS workflow builds this same `YouHostTests` target without `NDEBUG`, so the lock-reentry checks stay on, plus an Apple AUDelay silent-input load and an offline chain of AUDelay, AUDistortion, AUGraphicEQ, and AUBandpass at factory defaults. Silence must stay silent. An impulse must decay: the tail peak never grows, each 1 s window has less energy than the one before, and the signal is at or below -60 dBFS by 10 s. Parameters are not pulled to zero. The job fails if any of them fail.

`YouHostGroupStressTest` runs the real audio callback on a realtime thread at 48 kHz and 32 samples with 16 Apple Audio Units loaded, while the message thread creates, edits, recolours, and removes groups for more than 30 s. A deadline miss is a run of callbacks whose own CPU time stays past the buffer period. A hitch already present on the quiet baseline does not count, and the edit window may not add late callbacks on top of that baseline. The median of each window has to sit inside the period, and the median after the edits has to come back to the quiet reading. `YouHostMonoNullTest` compares a 1/1 Apple AU with the Fold L 2/2 path.

`testReleaseChecklist` is the report for this file. The other tests in that binary stay part of the same gate. A Mac build of the app still has to open every window below, once with no saved size and once with an old small saved size, and the text must be on screen. This test checks that open size instead of a screenshot: there is no display in the unit-test run.

## Groups are purely visual

Groups store a name, a colour, which channels are members, and whether the strips are folded. Create, edit, recolour, and remove — including Remove group while those channels have plugins loaded — must not touch plugin instances, the live graph, compensation delay, or the audio thread. No lock the callback takes, no allocation, no `publish` / graph rebuild, no `prepareToPlay`.

Remove group used to call compensation sync. That took the rack lock and published a new graph, rebuilding delay lines, feed rings, and scratch while the plugins were still loaded. The callback then ran long on every buffer, and the CPU meter stayed high until the process was restarted. Membership edits no longer publish.

Only plugins, output gain, and stereo link may change audio processing.

Per-group latency compensation still reads group membership when the LATENCY switch is set to Per group, and when a session is loaded or started. A group edit does not refresh that plan. See the release notes for how that switch is wired. Mika decides whether Per group stays.

## Rules

| Rule | What the test checks |
| --- | --- |
| Windows open full-size | LATENCY, CPU, DROPOUTS, SCAN, Audio setup, Shortcuts, Start session, New session, the group dialog, and the plugin list open at least as large as their content on a 1920×1080 screen. An old 320×180 saved size is replaced, not restored. A larger remembered size is kept. The result is clamped to the screen. |
| ALL buttons override | ALL REC, ALL INPUT, and ALL OFF set every visible channel on the REC page and the HOST page. A selection does not narrow them. Channels past the visible count stay as they were. |
| Cmd+Q prompt | A dirty session asks Save, Save As, Don't Save, or Cancel. Cancel does not quit. Save and Save As run first. Don't Save quits. A take that is still recording must be stopped before that prompt. |
| Buffer 32 | A new session requests a 32-sample buffer. |
| Device lock, no polling | While a chosen card is up and its inventory is already filled, a poll does not query devices. A real device loss, an empty inventory, or an explicit card change may query. |
| One audio card menu | Start session and Audio setup each show Audio card, Sample rate, Bit depth, and Buffer, in that order. Offline is in the card menu and is selected when no card is open. Buffer defaults to 32. There is no Input menu, Output menu, test meter, or Internal disk button. |
| Record starts | Cmd+Space with a session, a live device, and a REC channel that has an input starts the take. |
| No hangs | Fifty open-size and compensation calculations stay under 16 ms. The same binary also runs the plugin-edit and audio-engine stress tests. |
| New, Open, Save, Save As | New, Open, and Open Recent ask before discarding edits. Save and Save As are the choices that write first. A clean session does not ask. |
| Session compatibility | An older or newer session file still reads. `align` is kept. Unknown elements stay on the source node. A brand new session model is clean. |
| Per-group alignment | Global and Per group are the LATENCY switch. Global lines every included channel up on the slowest one. Per group delays a channel only to its own group's slowest plugin. An ungrouped channel gets no extra delay and does not move the group. |
| LATENCY has no Timeline button | DROPOUTS already shows the dropout graph. LATENCY keeps Global, Per group, and Reset. |
| ALL labels stay ALL | The buttons stay labelled ALL REC, ALL INPUT, and ALL OFF. A selection does not rename them or narrow the tooltip. |
| Group dialog | Cmd+G opens the name and colour dialog for the selected channels and the new group starts folded. An empty selection creates nothing. Plain G is still Go to channel. |
| Startup window | The startup window has the menus and New / Open / Recent. The instructional paragraph is gone. |
| MacBook built-in | The MacBook microphone and speakers are one Audio card row. Headphones stay separate. The menu label is Audio card. |
| Rapid REC/INPUT clicks | Fifty local listen writes stay under 16 ms. The click path does not rebuild the plugin graph. |
| Buffer saved in the session | A session file stores buffer 32, the card name, and the card channel count. It does not store a channels attribute. |
| Missing card | A missing saved card goes Offline, keeps that card's channel count, and the status is `Saved card X not found - Offline`. Explicit Offline still shows 128. |
| Go field focus | The Go field does not take focus until it is clicked or G is pressed. Return and Esc leave it. |
| File and Clear | The File button and Clear Timeline are gone. Open lists recent sessions and Browse. |
| Status line | DSP is padded to three digits and never goes above 100%. Every status column has a fixed width and a gap, so the card name, DSP, and session do not run together. The offline card name fits in full. |
| Install guide | The install note is the Mac steps after YouHost.app is already on the Mac. |
| Session required | The main window stays closed until a session exists. |
| Cmd+Q hook | Quit goes through the unsaved-changes prompt. |
| Record lock | The padlock is the only control that arms the lock. |
| Locked recording ignores keys | While a take is recording and the padlock is on, Space, Cmd+Space, and every other shortcut leave the take running. Play, Stop, Space, and Cmd+Space do not stop it and do not start another take. Save As and quit ask to unlock and do not stop the take. Arrow keys do not stop it. |
| DROPOUTS, CPU, and LATENCY stay open together | Clicking 3 DROPOUTS, 4 CPU, or 5 LATENCY opens that window and leaves the others open, the same as the number keys. Each one closes only from its own button, its own key, or its close box. |
| Device menu flicker | The audio card menu is rebuilt when the device list changes, not on every timer tick. |
| Show Backups | There is no Show Backups control. Backups still run on their own. |
| REC lane click | Clicking a channel on the REC page scrolls to that lane and makes it tall enough to read. |
| Show hidden channels | Audio setup offers `Show channels not on this card (17-128)` for a 16-input card, with one line: they stay in the session and are not recorded or processed. There is no per-channel "not on this card" list. |
| REC armed hint | While REC is armed and the take has not started, the transport shows `REC ARMED - press PLAY (or Cmd+Space)`. |
| Record start failure | A start that does not run writes `record start failed` to `youhost.log` and shows that reason in an alert. |
| Timeline paint lock | Timeline paint copies the takes under the recorder lock and builds the lanes after that lock is released. |
| No virtual autopick | Startup does not open a virtual or aggregate device unless that device was the last one chosen. A saved virtual name falls through to a real card. |
| Background polling | While a chosen card is up, no timer scans the device list. The 30 Hz timer samples CPU. Audio setup's 4 Hz timer reads the cached list. A scan runs when the device is lost, the inventory is empty, or the user picks another card. |
| Cmd+A and Cmd+G | Edit menu commands. A macOS Command event (no text character, uppercase key code) selects every visible channel or makes a group on REC and on HOST. A focused text field does not. The same dispatch covers 1–5, Space, Cmd+Space, Cmd+S, Cmd+Shift+S, T/R, the arrows, and G. |
| Group meters | REC and HOST group bars use one thin meter track. On REC it runs down almost the full strip. On HOST it runs across the bar. A level change repaints that track only. The checklist renders `rec_group_meter.ppm` and checks the track covers the bar. |
| Channel colour | Right-click on REC and HOST opens the colour menu. That menu had been replaced by the new-group dialog, which is why colour looked gone. The Group button and Cmd+G still open the new-group dialog. A channel with no colour of its own takes the group colour and can still be recoloured. An own colour stays when the group colour changes. Off follows the group again. Leaving the group drops a borrowed colour. Session files store `colorOwn` and still open older files. The session version stays 7. |
| DROPOUTS range | The DROPOUTS window opens on Whole session every time. Last 10 min and Last 1 hour stay in the menu. |
| F fits | Plain F fits the timeline on REC and HOST when no text field is focused. Option+R still fits. Cmd+F, Shift+F, and Option+F do not. |
| Mode change while recording | REC, INPUT, and OFF, for one channel and for ALL, on REC and HOST, ask twice while recording. The first warning shows Switch in red. The second is Are you sure? Cancel changes nothing. When not recording there is no dialog. The record lock still blocks the change. |
| RMS 0 reference | The RMS ruler stays from -60 dBFS to 0 dBFS. Choosing -20, -18, or -14 moves the 0 line and the green/yellow boundary. The top number is the headroom up to 0 dBFS. |
| Save As speed | Save As writes session.youhost once, copies WAV files on the background disk thread with a fast copy (clonefile when the volume allows), skips Backups, and shows the progress bar. The original folder is not saved first. |
| HOST column header | The HOST channel list has a fixed header, drawn once into a cache: CH, MODE, NAME, DELAY COMP, PLUGINS, GAIN, OUT. DELAY COMP starts at the EX button. OUT sits after GAIN. The header stays put while the channels scroll. |
| Plugin NaN | A plugin that emits NaN or a huge value is replaced with silence or a hard ceiling before it reaches the USB output. Removing the plugin clears that channel's delay line, scratch, and meter, including a latched clip. An empty chain is the dry input. |
| Stereo link | HOST only. Right-click Stereo link with ch N+1, or LINK, joins 1-2, 3-4, and so on. The even channel's slots are hidden. One stereo plugin processes the pair. Unlink keeps the chain on the odd channel. REC, INPUT, OFF, name, colour, and group stay per channel. The REC page menu has no Stereo link. The recorder still writes one mono WAV per channel. LINK is grey and does nothing when the channel sits in a group whose members are not exactly one adjacent pair. |
| Plugin list M/ST | Each plugin row shows M, ST, or M/ST from the scanned bus layouts. |
| Message heartbeat | The 30 Hz timer beats the watchdog before the card-open branch. An open card no longer looks like a growing stall while the message thread is idle. |
| New, Open, and Offline clear DSP | New, Open, and Offline zero every meter, clip latch, delay line, and per-slot scratch. A poisoned stereo scratch fed silence stays bit-zero, and a cleared meter sits at the -160 dB floor. Parameter and latency callbacks do not paint the whole window. |
| Groups stay visual | Create, edit, recolour, and Remove group do not call compensation sync or rebuild the plugin graph. Channels that already have plugins keep those instances. A hammer of those edits beside a running callback leaves those instances in place. There is no wall-clock deadline in this binary. YouHostGroupStressTest on macOS runs the real audio callback at 48 kHz and 32 samples with 16 Apple AUs while groups are created, edited, recoloured, and removed for more than 30 s. Adding channels to an existing group, or removing them, is the same kind of edit: membership and colour only. |
| Real CPU meter | The status line and the CPU window hero show DSP, the realtime audio path as a percent of the buffer. 100% means that path used the whole buffer and the next callback drops out. The number never goes above 100. Plugins is the sum of each plugin's own time, also capped at 100, and it is not the dropout line. Each core bar and each plugin line uses the same 0 to 100 cap. The displayed DSP and plugin figures average about one second and refresh twice a second. |
| Dropout log | A dropout is written to youhost.log with the take position, buffer, dsp percent, and the full plugin count. Events inside 500 ms share one line. The audio thread only pushes a fixed ring. |
| Playback through host | Record and live pass-through run the input to the recorder and, separately, through the host chain. Play feeds each take to that same chain instead of the live input. The handoff is a short crossfade on the dry input: it does not rebuild the graph, reset the delay lines, or replace the live input with silence while the play ring is still empty. Stop returns the live input through the host chain. The recorder copies the device input before the swap, so a take is never recorded over itself. |
| Network clock | SoundGrid, Dante, and AVB can stay in the device list when the network clock stops. Once a card the user opened has passed its 2 s grace and callbacks are steady, no callback for more than 200 ms, or dropouts that keep climbing for 200 ms while DSP is near 0%, raises SYNC LOST in the center. CoreAudio alive, running, clock-source, and nominal sample-rate changes do the same. Regular callbacks for about 1 s move the notice to the blinking bottom-left box. Offline never shows it. The watchdog reads atomics on the message-thread timer, including while the device list is not being scanned. |
| SYNC LOST | SYNC LOST appears only after a card that was already running in this session disappears. It does not appear at startup, it does not appear when a saved card fails to open, it does not appear when the user switches devices, and it does not appear in Offline. A saved card that is missing or will not open stays Offline with the notice Saved card … not found - Offline. The device list is checked again for about 2 s before that notice, in case it was not ready at launch. Choosing Offline clears the overlay. When a running card disappears, YouHost stays open and a large SYNC LOST window stays centered. OK does not dismiss that centered window while sync is still lost. The Finnish line is drawn from UTF-8 with a font that has ä and ö. After that card returns and audio resumes, the window shrinks to a small box in the bottom-left corner and blinks. The blink repaints only that box. Clicking the box opens a centered dialog, Sync was lost at hh:mm:ss for N s, now restored, with OK. OK removes the blinking box. The click does not stop audio or recording. The same card is opened again from the CoreAudio device-list notification, not from a timer scan. A different card is not opened. Plugins stay loaded. A take that was recording is closed and marked Device lost, then a new take starts when that card returns. youhost.log records the gap in milliseconds. The padlock stays armed. The reopen does not wait for the click. |
| Fast device switching | Five device changes inside 3 s, then Offline and back to the last card. That card runs immediately: no SYNC LOST, no hang, and no stuck window. A device change the user makes never raises SYNC LOST. Each newly opened card waits 2 s, and until its callbacks are steady, before the watchdog arms, and the first dropouts are ignored. Choosing a device or Offline closes a SYNC LOST window. A real clock loss during the show still raises the centered window. A real hot-plug loss while recording still shows SYNC LOST, closes the take, and resumes when that same card returns. When callbacks are steady again the window shrinks to the blinking bottom-left box. Clicking the box opens the restored dialog. OK there removes it. OK does not dismiss the centered window while sync is still lost. |
| HOST OUT meter | Each HOST row has an OUT meter after GAIN. It uses the same RMS or peak ballistics, scale, and colour thresholds as the HOST input meter, read after the output trim. A bypassed or unity chain matches the input meter within 0.1 dB. New and Offline clear it. |
| Bigger CLIP | A latched clip on the REC page is a full-width band, at least 16 px tall on a normal strip, with white CLIP text. |
| Undo and redo | Cmd+Z and Cmd+Shift+Z undo and redo mix edits: names, colours, groups, modes, gain, link, and plugins. A focused text field keeps its own undo. Undo never stops a take or deletes a WAV file. |
| Channel name outline | REC and HOST draw a thin white outline around a channel name while it is not being edited. |
| Before a gig | Help opens with Before a gig / for best performance, in English and Finnish, before the shortcut list body. INSTALL.md has the same section before step 1 in both languages. |
| Version label | The window title, Help, and About say YouHost 1.0.1. Beta is not part of that label. A saved session stores that in savedWith. Older sessions still open, and a session written by this version still opens. The session format stays 7. The Mac zip is YouHost-1.0.1-macOS.zip. The release name is YouHost 1.0.1. |
| GitHub Release | After the macOS run for a version is green, publish a GitHub Release on that commit. The tag is vX.Y.Z, the name is YouHost X.Y.Z, and it is not a pre-release. The asset is YouHost-X.Y.Z-macOS.zip. The download URL is https://github.com/PaanisYouHost/YouHost/releases/download/vX.Y.Z/YouHost-X.Y.Z-macOS.zip. |
| Markers | The MARKER button sits at the right of the transport. M and that button drop a marker at the playhead and do not start, stop, or move the take, including while the record lock is on. 6 opens the marker list. Markers live in the session file only. Space, M, and the number keys still reach the transport while MARKERS, CPU, DROPOUTS, or LATENCY is focused. A marker name editor keeps the key. Selecting a marker and pressing Space starts playback from that marker. Cmd+Backspace deletes the selected marker in that list or on the timeline. Right-click a marker in either place and choose Delete. The list also has a Delete button. Cmd+Z puts the marker back. A later gain change is its own step: the first Cmd+Z undoes the gain and the second brings the marker back. Cmd+Shift+Z puts those steps back in reverse. Deleting does not stop or move a take, including while the record lock is on. Plain Backspace and plain Delete do not delete a marker. The Device lost marker from SYNC LOST deletes the same way. Nothing is written to the WAV files. |
| Mono fold stays 2/2 | A stereo plugin on a linked pair stays one 2-in/2-out process. Fold L remains the fold when a plugin has no mono layout. In this build, unlinked mono channels use 1/1. YouHostMonoNullTest compared a 1/1 Apple AU with Fold L 2/2 and the levels matched within 0.1 dB. |
| WAV names | A new recording is channel_name_take.wav, for example 1_BD_1.wav, 1_BD_2.wav, and 2_Sn_1.wav. A channel with no name is channel_take.wav, for example 1_1.wav and 1_2.wav. The name is made safe for the filesystem. Files already stored in a session keep the names they have, including the older channel_take_name.wav layout, and those sessions still open. |

## Windows to photograph on a Mac release

Open each of these with no saved window size, then again after planting a small saved size (about 320×180):

- LATENCY
- CPU
- DROPOUTS
- SCAN
- Audio setup
- Shortcuts
- Start session
- New session
- Group name dialog
- Plugin list

The open size check above is what this repository runs on every test build. Attach the photographs to the release when the app is built on a Mac.
