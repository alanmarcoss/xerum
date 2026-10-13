# xerum-host

A headless plugin host: load a plugin, inspect it, drive it, render audio to a WAV.

It exists because three tickets need the same capability, and none of them can be satisfied by
reading files:

- **ticket 08** — render a preset through the real Serum 2 without a GUI
- **ticket 09** — capture golden reference renders to compare xerum against
- **ticket 07** — give REA a process to launch so the plugin's preset reads can be observed

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target xerum-host -j"$(sysctl -n hw.ncpu)"
```

Binary lands at `build/host/xerum-host_artefacts/Release/xerum-host`.

The harness links `juce_audio_processors_headless` — JUCE 9's GUI-free hosting path — so the build
pulls in no UI module. JUCE is fetched at configure time, pinned to the commit of the 9.0.4 tag.

## Use

```sh
BIN=build/host/xerum-host_artefacts/Release/xerum-host
PLUGIN="/Library/Audio/Plug-Ins/VST3/Serum2.vst3"

# what does this plugin actually expose?
"$BIN" --plugin "$PLUGIN" --list-params
"$BIN" --plugin "$PLUGIN" --list-programs

# render two seconds of MIDI note 60 to a 32-bit float WAV
"$BIN" --plugin "$PLUGIN" --out /tmp/init.wav --seconds 2 --note 60

# set parameters (values are normalised 0..1) and render again
"$BIN" --plugin "$PLUGIN" --set "Main Vol=0.25" --out /tmp/quiet.wav --seconds 1
```

`--set` matches the plugin's own parameter names, case-insensitively, and reports an error for a
name it cannot find rather than silently ignoring it. The render summary always prints peak
amplitude and flags silence explicitly, so a broken render cannot look like a successful one.

## What it has produced so far (Serum 2 2.1.5, 2026-10-13)

| Observation | Value |
|---|---|
| VST3 classes found | `Serum 2` (instrument), `Serum 2 FX` (effect) |
| Parameters exposed | **2623** |
| Programs exposed | 128 — but all named `Prog N`, empty slots rather than Serum's preset bank |
| Init patch render | peak **0.283682** (audible, not silence) |
| Determinism | two identical invocations produced **byte-identical** WAVs |
| `--set "Main Vol=0.25"` / `0.75` | peak 0.230778 / 0.635109 — monotonic, parameter control reaches the plugin |
| `presets.db` after headless use | **not created** — see ticket `18`, which predicted otherwise |

Two findings matter beyond the harness itself. **Parameter names on the plugin's VST3 surface
(`Main Vol`, `Bend Up`) do not match the preset model's names (`kParam*`)** — 2623 automatable
parameters against 778 distinct preset leaf paths means the importer needs a real crosswalk, which is
ticket `14`'s subject. And **determinism is conditional**: the init patch renders byte-identically
across runs, but a *loaded preset* loads its assets asynchronously and races the renderer — four runs
of the same preset gave four different WAVs, with 97.8 % of samples differing pairwise. Pass
`--pre-roll` and `--settle-ms` and it becomes byte-identical again. See **Settling** below.

## Settling — read this before comparing two renders

The reference loads a preset's assets **asynchronously**. A render that starts too soon races the
loader and varies from run to run, so an unsettled comparison measures timing, not sound.

Two flags fix it, and both are needed:

```sh
"$BIN" --plugin "$PLUGIN" --out x.wav --seconds 2 --pre-roll 128 --settle-ms 1500
```

`--pre-roll N` processes N silent blocks before the note; `--settle-ms N` waits N milliseconds after
instantiation. The defaults above are the smallest configuration measured to be reproducible;
multisample-heavy presets need more (a multisample piano needed 3000 ms). Pre-roll of 128, 256 and 512
all give *stable* renders that differ from **each other** — so the value is part of the protocol, not a
free parameter.

Because a capture that is not verified is not evidence, `tools/render-preset.sh --verify` renders twice
and requires the two files to be byte-identical.

## State experiments

| Flag | Purpose |
|---|---|
| `--dump-state <file>` | write the plugin's `getStateInformation` chunk |
| `--load-state <file>` | apply a chunk with `setStateInformation` before anything else |
| `--state-hex` | hexdump the state to stdout as well |
| `--class <name>` | instantiate a specific VST3 class (`Serum 2` vs `Serum 2 FX`) |
| `--pre-roll <blocks>` | silent blocks before the note — see **Settling** |
| `--settle-ms <ms>` | wait after instantiation — see **Settling** |

A state dump taken before any `processBlock` does not contain parameters set through `--set`, because
the VST3 wrapper delivers parameter changes during `processBlock`. Dumping pushes silent blocks first.

## Limits, deliberate

- **It does not apply a `.SerumPreset` file.** Doing so needs the unit ranges and discrete-enum orders
  that only ticket `14` can specify; guessing them here would bake an assumption into the one tool
  every other measurement depends on. Use `--list-params` to discover the real surface instead.
- **No GUI, and no plan for one.** Headless is the point.
- **One note, no MIDI file input.** Enough to capture a reference render; extend when a ticket needs it.
- **The instrument path sends note on/off only.** No expression, no aftertouch, and effects get a
  single-sample impulse rather than real material.
