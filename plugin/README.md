# xerum — the plugin wrapper (ticket 30)

One target, three formats: **AUv2, VST3 and Standalone**, per ticket `11`. The engine
(`src/dsp`), the model (`src/model`) and the content library (`src/content`) are unchanged by it; the
plugin is the adapter, and this file records the decisions that adapter makes.

```sh
cmake -S . -B build-30 -DCMAKE_BUILD_TYPE=Release
cmake --build build-30 --target xerum_All -j8        # AU + VST3 + Standalone
cmake --build build-30 --target xerum-plugin-tests   # this slice's suite (links the processor in-process)
```

Artefacts: `build-30/plugin/xerum_artefacts/Release/{AU/xerum.component,VST3/xerum.vst3,Standalone/xerum.app}`.

## Identity — ours, not the reference's

| | xerum | the reference |
|---|---|---|
| AU type / subtype / manufacturer | `aumu` / **`Xrm2`** / **`Xerm`** | `aumu` / `Xf2X` / `Xfer` |
| bundle id | `com.xerum.xerum` | `com.xfer.serum2` |

The AU registry keys on the triple, so registering the reference's would replace or shadow the real
plugin on any machine with both installed (ticket `11`). `aumu` is the *class* of unit and a synth
must be one; the subtype and manufacturer are identity, and they are ours. Verified in the built
bundle's `Info.plist` and by `auval -a`, which lists `aumu Xrm2 Xerm - xerum: xerum`.

VST3 has no such registry; the bundle id and the class UID JUCE derives from `PLUGIN_CODE`
(`Xrm2`) are the identity.

## The parameter bridge

A host sees **59 parameters**, plus the VST3 `Bypass` parameter JUCE necessarily adds. Not 776 and
not the reference's 2623: the surface is exactly what the engine implements. Exposing the rest would
put controls in a host that write a value nothing reads — the placeholder-fallback smell this project
forbids — and a preset's other parameters are not lost, because they stay in the document and in the
import report.

* **Ids are derived, not written by hand.** `parameterIdFromPath` states the rule — lower case, drop
  `plainParams`, drop the `kParam` prefix, join with `.` — so
  `Oscillator0/WTOsc0/plainParams/kParamWarpMenu` is `oscillator0.wtosc0.warpmenu`. The test suite
  asserts every entry's id equals the rule applied to its path, so the table cannot drift from it.
  This is ticket `13` D6's "stable ids derived from module paths".
* **Labels are xerum's own** (`A Level`, `1 Cutoff`, `Amp Envelope Attack`). They contain no `kParam`,
  no `kOsc`, and none of the reference plugin's VST3 display strings — measured through the harness,
  so a leak would be visible: `--dump-params` prints the whole surface as TSV.
* **Units are the engine's own**, which is what makes a preset's value usable unchanged: `kParamFreq`
  is the normalised 0–1 `VoiceFilter::setCutoff` takes, `kParamReso` is a percent, `kParamPitch` is
  semitones. The plugin converts only where the model's storage type differs from the engine's
  (`kParamEnable` is a float leaf holding 0/1 → a Boolean parameter; the state writer uses the leaf's
  own kind so a written document keeps the type the reference used).
* **Ranges are authored here, not read out of the schema.** The schema's `min`/`max` are
  **corpus-observed** (ticket `21`), and 7 of the 14 established defaults lie outside them (ticket
  `38`), so publishing them as a parameter range would be publishing an observation as a bound. Each
  range is xerum's, with the ceiling taken from the engine where the engine has one (`kMaxUnison`,
  `kMaxVoices`) and from the model's own observed span where it does not.

### Two discrete vocabularies

`kParamWarpMenu` (52 model names) and `kParamType` (83 filter names) are the only string-valued
parameters, and both are exposed as a choice list:

* built by asking the **engine** what it implements — `warpModeFromName` / `filterSpecFromName` over
  the schema's own enum, never a hand-written list;
* **collapsed by spec**: filter names the engine builds identically are one choice, because two
  labels for one behaviour is a second way to say the same thing;
* labelled by `describeFilterSpec` / xerum's warp-family names, so a label **cannot** disagree with
  what the engine will build, and labels are unique (a host that stores a parameter as *text* would
  restore the first match otherwise);
