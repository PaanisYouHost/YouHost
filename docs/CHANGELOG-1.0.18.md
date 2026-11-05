# YouHost 1.0.18

2026-11-05

Every item below is implemented in this build. None of it has been tested on real hardware with FabFilter, UADx, SSL, or Neural DSP. Do not read any line as "fixed".

## Quit requests

Implemented, not yet tested on real hardware. YouHost logs the caller when something asks the process to quit (`exit`, `_exit`, or a terminate request). A quit is refused unless it came from the close button, the Quit menu, or Command-Q. A scan helper and a protected helper may still exit when their own work is finished. This is aimed at the FabFilter VST3 case where the log showed `quit` about 120 ms after the plugin became active and the crash journal was clean.

## Editors

Implemented, not yet tested on real hardware. VST3 and Audio Unit editors are created on the message thread, on a fresh turn of the run loop. JUCE already requests an Audio Unit view controller asynchronously and, when that callback is not on the message thread, posts the view back to it. YouHost no longer creates that view on a worker thread. The previous worker is the path that logged "plugin window crashed on its own thread" for FabFilter Pro-C 3 and Pro-G Audio Units.

## Scanner

Implemented, not yet tested on real hardware. Each plugin is scanned in its own process, so a late exit from one module is not blamed on the next plugin. A worker that is still running at the time limit is a timeout, not a crash. A crash, a plugin exit, a timeout, and a worker that fails to start are tried once more before a blacklist decision. A timeout is not blacklisted. The plugin list XML is parsed off the message thread.

## Startup and card changes

Implemented, not yet tested on real hardware. Opening the audio device and applying a chosen card run on a background thread so the message thread can show the window. In the 5.11 log the ~0.8 s stall stacks were Core Audio `setAudioDeviceSetup` during a card change, and the message thread sitting in the run loop before the 30 Hz heartbeat during an unclean start.

The dropout x11 at 1% DSP happened while the callback was already 64 samples and the configured period was still the 32-sample period (666 us, late by about 670 us). Those gaps are the driver changing buffer size. YouHost does not count them as dropouts. This is not a claim that the interface itself is silent during the switch.

## Protected mode

Implemented, not yet tested on real hardware. The first time a plugin is loaded, and again when its version key changes, a helper process loads it, opens the editor, and passes audio for about 8 seconds. Only a helper that crashes or quits is marked protected. A timeout is not enough. Protected plugins run in that helper. The channel delay is 32 samples, shared by every protected plugin on that channel. Every other plugin stays in the host process with no added latency. The list is `protected-plugins.txt`. Retry runs the test again and, if it passes, the plugin is hosted in-process.

## Safety

Implemented, not yet tested on real hardware. When a protected helper dies, that channel's output becomes silence (a short fade, then mute). The dry input is not copied through. The helper is started again with the last saved settings. When it processes audio, the level fades back in on its own. OK only closes the notice. If the helper does not come back, the notice stays dark red with Retry and Bypass plugin. Bypass is the only path that fades the dry signal in. Other channels, including recording, are not stopped.

## State snapshots

Implemented, not yet tested on real hardware. A parameter edit sets a flag. A few seconds later, and only while the audio thread is not inside that plugin, the message thread copies `getStateInformation`. The audio thread does not call it.

## Notices

Implemented, not yet tested on real hardware. English only: PLUGIN CRASHED (red, blinking), PLUGIN RESTORED (green, and a bottom-left box at the moment the fade starts), PLUGIN NOT RESTORED (dark red), PLUGIN PROTECTED (blue, Retry / Cancel / OK). A protected channel shows a small blue mark on the strip.

## Session files

The session format stays 7. A `protect="1"` attribute is written only when a slot is protected. Older files omit it and open as before. An older YouHost does not know the attribute. Unknown attributes are still kept when a file is saved again.
