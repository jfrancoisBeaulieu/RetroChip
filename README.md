# RetroChip

A MIDI instrument with the sound of six console and cartridge sound chips, as an Audio Unit and VST3 for macOS on Apple Silicon, and as a VST3 for Windows (see [Other hosts and Windows](#other-hosts-and-windows)). Three tabs at the top of the window choose the console: **NES**, **Genesis** and **SNES**. Beside the tab, the NES offers its chip, **2A03**, and two Famicom cartridge chips, **VRC6** and **N163**; the Genesis its two chips, **FM** and **PSG**; the SNES its chip, **S-DSP**, or **SoundFont**, which plays `.sf2` files you provide (see [SoundFonts](#soundfonts)).

| Tab and choice | Chip emulated | Sound |
|---|---|---|
| **NES** > **2A03** | 2A03 | Pulse (4 duty cycles), triangle, noise or DPCM (samples), one per instance; FamiTracker `.fti` instruments |
| **NES** > **VRC6** | Konami's VRC6 (Famicom cartridges) | Pulse (8 duty cycles) or sawtooth; `.fti` instruments |
| **NES** > **N163** | Namco 163 (Famicom cartridges) | 4-bit waveforms, 8 included; `.fti` instruments with their waves |
| **Genesis** > **FM** | YM2612 | 4-operator FM synthesis, 12 timbres, adjustable algorithm and feedback, import of `.tfi` timbres, 8-bit PCM samples and kits |
| **SNES** > **S-DSP** | S-DSP | Playback of BRR-compressed samples, with envelope and echo |
| **Genesis** > **PSG** | SN76489, the Genesis's second chip | Square waves or noise, 16 volume levels |

Each button's tooltip gives the name of the chip it emulates. Preset names mention no console. **SoundFont** plays any `.sf2` file; it sits under the SNES because SoundFonts are mostly used here to bring back SNES game sounds.

## Language

The interface is in English by default. The **EN** and **FR** buttons, at the bottom right under the keyboard, switch it to English or French. The choice applies to every instance and every project, and is kept in `~/Library/Application Support/SleeperMusic/RetroChip/Settings.xml` on macOS, or `%APPDATA%\SleeperMusic\RetroChip\Settings.xml` on Windows. Synth terms (Attack, Pulse, Gain, the panel names) are the same in both languages. This README names the buttons by their English text.

The parameters Logic shows for automation have English names in both languages; RetroChip's own values ("My TFI timbre", "Infinite") follow the chosen language. A message already on screen keeps its language until the next one.

## Browsing sounds with the keyboard

Click in the plugin window, then use the arrow keys to move from one sound to the next; each sound plays right away.

| Console | ↑ and ↓ | ← and → |
|---|---|---|
| **SNES** > **SoundFont** | instruments in the file | files in the folder |
| **Genesis** > **FM** | timbres (your own only if a file is imported) | `.tfi` files in the folder |
| **SNES** > **S-DSP** | instruments | presets |
| **NES** (2A03, VRC6, N163) | the chip's presets | `.fti` files in the folder |
| **Genesis** > **PSG** | presets | presets |

At the end of a list, the arrow does nothing more. Other keys, and arrows with Cmd, Option, Ctrl or Shift, stay with the host: the space bar still starts playback in Logic. The on-screen keyboard never takes the computer keyboard's keys.

## Playing model: one sound per instance

Each plugin instance plays **a single sound**. With all six chips it has 8 voices, used for chords, overlapping notes and releases. When all 8 voices are busy, the oldest is replaced (the **SoundFont** choice has its own voices, described below). On the 2A03, the instance plays a single channel (pulse, triangle, noise or DPCM), like the chip.

A full song therefore needs one instance per sound: a bass, a melody, an accompaniment, percussion. There are 72 presets, 12 per chip, and the menu groups them by chip.

## Using it in Logic Pro

The build installs the plugin in `~/Library/Audio/Plug-Ins`. In Logic Pro:

1. Create a software instrument track.
2. In the Instrument slot, choose **AU Instruments > SleeperMusic > RetroChip**.
3. Choose the console with the selector, then a preset.
4. For a second sound, create a new software instrument track with a new RetroChip instance. One track is one sound: for example two **NES** (pulse) tracks, an **NES** (triangle) track for the bass, an **NES** (noise) track for drums, a **Genesis** > **FM** track for an FM bass and a **SNES** > **S-DSP** track for strings.
5. The console selector, the preset and the settings are saved in the project, and their parameters can be automated.

If the plugin does not show up, open **Logic Pro > Settings > Plug-in Manager** and run its scan again.

## Other hosts and Windows

The plugin comes in two formats: **Audio Unit** (Logic Pro, GarageBand, MainStage, macOS only) and **VST3** (Ableton Live, Reaper, Bitwig, Cubase, Studio One, FL Studio, on macOS and Windows). There is no AAX (Pro Tools) or CLAP version.

- **macOS**: the build installs the VST3 in `~/Library/Audio/Plug-Ins/VST3`. Rescan plugins in the host if RetroChip does not show up.
- **Windows**: the build leaves the VST3 in `build\RetroChip_artefacts\Release\VST3\RetroChip.vst3`, because the plugin folder needs administrator rights. Copy that whole `RetroChip.vst3` folder into `C:\Program Files\Common Files\VST3`, then rescan plugins in the host. The standalone application is in `build\RetroChip_artefacts\Release\Standalone`.

The sound folders are in the user's Music folder on both systems: `Music\RetroChip\SoundFonts`, `Genesis` and `NES`.

The arrow keys only work if the host lets the plugin window have the keyboard. Logic, Live and Reaper do after a click in the window; some hosts always keep the arrows for themselves.

## The NES: DPCM, FamiTracker instruments and cartridge chips

**The DPCM channel (2A03 > DPCM).** This is the NES's fifth channel: it plays 1-bit samples, which games used for drums, voices or basses. The **Import** button in the Envelope panel accepts a WAV: it is converted to the chip's fastest rate (about 33 kHz), and **limited to about 1 s** (4081 bytes, the chip's limit).

- **Root Note** sets the key that plays the sound as is. Other keys take the nearest of the chip's 16 rates, as in games: pitch moves in steps, and no key plays faster than the root note.
- Without **Loop**, the sound plays to its end, even after the key is released. With **Loop**, it starts over while the key is held.
- The channel has no volume: the envelope, velocity, vibrato and glide are greyed out.

**FamiTracker instruments (`.fti`).** In NES games, an "instrument" is a sequence of values the program changes 60 times per second: volume, arpeggio, pitch, duty cycle or waveform. FamiTracker saves them in `.fti` files, which the **Import** button also accepts.

- The instrument goes to its chip (2A03, VRC6 or N163), which is shown, and the **Instrument** box is ticked: it then replaces the envelope controls. Untick the box to go back to the envelope. Each chip keeps its own instrument.
- As in FamiTracker, the note stops when the key is released, unless the volume sequence has a release point: it then plays its ending.
- A 2A03 instrument that holds DPCM samples becomes a **kit** on the DPCM channel: each sample plays on its key, at its rate. The status tooltip shows which key plays what.
- Supported: versions 2.1 to 2.5 for the 2A03, VRC6 and N163 chips. VRC7, FDS and 5B instruments are refused with a message.
- Imported files are copied into the Logic project.

**The instrument folder.** Under the **Import** button, a menu lists the `.fti` files in `~/Music/RetroChip/NES/`, with one section per subfolder, as for SoundFonts. Choosing a file imports it, as if it came from the import window. The folder-shaped button opens this folder in the Finder (and creates it if it does not exist); the circular-arrow button reads the folder again after you add files, without closing the plugin. The folder is also read again every time the NES is shown. The three chips share this folder: each instrument goes to its chip.

**VRC6 and N163.** These chips were in some Japanese Famicom cartridges, which added channels to the console.

- **VRC6** (Konami, for example *Akumajō Densetsu*): a pulse with 8 duty cycles, or a louder sawtooth whose volume changes its shape as on the chip.
- **N163** (Namco): waveforms drawn over 32 steps of 4 bits. **Wave** picks one of the 8 included, or an N163 `.fti` instrument brings its own. **Channels** emulates the number of channels the chip runs: the more there are, the more slowly each one is updated, which gives the chip its typical grit.

## The Genesis: two chips

The Genesis has two sound chips, and its music uses both. For a song, use one instance per sound: for example **Genesis** > **FM** for the bass and melody, **Genesis** > **FM** with **My PCM kit** for drums, and **Genesis** > **PSG** for an arpeggio or a hi-hat.

With **Genesis** > **PSG** (the SN76489 chip):

- **Tone** plays a square wave. The chip does not go below about 109 Hz (the A1 key on the plugin's keyboard): lower, the pitch stays at that limit. High notes are slightly out of tune, as on the console, because the chip divides its clock by a whole number.
- **Noise** plays noise. **White** gives white noise, for percussion. **Periodic** gives a buzz that plays notes and goes lower than Tone, which games use for basses.
- **Noise Rate** picks one of the chip's three rates (**High**, **Mid**, **Low**) or **Note**, which follows the key played. At a fixed rate, the noise has no pitch and Vibrato and Glide are greyed out.
- The chip has no envelope: as in a game's sound driver, volume and pitch change in steps, 60 times per second, with 16 volume levels 2 dB apart. The envelope and vibrato can therefore sound like stairs, and that is intended.

## Importing a WAV (SNES, S-DSP)

With **SNES** > **S-DSP**, the **Import** button loads a WAV file of your choice, which becomes the "My sample" instrument.

- One sample per instance. A new import replaces the previous one.
- A stereo file is mixed to mono. The file is converted to 32 kHz, then compressed to BRR, off the audio thread.
- **Length limit: about 3.6 s** (64 KB of BRR, or 116,496 samples at 32 kHz). A longer file is cut and the interface says so.
- An unreadable, empty or unsupported file leaves the current sample in place, with a message.
- The root note sets which note the file plays as is. The loop option starts over from the beginning of the sample.
- The length is adjusted to the nearest multiple of 16 samples so that loops close without a gap. The pitch shifts by as much, which is inaudible except on a very short file.
- The sample is saved in the plugin's state, and so in the Logic project: the original WAV file is no longer needed when reopening.

## Importing a TFI or a WAV (Genesis, FM)

With **Genesis** > **FM**, the **Import** button accepts a `.tfi` file, one WAV or several WAVs. Each import replaces the previous one of its kind and leaves the other in place. Both are saved in the Logic project: the original files are no longer needed when reopening. An unusable file leaves the current sound in place, with a message.

**The timbre folder.** Under the **Import** button, a menu lists the `.tfi` files in `~/Music/RetroChip/Genesis/`, with one section per subfolder. Choosing a file imports it and selects it as the timbre. The folder-shaped button opens this folder in the Finder (and creates it if it does not exist); the circular-arrow button reads the folder again after you add files, without closing the plugin. The folder is also read again every time **Genesis** > **FM** is shown.

**`.tfi` timbre ("My TFI timbre").** A TFI file (TFM Music Maker) describes a 42-byte FM timbre and holds no recorded sound. The plugin loads its operators, its SSG-EG, its algorithm and its feedback. The timbre then plays like the others, with the same controls.

**WAV sample ("My PCM sample").** The file goes through the chip's DAC, the Genesis's 8-bit PCM channel.

- A stereo file is mixed to mono, then converted to about 13.3 kHz and 8 bits, off the audio thread.
- **Length limit: about 19.7 s** (256 KB). A longer file is cut and the interface says so.
- **Root Note** sets which note the file plays as is.
- **Loop** decides what the key does. Without a loop, the sample plays to its end, even after the key is released, which suits percussion. With the loop, it starts over while the key is held, then fades out in 20 ms to 2 s depending on the **Release** knob.
- The 8 voices are summed then clipped to 8 bits for the single DAC, as a console sound driver would: a chord of loud notes distorts.
- Feedback, Bright, Attack and Decay do not act on a sample and are greyed out. Vibrato, Glide, Velocity and Gain stay active.

**Several WAVs ("My PCM kit").** For drums, as in Genesis games, select several files at once in the import window: each goes on its own key and a single instance plays the whole kit.

- Files are placed in name order, starting from the lowest key of the keyboard shown (C1), one key per file. `1 kick.wav`, `2 snare.wav` and `3 hat.wav` therefore go on C1, C#1 and D1. The status tooltip shows which key plays what.
- Each sound plays at its original pitch and to its end, even after the key is released. Keys without a sound do nothing.
- Only **Velocity** and **Gain** act on a kit. The other controls are greyed out.
- **Limits:** 61 sounds, and 256 KB in all (about 19.7 s). Anything beyond is left out or cut, with a message. An unreadable file is skipped without leaving a gap.
- A new import replaces the whole kit. If the kit is already the chosen timbre, a single WAV gives a one-sound kit.

## SoundFonts

The **SoundFont** choice in the SNES tab plays an `.sf2` (SoundFont 2) file you downloaded yourself. RetroChip ships no SoundFont file and downloads none: what you download and use is your choice. SoundFonts taken from games are offered, for example, by William Kage: <https://www.williamkage.com/snes_soundfonts/>.

**Folder.** Put your files in `~/Music/RetroChip/SoundFonts/`. The interface's **Open folder** button creates it if it does not exist. Each direct subfolder is a section of the menu, whose name is shown as you wrote it. Files at the top level go under **Other**. For example:

```text
~/Music/RetroChip/SoundFonts/
├── SNES/
│   └── My game.sf2
├── My sounds/
│   └── Strings.sf2
└── My sound.sf2          (shows under "Other")
```

**Unzip first.** That site's SoundFonts download as `.zip` files. RetroChip does not read ZIPs: unpack the archive (double-click it in the Finder), then put the resulting `.sf2` file in a subfolder, like `SNES/My game.sf2` above.

**In Logic Pro.**

1. Create a software instrument track with **AU Instruments > SleeperMusic > RetroChip**.
2. Choose the **SNES** tab, then **SoundFont** beside it.
3. If you just added a file, click **Refresh** (the list is also read again when the interface opens).
4. Choose the file in the first menu, then the instrument in the second.
5. Play: each note uses the zones, loops and envelopes of the chosen instrument. Echo, gain and velocity are set in the interface.

**Limits.**

- **32 voices** at most per instance. Beyond that, a new note is ignored. A stereo pair (one zone left, one right) takes 2 voices: count about 16 notes for a stereo instrument.
- **Mono output**, like the other consoles. Stereo pairs are mixed to mono and panning is ignored; so that a pair does not sound twice as loud as a mono zone, each zone panned to one side is attenuated (6 dB when fully to one side).
- **Ignored in SF2 files:** modulators (so velocity to filter and mod-wheel vibrato) and reverb and chorus sends.
- **Not supported:** ZIP, SFZ and SF3 files. Only `.sf2` files are read.
- A file over 1 GB is refused.

**The project keeps a reference, not a copy.** The Logic project saves the file's path (relative to the SoundFonts folder) and the instrument number, but not the samples. As long as the file stays in place in the folder, the project reopens the same sound. If you move, rename or delete it, the instrument stays silent and a message gives the name of the missing file. So keep the files in place, or put them back before opening an old project. If you put the file back while the project is open, click **Refresh**: it is read again and the saved instrument comes back.

**Before selling anything.** William Kage's site advises consulting a lawyer before selling anything that uses these samples. RetroChip distributes no SoundFont; the rights to the files you use are your own concern.

## Building

You need Apple's Command Line Tools and CMake. JUCE, ymfm and TinySoundFont must be cloned into `external/`.

```bash
brew install cmake
```

JUCE 9.0.5:

```bash
git clone --depth 1 --branch 9.0.5 https://github.com/juce-framework/JUCE.git external/JUCE
```

ymfm, pinned to commit `81aec25ccbb98f4873a255f7551ac4dadac59b4a` (the YM2612 engine):

```bash
git clone https://github.com/aaronsgiles/ymfm.git external/ymfm
git -C external/ymfm checkout 81aec25ccbb98f4873a255f7551ac4dadac59b4a
```

TinySoundFont, pinned to commit `853a0a171759f1ddba0de1442133a75912bbeffa` (SoundFont playback):

```bash
git clone https://github.com/schellingb/TinySoundFont.git external/TinySoundFont
git -C external/TinySoundFont checkout 853a0a171759f1ddba0de1442133a75912bbeffa
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build -j8
```

**On Windows**, you need Visual Studio 2022 (with "Desktop development with C++"), CMake and Git. The clone commands are the same; then, in a command prompt:

```bash
cmake -S . -B build
```

```bash
cmake --build build --config Release
```

This build produces the Audio Unit, the VST3 and the standalone application, and copies the AU and VST3 into `~/Library/Audio/Plug-Ins`. The standalone application is in `build/RetroChip_artefacts/Release/Standalone/RetroChip.app`.

## Testing

```bash
ctest --test-dir build -C Release --output-on-failure
```

```bash
auval -v aumu Rc2a Slpr
```

There are three suites: the engine's, the plugin's, and `host_tests`, which loads the built VST3 and AU as a host does. It checks loading, sound at 44.1, 48 and 96 kHz with various block sizes, the presets, saving and reopening a project, the interface, and two instances at once.

The `.github/workflows/build.yml` file builds and tests the plugin on macOS and Windows at every push to GitHub. Each build's Windows VST3 can be downloaded from the build's page for 14 days.

The plugin tests write screenshots of the interface to `build/editor_*.png`.

If `auval` does not see the latest version after a build, run `killall -9 AudioComponentRegistrar` first.

## Structure

- `engine/`: sound engines in plain C++, without JUCE.
  - `engine/` (top level): NES engine (2A03 with DPCM, VRC6, N163), the shared `ConsoleEngine` interface and the resampler.
  - `engine/nes/`: FTI file reader, instrument sequences, DPCM encoder and N163 waveforms.
  - `engine/genesis/`: YM2612 engine (two ymfm chips, 12 timbres, PCM and kit playback through the DAC), TFI file reader and SN76489 engine.
  - `engine/snes/`: S-DSP engine (voices, envelope, echo, BRR, bank of 12 instruments).
  - `engine/soundfont/`: SoundFont file playback with TinySoundFont.
- `plugin/`: processor, parameters, presets, WAV, TFI and FTI import, SoundFont, timbre and instrument folders, French translation (`Localisation.cpp`) and the JUCE interface.
- `tests/`: engine, plugin and host-loading tests (`host_tests.cpp`), with the 2A03 references in `tests/reference/`.
- `.github/workflows/`: build and tests on macOS and Windows.
- `external/`: JUCE, ymfm and TinySoundFont, cloned by hand and ignored by the repository.
- `LICENSE`: the AGPLv3 licence text.
- `THIRD_PARTY_LICENSES.md`: licences of JUCE, ymfm, TinySoundFont and the components bundled with JUCE.

## Licence

RetroChip is free software, © 2026 SleeperMusic, distributed under the [GNU AGPLv3](LICENSE) licence, **without any warranty**. Anyone who distributes the plugin, modified or not, must also provide its source code under this licence.

Any distribution must include [`LICENSE`](LICENSE) and [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).

- **JUCE:** used under AGPLv3. It is also available under a commercial licence, which this project does not use.
- **ymfm:** Yamaha FM chip emulation by Aaron Giles, under the BSD 3-clause licence.
- **TinySoundFont:** SoundFont player by Bernhard Schelling, under the MIT licence.
- **SoundFonts:** none ship with the plugin. Their rights remain with their authors.
- **Chip tables:** documented hardware values, without third-party emulator code.

## Trademarks

RetroChip is not affiliated with or endorsed by Nintendo, Sega, Yamaha, Sony, Steinberg or Apple. Their trademarks are used here only to describe the hardware and formats concerned. VST is a trademark of Steinberg Media Technologies GmbH.

## Distribution

Builds from this repository only have a local (ad hoc) signature and only run on Apple Silicon. A ready-to-install release needs an Apple Developer account, signing, notarisation and a universal binary for Intel Macs.

The `Slpr` and `Rc2a` codes identify the plugin in projects: they must stay stable once the plugin is released.