* ending in one sentinel, `not in xerum's set`, so a value the engine does not implement is
  *visible in the interface* rather than silently replaced. A preset's exact token survives: the
  state writer compares each parameter against the value the document installed, and only rewrites
  what the user actually moved.

### Defaults: `defaultFor` first, then xerum's init — never zero

An absent parameter resolves in this order:

1. the loaded **document's** value, if the preset mentions it;
2. **`Schema::defaultFor`**, when the machine has an established value (ticket `38`'s table is
   generated locally and is absent on a machine with no reference installed);
3. **xerum's own init value** for that parameter, authored in the table.

**nullopt is not zero.** Of the 776 leaves, 658 will never have a default (ticket `38`), and the
`kParamVolume` leaf has none on any machine: it resolves to xerum's init of unity, and
`plugin/tests/bridge_test.cpp` asserts that, with a parameter whose init *is* 0 as the positive
control. Because (2) is machine-local, the tests assert the *rule* (established → use it, else →
init) rather than values, so they pass on a fresh clone and on this machine; through the harness the
rule is observable end-to-end — `1 Resonance (%)` reads 10.0 here, which is ticket `38`'s established
default for `VoiceFilter{N}/plainParams/kParamReso`, where the authored init is 0.

## Preset loading, and the report reaching the user

`.SerumPreset` → container framing → decompression → CBOR (or the legacy stream) → `loadPreset` →
`PresetDocument` + `ImportReport` → `applyDocument`. The plugin parses none of that itself; it calls
the model.

The report is surfaced where a user will see it: the editor shows every entry with its severity,
module path, item and reason, plus the counts per severity, and — for a clean import — the words
"Nothing to report", because the contract says a clean import's report is **empty, not absent** and
the two must not look the same. `XerumAudioProcessor::reportText()` is the same text, so tests and
the harness can read what the interface shows.

**The plugin completes the report** with the things only it knows, because the contract's rule is
that nothing is dropped silently and the model cannot see this layer:

| What | Severity | Why the model cannot report it |
|---|---|---|
| an asset that resolved case-insensitively | info | model has no filesystem (contract §5) |
| an asset that did not resolve; a documented default is substituted | high | same |
| an embedded wavetable (`embeddedWTData`), which xerum does not read | high | the engine reads files |
| a slot whose `kParamType` selects an engine v1 does not synthesise — left **silent** | info | the model reports the engine at high; this says what the plugin did about it |
| a warp whose name encodes a source or variant the engine does not route (`kPD_OSC`, `kDistTube`) | info | the family *is* applied; the suffix is not |
| `kParamUnisonStereo` | info | ticket `28` records the spread; the voice bus is mono |
| a `PZ_SVF` whose response is drawn in the preset | high | the engine takes a `FilterSpec` and no pole/zero table |

Only assets belonging to a module the plugin implements are resolved. A corpus preset referencing
twenty multisample files would otherwise bury the one entry that matters under twenty saying the same
thing — and the *engine* entry for `MultiSampleOsc` already names the reason.

## What xerum's init patch is, and what the reference's is

The table's `init` column is xerum's own init patch, and each entry has a reason. The ones that matter:

