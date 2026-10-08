# YouHost

YouHost is a free, open-source live plugin host for macOS. It is aimed at front-of-house work: pick an audio interface (from a stereo pair up to 128 channels, such as a Behringer X32 USB 32×32 or a Klark Teknik DN32-USB), send each input back out of the same-numbered output, and insert up to four plugins on each channel.

This build opens the device, passes input *n* to output *n*, draws the meter bridge, and hosts AU and VST3 plugins in-process. Scanning runs in a child process so a plugin that crashes while being identified does not take the mixer down. Playback is not sandboxed yet: a plugin can still crash the app while it is loaded. Multitrack recording is not in this build. The slot graph is the piece a later sandbox can replace.

YouHost is [AGPLv3](LICENSE). It is built with [JUCE 9.0.3](https://github.com/juce-framework/JUCE/releases/tag/9.0.3) under JUCE's AGPLv3 option, so the app costs nothing to build or run.

## Suomeksi

YouHost on ilmainen, avoimen lähdekoodin (AGPLv3) Macille tehty plugin-hosti livekäyttöön. Sisääntulo *n* menee ulostuloon *n*, ja jokaisella kanavalla on neljä plugin-paikkaa sarjassa (AU ja VST3). Skannaus tapahtuu erillisessä prosessissa. Mittari käynnistyy aina RMS-tilaan: 0 on linjataso, ja 0-viiva kulkee koko sillan poikki. Peak näyttää 0 dBFS:n ylhäällä. Klip jää päälle, kunnes sen klikkaa pois. Latenssinäytössä on puskuri, ajurin tulo ja lähtö, pluginien kompensointiviive ja dropout-laskuri. Hitaampi kanava viivästää muita, jotta ulostulot osuvat yhteen. Istunto tallentuu kansioon (`session.youhost` ja `audio/`). Nauhoitus ja kaatumisen kestävä sandbox tulevat myöhemmin.

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

On launch YouHost asks for audio-input permission, then opens the audio device selector. The meter mode is always RMS, even if Peak was selected last time. Peak comes back only when you open a session that was saved that way.

- Choose the input and output device, sample rate, and buffer size. Up to 128 channels are requested, and the device clamps that to what it actually has, so an X32 or DN32-USB should come up as 32 in and 32 out. The choice is saved and restored (on macOS, under `~/Library/Application Support/Ambient Audio/YouHost`). The device panel's Test button is removed: JUCE's built-in tone is about -6 dBFS, which is far too loud on a live desk.
- The audio callback copies input *n* to output *n*, meters the raw input, then runs that channel's plugins and the alignment delay. The callback does not allocate, take a lock, or log. Loading and removing a plugin happens off the callback, and the new chain is swapped in as one snapshot.
- The overview is a dark meter bridge. Channel numbers follow the device (channel 1 is the first device channel). RMS uses about a 300 ms window. On that scale, 0 is line level: the **RMS 0** menu chooses -14, -18, or -20 dBFS (default -20, so the top tick is 0 dBFS and the scale reads +20 down to -40). **Peak** switches the bar to a full-scale sample peak with a 1.5 s hold, and the scale becomes dBFS with 0 at the top (0, -3, -6, -10, -20, -30, -40, -60). Faint lines cross the bridge at each mark. The 0 mark is a brighter, thicker line: line level in RMS, 0 dBFS in Peak. The numbers sit at both ends of the bridge. A sample at or above full scale lights the clip mark and leaves it on. Click that lit mark to clear the channel, or press **Clear clips**.
- Under each meter are four plugin slots. Click one to select it. The panel on the right loads a plugin from the scanned list, bypasses it, opens the editor, or removes it. **Scan** looks for new AU and VST3 plugins. **Rescan** checks the ones already known. **Clear failed** retries plugins that crashed the scanner. The known list and the failed list are saved. The header shows CPU use from the audio device.
- Each plugin reports its latency. YouHost lines the included channels up on the slowest one and adds that delay to the round-trip figure. The panel shows the chain length and the extra delay for the selected channel. **Exclude from alignment** leaves that channel undelayed and keeps it from pushing the others later. A bypassed slot adds no latency. Stereo plugins are fed the same mono signal on both inputs, and only the first output channel is kept.
- **Save** and **Open** use a session folder. Inside it, `session.youhost` stores the device, the meter mode, the chains, bypass, exclude, and plugin state. An empty `audio/` folder is created beside it for the recorder later. After the first save, further edits autosave into that folder. Launch does not reopen the last session, so a cold start stays on RMS.
- The latency card shows buffer size, the driver-reported input latency, the driver-reported output latency, compensation, and the round trip, each in samples and milliseconds. **Dropouts** counts late callbacks, callbacks that outlast the buffer, and CoreAudio overload notifications. macOS does not report a usable xrun total through JUCE, so this counter replaces that `n/a`. **Reset** clears it. The count is kept across a buffer-size change; only the gap caused by reopening the device itself is ignored.

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

`YouHostTests` does not open a sound card. It checks the passthrough, the meter ballistics, the 128-channel layout, dropout decisions, compensation arithmetic, and the session folder layout.

## Roadmap

The phases below follow the research notes.

| Phase | What it adds |
| --- | --- |
| P0 | Device picker, passthrough, meters, latency readout, dropout counter. In this tree. |
| P1 | Per-channel record enable, one disk thread, mono 24-bit WAV into the session `audio/` folder, header flush, crash journal. Not in this build. |
| P2 | Out-of-process AU/VST3 scan, four in-process slots per channel, editors, latency compensation, session folder, autosave. This build. A loaded plugin can still crash the app. |
| P3 | Sandbox processes for the slots, shared-memory audio, a short deadline with a dry fallback, restart after a crash. |
| P4 | Startup guard, disk-space and overrun warnings, loopback measurement. |
| P5 | Release packaging. Developer ID and notarization only if that cost is accepted later. |
| P6 | Windows, later. |

Recording, when it arrives, stays on the raw input in the process that does not load plugins. The session folder is already the place those files will go.

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
- Press Scan. AU and VST3 plugins should appear, grouped by manufacturer. Load a de-feedback plugin on the vocal channels, open its editor, and confirm the channel still returns on the same-numbered output. Bypass should drop that slot's latency out of the chain.
- Put a different plugin on one channel only. The compensation figure should rise by the slowest included chain, and the other channels should show the extra delay that lines them up. Exclude one channel and confirm it stops moving that number.
- Save a session folder, quit, launch (RMS again), then Open the session. The chains, bypass, meter mode, and device should return. The folder should contain `session.youhost` and an empty `audio/` directory.
- Watch the dropout count while the desk is running. Reset it, then try the smallest buffer. If it climbs, try the next larger buffer.
