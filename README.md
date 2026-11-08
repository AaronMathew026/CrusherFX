# CrusherFX

A tiny Windows tray app for the Skullcandy Crusher 1080 ANC: headset controls plus FxSound-style
system-wide effects. One 600 KB `.exe`, no installer, no account, no network use.

## Status

Work in progress. The headset protocol has been tested on a real Crusher 1080 ANC using test scripts, but
**the app's own Bluetooth connection has not yet succeeded**: in the last test it failed with error 10064
while the headphones had also stopped accepting connections from those scripts. Offline tests pass, but the
PC effects have not yet been tried with Equalizer APO. Expect rough edges until these are confirmed.

## Run it

You need Windows 10 or 11 and headphones already paired with the PC.

1. Install the compiler (once):

   ```powershell
   winget install BrechtSanders.WinLibs.POSIX.UCRT
   ```

2. Get the code and build it:

   ```powershell
   git clone https://github.com/AaronMathew026/CrusherFX.git
   cd CrusherFX
   powershell -ExecutionPolicy Bypass -File .\build.ps1
   ```

   This runs the offline tests and creates `CrusherFX.exe` in the same folder.

3. Connect the headphones to the PC, then start the app:

   ```powershell
   .\CrusherFX.exe
   ```

## Use it

Left-click the tray icon to open or hide the panel. Right-click it for quick ANC, Crusher and profile
switching.

- **Run in the background:** Settings > "Start with Windows, hidden in the tray". You can also start
  it hidden with `CrusherFX.exe --background`.
- **Run only when you want it:** turn off "Closing the window keeps it in the tray".

Settings are in `%APPDATA%\CrusherFX\settings.ini`.

## What it controls

**On the headphones** (works without anything else installed; settings live on the headset):
Quiet/Aware/Off and their levels, Crusher on/off, intensity (0-31) and feel, Music/Podcast/Movie/Custom
profiles, the 5-band Custom EQ, Immersive Audio, sidetone, battery.

**On the PC** (needs the free [Equalizer APO](https://sourceforge.net/projects/equalizerapo/)):
Crusher 1080 tuning, Clarity, Bass, Surround, Ambience, a 10-band EQ and six presets.

### Setting up PC effects

1. Install Equalizer APO. In its device list, tick the Crusher 1080 under Playback devices.
2. Restart Windows.
3. In CrusherFX, turn on PC effects.
4. If FxSound is installed, quit it and set Windows output to the Crusher, not "FxSound Speakers",
   so effects don't stack.

The first time, CrusherFX adds one `Include` line to Equalizer APO's `config.txt` and saves the original
as `config.txt.crusherfx.bak`. If the file holds only the installer's demo settings, those are commented
out. Anything else in it stays active. Equalizer APO's installer also changes a Windows audio setting
that some DRM-protected playback may refuse to run with.

## How it stays light

CrusherFX never touches audio. Effects are written to a config file and run inside the Windows audio
engine, so that CPU cost shows up in `audiodg.exe`, not here. The app itself only wakes for a click or a
message from the headset, and polls just once every 10 s (a local lookup) while the headset is away.

Measured with the panel closed to the tray, headphones not connected: **2.15 MB RAM, 0 ms CPU over 30 s**.
With the panel open: 15.6 MB working set, 2.6 MB private.

## Differences from FxSound

Equalizer APO has no limiter or harmonic exciter, so:

- **Clarity** is EQ only: it can't synthesize harmonics as FxSound's exciter does.
- **There is no Dynamic Boost.** Headroom is lowered instead, by the amount needed to prevent clipping.
  Adding boost or effects therefore makes the output quieter, so raise the volume to compensate.

## How well the tuning is grounded

The "Crusher 1080 tuning" comes from one SoundGuys B&K 5128 measurement of this model (`tools/fit_tuning.py`).
Treat it as a starting point to audition:

- The Crusher wheel position in that measurement is unknown, so nothing below 200 Hz is corrected.
- The target is an approximate Harman-style curve entered by hand, on a different rig from the measurement.
  Treble correction is therefore halved and every filter is capped at 5 dB.
- The presets (Music, Game, and so on) are hand-picked values, not measured results.

## The Crusher bass

Crusher intensity is a setting on the headset, so CrusherFX sets it directly. The official guide says the
physical wheel and an app-set level can disagree until the wheel is next touched. The PC Bass slider is
separate: extra low frequencies probably also drive the haptic drivers harder, because the Crusher appears
to take its signal from the audio itself (inferred from it working over AUX, not measured).

## Safety

Headset calls pass through an allow-list in `src/proto.h`, so firmware update, pairing and unknown calls
can't be sent. The serial number the headset reports is never stored or displayed.

## Test against the headphones

Build first (see [Run it](#run-it)), then:

```powershell
.\build\hwtest.exe             # live read-only check against the headphones
.\build\hwtest.exe --write     # also changes each setting briefly and restores it
.\build\hwtest.exe --watch     # prints pushes for 30 s: turn the wheel, press the buttons
```

Protocol layout credit: [Earshot](https://github.com/HellzYeahh/earshot) (MIT), which derived it from analysis
of the official app. Not affiliated with Skullcandy, Bose, Bragi or FxSound.