| Parameter | xerum's init | Why |
|---|---|---|
| `Oscillator0/plainParams/kParamEnable` | **on** | the reference's init state says so — measured through the harness: `OSC A │ A Enable │ 1.000000000 │ On` — and a synth that renders silence until a preset arrives is broken in a way no validator checks |
| `Oscillator1/2/…/kParamEnable` | off | same source: `OSC B │ B Enable │ 0.000000000 │ Off`, `OSC C │ C Enable │ … │ Off` |
| `VoiceFilter{N}/…/kParamEnable` | off | same source: `Filter 1 │ Filter 1 On │ 0.000000000 │ Off` |
| `…/kParamVolume`, filter cutoff, envelope times | the engine's own defaults (`OscillatorSettings::volume` = 1, `VoiceFilter::cutoff` = 1, `AmpEnvelope` = 5 ms/0/1.0/50 ms) | see below |
| `VoiceFilter{N}/…/kParamType` | a 12 dB/oct low pass | xerum's authored choice. The reference's init *displays* `MG Low 12`, but that is a display string: the model stores the default as **absence** (there is no `MgL12` in the schema's enum), so mapping it onto a choice would be a guess. `Schema::defaultStringFor` returns it (ticket `38`) and the plugin deliberately does not use it for a choice. |

**The reference's init, measured, for the record** (`xerum-host --dump-params` against Serum 2):
`A Level = 75% [-5.0 dB]` — which is preset units **0.5625**, since ticket `32` measured the stored value
to be the square of the displayed percentage; `Env 1` = attack 1.0 ms, decay 1.00 s, sustain 0.0 dB,
release 15 ms; `Filter 1` = freq 425 Hz, res 10, drive 0, `MG Low 12`.

Where those values are established in **preset units**, the plugin already uses them — they arrive
through `defaultFor`, and the arrival is observable: through the harness, xerum's own
`1 Resonance (%)` reads **10**, which is ticket `38`'s established default, where the authored init is 0.
Where they are not (the oscillator level, the envelope times, the filter type above), xerum's own init
plays, and the difference is a **fidelity** matter for the fidelity suite — not a correctness one, and
not something to fix by pasting the reference's numbers into this table, which would commit
reference-derived data that tickets `10` and `38` deliberately keep out of the tree.

## State: same mechanism, two codecs — and not a preset

**One mechanism.** The in-memory form is a `PresetDocument` plus its report, whichever way it
arrived, and both paths go through the same `applyDocument`. What differs is the container, and
ticket `32` is why it must:

* a `.SerumPreset` is the reference's format, read-only here;
* a state block is xerum's own: `xerum-state\0`, a format version, then CBOR of the document.

`getStateInformation` therefore contains: the format marker, the schema version, the preset name,
every parameter of the document (including paths the schema does not describe, which ticket `23`
retains on purpose), the asset references, the import report as the user saw it, and which of those
entries came from the model. For each parameter the user actually moved since the document was
applied, the live value is written over the document's; everything untouched keeps the preset's own
token, byte for byte.

**What it therefore cannot round-trip**, stated rather than discovered later:

* everything ticket `32` enumerates as structure — the payload's `"default"` sentinels, module
  presence, array lengths, the sub-engine dicts of engines a slot does not select, per-module UI
  state. A document never held them, so no state block can;
* the **metadata block** — it is not stored, so a state file cannot be turned back into a faithful
  `.SerumPreset`. Export is ticket `32`'s problem and is not attempted here;
* the reference's *exact* token where several names collapse onto one engine spec (`Phase12P` vs
  `P12`): the collapse is sound for a state round-trip — same engine spec, same sound — but a
  token the user explicitly *moves* is rewritten as xerum's canonical name for that spec.

`plugin/tests/state_test.cpp` asserts the round-trip on documents, on parameters and on a rendered
block; the harness asserts it end-to-end (below).

## Real-time discipline

* The audio thread allocates nothing, locks nothing, and never lets an exception out. Measured by
  replacing the global allocation functions with a counter — the technique from
  `tests/dsp/voice_test.cpp`, not a weaker one — across a 200-block render that includes note-on,
  **more notes than the polyphony so a steal happens**, a sustain pedal, a pitch bend, a note-off and
  a parameter written between blocks. **0 allocations**, with a positive control that proves the hook
  works.
* Every reading of model data (reading a preset, decoding state, resolving content) is wrapped, so a
  damaged file is an error string and a report line rather than a throw into someone's DAW.
* **Replacing a wavetable while audio is running** is the one place where the message thread touches
  memory the audio thread reads. It is handled with an explicit handshake: the audio thread brackets
  `processBlock` with an atomic counter, the message thread sets a `frozen` flag and waits for that
  counter to reach zero before it swaps and frees. The audio thread does two atomic increments and a
  load — no lock — and it rebuilds its settings *before* it reads a table, so it can never hold a
  pointer the message thread has freed. If a host never leaves `processBlock`, the wait times out
  after 250 ms and the old tables are **kept rather than freed**: a leak beats a use-after-free.
* Parameters are the live store and are read as atomics; the model's map is only ever touched off the
  audio thread. The engine's settings are rebuilt only when a value actually moved (a per-block
  comparison of 59 doubles), so a steady patch is not re-applied 100 times a second.

