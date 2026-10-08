# YouHost

YouHost is a free, open-source live plugin host for macOS. It is aimed at front-of-house work: pick an audio interface (from a stereo pair up to 128 channels, such as a Behringer X32 USB 32×32), send each input back out of the same-numbered output, and later insert plugins and record the raw inputs in the background.

This repository is **phase P0**, the smallest build that can be tried on a real desk. It opens an audio device, passes input *n* straight to output *n*, draws one meter per channel, and shows the driver latency. Plugin slots, multitrack recording, out-of-process sandboxes, and measured latency compensation are not in this build. The code is arranged so those pieces can land in the engine without turning the audio callback into a second product.

YouHost is [AGPLv3](LICENSE). It is built with [JUCE 9.0.3](https://github.com/juce-framework/JUCE/releases/tag/9.0.3) under JUCE's AGPLv3 option, so the app costs nothing to build or run.

## Suomeksi

YouHost on ilmainen, avoimen lähdekoodin (AGPLv3) Macille tehty kevyt plugin-hosti livekäyttöön. P0-versio tekee läpivedon: sisääntulo *n* menee ulostuloon *n*. Ikkunassa on yksi mittari kanavaa kohti (oletuksena RMS noin 300 ms, vaihdettavissa näytehuippuun) ja iso latenssinäyttö: puskurin koko, ajurin tulo- ja lähtölatenssi, kompensointiviive (vielä 0) ja arvioitu kiertoviive millisekunteina ja näytteinä. Nauhoitus, pluginit ja erillisprosessit tulevat myöhemmin.

Valmis `YouHost.app` ladataan GitHub Actionsin artefaktina tai version mukana Releasena. macOS Sequoia ei avaa allekirjoittamatonta sovellusta Control-klikkauksella. Avaa YouHost kerran, mene sitten kohtaan **Järjestelmäasetukset → Tietosuoja ja suojaus → Avaa silti** ja vahvista salasanalla. Tai poista karanteeni päätteessä: `xattr -dr com.apple.quarantine YouHost.app`. Mikrofonilupa tarvitaan myös USB-mikserille: **Järjestelmäasetukset → Tietosuoja ja suojaus → Mikrofoni**.

## Download a build

Public builds come from GitHub Actions on a free macOS runner. Each successful run on `main` and on a pull request produces one universal app (Apple silicon and Intel).

1. Open the [Actions tab](https://github.com/PaanisYouHost/YouHost/actions/workflows/build-macos.yml).
2. Choose the newest green **Build YouHost** run.
3. Download the artifact named **YouHost-macOS-universal**.
4. Unzip it. GitHub wraps the artifact, so you may need to unzip twice until you see `YouHost.app`.

When a tag named `v*` is pushed (for example `v0.1.0`), the same zip is attached to the [GitHub Release](https://github.com/PaanisYouHost/YouHost/releases) for that tag. That download is a single zip with `YouHost.app` inside.

The app is ad-hoc signed and does **not** use the Hardened Runtime. That is deliberate: a later plugin build has to load other developers' VST3 and AU binaries. There is no Apple Developer ID and no notarization, so Gatekeeper will quarantine a downloaded copy.

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

## What P0 does

On launch YouHost asks for audio-input permission, then opens the audio device selector.

- Choose the input and output device, sample rate, and buffer size. Up to 128 channels are requested, and the device clamps that to what it actually has, so an X32 should come up as 32 in and 32 out. The choice is saved and restored (on macOS, under `~/Library/Application Support/Ambient Audio/YouHost`).
- The audio callback copies input *n* to output *n*. It does not allocate, take a lock, or log.
- The overview is a dark meter bridge. Channel numbers follow the device (channel 1 is the first device channel). The default ballistics are RMS with about a 300 ms window. **Peak** switches the bar to a full-scale sample peak with a 1.5 s hold. A sample at or above full scale lights the clip mark for about 2 s. Click a lit mark to clear it.
- The latency card shows buffer size, the driver-reported input latency, the driver-reported output latency, compensation (always 0 in this build), and the round trip, each in samples and milliseconds. Xruns are shown when the driver reports them (`n/a` when it does not).

**Round trip on macOS.** JUCE 9's CoreAudio backend includes the hardware buffer in *both* the input latency and the output latency. Adding those two figures double-counts one buffer. YouHost shows the raw driver numbers, and the large round-trip figure is input + output − one buffer + compensation. On ALSA the JUCE figure already omits one period, so the round trip adds the buffer back. This is still the driver's story, not a cable measurement. A loopback check is planned for a later phase. Treat the number as something to compare against a loop on the desk, not as a finished alignment delay.

Compensation delay is a placeholder. Nothing is delayed yet, and no channel is excluded from alignment, because there is nothing to align.

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

`YouHostTests` does not open a sound card. It checks the passthrough, the meter ballistics, the 128-channel layout, and the latency arithmetic.

## Roadmap

The phases below follow the research notes. Only P0 is in this tree.

| Phase | What it adds |
| --- | --- |
| P0 | Device picker, passthrough, meters, latency readout. This build. |
| P1 | Session folder, per-channel record enable, one disk thread, mono 24-bit WAV, header flush, crash journal. |
| P2 | Out-of-process plugin scan, four in-process slots per channel, editors, session recall. Not crash-safe yet. |
| P3 | Sandbox processes for the slots, shared-memory audio, a short deadline with a dry fallback, restart after a crash. |
| P4 | Startup guard, autosave, compensation delay with a per-channel exclude, disk-space and overrun warnings, loopback measurement. |
| P5 | Release packaging. Developer ID and notarization only if that cost is accepted later. |
| P6 | Windows, later. |

Session files, when they exist, will live in a folder the user picks: the session file next to an `audio/` directory. Recording stays on the raw input, in the process that does not load plugins.

## License

YouHost is released under the GNU Affero General Public License v3.0 only. See [LICENSE](LICENSE).

JUCE is included under the same AGPLv3 option ([JUCE license](https://juce.com/legal/juce-9-licence/) or [AGPLv3](https://www.gnu.org/licenses/agpl-3.0.html)). Anyone who receives a YouHost binary can get the corresponding source from this repository. The VST3 SDK that ships inside JUCE is MIT; this prototype does not host plugins yet, so that code is not linked.

## Try it on a desk

A useful first pass on an X32, or any multichannel USB interface:

- Confirm macOS shows the microphone prompt, and that all 32 inputs and 32 outputs appear.
- Set 48 kHz and step the buffer through the sizes the interface offers, especially the smallest ones you would actually use (often 64 and 128).
- Feed one channel at a time and check that the matching output, meter, and channel number follow it, and that the other outputs stay quiet.
- Clip one channel on purpose and clear the red mark.
- Read the round-trip figure, then compare it with a short cable from one output back to one input if you have a way to measure that. Write down both numbers, the buffer, and the macOS version.
- Watch the xrun count while the desk is running. If it climbs, try the next larger buffer.
