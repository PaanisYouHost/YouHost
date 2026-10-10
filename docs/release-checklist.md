# YouHost release checklist

Every release runs the engine tests. They print one line per rule. The run passes only when every line is `[pass]`.

```
g++ -std=c++20 -Wall -Wextra -Wpedantic -I src tests/engine_tests.cpp -o /tmp/YouHostTests && /tmp/YouHostTests
```

`testReleaseChecklist` is the report for this file. The other tests in that binary stay part of the same gate. A Mac build of the app still has to open every window below, once with no saved size and once with an old small saved size, and the text must be on screen. This test checks that open size instead of a screenshot: there is no display in the unit-test run.

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
| Status line | CPU is padded to three digits. Every status column has a fixed width. |
| Install guide | The install note is the Mac steps after YouHost.app is already on the Mac. |
| Session required | The main window stays closed until a session exists. |
| Cmd+Q hook | Quit goes through the unsaved-changes prompt. |
| Record lock | The padlock is the only control that arms the lock. |
| Device menu flicker | The audio card menu is rebuilt when the device list changes, not on every timer tick. |
| Show Backups | There is no Show Backups control. Backups still run on their own. |
| REC lane click | Clicking a channel on the REC page scrolls to that lane and makes it tall enough to read. |

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