## Acceptance, measured

| Criterion | Result |
|---|---|
| `pluginval` | **SUCCESS at strictness level 10** (maximum) on the VST3 *and* the AU, 25 test groups including "Fuzz parameters", exit 0 |
| `auval` | **`auval -strict -v aumu Xrm2 Xerm` → "AU VALIDATION SUCCEEDED"** |
| loads, plays, automates in the ticket `08` harness | loads (`found: xerum \| VST3 \| instrument=yes`); **plays with nothing set: peak 0.787401** on a bare render (the init patch has OSC A on, as the reference's init state does); `--automate "A Level=0.05:1"` ramps mid-render (peak 0.401150, so the ramp is heard), and two identical automation runs are byte-identical |
| state round-trips byte-identically | `--set … --out r1.wav`, `--set … --dump-state s.bin`, `--load-state s.bin --out r2.wav` → `cmp r1.wav r2.wav` **identical** (peak 0.820550 both, 568-byte state block) |
| no allocation on the audio thread | 0 allocations across a 200-block full render, positive control ≥ 1 |

### The tools, exactly

* **`pluginval`** was **not installed** on this machine (`which pluginval` empty, no Homebrew formula,
  nothing on disk). It was fetched for this run: the ticket `11` choice is `2.0.0-beta1`, and that
  release's macOS download is **broken as shipped** — `dyld: Library not loaded: @rpath/librtcheck.dylib`,
  the dylib is not in the bundle and its rpath points at the CI build machine. So **v1.0.4** (the
  latest stable, JUCE 8.0.3) was used, unpacked to `/tmp/pluginval-104/pluginval.app`, with the
  quarantine attribute removed. Command:

  ```sh
  /tmp/pluginval-104/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 \
      --validate build-30/plugin/xerum_artefacts/Release/VST3/xerum.vst3
  ```

  **Strictness is fixed once, at level 10.** Ticket `15` requires starting strict and relaxing only
  with a recorded reason; nothing needed relaxing, so level 10 it is. Re-running after a plugin
  change should use the same level, and any future relaxation must be recorded here with its reason.
* **`auval`** is Apple's, at `/usr/bin/auval`. Validating an AU is a real install, so recording it:
  the built bundle was copied to
  **`~/Library/Audio/Plug-Ins/Components/xerum.component`** (that directory was empty beforehand;
  the copy is the only file in it) and `AudioComponentRegistrar` was restarted so the registry picked
  it up. `auval -a` then lists `aumu Xrm2 Xerm - xerum: xerum`.

  Beyond `-strict`, two of auval's optional checks were tried:
  **`auval -strict -stress 5 -v aumu Xrm2 Xerm` → exit 0, "Stress Test / Render for 5 seconds of audio
  at frame size: 512, sample rate: 44100 Hz — PASS"** (the multi-threaded audio-I/O run, which is the
  tool's own concurrency check). **`auval -real-time-safety` produced no result**: it loops
  "Create DTrace handshake / Checking if DTrace started" because it needs `sudo` and a DTrace
  entitlement, and it was killed at a 20-minute deadline. That one is *unmeasured*, not passed.

## This slice's suite

```sh
build-30/plugin/tests/xerum-plugin-tests              # all of it: 26 cases, 2,990 assertions
build-30/plugin/tests/xerum-plugin-tests "[plugin]"   # the whole slice by tag
build-30/plugin/tests/xerum-plugin-tests "[realtime]" # the allocation invariant alone
ctest --test-dir build-30                             # every suite in the tree
ctest --test-dir build-30 -R "state round-trips"      # one case, selected by name
```

Measured, per tag: `[bridge]` 6 cases / 1,466 assertions, `[state]` 5 / 118, `[preset]` 6 / 1,315
(1,255 of them the corpus walk), `[audio]` 6 / 19, `[realtime]` 2 / 5, `[defaults]` 1 / 67,
`[corpus]` 1 / 1,255. The corpus case walks **all 626 factory presets** through the plugin and asserts
each produces a document and a report; it **skips loudly** (`SKIP`, not a silent pass) on a machine
with no corpus. Note that `ctest -R xerum-plugin-tests` does *not* select these: `catch_discover_tests`
registers each Catch2 *case* under its own name, which is why the tag commands above are the
addressable route.

The cases are: the bridge's own consistency (ids, labels, ranges, choice lists, coverage of every
implemented name); the default-resolution rule; state round-tripping (documents, exact tokens,
parameters, a byte-identical render); a refused foreign state block keeping the patch; a refused future
format version; a state block that is not a preset container; playing (the init patch audible, silence
when the oscillator is switched off, monotone level response with exact attribution, a filter that
measurably attenuates); automation mid-render; mono/legato through a document; bus layouts; preset
loading from a fixture xerum writes itself; **damaged preset files returning an error rather than
throwing**; a missing asset substituted with the documented default; a slot selecting a deferred engine
going silent and saying so; and the real-time invariant.

