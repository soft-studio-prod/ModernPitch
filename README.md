# ModernPitch

Low-latency pitch-shifting VST3 for modern metal guitar, by Soft Studio.

ModernPitch has three sections. All of them run on one streaming PSOLA
engine that is tuned for guitar:

- **TUNE**: shifts from −12 to +12 semitones, for drop tunings without
  retuning. **EQ COMP** counters the tonal shift that pitching causes.
- **WHAMMY**: 0 to +24 semitones, smoothed so it can be swept or automated.
- **OCTAVER**: adds a voice below the TUNE + WHAMMY pitch.
  - **MODE**: Octave (the same notes one octave down) or Ring (see below).
  - **MIX**: blends the octave voice with the main signal.
  - **GRIT**: saturation on the octave voice.
  - **LOW CUT**: 12 dB/oct high-pass, from OFF to 400 Hz, applied to the
    octave voice.

Each section shows its current delay (`lag ~x ms`). The delay follows the
note being played; there is no fixed window.

## Installation (Windows)

Download the latest release from the
[Releases page](https://github.com/soft-studio-prod/ModernPitch/releases):

- **Installer** (`ModernPitch-x.y.z-Windows-x64-Setup.exe`): installs the plugin
  to `C:\Program Files\Common Files\VST3` and adds an uninstaller in
  *Apps & features*. The installer is not code-signed, so Windows SmartScreen
  may warn: choose *More info* → *Run anyway*.
- **Zip** (`ModernPitch-x.y.z-Windows-x64-VST3.zip`): copy the
  `ModernPitch.vst3` folder to `C:\Program Files\Common Files\VST3` yourself.

Then rescan plugins in your DAW. Windows x64 only for now.

## How it works

The engine is in `Source/PsolaEngine.h`. It is pitch-synchronous
overlap-add (PSOLA) with **resampled grains**. The whole harmonic series
scales, as if the string itself were retuned, which keeps octave-down
clean.

1. **Pitch detection** (`Source/PsolaPitchDetector.h`). The detector runs
   on a low-passed copy of the input, decimated to about 12 kHz, and uses
   past samples only. It is a McLeod (MPM) detector with a lag-dependent
   window: a candidate period *τ* is tested on the last max(*τ*, 10 ms)
   samples. A new note is therefore recognised after about two of its own
   periods, instead of after a window sized for the lowest string. A new
   estimate arrives every 2.5 ms.
2. **Note tracking.** Pitch marks are placed one period apart by waveform
   correlation. Several rules make this robust on low strings:
   - short ×2/×3/×4 jumps of the estimate (octave errors) are ignored while
     the marks still correlate;
   - short detector dropouts are bridged;
   - a new note is locked on its first voiced estimate.
3. **Synthesis.** The engine renders two voices from one analysis:
   TUNE + WHAMMY, and the OCTAVER.
   - Grains are Hann windows two output periods long, placed one output
     period apart and resampled by the pitch ratio.
   - Each grain reads the input lazily, as late as possible. The delay is
     therefore about `halfWidth × |1 − ratio|` plus a small phase slack.
   - The grain phase is chosen per note to keep that slack near zero.
   - When a new note is locked, its first grain starts at once, and the
     previous note's grains fade out over 2 ms.
   - A grain scheduled late never starts mid-window, so it cannot click.
4. **Fallback.** When no pitch is detected (chords, noise), the engine
   uses a 10 ms granular shifter.
5. **Ring** (`Source/RingModulator.h`). The main signal is multiplied by a
   pure sine at exactly half its pitch. This moves every harmonic k·F to
   (k ± ½)·F: a hollow, square-like tone one octave down, with odd harmonics
   only. The carrier frequency comes from the engine's pitch marks. It is
   delayed by the engine's current lag, so it stays in sync with the shifted
   signal. When no pitch is locked, the last frequency is held.

PSOLA is monophonic by nature. Chords are handled by the granular fallback
and sound less clean than single notes.

Host latency (PDC) is reported as 0. The real delay depends on the note and
the shift. On a 24 s DI riff, the mean delay was 12 ms at −12, 7.5 ms at
−5, and 8–9 ms at +7 and +12.

### Compile-time options

The defaults are the settings validated by listening. These switches are
in `Source/PluginProcessor.h`:

| Switch | Default | Effect when changed |
| --- | --- | --- |
| `useImprovedTracking` | `true` | `false`: YIN detector, two estimates to lock, no octave-error rejection |
| `useNoteRestart` | `true` | `false`: the previous grain train keeps running until its next grain |
| `useTightGrains` | `false` | `true`: grains 1.4 periods long. 1–3 ms less delay, but rougher attacks |

## Build

Requirements:

- CMake 3.22 or later;
- a C++17 compiler (tested with MSVC);
- JUCE 8.

```sh
cmake -B build -S . -DJUCE_PATH=/path/to/JUCE -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target ModernPitch_VST3
```

The plugin's About panel (click `vX.Y.Z · AGPLv3` at the bottom right) links
to the source code at `MODERNPITCH_SOURCE_URL`, which defaults to this
repository. If you distribute builds of a fork, set
`-DMODERNPITCH_SOURCE_URL=<your repository>`.

The VST3 is written to `build/ModernPitch_artefacts/Release/VST3/`. It is
also copied to the system VST3 folder. That copy fails while a DAW has the
plugin loaded.

To build the installer, install [Inno Setup 6](https://jrsoftware.org/isinfo.php)
and run, after a Release build:

```sh
ISCC Installer/ModernPitch.iss /DAppVersion=x.y.z
```

The installer is written to `Installer/Output/`.

Note for localised MSVC with the NMake generator: CMake may miss changes to
header files, because the localised `/showIncludes` output is not
recognised. After editing a header, touch the `.cpp` files or do a clean
build.

## Offline tools

The render tool (`Tools/PsolaRender.cpp`) runs the engine outside a DAW.
Build it with `-DMODERNPITCH_BUILD_TOOLS=ON`:

```sh
PsolaRender synth                                  # synthetic plucks: pitch error, attack delay, lag
PsolaRender render in.wav outDir -12 -5 7 [--legacy] [--no-restart] [--tight] [--events] [--ring]
```

`render` uses the plugin's settings and writes one WAV per shift.
`--events` also writes a CSV with the detector estimates, pitch marks and
grains. `--ring` writes the Ring voice (wet) instead of the shifted
signal.

The measurement scripts are in `Tools/analysis/` (Python;
`pip install -r Tools/analysis/requirements.txt`):

| Script | Measures |
| --- | --- |
| `pitch_quality.py in.wav out.wav <st>` | Frames more than 50 cents off; rough (glitchy) frames |
| `attack_quality.py in.wav out.wav` | Delay of each attack (median / 90th percentile), rise time |
| `click_detect.py out.wav` | Click-like discontinuities. Run it on the input too: its score is the detector's floor |
| `ring_quality.py in.wav ring.wav [--semitones N]` | Ring carrier tracking: energy share on the (k + ½)·F lines, off-tune frames |

## Tuning history

Everything was measured on the same 24 s DI riff, at 44.1 kHz. "Rough"
frames are those where the output is much less periodic than the input.

| Version | Change | Result |
| --- | --- | --- |
| 0.1.x | FFT phase vocoder, SBSMS, granular, WSOLA | Replaced: 20–45 ms of fixed window |
| 0.1.26 | Eight PSOLA variants to compare by ear | Resampled grains ("harmonic") were cleanest |
| 0.2.0 | PSOLA only; per-note phase alignment | Mean delay at −12: 13.3 → 10.9 ms |
| 0.2.4 | Adaptive MPM, octave-error rejection, dropout hold | Rough frames at −12: 19.3 → 13.9 % |
| 0.2.5 | Tight grains off | Rough frames at −12: 13.9 → 10.8 % |
| 0.2.6 | Late grains start from zero | Clicks at −12: 26 → 0 |
| 0.2.7 | Grain restart on a new note | Rough frames at −12: 11.8 → 7.0 %; at −5: 5.5 → 1.5 % |
| 0.3.1 | Ring rebuilt: main signal × sine at F/2, driven by the engine's tracking | Energy on the ring lines: 0.32 → 0.92; off-tune frames: 97 → 1.5 % |

Two ideas were tried and rejected:

- flat-top grains about one period long, which made the output rougher;
- a pick-attack detector, which fired inside notes and doubled the
  roughness.

Attacks of low notes at −12 remain the main weak spot: they arrive 9 ms
late at the median and about 30 ms late for the slowest 10 %.

## License

Copyright (C) 2026 Mickaël Doux (Soft Studio).

ModernPitch is free software: you can redistribute it and/or modify it under
the terms of the GNU Affero General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option) any
later version. It is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; see [LICENSE](LICENSE) for details.

ModernPitch is built with [JUCE](https://juce.com), used here under the
AGPLv3. Anyone who distributes a build of the plugin must also provide its
source code under the same licence.

VST is a registered trademark of Steinberg Media Technologies GmbH.
