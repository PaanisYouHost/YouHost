# YouHost

YouHost is a free, open-source live plugin host for macOS. It is aimed at front-of-house work: pick an audio interface (from a stereo pair up to 128 channels, such as a Behringer X32 USB 32×32 or a Klark Teknik DN32-USB), send each input back out of the same-numbered output, and insert up to four plugins on each channel.

This build opens the device, passes input *n* to output *n*, records a WAV for each armed channel, and hosts AU and VST3 plugins in-process. Scanning runs in a child process so a plugin that crashes while being identified does not take the mixer down. Playback is not sandboxed yet: a plugin can still crash the app while it is loaded. The slot graph is the piece a later sandbox can replace.

YouHost is [AGPLv3](LICENSE). It is built with [JUCE 9.0.3](https://github.com/juce-framework/JUCE/releases/tag/9.0.3) under JUCE's AGPLv3 option, so the app costs nothing to build or run.

## Suomeksi

YouHost on ilmainen, avoimen lähdekoodin (AGPLv3) Macille tehty plugin-hosti livekäyttöön. Sisääntulo *n* menee ulostuloon *n*, ja jokaisella kanavalla on neljä plugin-paikkaa sarjassa (AU ja VST3). Skannaus tapahtuu erillisissä prosesseissa, useita kerrallaan, ja jumittunut skanneri tapetaan. Waves-shellit ohitetaan oletuksena. Mittari käynnistyy aina RMS-tilaan. Nauhoitus kirjoittaa WAV-tiedostot session `audio/`-kansioon. Stop jättää otot aikajanalle, ja ne tallentuvat `session.youhost`-tiedostoon. Space soittaa tai pysäyttää. Toisto on virtuaalinen soundcheck: jokainen nauhoitettu kanava, myös ilman plugineja, menee samaan USB-lähtöön ja latenssikorjaus pitää lähdöt yhdessä. Dropout-ikkuna aukeaa näppäimellä 4 tai D.

Valmis `YouHost.app` ladataan GitHub Actionsin artefaktina tai version mukana Releasena. macOS Sequoia ei avaa allekirjoittamatonta sovellusta Control-klikkauksella. Avaa YouHost kerran, mene sitten kohtaan **Järjestelmäasetukset → Tietosuoja ja suojaus → Avaa silti** ja vahvista salasanalla. Tai poista karanteeni päätteessä: `xattr -dr com.apple.quarantine YouHost.app`. Mikrofonilupa tarvitaan myös USB-mikserille: **Järjestelmäasetukset → Tietosuoja ja suojaus → Mikrofoni**.

## Download a build

Public builds come from GitHub Actions on a free macOS runner. Each successful run on `main` and on a pull request produces one universal app (Apple silicon and Intel).

1. Open the [Actions tab](https://github.com/PaanisYouHost/YouHost/actions/workflows/build-macos.yml).
2. Choose the newest green **Build YouHost** run.
3. Download the artifact named **YouHost-macOS-universal**.
4. Unzip it. GitHub wraps the artifact, so you may need to unzip twice until you see `YouHost.app`.

When a tag named `v*` is pushed (for example `v0.1.0`), the same zip is attached to the [GitHub Release](https://github.com/PaanisYouHost/YouHost/releases) for that tag. That download is a single zip with `YouHost.app` inside.

The app is ad-hoc signed and does **not** use the Hardened Runtime. That is deliberate: this build loads other developers' VST3 and AU binaries. There is no Apple Developer ID and no notarization, so Gatekeeper will quarantine a downloaded copy.

## Open it on macOS Sequoia

Sequoia removed the old Control-click → Open bypass.

1. Double-click `YouHost.app`. macOS will say it cannot be opened.
2. Open **System Settings → Privacy & Security**.
3. Near the bottom, choose **Open Anyway**, then confirm with an administrator password.
4. The next double-click asks once more. Choose **Open**.

The same thing from Terminal, in the folder that contains the app:

```bash
xattr -dr com.apple.quarantine YouHost.app
open YouHost.app
```

macOS then asks for microphone access. USB interfaces, including the X32, are behind that same switch. If the meters stay dark, check **System Settings → Privacy & Security → Microphone** and turn YouHost on. Ad-hoc signatures change every build, so each downloaded build asks again.

Quit with the window's close button, or YouHost → Quit.

## What this build does

On launch YouHost asks for audio-input permission and opens the last audio device, or a default with every channel up to 128. **Audio setup** shows the device, sample rate, and buffer size. The meter mode is always RMS, even if Peak was selected last time. Peak comes back only when you open a session that was saved that way.

- Choose the input and output device, sample rate, and buffer size. Up to 128 channels are requested, and the device clamps that to what it actually has, so an X32 or DN32-USB should come up as 32 in and 32 out. The choice is saved and restored (on macOS, under `~/Library/Application Support/Ambient Audio/YouHost`). The device panel's Test button is removed: JUCE's built-in tone is about -6 dBFS, which is far too loud on a live desk.
- The main window has two pages and a separate scanner window. **1 Recorder** and **2 Plugins** switch pages. **3 Scanner** opens or closes the scanner. The same keys work when you are not typing in a text field or a plugin window.
- The audio callback copies input *n* to output *n*, meters that signal, then runs that channel's plugins and the alignment delay. While a take is recording, the callback also copies the raw input into a lock-free ring. A disk thread writes the files. The callback does not allocate, take a lock, or log. If the disk falls behind, samples are dropped from the file and the audio keeps going. Loading and removing a plugin happens off the callback, and the new chain is swapped in as one snapshot.
- **Recorder.** As soon as a device is open, YouHost creates `~/Music/YouHost/YYYY-MM-DD HH-MM-SS/` with `session.youhost` and `audio/` if you have not opened a folder yet. **New** saves the current session, then starts another dated folder and keeps the plugins. **Open** loads a different session. **Save** (Cmd+S) writes `session.youhost` immediately. **File** is the same menu as the macOS File menu: Open Recent (the last 10 sessions), Save As, Import recording folder, and Clear timeline. **Save As** copies the whole folder, including `audio/`, and leaves the original where it was. **Import recording folder** copies WAVs into the current session (or opens the folder if it already contains `session.youhost`). Names like `Take01_Ch03_Kick.wav` land on the right take and channel. Every channel starts record-armed. The red dot under a meter arms or disarms it for the next take. Press **Rec** and each armed channel that has an input becomes a mono 24-bit WAV, named like `Take01_Ch03_Kick.wav`. **Stop** ends the take and leaves it on the timeline. The session file is written straight away, with the WAV names. The next **Rec** appends another take on the same timeline, which always starts at 0. Take edges are marked. The timeline is about 2 cm tall and grows with the window. Click or drag it to move the playhead. The big counter is `hh:mm:ss`, red while recording, and it stays visible on the plugin page. **Clear timeline** asks first and only forgets the takes in the session. It does not delete the WAV files.
- **Playback** is a virtual soundcheck. **Play** reads the takes from the playhead and sends every recorded channel to the same-numbered USB output, instead of the live input. A channel with no plugin stays dry. Channels that do have plugins run those plugins. Every included output is then delayed so it lines up with the slowest chain, including the dry ones. The word PLAYBACK shows next to the counter. Channels that were not recorded are silent. Playback has to be at the same sample rate as the recording. Opening a session restores the takes, so Play works as soon as the device is at that rate.
- The recorder page is a dark meter bridge. Channel numbers follow the device (channel 1 is the first device channel). RMS uses about a 300 ms window. On that scale, 0 is line level: the **RMS 0** menu chooses -14, -18, or -20 dBFS (default -20, so the top tick is 0 dBFS and the scale reads +20 down to -40). **Peak** switches the bar to a full-scale sample peak with a 1.5 s hold, and the scale becomes dBFS with 0 at the top (0, -3, -6, -10, -20, -30, -40, -60). Faint lines cross the bridge at each mark. The 0 mark is a brighter, thicker line: line level in RMS, 0 dBFS in Peak. The numbers sit at both ends of the bridge. A sample at or above full scale lights the clip mark and leaves it on. Click that lit mark to clear the channel, or press **Clear clips**.
- **Plugins.** Channels are rows, 1 at the top. Each row has the number, a name (double-click to edit; the name is used in the WAV filename), a small meter, a record arm, an **Ex** button, and four slots. An empty slot opens a searchable list. Choosing a plugin loads it and opens its window. Click the slot again to close the window, and again to reopen it. The label is the slot number plus the plugin name, for example `1 Pro-Q 3`. A loaded slot is green; bypass is dim. Right-click for Open, Bypass, and Remove. **Ex** is **Exclude from alignment**.
- **Scanner.** **Scan** looks for new AU and VST3 plugins. AU plugins come from the AudioComponent registry, which includes `/Library/Audio/Plug-Ins/Components` and the user Library. VST3 plugins come from `/Library/Audio/Plug-Ins/VST3` and `~/Library/Audio/Plug-Ins/VST3`. Four scanner processes work at once. Each one is reused for a few ordinary plugins, then quit. A shell plugin gets its own process, which is killed when that plugin finishes. If a plugin is still going after 20 seconds (45 for a shell), it is killed, marked failed with the reason, and the scan moves on. Stuck scanners are not left running. **Rescan** only reopens plugins whose file time or AU version changed. **Stop** kills the current scanners. **Scan file…** checks one `.vst3` or `.component`. **Rescan selected** does that for the highlighted row. The progress bar shows `done / total`, the current plugin, and the elapsed time. Found and failed counts update while the scan runs. Failed rows include the reason. **Scan Waves shells** is off by default: one WaveShell lists hundreds of plugins and used to stall the scan before later plugins such as De-Feedback were reached. Turn it on when you want those Waves plugins; they are scanned last. Apple's built-in Audio Units, and instruments or plugins with no audio input, stay out of the insert list until the checkboxes in the scanner are on. The scanner window itself still lists everything that was found. The header shows CPU use from the audio device.
- Each plugin reports its latency. YouHost lines the included channels up on the slowest one and adds that delay to the round-trip figure. A bypassed slot adds no latency. Stereo plugins are fed the same mono signal on both inputs, and only the first output channel is kept. The corner line shows compensation delay and the USB round trip. **Latency** opens the full card: buffer size, driver input, driver output, compensation, round trip, and dropouts.
- `session.youhost` stores the device, the meter mode, the page, record arms, channel names, take positions, WAV names, the chains, bypass, exclude, and plugin state. Edits autosave. During a take the session file is rewritten every couple of seconds, and each WAV header is rewritten about once a second, so a crash still leaves playable files. When a file passes 4 GB, JUCE writes it as RF64 in the same `.wav` name. Launch does not reopen the last session, so a cold start stays on RMS and a new dated folder is created when the device opens.
- **Dropouts** counts late callbacks, callbacks that outlast the buffer, and CoreAudio overload notifications. macOS does not report a usable xrun total through JUCE, so this counter replaces that `n/a`. **Reset** clears the count and the graph. The count is kept across a buffer-size change; only the gap caused by reopening the device itself is ignored. **Dropouts** (or **Graph** on the latency card, or the keys 4 and D) opens a timeline of the last 10 minutes, the last hour, or the whole session since reset. Each dropout is a red mark, with a CPU line behind it. The window shows the total, the time since the last dropout, and a green **STABLE** when the chosen range has none. New dropouts are appended to `dropouts.csv` in the session folder, with the timeline position when a take was recording. The audio thread only drops a timestamp into a fixed ring. The file is written on the interface timer.

## Keys

These work from the recorder and plugin pages. They do not fire while a text field or a plugin window has focus. **?** in the window shows the same list.

| Key | Action |
| --- | --- |
| 1 | Recorder page |
| 2 | Plugins page |
| 3 | Open or close the plugin scanner |
| 4 or D | Open or close the dropout timeline |
| Space | Play when stopped. Stop when playing or recording |
| Cmd+S | Save the session |
| Cmd+Shift+S | Save a copy of the session folder |
| Shift+Space, R, or Cmd+Space | Record a new take on the end of the timeline |
| Left / Right | Previous or next take marker |
| Shift+Left / Shift+Right | Move the playhead 5 seconds |

Cmd+Space only reaches YouHost if macOS Spotlight is not using that shortcut. Shift+Space and R do not depend on that.

**Round trip on macOS.** JUCE 9's CoreAudio backend includes the hardware buffer in *both* the input latency and the output latency. Adding those two figures double-counts one buffer. YouHost shows the raw driver numbers, and the large round-trip figure is input + output − one buffer + compensation. Compensation here is the slowest included plugin chain, because that is how much later the desk hears the outputs. On ALSA the JUCE figure already omits one period, so the round trip adds the buffer back. This is still the driver's story plus the plugin delay, not a cable measurement.

## Build from source

Requirements: CMake 3.22 or newer, a C++20 compiler, and Ninja. macOS uses Xcode's command-line tools. Linux also needs the usual JUCE packages (`libasound2-dev`, Freetype, Fontconfig, X11, and GTK 3).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure  # optional; the test binary is built either way
./build/YouHostTests
```

JUCE 9.0.3 is downloaded by CMake from the upstream release tarball. It is not vendored in this repository.

The app binary is under `build/YouHost_artefacts/`. On macOS that is `YouHost.app`. A universal build, matching CI, is:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build --parallel
codesign --force --sign - --timestamp=none build/YouHost_artefacts/Release/YouHost.app
```

`YouHostTests` does not open a sound card. It checks the passthrough, the meter ballistics, the 128-channel layout, dropout decisions, compensation arithmetic, take markers, WAV filenames, and the session folder layout.

## Roadmap

The phases below follow the research notes.

| Phase | What it adds |
| --- | --- |
| P0 | Device picker, passthrough, meters, latency readout, dropout counter. In this tree. |
| P1 | Per-channel record enable, one disk thread, mono 24-bit WAV into the session `audio/` folder, header flush, timeline, and virtual soundcheck. This build. |
| P2 | Out-of-process AU/VST3 scan, four in-process slots per channel, editors, latency compensation, session folder, autosave. This build. A loaded plugin can still crash the app. |
| P3 | Sandbox processes for the slots, shared-memory audio, a short deadline with a dry fallback, restart after a crash. |
| P4 | Startup guard, disk-space and overrun warnings, loopback measurement. |
| P5 | Release packaging. Developer ID and notarization only if that cost is accepted later. |
| P6 | Windows, later. |

Recording taps the raw input on the audio thread and writes it on a disk thread, so a slow disk does not stall the plugins. The plugin sandbox is still later: a loaded plugin can crash the app, and the WAVs are what survive that.

## License

YouHost is released under the GNU Affero General Public License v3.0 only. See [LICENSE](LICENSE).

JUCE is included under the same AGPLv3 option ([JUCE license](https://juce.com/legal/juce-9-licence/) or [AGPLv3](https://www.gnu.org/licenses/agpl-3.0.html)). Anyone who receives a YouHost binary can get the corresponding source from this repository. The VST3 SDK that ships inside JUCE is MIT, and this build links it so VST3 plugins can load.

## Try it on a desk

A useful first pass on an X32, or any multichannel USB interface:

- Confirm macOS shows the microphone prompt, and that all 32 inputs and 32 outputs appear.
- Set 48 kHz and step the buffer through the sizes the interface offers, especially the smallest ones you would actually use (often 64 and 128).
- Feed one channel at a time and check that the matching output, meter, and channel number follow it, and that the other outputs stay quiet.
- Clip one channel on purpose. The red mark stays lit through silence until you click it, or until you press Clear clips.
- Quit and launch again. RMS should be selected, with the bright 0 line across the bridge at line level. Peak should not come back until you open a session that was saved in Peak.
- Switch to Peak and check that the bright line moves to the top (0 dBFS) and the side numbers change. On a wide bridge the same numbers should be readable at both ends.
- Press 3, then Scan. The bar should move, ordinary plugins should appear before any shell, and De-Feedback should show as both AU and VST3 if both are installed. Waves shells stay out until **Scan Waves shells** is on. A failed plugin shows a reason, and the scanner processes should be gone when the scan finishes or after Stop. On page 2, Apple's built-in Audio Units and instruments should be hidden. Click an empty slot, load De-Feedback, and confirm its window opens and the channel still returns on the same-numbered output. Click the slot to close the window, and right-click to bypass. Bypass should drop that slot's latency out of the chain.
- Put a different plugin on one channel only. Open Latency and check that the compensation figure rises by the slowest included chain. Press Ex on one row and confirm that channel stops moving the number.
- With the device open, a folder appears under `~/Music/YouHost/`. Press R or Shift+Space. The counter turns red and a take grows from 0 on the timeline. Stop, then record again. The second take starts where the first ended. In `audio/`, expect one 24-bit WAV per armed input, named with the take and channel.
- Press Space. PLAYBACK shows. Every recorded channel, including channels with no plugin, comes out of the matching USB output. Channels with plugins run those plugins, and the dry channels are delayed so the outputs stay together. Click the middle of a take and press Space again to start there. Left and Right jump between take markers. Shift+Left and Shift+Right move 5 seconds.
- Press Stop. The take stays on the timeline. Cmd+S, quit, and launch again. RMS should be selected. Open the session from File → Open Recent. The takes, chains, bypass, record arms, and device should return, and Play should run the virtual soundcheck without recording again. The WAV files should still be in `audio/` even if you clear the timeline, and even if YouHost was force-quit during the take.
- Press 4. The dropout window should open on STABLE. Reset it, then try the smallest buffer. If the count climbs, the marks should sit on the graph and `dropouts.csv` in the session folder should gain a line. A disk warning means the WAV lost samples; the plugins should still have been running.