## Known gaps, named rather than hidden

* **`Global Polyphony` is applied at the next `prepareToPlay` or preset load, not on the knob turn.**
  The active voice count is a `prepare()` argument (ticket `28`) and re-preparing the pool allocates
  (the modulation matrix), so it cannot be done from `processBlock`. A preset's value is applied
  immediately — `applyDocument` runs off the audio thread — and a live turn lands at the next
  prepare. A `VoiceEngine::setPolyphony` in `src/dsp` would close it; that file is not this slice's.
* **Warp source routing, stereo unison and drawn pole/zero responses** are not applied. All three are
  *reported* by the plugin (see the table above), which is the contract's requirement; closing them
  is `src/dsp` work.
* **The amplitude envelope is bound to `Env0`.** `tools/reference-init-defaults.py`'s crosswalk maps
  the reference's "Env 1" group onto `Env{N}` instance 0, and the reference's envelope 1 is its fixed
  amplitude envelope. The crosswalk half is evidence; the "envelope 1 is the amp envelope" half is
  **[INFERENCE]** — it is the reference's convention and this ticket did not measure it. If it is
  wrong, one table entry moves.
* **There is no wavetable browser.** A slot's table is whatever the preset's asset reference names,
  or xerum's own `Tables/xerum/Sine.wav` when it names none. A table *chooser* is a UI feature with a
  dynamic choice list, which hosts dislike; recorded rather than half-built.
* **Pitch bend is ±2 semitones**, the reference's init state's bend range (ticket `38`'s table), and
  `kParamBendRangeUp/Dn` are not exposed. Stated, not measured.
* **The editor is minimal on purpose**: the parameter surface plus the import report. The report is
  what ticket `30` needed the interface for.

## JUCE observations worth keeping

Found by building and driving the plugin through the harness, not by reading the docs:

1. **JUCE's VST3 wrapper synthesises 2048 `MIDI CC c|n` parameters plus channel-expression
   parameters for an instrument** (`juce_audio_plugin_client_VST3.cpp`, on by default). Measured: the
   exposed surface went from 59 to **2140**. It is disabled here
   (`JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0`) because those are neither xerum's identity nor the
   reference's, and MIDI CCs still arrive as MIDI events in `processBlock`, which is where xerum reads
   them.
2. **`AudioParameterInt` does not claim to be discrete.** It overrides `getNumSteps()` but not
   `isDiscrete()`, and `AudioProcessorParameter::isDiscrete()` returns false; the VST3 wrapper asks
   `isDiscrete()` before writing `ParameterInfo::stepCount`, so an integer parameter reaches a host as
   a *continuous* one (`steps=2147483647` rather than 16). Fixed here by a two-line subclass,
   `DiscreteIntParameter`.
3. **A parameter whose minimum is 0 reads back as 1 through the VST3 wrapper/host path**: a 0..255
   parameter showed a host-side normalised value of 1/255 at its default while the plugin's own value
   was 0 (asserted in-process), and every 1-based parameter read back exactly. Not root-caused inside
   JUCE; avoided by keeping `kParamTablePos` — whose model range starts at 1 anyway — 1-based. This
   is the one host-visible number in this slice that is explained by observation rather than by
   mechanism.
4. The standalone app runs (launched for a 5-second smoke test, no output, no crash); a GUI needs a
   person, so that is all this records.
