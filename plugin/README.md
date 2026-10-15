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

A host sees **587 parameters**, plus the VST3 `Bypass` parameter JUCE necessarily adds. That is the 75
of the interface surface — the parameters the engine implements — and, since ticket `45`, the **512**
of the modulation surface: the eight editable fields of each of the matrix's 64 rows. Not 776 and not
the reference's 2623: a parameter with no engine behind it would put a control in a host that writes a
value nothing reads — the placeholder-fallback smell this project forbids — and a preset's other
parameters are not lost, because they stay in the document and in the import report.

The two surfaces are separate lists on purpose. `ParameterBridge::specs()` is the interface surface
(86), and `InterfacePlan` turns it into panels placed in the design's composition — one panel per group,
one section per row of `docs/design/xerum-direction-warm.html`; `ParameterBridge::modulationSpecs()` is
the matrix surface, which `InterfacePlan` deliberately never sees — a 512-row section is not a matrix.
Everything that feeds the engine or the document iterates both.

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

### Four discrete vocabularies

`kParamWarpMenu` (52 model names), `kParamType` (83 filter names), and ticket `40`'s two menus —
`kParamNoiseType` (four colours) and `kParamShape` on the SUB (six waveforms) — are the string-valued
parameters, and each is exposed as a choice list:

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

Two of those sentinels carry a meaning of their own, and the README states them because the *default*
depends on it:

* the **NOISE colour** menu's sentinel is the reference's *sample* mode — `kParamNoiseType`'s absence,
  which is 531 of the 626 factory presets. It is the choice's default, because handing a preset that
  named a file a generated colour would be the substitution the import contract forbids; the slot
  renders silence and the report names the file, at `high`.
* the **SUB waveform** menu's six members are the schema's five stored names plus `Sine`, which the
  schema never carries because the writer omits a value equal to the default and `Sine` *is* the
  default (`xerum-host --dump-params`: `Sub Shape … 6 steps`, display `Sine`).

### Defaults: `defaultFor` first, then xerum's init — never zero

An absent parameter resolves in this order:

1. the loaded **document's** value, if the preset mentions it;
2. **`Schema::defaultFor`**, when the machine has an established value (ticket `38`'s table is
   generated locally and is absent on a machine with no reference installed);
3. **xerum's own init value** for that parameter, authored in the table.

**nullopt is not zero.** Of the 776 leaves, 658 will never have a default (ticket `38`). `kParamVolume`
has none on any machine either — its entry in ticket `38`'s table is normalised-only, which is not a
preset-unit value — so it resolves to xerum's init, and since ticket `63` that init is the reference's
own default patch's level rather than unity (see *What xerum's init patch is* below).
`plugin/tests/bridge_test.cpp` asserts the value, with a parameter whose init *is* 0 as the positive
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
| an embedded wavetable (`embeddedWTData`) that is not a whole number of 2048-sample cycles | high | the engine reads cycles |
| a slot whose `kParamType` selects an engine v1 does not synthesise — left **silent** | info | the model reports the engine at high; this says what the plugin did about it |
| a warp whose name encodes a source or variant the engine does not route (`kPD_OSC`, `kDistTube`) | info | the family *is* applied; the suffix is not |
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
| `…/kParamVolume` (all three wavetable slots) and `Global0/…/kParamMasterVolume` | **the reference's own default patch**: `0.5625` (`A Level` 75%) and `0.3531344` (`Main Vol` 50%) | ticket `63`. The reference attenuates its default patch **14.0 dB through these two of its own controls** while xerum carried neither leaf, so one key reached 0 dBFS where the reference's reached −10.81. See *the two level leaves* below |
| filter cutoff, envelope times | the engine's own defaults (`VoiceFilter::cutoff` = 1, `AmpEnvelope` = 5 ms/0/1.0/50 ms) | see below |
| `VoiceFilter{N}/…/kParamType` | a 12 dB/oct low pass | xerum's authored choice. The reference's init *displays* `MG Low 12`, but that is a display string: the model stores the default as **absence** (there is no `MgL12` in the schema's enum), so mapping it onto a choice would be a guess. `Schema::defaultStringFor` returns it (ticket `38`) and the plugin deliberately does not use it for a choice. |

**The reference's init, measured, for the record** (`xerum-host --dump-params` against Serum 2):
`Main Vol = 50% [-9.0 dB]` and `A Level = 75% [-5.0 dB]`; `Env 1` = attack 1.0 ms, decay 1.00 s,
sustain 0.0 dB, release 15 ms; `Filter 1` = freq 425 Hz, res 10, drive 0, `MG Low 12`.

### The two level leaves, carried since ticket `63`

Both leaves are a **gain** and the engine applies each as one, so the values are preset units. Ticket
`32` measured the taper directly on `kParamVolume` — writing a stored `0.25` makes the reference report
`50% [-12.0 dB]`, so the knob is the **square root** of the stored value and the display is `20·log10`
of it. That gives both defaults:

| leaf | reference's own default | in preset units |
|---|---|---|
| `Oscillator{0,1,2}/plainParams/kParamVolume` | `A Level` 75% [-5.0 dB] | `0.75² = 0.5625` |
| `Global0/plainParams/kParamMasterVolume` | `Main Vol` 50% [-9.0 dB] | `0.5² · 10^(3/20) = 0.3531344` |

`Main Vol`'s taper is the same square under a **+3 dB top** — swept against the reference, its display
reads `+3.0 dB` at the extreme and `-9.0 dB` at half, so `0.5² · 10^(3/20)` is what reproduces `20·log10
= -9.04 dB`, i.e. the reference's own `-9.0 dB` to the one decimal it prints. The two together are
`-14.04 dB`: **the reference's own 14.0 dB of default attenuation, at the reference's own values.**

* **Why carry the leaves rather than attenuate the output by the measured 10.8 dB difference.** The
  difference is a *result*; the leaves are the *cause*. Carrying them means an un-presetted instance, and
  every preset that omits one of them, resolves to the reference's own default through the same rule the
  plugin already uses for every other leaf — so the two instruments agree for the same reason rather than
  by coincidence. `[gain-staging]` asserts exactly that: the init peak is the unity peak times the two
  leaves, and with the leaves forced back to 1.0 the same patch reproduces ticket `58`'s 1.000000.
* **What they do not close, and it is reported rather than absorbed.** The two *default patches* still
  differ by **3.23 dB** in audio (xerum 0.198638 / −14.04 dBFS, the reference 0.288216 / −10.81): the
  reference's raw voice is ~3.2 dB hotter than xerum's at unity control positions. No level leaf can
  close that, it is the same disagreement as ticket `61`'s 8.6 dB on a shared preset, and it belongs to
  ticket `62` — it is recorded, with this evidence, rather than tuned away. The same fixture family shows
  it in the other direction: `808-diesel` omits an oscillator's level, so ticket `63` *moved* that row's
  fidelity numbers (recorded in `plugin/tests/fidelity_test.cpp`'s gate table, with the reason).
* **On the content policy.** These are two **facts** — a default level the reference's own parameter
  surface states — not artefacts, which is the line ticket `10` draws ("facts survive; artefacts do not").
  Everything ticket `38` captures in bulk stays machine-local and uncommitted in
  `src/model/generated/defaults_data.h`; these two numbers are committed because the *default patch* has to
  be faithful on a machine with no reference installed, which is what "by construction" means.

Where the reference's values are established in **preset units** by ticket `38`'s table, the plugin
already uses them, and the arrival is observable: through the harness, xerum's own `1 Resonance (%)`
reads **10**, which is ticket `38`'s established default, where the authored init is 0. Where they are
not (the envelope times, the filter type above), xerum's own init plays, and the difference is a
**fidelity** matter for the fidelity suite rather than a correctness one.

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
| loads, plays, automates in the ticket `08` harness | loads (`found: xerum \| VST3 \| instrument=yes`); **plays with nothing set: peak 0.156408** on a bare render (`--seconds 2 --note 60`, the init patch — OSC A on, as the reference's init state does, at the reference's own default level, ticket `63`); `--automate "A Level=0.05:1"` ramps mid-render (peak 0.077691, so the ramp is heard), and two identical automation runs are byte-identical |
| state round-trips byte-identically | `--set "A Level=0.5" --out r1.wav`, `--set "A Level=0.5" --dump-state s.bin`, `--load-state s.bin --out r2.wav` → `cmp r1.wav r2.wav` **identical** (peak 0.139029 both, 380-byte state block) |
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
build-30/plugin/tests/xerum-plugin-tests              # all of it: 42 cases, 4,507 assertions
build-30/plugin/tests/xerum-plugin-tests "[plugin]"   # the whole slice by tag
build-30/plugin/tests/xerum-plugin-tests "[realtime]" # the allocation invariant alone
ctest --test-dir build-30                             # every suite in the tree
ctest --test-dir build-30 -R "state round-trips"      # one case, selected by name
```

Measured, per tag: `[bridge]` 7 cases / 1,763 assertions, `[state]` 5 / 134, `[preset]` 9 / 1,374
(1,255 of them the corpus walk), `[audio]` 7 / 24, `[realtime]` 3 / 7, `[defaults]` 1 / 83,
`[corpus]` 1 / 1,255, plus ticket `41`'s two: `[morph-wired]` 1 / 26 and `[unison-stereo]` 1 / 5. The
corpus case walks **all 626 factory presets** through the plugin and asserts
each produces a document and a report; it **skips loudly** (`SKIP`, not a silent pass) on a machine
with no corpus. Note that `ctest -R xerum-plugin-tests` does *not* select these: `catch_discover_tests`
registers each Catch2 *case* under its own name, which is why the tag commands above are the
addressable route.

The cases are: the bridge's own consistency (ids, labels, ranges, choice lists, coverage of every
implemented name); the default-resolution rule; state round-tripping (documents, exact tokens,
parameters, a byte-identical render); a refused foreign state block keeping the patch; a refused future
format version; a state block that is not a preset container; playing (the init patch audible, silence
when the oscillator is switched off, monotone level response with exact attribution, a filter that
measurably attenuates, and ticket `41`'s unison spread measured as a difference between the output
channels); automation mid-render; mono/legato through a document; bus layouts; preset
loading from a fixture xerum writes itself; **damaged preset files returning an error rather than
throwing**; a missing asset substituted with the documented default; a slot selecting a deferred engine
going silent and saying so; `KY - Delicacy` — the one factory preset asking for a spectral morph after
load — resolving its embedded table, expanded, with neither of the two report entries that used to
cover it; and the real-time invariant.

## Known gaps, named rather than hidden

* **`Global Polyphony` is applied at the next `prepareToPlay` or preset load, not on the knob turn.**
  The active voice count is a `prepare()` argument (ticket `28`) and re-preparing the pool allocates
  (the modulation matrix), so it cannot be done from `processBlock`. A preset's value is applied
  immediately — `applyDocument` runs off the audio thread — and a live turn lands at the next
  prepare. A `VoiceEngine::setPolyphony` in `src/dsp` would close it; that file is not this slice's.
* **Warp source routing and drawn pole/zero responses** are not applied. Both are *reported* by the
  plugin (see the table above), which is the contract's requirement; closing them is `src/dsp` work.
  The stereo unison spread was the third item here until ticket `41` put it in the signal path.
* **The amplitude envelope is bound to `Env0`.** `tools/reference-init-defaults.py`'s crosswalk maps
  the reference's "Env 1" group onto `Env{N}` instance 0, and the reference's envelope 1 is its fixed
  amplitude envelope. The crosswalk half is evidence; the "envelope 1 is the amp envelope" half is
  **[INFERENCE]** — it is the reference's convention and this ticket did not measure it. If it is
  wrong, one table entry moves.
* **There is no wavetable browser.** A slot's table is whatever the preset's asset reference names,
  or xerum's own `Tables/xerum/Sine.wav` when it names none. A table *chooser* is a UI feature with a
  dynamic choice list, which hosts dislike; recorded rather than half-built. Ticket `52` draws the
  design's wavetable slot in each oscillator's head — the loaded table's **name**, from the document —
  and its arrows step the slot's own **frame position** (`Table Position (frames)`), because that is the
  one per-slot table control the model has and a slot without it gets no arrows rather than arrows that
  write nothing. The design's arrows choose a different wavetable; that needs the chooser this note
  records as absent.
* **Pitch bend is ±2 semitones**, the reference's init state's bend range (ticket `38`'s table), and
  `kParamBendRangeUp/Dn` are not exposed. Stated, not measured.
* **The editor is minimal on purpose**: the parameter surface plus the import report. The report is
  what ticket `30` needed the interface for. Ticket `44` implemented the *design* of ticket `43` on
  top of that surface — pages of panels, the matrix, the pole/zero plane, the report bar and drawer —
  and ticket `52` arranged the panels the way the design's own screen arranges them: one composed page
  of sections. Those are the sections at the end of this file.
* **The interface is one fixed ratio, scaled into the window** (ticket `59`), and that replaced the
  "resized window re-flows" behaviour this file used to describe. The composition is laid out **once**, at
  the design's baseline width (1080) with the page's own height at it, and the whole of it is scaled into
  whatever window the host gives — so every section's rectangle is a constant and one number changes with
  the window, which is what `plugin/tests/interface_test.cpp` measures as *the rectangles are equal at every
  scale* rather than as a proportion. The ratio is therefore the composition's own: **1080×1128**, 0.957
  wide per tall, derived from the content and not written down. See the ticket `59` section below for the
  arithmetic, for what the design mock's own 1082×668 ratio would cost (42 % of its width empty), and for
  the amplitude envelope's drawn curve — the display ticket `57` reported the band was missing.

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
   person, so that is all this records. Ticket `44` went further and *measured* the running standalone
   — the audio thread by `sample`, the window by a screen capture and a pixel histogram — see the
   ticket `44` section below.

---

# Ticket 44 — the interface design, implemented

Ticket `43` settled the design (variant 3, "Devices") and ticket `31`'s binding already reached every
parameter. This section records what implementing the design changed, and — the part that matters —
where the direction and the model do not meet.

> **Superseded in one respect by ticket `52`.** The pages described below — one device per page behind a
> strip — are now **sections of one composed page**, because the design's own arrangement (three
> oscillators across the top, the filters with their displays, the macros as a strip) cannot be expressed
> as one panel per page. The binding rule this section is about did not move; see the ticket `52` section
> below for what changed, and ticket `53` for what the composed page costs in window size.

```sh
cmake -S . -B build-44 -DCMAKE_BUILD_TYPE=Release && cmake --build build-44 -j8
build-44/plugin/tests/xerum-plugin-tests "[interface]"             # 12 cases, 1,746 assertions
build-44/plugin/tests/xerum-plugin-tests "[interface][headless]"   # the no-editor assertion
build-44/plugin/tests/xerum-plugin-tests "[interface][ui]"         # the cases that open the interface
```

## One composed page, behind a strip that always shows the other sections

A **panel is a `ParamSpec::group`** — the model's own grouping. Panels are *derived* from the parameter
table, one per group, and nothing is hand-listed: `Osc A`, `Osc B`, `Osc C`, `Noise`, `Sub Osc`,
`Filter 1`, `Filter 2`, `Global`, `Amp Envelope`. **Where they sit is a composition rather than a
derivation**: `ui::designedComposition()` is an ordered list of sections — the three oscillators in a row,
noise and sub beneath them, the amplitude envelope's own section, the filters beside their response
display, then the effects and the macros, and the compact modulation matrix **last** — each naming the
groups it holds and how they arrange, and a group no section names falls through to a default section
rather than vanishing. (The envelope's section and the matrix's place are ticket `57`; see that section at
the end of this file.) So which section a group lands in is data; *whether* it lands
is still derived and still total. `ui::SectionStrip` draws one tab per section, always all of them,
**scrolling the single composed page** — larger than the window the plugin shipped then, and since ticket
`53` larger only than a window smaller than the instrument — and carrying a severity dot on any section
whose module the import report named, so a half-loaded preset is located where it happened and not only
collected in the drawer.

**The plan's balance moved, and it moved because the design did.** `InterfacePlan`'s rule is unchanged
— a control is a function of the model type, a panel is a group in the table's order, and columns are
balanced — but a page now holds *one* panel, so the greedy balance that used to distribute panels
across the editor's columns distributes that page's **rows** across its columns. The point of the
balance is what it always was: every row lands in exactly one column, so a parameter cannot be dropped
from a page by a layout edit, and `plugin/tests/interface_test.cpp` asserts exactly that for every
panel. `columnsForRows` is the one design number: a page keeps one column until it has more than eight
rows, so a 13-control page is two columns rather than one very long one, and a four-control page is one
column rather than three absurdly narrow ones.

**The strip is flat.** The prototype grouped its tabs under family headings (`OSC`, `FILTER`, `MOD`,
`GLOBAL`). Those headers are not reproduced, and the reason is not aesthetic: the table's grouping is
flat, so the only derivable family name is the model's module stem (`Oscillator`, `VoiceFilter`,
`Global`, `Env`) and their *display* names would be new hand-written vocabulary — the one thing
`InterfacePlan` exists to avoid. The strip's job (showing what else exists) is met without them.

## The two hard surfaces — and they are editors now

Ticket `44` built both as **views**, and its reason was measured rather than preferred: `ParameterBridge`
bound no modulation parameter and nothing in `plugin/` reached `VoiceFilter::setPoleZero`, so a control
would have written a value nothing read. **Ticket `45` built the missing path**, and both surfaces edit:

| Surface | Shows and edits |
|---|---|
| `ui::ModulationMatrixView` — 64 rows × 12 columns | the loaded preset's `ModSlot0`…`ModSlot63`. Source and aux source are **named** from `dsp::modSourceName` (the 60-name table ticket `27` recovered — the correction ticket `43` recorded against the prototype's conclusion that the interface could not name a source); amount (signed: the sign *is* the polarity), bipolar, bypass, both curve knobs, the destination `(class, instance, parameter)` triple, and the engine's own verdict from `dsp::modDestinationUnresolvedReason` / `dsp::modSourceIsImplemented`. **Eight of the twelve columns are `ParameterControl`s bound to the bridge's own parameter objects** — the source pair, amount, polarity, bypass and the destination triple — and the other four (the slot, the two curve knobs and the note) stay painted from the document. |
| `ui::PoleZeroPlane` — up to 32 sections | the sections a preset draws (`VoiceFilter{n}/PZs[]`, four objects of eight), pole above the axis and zero below, marker size from the section's Q. It **edits**: dragging a pole or zero marker sets that marker's frequency, and dragging it up or down sets its Q. An edit calls `XerumAudioProcessor::setPoleZeroSection`, which writes the document's `PZs[n]` object and republishes the response to the engine. |

The rows are the reference's own 64 whether or not a preset stores them: an empty slot is a **row**
(the reference writes all 64 in every preset), it is drawn as em-dashes, and its note is empty. Every
row's cells are bound, so an unstored row is editable too.

### Where the edited rows live, and how they reach the engine

**The parameters are the live store; the document is where the edit lands.** The `ModSlot` row paths are
addressed the same way every other parameter is, so a matrix edit is a *preset edit* rather than a
parallel state:

* `ParameterBridge::modulationSpecs()` declares one parameter per editable field, with the **document's
  own model path** (`ModSlot3/plainParams/kParamAmount`) as its key. That is what makes a row a
  *declared* parameter, which ticket `40` measured to be load-bearing: `applyDocument` only writes the
  parameters the bridge declares, so an undeclared row would be dead the moment a preset arrived.
* `applyDocument` writes those 512 from the loaded document; `getStateInformation` writes every one the
  user moved back into the state block. So a row survives a save, a restore and a preset load.
* `XerumAudioProcessor::publishModulationSlots` rebuilds the engine's rows from the live parameters —
  with each row's **curves** and **destination name** taken from the document, since neither is an
  editable parameter — writes any changed row back into the document, and hands the engine the table.
* **Neither rebuild happens on the audio thread.** `VoiceEngine::setModulationSlots` allocates, so the
  audio thread only *flags* a change (`refreshModulationParameters` sees a host's automation; a
  parameter listener sees the editor's). A message-thread timer drains the flag, and a change made on
  the message thread republishes immediately. The render therefore still allocates nothing.
* The drawn response reaches the engine the same way: `publishPoleZero` reads `VoiceFilter{n}/PZs[]` into
  `FilterSettings::poleZero` while the audio thread is held out, and `Voice::setSettings` — with the rest
  of the settings — calls `VoiceFilter::setPoleZero` when the model is `PoleZero`.

The measurement that says the path is real is a **render difference**, not a parameter value:
`plugin/tests/interface_test.cpp` shows a row at full depth opening a shut filter, and every way of
neutralising it — zero amount, no source, bypass, a zero aux source, a destination the voice does not
read — making it quiet; a live edit of the amount changing a render; and a pole/zero drag changing one.

### The retraction

The report used to name a drawn response at **`high`**, because the response was shown and not applied.
That is no longer true, so the entry is **retracted** in the same change — the rule ticket `41` applied to
`interpolateAfterLoad`: an entry true before and false after is worse than no entry.
`plugin/tests/interface_test.cpp` asserts no report entry mentions the response being unapplied, and that
the plane still draws and edits its sections.

### One field that is not editable, reported rather than approximated

A row's **destination name** (`destModuleParamName`) has no parameter: the engine addresses a destination
by `(class, instance, parameter-id)`, and the name is the reference's own label, preserved from the
document. So the eight editable fields are the address, and a row that a preset does **not** carry cannot
be given a name through the interface — such a row is reported by the matrix as naming no destination
until a preset supplies one. That is the honest boundary; inventing a name would be inventing the address.

### Why an edit is coalesced, and the measurement that forced it

A parameter change is **not** republished synchronously. The listener's whole job is an atomic flag; a
20 Hz message-thread timer (or `XerumAudioProcessor::flushModulation()`) drains it. That is not a
micro-optimisation — it was forced by `pluginval`:

* the first implementation republished on every change. At strictness level 10, pluginval's *Parameter
  thread safety* test calls `setValueNotifyingHost` on **all 587 parameters, 500 times over**, in one
  message — a quarter of a million writes. Rebuilding 64 matrix rows per write timed the test out at
  30 s. Coalesced, the same test finishes in seconds and `--strictness-level 10 --validate` on the VST3
  returns **SUCCESS, exit 0** (re-run on the ticket `45` change; the whole run took 17 s).
* the 250 ms handshake bound matters for the same reason: a publish that cannot get the block within the
  bound **defers** (re-raises the flag) rather than spinning, so a host that calls `processBlock` on the
  message thread — or one that never leaves it — cannot hang the message thread.

The cost of coalescing is at most 50 ms between an edit and hearing it; the render's allocation
behaviour is unchanged, which the `[realtime]` and `[interface][realtime]` cases still measure at zero.

### One bug the retention found: a cascade that could overflow

Ticket `46` reported a corpus fixture (the only `PZ_SVF` one that renders, `ba-geiger-wub`) reading
**non-finite once**, attributed there to the tree moving. With the response now actually applied, that
is worth measuring rather than attributing, so the ticket `45` plane case renders 32 drawn sections at
the extremes — a pole radius of `0.9999` (Q 1, pole at 0 of Nyquist) against a zero at Nyquist — and
asserts every sample is finite. **It failed**, and the cause is structural: `radiusForQ` clamps a
section's pole radius at `0.9999`, so one section is stable, but a pole at DC at Q 1 has that radius and
eight of those in series have a DC gain near `1e64` — finite in the `double` the cascade accumulates in,
an **infinity in the `float` it returns**, and `inf` in the biquad state poisons every later sample.

`VoiceFilter::processPoleZero` therefore checks exactly what a float cannot hold
(`!std::isfinite (static_cast<float> (value))`) and, on tripping, clears the sections and emits silence
for that sample — the filter returns on the next sample rather than emitting NaN for the rest of the
note. Every finite render is untouched, so no baseline moves: the gate's 7 fixtures are finite at all 15
configurations (`ba-geiger-wub` included), which is the measurement that says the guard is a no-op for
them.

Ticket `46` closed the *class* rather than the instance on its own side: the fidelity sweep now asserts
finiteness for **every one of its 480 rows** (32 fixtures × 15 configurations), not only for the gated
set, and it measured **0 non-finite** with the guard in place and the 7 baselines unchanged to the digit.
A NaN at one rate and not another is exactly the failure that axis exists to find, and it needs no
threshold — which is why silence stays *reported* there rather than asserted: a deferred engine or a
recorded gap can legitimately render nothing.

The guard's own failure shape is observable there too, which is the part worth knowing: it emits **one
silent sample**, not a NaN, so a finiteness check alone would miss it. Ticket `46` added a reported
`stepXerum`/`stepGolden` column for exactly that class — a sample-to-sample step, because a peak bound
barely moves for a dropout at moderate amplitude (`0.11843 → 0.11757`) where the step does
(`0.04686 → 0.11358`, 2.4×, measured on a render with its loudest sample zeroed) — and never gated, since
a transient sharper than the reference's is not by itself a defect. It measured no dropout signature
anywhere in the corpus, so the guard did not fire in this run.

## The report is a bar plus a drawer, never a modal

`ui::ReportBar` is always on screen — a state dot (four states: a load failure, each severity, and
*clean*), the summary, the preset it belongs to, and the handle that opens `ui::ImportReportView`
above it. `Nothing to report` is on the bar **with the drawer shut**, which is the acceptance: a clean
import is a state, and empty and absent do not look alike. Neither surface is a dialog:
`plugin/tests/interface_test.cpp` asserts both are components of the editor's own window, which is what
"never a modal" measures to.

## The coverage readout, because a gap should be visible

The header shows `587 of 776 schema leaves bound` — `boundControlCount()` over
`Schema::instance().leafCount()`, so the number cannot drift from what is on screen. It is the plan's 75
controls **plus the matrix's 512 bound cells** (ticket `45` made the cells real controls; leaving them out
would have understated the surface by 512). This is the honest measure of the gap, stated where a user can
see it rather than implied away, and it is also why a device page is short: the bridge implements a
fraction of the model. The empty area under a short page is where the drawer opens.

## The standalone, run and observed

```sh
open build-44/plugin/xerum_artefacts/Release/Standalone/xerum.app
pgrep -fl xerum                       # pid 21390
sample 21390 3 -file /tmp/xerum-sample.txt
```

**Observed.** The audio thread is running the render path: `com.apple.audio.IOThread.client` →
`HALC_ProxyIOContext::IOWorkLoop` → `juce::PlaybackHandler::audioDeviceProcessorCallback` →
`AudioProcessorPlayer::audioDeviceIOCallbackWithContext` → `XerumAudioProcessor::processBlock` →
`dsp::VoiceEngine::process`.

**And no interface frame is on the audio path.** The sample above was taken with the editor open and
being laid out (`JUCE v9.0.4: Message Thread` has 11 `ParameterPanel` frames in it), and of every
`XerumAudioProcessorEditor`/`ParameterPanel`/`ModulationMatrixView`/`PoleZeroPlane`/`SectionStrip`/
`ImportReportView` frame in the file, **0 fall inside the `com.apple.audio.IOThread.client` block** —
all 11 are on the message thread. That is the acceptance criterion "no interface code on the audio
path" measured on the running product rather than argued from the source.

The **window** was observed by capture rather than by eye (this session had no visual channel), and by
capturing the window itself rather than the screen, because the desktop is shared and a screen capture
measures whatever is on top:

```sh
xcrun swift /tmp/winid.swift          # CGWindowListCopyWindowInfo -> the xerum window's number
screencapture -x -o -l<id> /tmp/xerum-win.png
ffmpeg -y -i /tmp/xerum-win.png -f rawvideo -pix_fmt rgb24 /tmp/xerum-win.raw   # then histogram it
```

The window is 1242×688 at (659, 348), on screen, named `xerum` — and it is this interface. Every colour
the layout paints in this state is in the capture, at its own share of the window's pixels (the report's
severity colours are absent because this session's report is clean, and `ok` appears only as the bar's
8×8 state dot):

| Constant | On screen | Share of the window |
|---|---|---|
| `background` (21, 23, 26) | identical | 48.19 % |
| `panelFill` (30, 34, 40) | (31, 34, 40) | 31.80 % |
| `barFill` (26, 30, 36) | (27, 30, 36) | 8.96 % |
| `panelBorder` (46, 52, 64) | (47, 52, 63) | 0.76 % |
| `selectedTab` (38, 45, 53) | (39, 45, 52) | 0.51 % |
| `dimText` (141, 151, 166) | (143, 151, 165) | 0.29 % |
| `text` (230, 233, 238) | (231, 233, 238) | 0.26 % |
| **`accent`** (84, 201, 180) | **(118, 197, 181)** | **1.37 %** |

Every value is off by one or two counts because `screencapture` writes the display's colour profile;
the accent is off by 34 in red, which is what a saturated colour does under that conversion. Where the
accent is *drawn* is the design's own geometry: 46,808 accent pixels in three groups — device rows
140–141 (logical y 42, full width: `kHeaderHeight - 1`), 222–225 (logical y 83–84, 236 pixels wide:
the selected tab's two-pixel rule at the bottom of a 42-pixel strip, one tab wide), and 288–787 (the
controls' slider tracks and the panels' accent edges). A coarse map of the capture also shows the
structure in the right places: a 42-pixel strip, a 30-pixel page header, a ~252-pixel device panel, and
the report bar across the bottom inside its margin.

The same measurement is a test now (`the interface draws xerum's own identity, and the pages draw`),
which renders the editor into an image and counts the palette in it — against the constants exactly,
with no capture in between — including each of the matrix's twelve column titles drawn in its own
x-range.

**Not observed, and stated rather than implied.** Nobody looked at the window: nothing here says the
interface *looks* right — only that it is drawn, in this palette, at these positions. Clicking a tab,
opening the drawer and importing a preset file in the standalone were not driven by hand; those paths
are covered in-process (`showDevice`, `setReportDrawerOpen`, `loadPresetFromFile`). The window's empty
lower area in the capture is the ~250 logical pixels below a device page — see *the coverage readout*
above for why a page is short, and note that it is also where the report drawer opens.

## What could not be implemented as recorded

Ticket `45` closed the first two of ticket `44`'s gaps: the **matrix is editable** (eight columns bound
to declared parameters, edits reaching `VoiceEngine::setModulationSlots` and heard) and the **drawn filter
is applied** (`VoiceFilter::setPoleZero`, with the `high` report retracted). What is left:

* **A row's destination name is not editable.** It has no parameter, and it is preserved from the
  document rather than invented — see the retraction's own section. A row a preset does not carry is
  reported by the matrix as naming no destination.
* **The matrix's two curve knobs are not editable either**, though the engine *does* evaluate them. They
  are carried from the document into the engine's rows untouched, so a preset's own curve survives an
  edit; making them editable is another two parameters per row, and nothing in the acceptance needs them.
* **The prototype's device families** (`OSC`/`FILTER`/`MOD`/`GLOBAL` headings in the strip) — see
  above: their display names would be invented vocabulary.
* **The parameter-and-leaf inventory behind the design** (452 controls, 280 arrays, 42 containers) is
  still not on screen. Ticket `43`'s finding stands: the arrays and containers are addressed by the
  pages they belong to, and this interface shows what the bridge binds — 75 page controls and 512 matrix
  cells — with the gap stated in the header rather than a page of raw arrays with no engine behind them.

# Ticket 51 — the visual design language

Ticket `44` implemented `43`'s *structure* — one device per page behind a strip — and left the interface
as **stock JUCE widgets plus a palette**: 66 lines of `InterfaceLook.h` and a `ParameterPanels.cpp` that
drove `juce::Slider`s. Nothing drew anything, which is why the dev's reaction was "the ui looks nothing
like serum2". This ticket is the drawing half. The boundary is unchanged and is the dev's own call: the
design is a **conventional wavetable-synth grammar**, not the reference's skin, and everything here comes
from `docs/design/` (`xerum-design-system.html` for the tokens, `xerum-svg-elements.html` for the control
geometry, `xerum-direction-warm.html` for the composed screen). Nothing was sampled or traced from
anywhere else.

## The tokens are the design system's, and one place holds every colour

`InterfaceLook.h` is now the fourteen roles of `xerum-design-system.html` — `bg`, `surface`, `surface2`,
`border`, `borderHi`, `fg`, `fgDim`, `fgMuted`, `accent`, `accentDim`, `accentBg`, `red`, `orange`,
`green` — plus the type scale, the 2px spacing grid and the knob geometry (44/28/26px, the 225°–495° arc,
the body/track/stroke/pointer fractions). **No hex lives outside that header**, and the old bespoke names
(`background`, `panelFill`, `barFill`, `dimText`, `selectedTab`, `severityHigh`…) are gone rather than
aliased, because "cold and phosphor are a swap rather than a rewrite" only holds if the drawing code asks
for a role. Every call site and every assertion that used the old names was updated with it.

**Type.** The design's families are DM Sans and IBM Plex Mono, both OFL. Neither is embedded: the
load-bearing use is the **monospaced numerals** (a value that jitters as it changes is the thing to avoid)
and `juce::Font::getDefaultMonospacedFontName()` is a monospaced face that already ships with JUCE, so the
numbers do not jitter and no font binary enters the repository — therefore no new `THIRD_PARTY.md` entry.
Body text is JUCE's default sans. That is a deliberate substitution of the body face and is recorded here
rather than implied.

## What draws now

* **The knob** (`ui::Knob`, a `juce::Slider` subclass so the attachment and the harness still see a
  slider). A track arc over the whole 225°→495° sweep, the value arc in `accent`, **a modulation arc** in
  `accentDim` from the value to the modulated value, a pointer at the value's angle, the design's hover /
  drag / focused body strokes, and a focus ring on keyboard focus. Diameter comes from the plan
  (`ui::knobSizeFor`, total like `controlKindFor`); a 44px knob for a page, 28px for NOISE and SUB, 26px
  for the eight macros. A matrix cell is the same widget in `Cell` mode: a compact numeric field, because
  512 dials down a table is a grid of dials, not a matrix.
* **The second arc is a fact about the loaded preset**, not decoration: `updateModulationArcs` walks the
  document's 64 rows, skips the ones the engine does not apply, composes each destination's
  `(class, instance, parameter)` into the very `ParamSpec::modelPath` the controls are built from, and sums
  the amounts. A knob no row addresses draws no second arc.
* **A boolean** is a pill toggle with a state dot, not a stock checkbox; a choice is a themed picker; a
  matrix boolean is the dot alone, and the **bypass** dot is green while the row is live (the parameter is
  `kParamBypass`, so the cell inverts the dot rather than the parameter).
* **Panels and sections**: a `surface` panel with the design's 5px radius and 9px uppercase title, a header
  with the gradient, the wordmark, the preset field, the coverage readout and the import button, and a
  9px uppercase title on every section — the matrix's carrying its `N / 64` tag, the number of rows its own
  geometry shows.
* **`SectionStrip` is a section tab strip**: pills, `fgDim` when inactive and the accent on `accent-bg` when
  active, **scrolling the single composed page** when the window is smaller than it — and, since ticket
  `53`, doing nothing but locating when it is not — with the severity dot on the tab of the section whose
  module the report named.
* **The matrix is a grid**: the header in the design's 7px monospaced table-head colour, stored rows
  striped on the surface and empty slots on the window, the source column in the accent, the amount cell
  colouring its **sign** (for an amount the sign is the polarity) and the bypass a dot.
* **The report is a designed surface**: a 22px bar with a 5px state dot (`green` clean, `red` load
  failure, else the severity ramp), the summary in monospace, the preset name at the right, and the drawer
  with severity chips in **red / orange / green** for high / medium / info and the loader's own strings.
  `Nothing to report` is the clean state's words, and an absent report is the failure's — they do not look
  alike. The design system's three colours force one overload — `green` is both info and clean — and the
  states are told apart by their words, which is exactly the distinction the contract requires be visible.
* **`PoleZeroPlane`** is the design's filter display: a `bg` surface with a 3px radius, a quarter-labelled
  frequency axis, poles as crosses in `accent` above the axis, zeros as circles in `accentDim` below it,
  marker size carrying Q, and the selected section ringed.

## What did not move

`InterfacePlan` still derives everything: which control a parameter gets is `controlKindFor` over its model
type, which panel is its `ParamSpec` group in the table's order, and which column is a greedy balance over
the panel's rows. Two things were added and both are functions of the spec, not hand placement:
`knobSizeFor` and `rowHeightFor` — a knob row is its own diameter plus its readout and label, so a page's
height had to become the sum of its rows' heights rather than a row count times one number. `kRowsPerColumn`
moved from 8 to 5 because eight 68px knob rows do not fit the design's 600px window, and the balance was the
only thing that decided how tall a page was. (Ticket `52` then replaced that balance with the design's own
composition — a panel now lays its controls out itself at the width it is given, and `kRowsPerColumn` is
gone; see that section.) The coverage readout, the one-device-per-page structure (which ticket `52` then
made one composed page), the headless-construction counters and the lazy editor were all untouched by
ticket `51`.

## Acceptance, measured

```sh
cmake --build build-51 -j8
./build-51/plugin/tests/xerum-plugin-tests "[interface]"      # 5146 assertions, 18 cases — all passed
./build-51/plugin/tests/xerum-plugin-tests                    # the full plugin suite: 15004 assertions
                                                              # in 68 cases — all passed
/tmp/pluginval-104/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 \
    --validate build-51/plugin/xerum_artefacts/Release/VST3/xerum.vst3      # SUCCESS, including
                                                                            # Editor + Editor Automation
cp -R build-51/plugin/xerum_artefacts/Release/AU/xerum.component \
      ~/Library/Audio/Plug-Ins/Components/                    # an install, recorded: it replaced the
                                                              # xerum.component a previous ticket left there
auval -strict -v aumu Xrm2 Xerm                               # AU VALIDATION SUCCEEDED
```

Two new cases cover what the old look's assertions could not: **`a slider is a drawn knob, and its second
arc is the preset's own modulation`** measures the widget class, the diameter (44px on screen, 28px for
NOISE), the arc's own geometry through `pointAtProportion` (both ends below the centre, the middle at the
top), the `accent`/`border`/`surface2` pixels in the knob's own snapshot, and the depth two matrix rows sum
to (0.075) against a parameter no row addresses (0). **`the interface is laid out at the design's baseline
window size`** — the case tickets `52` and `53` renamed, now `the composed page fits the window it opens
at, and stays legible at 900x600` — sets the editor to **900×600** and requires every control of every page
to be non-empty and inside its panel — the measurement of "works at the baseline size" that this session
could make, since driving another app's window needs Assistive access this machine does not grant
`osascript`.

The palette assertions in `the interface draws xerum's own identity, and the pages draw` were updated, not
weakened: the old constants no longer exist, and the filter page's zero markers moved from the report's
info blue to `accentDim`, so that check now counts `accent` and `accentDim` on **the plane's own snapshot**
rather than on the whole page.

## The standalone, run and observed

```sh
open build-51/plugin/xerum_artefacts/Release/Standalone/xerum.app
xcrun swift /tmp/xerum51-winid.swift        # CGWindowListCopyWindowInfo -> 31079, xerum, 1082×668
screencapture -x -o -l31079 /tmp/xerum51-final2.png
```

**The palette actually on screen.** The capture is 2164×1336, i.e. the window's 1082×668 logical pixels at
this display's 2× backing. The two most frequent *exact* colours are the design's own:

| Exact colour on screen | Share of the window |
|---|---|
| `rgb(26,25,23)` = `surface` #1a1917 | 48.10 % |
| `rgb(17,17,16)` = `bg` #111110 | 40.63 % |
| `rgb(38,50,55)` | 3.70 % — **the macOS title bar**, logical rows 1–26, full width |
| `rgb(34,33,32)` = `surface2` #222120 | 1.04 % |
| `rgb(43,40,38)` = `border` #2a2826 (+1 per channel from the profile) | 0.97 % |

Every other colour in the top twenty is an anti-aliased blend of those. Counting against the tokens (the
two large areas match their tokens exactly; the small ones are counted within a tolerance of a few counts
per channel, and the accent — a saturated colour, which the display profile shifts most — within 40):
`accent` #e89b30 0.100 % in logical rows 42–368 (the wordmark, the coverage number, the module tag and the
knobs' value arcs), `fgDim` #7a756b 0.109 % (rows 42–659: labels and readouts), `fgMuted` 0.081 %,
`fg` #d4cec4 0.061 % (rows 105–388: the knob readouts and the panel titles), `border-hi` 0.037 %,
`green` #50b868 0.002 % (rows 653–658: the report bar's clean dot). **`red` is absent**, which is what a
clean import must be. The palette is therefore on screen at the proportions a page of panels and knobs
predicts, and nothing outside it is drawn except the OS title bar.

**No interface code on the audio path**, reproduced the way ticket `44` measured it:

```sh
sample 4531 4 -file /tmp/xerum51-final2-sample.txt
```

The `com.apple.audio.IOThread.client` block runs the render path —
`juce::AudioProcessorPlayer::audioDeviceIOCallbackWithContext` → `XerumAudioProcessor::processBlock` →
`refreshSettingsFromParameters` → `ParameterBridge::currentModelValue` — and contains **0** interface
frames. The `JUCE v9.0.4: Message Thread` block contains **2** of them,
`xerum::plugin::ui::PillToggle::paintButton` and `xerum::plugin::ui::DeviceTab::paintButton` (an earlier
sample of the same build caught `DeviceTab::paintButton` ← `ui::look::bodyFont` instead; the sample's
timing decides which drawing call it lands in), so the two are separated in the running product and not
merely in the source. `sample` caught fewer editor frames than ticket `44` did because this session could
not resize the window to force a layout pass.

## What could not be seen, and is not claimed

**Nobody looked at it.** Every statement above is a measurement: a count of colours at a position, a
rectangle's size, a symbol in a stack. Nothing here says the interface *looks* good, or that it looks like
a finished instrument — that judgement needs a person, and the acceptance's own first line ("a person
familiar with wavetable synths can find an oscillator's tuning and a filter's cutoff without instruction")
is precisely the one this session cannot answer. **Who judged it: nobody yet.** The layout makes an
oscillator's tuning a knob labelled `A Pitch (semitones)` on the page tagged `Oscillator0` and a filter's
cutoff a knob labelled `1 Cutoff` on the page tagged `VoiceFilter0`, with the values in monospace
underneath — that is what is on screen; whether it is findable is the test that requires eyes.

Also not observed: no knob was dragged in the standalone, no tab clicked, no preset imported through the
standalone's own dialog, and the window was never resized by hand (that needs Assistive access). Those
paths are covered in-process: `showDevice`, the plane's synthesised drag, `loadPresetFromFile`, and the
900×600 layout assertion.

# Ticket 52 — the interface is arranged by the design's composition

Ticket `51` gave the interface the design's **elements** — the tokens, the knobs with their arcs, the
pills, the chrome, the plane — and the dev's reaction was that they look like the design **but the
placements do not**. That was correct and structural: the design specifies a *composition* (three
oscillators across the top, noise and sub beneath them, the two filters with their response displays in
the middle, the matrix under them, the effects and macros side by side at the bottom) and xerum derived a
**grid** — one panel per group of the parameter table, columns balanced.

## The resolution: the composition is data, the binding stays derived

`ui::designedComposition()` is now an ordered list of `ui::SectionSpec` — `osc-row`, `noise-sub-row`,
`env-section`, `filter-section`, `bottom-row` (its two halves sharing a `band`, which is how "side by side"
is expressed), the **default section**, and `mod-panel` — each naming the groups it holds (`Osc` claims
`Osc A`…`Osc C` by prefix, `Macro` claims the eight) and how they arrange (`Panels`, `Strip`, `Matrix`).
The first four entries are ticket `57`'s; the rest is ticket `52`'s. **The flexible flag is gone** (ticket
`53` gave the design's `1fr` to the matrix, ticket `57` moved it to the amplitude envelope, and ticket `59`
removed it): a row that grows when the window does is a section re-flowing, and the layout is now given a
width and nothing else.

`ui::planInterface` resolves that over the table: panels are still one per group in the table's order,
every panel lands in exactly one section, and **a group no section names falls through to the default
section** rather than vanishing — which is why `Global` is on the page at all (`Amp Envelope` fell through
here until ticket `57` gave it a section of its own), and why `InterfacePlan::sectionForGroup` exists as
the answer to "where did it land" that the evidence reads.

The binding did not move. Every control is still built from its `ParamSpec` by model type
(`controlKindFor`), still bound through JUCE's own attachment to the bridge's parameter object, and the
editor still cannot be constructed eagerly. What moved is the arrangement:

* **`ui::CompositionView`** arranges whole sections on **one page**, in the design's order, with the
  design's grid (6px between panels, 3px between rows, 10px of side padding) — and **the page is not in a
  viewport any more** (ticket `59`): it is laid out at exactly the size of the space it is given and the
  whole canvas is scaled with the window, so it can never scroll and the strip no longer scrolls it. Ticket
  `52` had the page taller than every window and the strip scroll it; ticket `53` packed the page so that it
  fitted the window it opened at and left the scroll for a window smaller than the instrument; ticket `59`
  removed the smaller-than-the-instrument case altogether, because a window off the ratio is letterboxed
  rather than allowed to show a clipped or scrolling page. `SectionStrip` (the renamed `DeviceStrip`) names
  the sections and carries each section's severity dot; the model module left the tab because a section now
  holds several (`Filter` is `VoiceFilter0` **and** `VoiceFilter1`) and the design's tabs are labels.
* **`ui::ParameterPanel`** lays its own controls out, from their model types: the design's grammar is a row
  of knobs, then the pickers, then the pills at the bottom, and NOISE/SUB — the design's one compact band —
  put their knobs and their option buttons on one line (`--gap-inline`). A panel's height is derived at the
  width it is given (`preferredHeight`) — and since ticket `59` that width is the nominal one at every
  window, so a panel's rows are decided once for the whole product rather than per window size.
* **A choice whose vocabulary is short is drawn as the design's option row** (`ui::ChoiceButtons`, the
  `btn-opt` pill bound through `juce::ParameterAttachment`): NOISE's four colours and SUB's six waveforms,
  which is exactly where the design draws buttons. The filter's **83-value** vocabulary cannot be a row of
  pills, so the filter panels carry the themed picker — recorded here because it is a deviation from the
  design's four buttons, and it is the vocabulary's length, not a preference.
* **The wavetable slot** — a monospaced name field and two 16px arrows — is in each oscillator's head, as
  the design draws it. See the "no wavetable browser" note above for what the arrows do and why.
* **The response display is inside the filter section**, beside that filter's knobs (`ui::ParameterPanel`
  carries it as an inline display): the pole/zero plane is a child of the **Filter 1/2 panel** it draws and
  edits, rather than a page of its own. The design draws one display in Filter 1 and none in Filter 2;
  xerum has a drawn response *per voice filter* and the plane is how it is edited, so each filter panel
  carries its own. Noted as a deviation.
* **The matrix is a section** (`mod-panel`), headed `MOD MATRIX` with the design's tag: the number of rows
  on screen over 64 — the section's own height divided by the row height, so the tag cannot drift from the
  geometry. Ticket `53` made the section the composition's flexible row; **ticket `57` gave that flag to
  the amplitude envelope and made the matrix a compact panel** — four of the 64 rows, at the **end** of
  the composition, after the macros — and the tag is unchanged, because a caption saying `4 / 64` is how a
  compact matrix says what it is not showing. The four effects cards are the design's `fx-section`;
  **xerum's table has no FX group** (the FX rack is not in the bridge), so that section resolves to zero
  panels and is not drawn. It stays in the composition so the bottom row completes itself the day the table
  has one — and its absence is a stated fact rather than a silent drop.
* **The macros are a strip**: eight bare 26px knobs with their labels, no panel chrome —
  `ParameterPanel::setCompact`, which the `Strip` section kind turns on. The fall-through section —
  `Global`, the one group the composition does not name since ticket `57` named the envelope, and
  `Amp Envelope` beside it before that — was its own line under the matrix in ticket `52` and is, since
  ticket `53`, the **other half of the design's bottom row**, beside the macro strip.

## What did not move

`controlKindFor`, `knobSizeFor`, `rowHeightFor`, the design's diameters (44/28/26px) and arc, the header
with the **computed** `N of 776 schema leaves bound` readout, the report bar and drawer, the two drawn
surfaces, the lazy editor and the counters that prove it. `plugin/tests/interface_test.cpp` asserts the
composition at both acceptance sizes and, in `the design's sections are in the design's positions`, the
arrangement itself — three oscillator panels on one line, noise and sub beneath them, the plane a child of
its filter panel and to the right of every control in it, the matrix's visible rows as its heading reports
them, the macro strip on one line, and the fall-through holding exactly `Global` (and `Amp Envelope` beside
it until ticket `57` gave the envelope a section of its own).

## Acceptance, measured

```sh
cmake --build build-52 -j8
./build-52/plugin/tests/xerum-plugin-tests "[interface]"   # 5493 assertions, 19 cases — all passed
./build-52/plugin/tests/xerum-plugin-tests                 # the full plugin suite: 15351 assertions
                                                           # in 69 cases — all passed (the fidelity case
                                                           # needs the VST3 bundle, so it is skipped until
                                                           # the wrapper is built)
/tmp/pluginval-104/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 \
    --validate build-52/plugin/xerum_artefacts/Release/VST3/xerum.vst3    # SUCCESS, including its
                                                                          # Editor + Editor Automation tests
```

**The composition, in rectangles, at both sizes the acceptance names** (measured in-process at
`1082×668` and `900×600`; the composed page is `1054×1258` and `872×1258` — **those are ticket `52`'s
numbers, before ticket `53` packed the page; the current ones are in the ticket `53` section below**):

| section | `1082×668` | `900×600` | holds |
|---|---|---|---|
| `osc-row` "Oscillators" | 1054×385 | 872×385 | Osc A/B/C, 347 and 286 wide, one line, 13 controls each |
| `noise-sub-row` "Noise + Sub" | 1054×173 | 872×173 | Noise 524 / Sub Osc 524 (433 each at 900) |
| `filter-section` "Filter" | 1054×225 | 872×225 | Filter 1 / Filter 2 side by side, plane inside each panel |
| `mod-panel` "Mod Matrix" | 1054×219 | 872×219 | the 64-row grid, 8 rows shown, 26px header |
| `macro-section` "Macros" | 394×82 | 394×82 | eight 44px items on one line, centred |
| `default-section` "Other" | 1054×159 | 872×159 | Global + Amp Envelope — the fall-through |

**Palette shares**, counted on the editor's own render at `1082×668` (exact colours, tolerance ≤8 per
channel): `surface` 72.5 %, `bg` 15.6 %, `surface2` 11.6 % (the knob bodies and the header gradient),
`border` 7.7 % (the knob tracks — `surface2` is 8 counts away per channel, so the two overlap at this
tolerance), `fgMuted` 0.37 %, `accent` 0.25 % (the value arcs, the wordmark, the coverage number),
`fgDim` 0.19 %, `fg` 0.09 %, `accentDim` 0.06 % (the modulation arc on `voicefilter0.freq`, depth 0.075).
With the half-loading fixture loaded, `red` is 0.005 % (the strip's high-severity dot) and `orange` is
absent — correct for a fixture with no medium entry. **Every colour on the page is a token**, and ticket
`53` made that true rather than nearly true: the window capture found the matrix viewport's **own scroll
bars** drawn by JUCE's default look-and-feel in its dark scheme's fill blue `#42a2c8` (0.42 % of the
window), because the interface had never given its remaining stock widgets a look-and-feel of its own.
`ui::look::XerumLookAndFeel` is that look-and-feel (below), and the same two bars are now `border-hi`.

**The fall-through, named**: `Global` and `Amp Envelope`, in `default-section`, because the design's
composition names no section for them. `fx-section` names `FX`, which matches no group in this table.

**No interface code on the audio path**, the way ticket `44` measured it:

```sh
sample 10214 8 -file /tmp/xerum52-sample.txt
```

The `com.apple.audio.IOThread.client` block runs `juce::AudioProcessorPlayer::audioDeviceIOCallback…` →
`XerumAudioProcessor::processBlock` → `ParameterBridge::currentModelValue` and contains **0** interface
frames. The message thread is servicing `juce::NSViewComponentPeer::AsyncRepainter::handleAsyncUpdate`
(this session's samples caught no `paintButton` frame — the window was idle — where ticket `51` caught two
of them; the drawing itself is measured in-process instead, see below). The zero-allocation render **with
the interface open** is asserted by `[interface][ui][realtime]`, which passed.

## What could not be seen, and is not claimed

**This session's display is asleep** (`CGDisplayIsAsleep` → 1, `CGDisplayIsActive` → 0), so a window
capture was impossible: `screencapture -l31215` fails with *"could not create image from window"* and a
full-screen capture comes back **entirely black** (`rgb(0,0,0)` at 100 %). The evidence above is therefore
*not* a photograph of the screen: the rectangles come from the editor's own laid-out components and the
palette from `createComponentSnapshot` — JUCE's own paint path at the window's own size, but not the
window as the OS composited it. Ticket `51`'s capture was made when the display was awake; this one could
not be.

**Nobody looked at it.** The standalone *did* launch and *did* create its window — `CGWindowListCopyWindowInfo`
reports id `31215`, layer 0, onscreen, owner `xerum` pid 10214, **974×602 logical** at (793,394) — and it ran
the render loop with no interface frame on the audio thread. What no one has done is see whether the
placements now *read* right: whether three oscillator panels across the top, with the filters' response
displays beside their knobs and the macros as a strip at the bottom, is the instrument the design drew. The
dev is the only judge of that, and the design file is the standard.

Also not observed: no knob dragged, no tab clicked, no preset imported through the standalone's own dialog,
and the window never resized by hand. Those paths are covered in-process: `scrollToSection`, the plane's
synthesised drag, `loadPresetFromFile`, and the two-size layout assertion.

One environment note for the next session: launching the standalone **on first run of a newly built
binary** blocks in `XerumAudioProcessor`'s constructor on `TableLibrary::loadTable` →
`std::filesystem::directory_iterator` over `/Library/Audio/Presets/Xfer Records/Serum 2 Presets` — a macOS
permission prompt for the reference's content tree, which nothing in this session could answer. Launching
with `XERUM_CONTENT_ROOT=<repo>/content` resolves xerum's own default table from the first root and the app
starts normally. That is a first-launch/environment fact and not a ticket `52` regression: the code path is
ticket `30`'s and unchanged.

# Ticket 53 — the composed page is packed to fit its window

Ticket `52` arranged the interface as the design composes it, and the dev's report was that **the whole
interface has to be scrolled to be seen**. That is a defect in the composition's *density* rather than in
its order, and ticket `52`'s own measurement contained the diagnosis: the composed page was `1054×1258`
against a `1054×578` viewport — **2.2 times taller than its window** — in six full-width bands that each
paid their own chrome.

## What changed, and where each number comes from

Every compaction below is the **design system's own number** (`ui::look`) or a line the control of that
kind actually needs; none of them is a new, smaller vocabulary:

| what | was | is | where it comes from |
|---|---|---|---|
| a panel's inner padding | 10 all round | `7` across, `5` down | the design's `--pad-panel: 5px 7px`, already named in `look::padPanelX/Y` |
| a panel's title band | 26 | 15 | the design's 9px `.panel-title` line (11.7 at its 1.3 line-height) plus the panel's own gap |
| the gap between a panel's rows | 4 | 3 | the design's `--gap-panel` (`look::space::panel`) |
| a section's heading | 15 | 12 | the design's 8px `.section-title` line |
| the filter's inline response display | 190 minimum | 150 minimum | still taller than the design's own response display (68 at 900×600); 150 is where a pole/zero marker still has room to be grabbed |
| the matrix's tag | the constant `8 / 64` | the geometry's row count, `N / 64` | the design's own tag, and the section now takes the window's slack |
| the default window | 1080×640 | 1080×1108 | derived from the packed page at the default width (below) |

Two of them are not numbers at all, they are things the composition was doing wrong:

* **NOISE and SUB were not in the design's compact band.** `ParameterPanel::rowsFor` asked the panel's
  *first control* for its knob size to decide whether the panel was a compact band — and `Noise`'s first
  row is its `Enable` pill, which is not a slider, so the answer was `Default` and the band never
  happened: each panel put its option row on a line of its own, which is the design's `osc-controls`
  grammar and not its `noise`/`sub` one. The band is now a fact about the panel's **group** (any control
  in it draws a `knob-sm`), and the design's one line — `[knob-sm][knob-sm][btn-row]` — is what it draws.
* **The fall-through section shared nothing.** The design's bottom row pairs the effects cards with the
  macro strip; xerum's table has no effects group, so the macro strip stood alone and `default-section`
  took a full line of its own beneath it. Ticket `53` gives the fall-through section that empty half:
  `defaultSection()`'s band is `bottom`, so the macros and `Global` + `Amp Envelope` are one row. The
  section's *order* is unchanged — it is still the last entry — which is what keeps it the fall-through.

And one colour, which the window capture found rather than the layout: the interface had never given its
remaining stock widgets a look-and-feel of its own, so JUCE's default — `LookAndFeel_V4`'s dark scheme,
whose `defaultFill` is `#42a2c8` — drew the matrix viewport's **scroll bars** (0.416 % of the window) and
would have drawn a focused picker's outline. **`ui::look::XerumLookAndFeel`** (a `ColourScheme` built
entirely from the palette plus the ids `LookAndFeel_V4` fixes by hand, for every stock class this interface
can contain) is set on the editor, so a drawn surface beneath it resolves a colour from `InterfaceLook.h`
or from nothing; the same two bars are now `border-hi`.

And one structural change, which is the design's own answer to "the matrix is a 64-row grid in a window
that can show eight":

* **The matrix is the composition's flexible row.** The design's content box is
  `grid-template-rows: auto auto auto 1fr auto auto` with `.mod-panel { min-height: 0; flex: 1 }` — the
  matrix absorbs whatever the other rows do not use. `CompositionView::layout` now measures the sections
  and gives the matrix the difference between what they need and the window's height, so the page is
  exactly as tall as the window whenever the sections fit in it, and never shorter than its own eight
  rows. Nothing is clipped: below its natural height the page is taller than the window and scrolls, which
  is what the minimum-size window does.

## Where the page is now

Measured in-process by `plugin/tests/interface_test.cpp`, which prints the table it asserts:

```sh
build-53/plugin/tests/xerum-plugin-tests "[interface]" -s
```

**These are ticket `53`'s own numbers**, from the build that packed the page; ticket `57` then moved the
page's slack to the amplitude envelope and made the matrix compact, so the default window is 1080×1128 and
the page 1038 now — the current tables are in ticket `57`'s section at the end of this file. What did *not*
change is the claim these tables make: the page fits the window it opens at, and scrolls only where the
window is smaller than the instrument.

| window | viewport | page | |
|---|---|---|---|
| **1080×1108** (the editor's own default *then*) | 1060×1018 | **1018** | fits, and no scroll bar is drawn |
| 1082×668 (ticket `52`'s baseline) | 1054×578 | 1018 | scrolls |
| 900×600 (the window's minimum) | 872×510 | 1144 | scrolls; every control still laid out and inside its panel |
| 1400×900 | 1372×810 | 947 | scrolls by 137; the oscillator row is one knob row shorter at this width |

| section | at 1060 wide | at 872 wide | holds |
|---|---|---|---|
| `osc-row` "Oscillators" | 344 | 344 | Osc A/B/C, 350 / 286 wide, one line, 13 controls each |
| `noise-sub-row` "Noise + Sub" | 144 | 199 | Noise and Sub Osc, one line of small knobs with their option rows |
| `filter-section` "Filter" | 172 | 172 | Filter 1 / Filter 2 side by side, the plane inside each panel |
| `mod-panel` "Mod Matrix" | 216 (8 / 64) | 216 | the 64-row grid, its own scroll bars — four rows and last since ticket `57` |
| `macro-section` "Macros" | 316×130 | 314×201 | eight items on one line, the design's strip |
| `default-section` "Other" | 738×130 | 552×201 | Global + Amp Envelope — the fall-through, in the bottom row |

The page is **1018** where ticket `52` left it at **1258**: 19 % off, and every remaining pixel is a
control the table has. The band-by-band account of where it came from: `osc-row` 385→344, `noise-sub-row`
173→144, `filter-section` 225→172, `mod-panel` 219→216, and the bottom of the page 82+159→130.

## The window, and why it is that size

The default window is now **derived from the layout**: `setSize (kDefaultWidth, composition_.totalHeight()
+ kHeaderHeight + kStripHeight + kReportBarHeight)`, clamped to the resize limits. It is the same call
`resized` makes, so the window cannot drift from the page, and a table that outgrows the screen opens a
maximum-size window that scrolls rather than one off the screen.

**This is the part of the ticket that is a report rather than a fix.** The instrument's own size at the
default width is 1018 logical pixels of page, and the packing above — every lever the design's own
vocabulary offers — gets it to that and no further. Where the 1018 sits:

* **the oscillator row is 344 of it, and 344 is its floor at three panels across.** Each panel holds
  thirteen controls (ten sliders, two pickers, one pill) and the ten sliders need 651 px of cell width at
  a 332 px inner width — three rows, and rows of 44 px knobs with their readouts and labels are 68 px each.
  Ticket `52`'s own greedy packing already found the best three rows; only *reordering* the knobs (which
  would break the panel's read-in-the-table's-order) or drawing them below the design's 44 px would make
  it two;
* the design's own mock puts **five** knobs and three pills in an oscillator panel and is 138 px tall;
  xerum's table has thirteen and the design's sizes put a floor under them. The design's whole 900×600
  window holds about forty-five controls in 566 px of content; this instrument has eighty-four, so it is
  roughly twice the mock's area, and the mock's own window cannot hold it.

Two decisions would close the rest, and neither is this ticket's to make: **moving a section behind the
strip** (the matrix's 216 px, or the eight macros' 130 px, are the candidates) or **drawing the oscillators'
knobs at `knob-sm`**, which the design uses for NOISE, SUB and its Filter 2 sidebar and *not* for its
oscillators. Both are stated here rather than taken. The minimum window (900×600) is legible — everything
is laid out, nothing clipped — and scrolls, because at that size the window is smaller than the
instrument.

## Acceptance, measured

```sh
cmake --build build-53 -j8
./build-53/plugin/tests/xerum-plugin-tests "[interface]"   # 5566 assertions, 20 cases — all passed
./build-53/plugin/tests/xerum-plugin-tests                 # the full plugin suite: 15424 assertions in
                                                           # 70 cases — all passed
/tmp/pluginval-104/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 \
    --validate build-53/plugin/xerum_artefacts/Release/VST3/xerum.vst3     # SUCCESS, including its
                                                                           # Editor + Editor Automation tests
```

**The page against its window, in-process** — the fit as two numbers, asserted and printed:

```
[interface] default window: window 1080x1108, viewport 1060x1018, page 1018
```

and the chain ticket `51` asserted is kept at the minimum: every control of every panel non-empty and
inside its panel, every panel inside its section, every section inside the page, at 900×600. The fit is
asserted on top of it, at the default: `compositionHeight() <= compositionViewportHeight()`, the vertical
scroll bar not visible, `compositionScrollY() == 0`, and every section's rectangle above the fold.
`Viewport::isVerticalScrollBarShown` returns the `setScrollBarsShown` *flag*, which is true either way —
the assertion uses `getVerticalScrollBar().isVisible()`, which is the thing a user sees, and asking the
other one would have "proved" the old build did not scroll too.

**The window in the running product**, the standalone launched and its own window read out of the window
server:

```sh
open build-53/plugin/xerum_artefacts/Release/Standalone/xerum.app
xcrun swift /tmp/xerum52-allwin.swift <pid>        # CGWindowListCopyWindowInfo
```

**1082×1136 at (739, 219), layer 0, on screen** — the editor's 1080×1108 plus the 1 px frame and the 28 px
title bar — entirely on this display (219 + 1136 = 1355 < 1440).

**The palette, counted on the editor's own render** (`createComponentSnapshot` at 1080×1108 — JUCE's own
paint path at the window's own size, the measurement ticket `52` made, and the one available here: a fresh
desktop capture of the window was not possible this session — `screencapture -l<id>` fails with *"could not
create image from window"*, the failure `52` recorded, and the shared desktop had Chrome, Slack, WhatsApp
and Ghostty in front of the xerum window with no Assistive access to raise it):

| exact colour in the render | share | what it is |
|---|---|---|
| `rgb(26,25,23)` = `surface` | 53.38 % | the panels and the page |
| `rgb(17,17,16)` = `bg` | 29.36 % | the window behind them |
| `rgb(34,33,32)` = `surface2` | 3.87 % | knob bodies, the header gradient |
| `rgb(42,40,38)` = `border` | 3.58 % | the panel chrome and the knob tracks |
| `rgb(58,56,54)` = `borderHi` | **0.426 %** | **the two scroll bars, which is where the blue was** |
| `rgb(232,155,48)` = `accent` | 0.19 % | value arcs, the wordmark |
| `rgb(212,206,196)` = `fg` | 0.12 % | readouts and titles |

**`#42a2c8` anywhere in the render: 0 px.** Every other colour above 0.02 % is an anti-aliased blend of two
tokens. On screen (the earlier capture, before the fix) the same two scroll bars held JUCE's dark-scheme
`defaultFill` at 0.416 %; they now hold `border-hi` at 0.426 % of the editor's render.

**No interface code on the audio path**, the way tickets `44`, `51` and `52` measured it:

```sh
sample <pid> 8 -file /tmp/xerum53-sample4.txt
```

The `com.apple.audio.IOThread.client` block runs `juce::AudioProcessorPlayer::audioDeviceIOCallback…` →
`XerumAudioProcessor::processBlock` → `refreshSettingsFromParameters` →
`ParameterBridge::currentModelValue`, and contains **0** interface frames out of 121. The
`JUCE v9.0.4: Message Thread` block in the same sample carries `ui::ParameterControl::paint` (×5),
`ui::ParameterPanel::paint` (×3), `ui::ModulationMatrixView::paint` (×3), `ui::ChoiceButtons::paint` (×2),
`ui::WavetableSlot::paint`, `ui::PoleZeroPlane::paint`, `ui::Knob::paint`,
`ui::PillToggle::paintButton` and `ui::SectionTab::paintButton` — the whole packed page drawing — so the
two threads are separated in the running product and not merely in the source.

## Assertions that became false were replaced with reasons

* `plugin/tests/interface_test.cpp`'s `the composition is laid out at the design's baseline window size
  and at 900x600` asserted the parent chain at both fixed sizes and said nothing about whether the page
  fitted either of them — the defect this ticket is about. It is now `the composed page fits the window it
  opens at, and stays legible at 900x600`: the fit is asserted at the default size and the whole parent
  chain is kept at the minimum. Nothing that still held was weakened.
* The strip's click asserted `compositionScrollY() > 0` after `scrollToSection (last)`. At a window the
  page fits there is nothing to scroll, so the case now asserts both halves: at the default the offset
  stays at nought and the page fits; at 900×600 the tab scrolls the page. The strip is a map at both sizes.
* The sections' stacking assertion (`y >= previous.getBottom()`) became "either below the previous band or
  beside it in the same band", which is what the design's `bottom-row` means.
* The matrix's row count was asserted to equal the constant `8`. It is now asserted to equal the geometry
  and to be at least `8`, which is stronger: the heading cannot promise a count the panel does not show.
* One case is **new**, for the colour the window capture found: `every colour a drawn surface resolves is
  a token` walks the editor's own component tree — 1440 components, 4532 colour ids of every stock class it
  contains — and requires each resolved colour to be in `ui::look::palette()` or to draw nothing, then counts
  the editor's own render and requires zero pixels of `#42a2c8`.
* One assertion is **new**, for a defect the compaction would have introduced: the compact band puts a pill
  on the same line as a `knob-sm` and `ParameterControl::resized` gave a toggle its whole row — 48 px tall
  beside a 28 px knob, nearly three times the design's own `.toggle`. The pill is now one line tall wherever
  it sits, and every toggle is asserted to be no taller than `kRowHeight`.

## What could not be seen, and is not claimed

**Nobody looked at it.** Everything above is a count: a rectangle, a height, a colour at a share of the
window, a symbol in a stack. Nothing here says the packed page *reads* as an instrument — whether thirteen
controls in a 350-wide oscillator panel is findable, whether the macros beside `Global` and `Amp Envelope`
is the bottom row the design meant, or whether a window this tall is the right shape for the design's
directions. The dev is the judge, and `docs/design/xerum-direction-warm.html` is the standard.

Also not observed: no knob dragged, no tab clicked, no preset imported through the standalone's dialog,
and the window was never resized by hand (that needs Assistive access this machine does not grant
`osascript`). The resize path is covered in-process at four sizes, including one wider than the default.

# Ticket 48 — the modulation sources that had no generator

Thirty-seven of the matrix's fifty-seven source cases read a field nothing in `src/dsp` or
`plugin/Source` ever assigned, so an editable matrix evaluated a constant zero — worse than no matrix,
because it looked like it worked. `WavetableOscillator::setWarpModulator` was declared and never called,
which made the corpus's most-used warp families (`kPD_OSC` 12.6%, `kFM_OSC` 7.3%) transparent in the
product while passing their own unit tests, because those tests handed the warp a modulator themselves.

## What is generated now

* **The ten LFOs** (`LFO0..9`), from the schema's own `plainParams`: rate (`kParamRate` percent, with the
  reference's `kParamRate10x`, dotted and triplet multipliers), `kParamPhase`, `kParamDelay`,
  `kParamRise`, `kParamSmooth`, `kParamSwing`, `kParamDirection`, `kParamMode` (`Envelope` = one cycle
  per note, `Free` = free-running) and `kParamMono`. The **drawn shape** is `curveData`'s point arrays
  sampled as the preset drew them, including the vertical edge a repeated `x` encodes; the built-in
  shapes (`Sine`, `Triangle`, `SawUp`, `SawDown`, `Square`) cover an LFO that draws nothing, which is
  most of the bank (1411 of ~1554 LFO instances).
* **`LFO n Y`**, each LFO's second output, from `pathData` — and following the curve when the preset drew
  no path, so the source moves rather than sitting at zero.
* **The four matrix envelopes** (`Env0..3`), with attack, hold, decay, sustain, release, the three
  segment curves, start and end. They are **separate generators from the amplitude envelope**, with
  their own state; `in.envelope[0..3]` is theirs.
* **The eight macros**, exposed as eight host parameters (`Macro0..7/plainParams/kParamValue`, one
  one-control group each, because a page is a group and lives in one model module). A macro that nobody
  can move would be the same defect one step smaller.
* **The audio-rate sources**: `OSC A/B/C`, `NOISE OSC`, `SUB OSC` and `Filter 1/2` — the voice's own
  samples, one frame old, which is what makes a cross-oscillator read causal without reading an
  oscillator twice.
* **`setWarpModulator` is called.** The signal a mode's name encodes (`_OSC`, `_OSC2`, `_SUB`, `_NOISE`,
  `_FILT1`, `_FILT2`, `kSelfPD`) is recovered by `dsp::warpSourceFromName` and handed to the warp that
  reads it. The report entry that used to say the variant "is not routed yet" is **retracted** — an entry
  true before and false after is worse than no entry.
* **The player's controls** reach the matrix too: `Mod Wheel` (CC 1), `Aftertouch` (channel pressure),
  `Poly Aftertch` (polyphonic key pressure) and `Pitch Bend`. No `ModulationInputs` was constructed in
  the plugin at all before this, so those sources were as zero as the LFOs.

## What is still not generated, by name

* **The `Lorenz` and `Rossler` LFO types** (`kParamType`, 33 LFO instances in the sampled corpus). The
  preset stores the type's *name* and none of its coefficients, and the reference's own constants are not
  measured; integrating an invented attractor would be inventing the sound. Such an LFO plays its curve —
  so it still moves — and the report names the type at `medium`.
* **`kParamBeatSync`** on an LFO: the rate is a free rate in hertz, and the host's tempo is not applied.
  The corpus never sets the flag (measured: 0 of 178 sampled LFO instances), and the report names it
  where a preset does.
* **`kParamAnchored`, `kParamDefaultMode` and `kParamPhaseSnap`**: carried, not acted on — what the
  reference anchors an LFO's phase to, and its own default mode, are not established.
* **`curveData/loopbackPointNum`**: carried, not acted on. It is not a point index — 481 occurs with
  `numPoints` 3 — so its unit is not established, and the drawn shape loops over its whole cycle.
* **`kParamGridX`/`kParamGridY`** are the curve editor's drawing grid and have no audible meaning.
* **A warp's *variant* is a preset field, not a parameter.** The interface's warp menu selects the
  family (ticket 25's choice list); the signal the mode reads is only in the name the preset stored, so it
  is published from the document. A family chosen from the interface therefore takes that family's
  canonical variant, which is now the plain `_OSC` one rather than whichever the alphabetical enum
  happened to put first.
* **`Voice Mod 1/2`** (sources 56/57) still have no generator: no corpus evidence establishes what
  produces them, so they are left named and zero rather than filled with something plausible. The same
  goes for `NoteOn Rand2` (22), which `modulation.h` already reports, and `ModSlot` (60).
* **`Expr X/Y/Z`** (34/35/36) are MPE axes: `Expr Y` and `Expr Z` map onto CC 74 and channel pressure,
  which xerum reads under their own names above, and `Expr X` is per-note pitch bend, which this engine
  has no per-note place for. None of the three is filled.

## How it is asserted

`tests/dsp/mod_sources_test.cpp` is the generators' arithmetic (a drawn shape sampled as drawn, a
repeated `x` as a step, the envelope's stages and their curves, the rate/direction/swing/delay/rise of an
LFO, the name-to-source decode, and the real-time invariant). `plugin/tests/mod_sources_test.cpp` is the
**whole path from a preset** — `writeContainer` -> `loadPresetFromFile` -> parameters -> engine ->
generators -> matrix -> render — and every assertion there is a render difference: an LFO's rate and
shape, a matrix envelope moving a filter while the amplitude envelope shapes the level, a macro turning
up, the audio-rate sources, and a preset whose `kFM_OSC` warp renders differently from the same preset
unwarped *and identically to it when the oscillator it names is silent*. That last pair is the assertion
ticket 25's unit tests could not make.

# Ticket 55 — moving through a hundred presets

Ticket `54` authored 104 presets into `content/Factory/`. This ticket is the other half, and the dev's
sentence was the brief: **a bank nobody can move through is a folder.** The four things the ticket names,
and what each of them became:

| the ask | what it is now |
|---|---|
| **categories as the first cut**, not a flat list of 104 | the manifest's own eight categories, in the manifest's own order (`category`: "in the order a player moves through them"), drawn as the composition strip's pills with the number of presets behind each; `All` is the ninth |
| **previous/next for auditioning in place** | ◀ ▶ inside the header's preset field, and `[` / `]` from anywhere in the window. **A step loads**, so what you are hearing changes as you move, and either one repeats while held — which is how a hundred sounds actually get auditioned |
| **search by name** | typing reaches the field; it says how many rows match and says `no match` when none does |
| **the keyboard doing all of it** | ↑ ↓ step what the filter shows, ← → move between categories, letters search, Home/End go to the ends, Return or Escape closes, and `p` opens or closes the panel from anywhere |

## Over the manifest, never over the filenames

The browser's list is built from `<content root>/Factory/manifest.json` — the bank's own contract — and
not from a directory listing. A listing would offer whatever happened to be on disk, including a file the
loader rejects, and the ticket's rule is the opposite: **a preset the browser offers is one that loads.**

The reader is `xerum::presets::readBankManifest` (in the bank's own module, `src/presets/factory_bank.cpp`,
next to the directory, extension and manifest name it reads), so the browser and the bank cannot disagree
about the shape of the bank. It is deliberately intolerant in one direction and tolerant in the other:
unknown members are ignored, but a manifest with no `presets` list, a preset with no name or no path, or a
preset filed under a category the manifest does not declare is **an error and an empty catalogue** — never
a half-built one a caller could browse by mistake. `tests/model/factory_bank_test.cpp` holds it to the
real file and to four damaged ones.

## Two doors, and they must not look alike

xerum has exactly one way in for a user's **own** preset: the `Import .SerumPreset…` button, which opens
a file dialog on the reference's format and leaves what it could not honour in the report bar. The browser
is the other thing entirely, and the ticket's requirement is that a user can tell. Four things carry it,
each of them asserted rather than described:

1. **the words.** The panel is titled `FACTORY`, its footer reads *"Factory bank — xerum's own presets.
   Your Serum presets arrive through Import .SerumPreset…"*, and every name the panel offers is spelled
   on the header field as `Category · Name`. A preset that arrived any other way has **no category in the
   manifest**, so it has no prefix — the same field, told apart by whether it says where the preset is
   filed;
2. **the list.** Every entry the browser can offer is a `.xerumpreset` under `Factory/` — asserted over
   the whole list, as a property of every entry, so it would fail for a row added anywhere;
3. **the surface.** Import is an explicit file action that leaves a report in the bar at the bottom; the
   browser is a panel over the page and leaves nothing behind;
4. **the door.** The browser has **no file dialog at all**. Nothing in it can reach a file that did not
   come from the manifest.

The test that measures all four loads a `.SerumPreset` fixture through the import path and reads the
header field back: it must carry no category and point at no row.

## One code path: `loadPresetFromFile` now reads both of xerum's own formats

The ticket asks that a preset selected in the browser load "through the product's own loader
(`loadPresetFromFile`) … no second code path", and at the start of this ticket **that was not true**:
`PresetState::loadPresetFile` read only the reference's container (`model::readContainer`), and a
`.xerumpreset` is xerum's own block — `model::encodePresetBlock`, the bytes `getStateInformation`
writes. The bank's own test loaded its files through `setStateInformation` for exactly that reason.

So the ticket changed the loader rather than adding a second call beside it. `loadPresetFile` now tries
`model::decodePresetBlock` **first** — a fixed-string comparison against the block header, so a
reference container pays one `memcmp` — and falls through to the container reader. The two formats are
told apart by the file's own magic, not by which call site opened it, and a file that carries xerum's
magic and does not decode is reported *as a damaged xerum preset* rather than as "not a Serum container"
(true, and not the thing to tell someone whose bank file is corrupt). One door, both formats.

The consequence for the ticket: **the browser's loader is the same call the import button makes**, and
the bank is read by the format the product already round-trips. `loadPresetFile`'s header now says both.

## The composition is at capacity, so the browser is an overlay

Ticket `53` packed the page into a window whose height is *derived from the page*, and its test measures
the page against the viewport. A dropdown, a list or an overlay added to the composition would cost that
fit, so the browser is not in the composition at all: it is a **sibling** of the composed page, placed
over the strip and the page and hidden until it is asked for. `resized` positions it and the page is laid
out below it, so opening the browser changes no section's height and no panel's bounds — and the test
measures the page at both window sizes **with the browser open and with it shut** and requires the same
number each time.

The header's two arrows are the design's own wavetable-slot arrows at the design's own 16px, and they are
laid out **inside the width the header already gave the preset field** — the same construction the design
uses for its wavetable slot, whose field carries its own arrows. Nothing else on the header moved:

| | 1080×1108 (the window it opens at) | 900×600 (the minimum) |
|---|---|---|
| header inner width | 1060 | 880 |
| wordmark 84 + gap 6 + preset field 240 | 330 | 330 |
| left for the coverage readout and the right-hand controls | 730 | 550 |
| **coverage readout** | **278** | **98** |
| import 168 + gap 8 + status 260 | 436 | 436 |
| preset field, and each arrow inside it | 240×22, 16×16 | 240×22, 16×16 |
| the composed page, browser open **and** shut | **1018** | **1144** |

(The page at 900×600 is taller than that window — that is ticket `53`'s own finding about the *minimum*
window, and the browser is measured against it rather than against a number it cannot reach.)

If the space had genuinely not been there, this would have said so with the arithmetic instead of
overflowing; it was there, because the design already puts arrows inside a field.

## The panel, as rectangles

Measured by `plugin/tests/interface_test.cpp` at 1080×1108 and printed, the way ticket `53` prints its
composition. The panel covers `1080×1048` at `0,38` — the strip and the page, with the header above it
(so the field and the arrows that step the bank stay usable while browsing) and the report bar below it
(so what a load reported stays on screen):

```
heading  1060x16  @10,10          FACTORY, with "104 presets · 8 categories" as the design's tag
search   1060x22  @10,30          the design's own field, caret in the accent
pill All         29x20 @10,56     <- selected
pill Bass        35x20 @43,56     ... and the manifest's other seven, in its order, each with its count
list     1060x924 @10,80          46 rows of 20px
row 0    1060x20  @10,80          Molten Sub                    (the cursor's row carries accent-bg)
footer   1060x30  @10,1008        the selected preset's description, and the two-doors sentence
```

## Acceptance, measured

```sh
ctest --test-dir build-55 -j8                                  # every suite
build-55/plugin/tests/xerum-plugin-tests "[browser]" -s        # the two cases this ticket adds
```

The ticket's warning was that a browser can pass every assertion that merely *describes* it. So every
assertion this ticket adds is a **difference** — what changed when the gesture happened — and each of
them fails for a browser that is a picture of a browser:

| the claim | the measurement |
|---|---|
| stepping the bank loads the bank | all **104** slots loaded through `loadPresetFromFile`, each load's document naming the manifest's own preset, and **104 distinct documents** by parameter digest (the name is deliberately *not* in the digest) plus 104 distinct names |
| and the sound changes | the bank's slot 0 against its slot 52, rendered through the engine the browser loaded them into: **channel difference 1.8818** (floor 0.01) |
| a search that matches nothing shows nothing | `visibleCount() == 0`, `visibleRows()` empty, no row rectangle, **and the loaded document's digest is unchanged** — a filter that fails open is worse than no filter |
| a search that matches one thing loads it | the unique query is *derived from the bank* (the longest name no other name contains), and the loaded document is that preset's, not the one that was playing when it was typed |
| the categories are the manifest's, not the browser's opinion | the expected count per category is counted from the manifest **by a second parser** (JUCE's JSON) and compared with the rows; the rows are the manifest's own subsequence, and stepping wraps **within** the category |
| the two doors | the list holds no `.SerumPreset`; the import button's text names it; and after importing a `.SerumPreset` fixture the header field carries **no category** and the browser points at no row |
| the fit | the composed page is **1018** at 1080×1108 and **1144** at 900×600, each measured **with the browser open and with it shut** — the same number both times, at both sizes, with the header's rectangles above unchanged |
| the browser changes nothing outside itself | the window rendered with the browser open against the same window with it shut: **0 of 64800 pixels outside the browser's own rectangle differ** |
| the palette | **94.28 %** of the rendered window is exactly a `ui::look` token; the largest non-token colour is `rgb(47,38,26)` at 1.79 % — `accentBg`'s 10 % alpha composited over `surface`, which is what the design's `--accent-bg` *is* (`rgba(232,155,48,0.10)`) |
| **the panel drew where its rectangles say** | a point inside each rectangle, chosen where no glyph is, carries exactly that part's fill: the search field is `ui::look::bg` (`111110`), a row that is not the cursor's is the panel's `surface` (`1A1917`), and the cursor's row **and** the selected pill are both the accent composited over the panel (`2F261A`, against `Colour::overlaidWith`'s `2F2619` — one unit of JUCE's own blend rounding). This is the closest a session with no eyes gets to looking at it |
| the VST3, as a host sees it | `pluginval --strictness-level 10 --validate build-55/plugin/xerum_artefacts/Release/VST3/xerum.vst3` → **SUCCESS**, with its `Editor`, `Open editor whilst processing` and `Editor Automation` tests run (they construct, show, resize and destroy this editor, which is what the browser is a child of) |
| the standalone, launched | its window read from the window server: **1082×1136 at (739, 219)**, layer 0, owner `xerum` — the editor's own 1080×1108 plus the 1 px frame and the 28 px title bar, which is ticket `53`'s measurement reproduced by this build. Measured at 12:32 on the build that first carried the browser, field and editor wiring; the re-launch after the last three cleanups did **not** put a window up (see below) |

The renders at the four steps (the first preset, the last, a search narrowed to one, a search narrowed to
none) are written as PNGs to `<build>/browser-evidence/` by that case — 1080×1108, the window the host
opens — and copied to `.scratch/xerum/evidence/browser/` with the ticket, so the drawing can be looked at
rather than only asserted. **That is the half of the acceptance this session could not check itself**: a
person has to look at them.

## Assertions that became false were replaced with reasons

**None of ticket `53`'s assertions changed at all** — its fit case, its colour-id tree walk and its render
measurement pass unmodified, and the diff of `plugin/tests/interface_test.cpp` against the previous ticket is
four added includes and one appended block, with **no deletions**. That is the strongest form of the
requirement: nothing was replaced because nothing that ticket asserted became false. Two things did move, and
both are in the editor's *drawing* code rather than in what is asserted about it:

* **the header's preset field is no longer painted by the editor's `paint`.** It is a component
  (`ui::PresetField`), because it is now clickable and carries the two arrows. Its rectangle is the one the
  header laid out before — the arrows are inside that width — and both the drawing and the layout now read a
  single `HeaderLayout`, so what the evidence prints is what was drawn;
* **the header's rectangles became numbers a case can read** (`coverage` 278 at the default window, by the
  arithmetic above) rather than ones inferred from the drawing code.

## What could not be seen, and is not claimed

* **The last standalone re-launch never put a window up.** The 12:32 measurement above (1082×1136 at
  739,219) is real, taken with the browser, the field and the editor wiring already in the build; the
  re-launch after the final three cleanups (two unused `BankCatalog` fields removed, `kRowHeight` made
  private, and the browser/drawer exclusion) sat with **no window for over a minute** and was killed.
  `sample` puts its main thread in the processor's **constructor** —
  `applyDocument → resolveContent → TableLibrary::loadTable → directory_iterator → open$NOCANCEL` — with
  **no frame of this ticket's code anywhere in the stack**, so it is the init document's content
  resolution blocking on a directory open, not the browser. `[INFERENCE]`: the source content root is
  under `~/Desktop`, which macOS guards with a consent prompt, and a rebuilt ad-hoc-signed binary is
  re-asked; a prompt nobody can answer is consistent with a blocked `open()` and with the crash reports
  `xerum-host` left in the same window. What is **not** in doubt: `ctest` (356/356, 166.63 s) and
  `pluginval` level 10 (SUCCESS) both ran on that same final build, and neither reaches that code path.
* **A pixel capture of the running standalone's window.** The window is there and its geometry was read
  from the window server, but **none of the three ways to take its picture works on this machine**: both
  `screencapture -l<id>` and `screencapture -R<rect>` report *"could not create image"*, and
  `CGWindowListCreateImage` (resolved by `dlsym`, since the macOS 27 SDK marks it obsoleted) returns a
  correctly-sized image that is **100 % one colour** — Screen Recording permission is not granted to this
  session's tools. `osascript` cannot drive the window either: *"osascript does not have permission to
  send keystrokes"*, so the browser could not be opened **in the running product** by a script. The four
  renders above are therefore from the product's own paint path (`createComponentSnapshot`) on the same
  editor, at the same size the standalone's window measures, and that difference is stated rather than
  papered over. The capture tool is `.scratch/xerum/tools/window-capture.swift` (local, not committed).
* **Whether browsing is *pleasant*.** Nothing here can say. What is asserted is that a step changes what
  is loaded, that the keyboard reaches every gesture without a mouse, that a search narrows to the row it
  should, and that nothing the design fixed moved. Whether the panel is the right size to browse
  comfortably is the dev's call.
* **Whether the presets sound good.** Ticket `54`'s suite renders them and refuses two that render the
  same; no assertion in this ticket or that one says a preset is worth playing.
* **Whether the panel's own drawing looks right.** The session has no eyes: the PNGs above were produced
  from JUCE's paint path and are printed with their palette shares and their rectangles, and that is the
  whole of what can be said here.

# Ticket 57 — the amp envelope is prominent, the matrix is quiet

The dev's editorial call, and the first answer to this map's fog about *which* controls deserve the front
panel: **the matrix is too prominent and the ADSR is not prominent enough.** The composition is data, so
that turned out to be an answerable question rather than a matter of taste — and the answer is a pair of
rectangles.

## What was wrong, measured before it was changed

* the amp envelope sat in the **fall-through** section, sharing the half of the design's bottom row the
  absent FX section leaves: a **367×118** panel third-from-nothing on the page, for the control a player
  touches on every patch;
* the matrix was the composition's **flexible row** — the design's `1fr`, hardwired as
  `SectionKind::Matrix` in `CompositionView::layout` — and it was the tallest band of the lower half of the
  page: **1060×216**, 8 of its 64 rows, sitting in the middle of the instrument.

## What changed, and it is all data

| | before | after |
|---|---|---|
| the composition | `osc-row`, `noise-sub-row`, `filter-section`, `mod-panel`, `macro-section`, `default-section` | `osc-row`, `noise-sub-row`, **`env-section`**, `filter-section`, `macro-section`, `default-section`, **`mod-panel`** |
| the flexible row (the design's `1fr`) | implied by `SectionKind::Matrix` | `SectionSpec::flexible`, an entry's own flag — carried through to `SectionPlan` — and it is the **envelope's** |
| the matrix's rows | 8 of 64 | **4** of 64 |
| the matrix's place | between the filters and the bottom row | **the end of the composition**, after the macros |
| the envelope's band | a 367×118 panel in the bottom row | a **1060×105 band of its own**, third row, four knobs at 44px |

The claim on the slack became a fact about an **entry** rather than about a **section kind**, which is what
let one data edit move it: `CompositionView::layout`'s slack block reads `SectionPlan::flexible`, and the
matrix keeps its own index in the band because it still has a grid to place and an `N / 64` caption to keep
honest. Two names in one struct, because ticket `57` made them two facts.

**The matrix's four rows** are half the design's eight and few enough that the page's last band (128) is no
taller than the bottom row above it (130) — that is the arithmetic behind the number, and the tag says
`4 / 64` so the loss is stated rather than hidden.

## Who absorbs the slack: the measurement

The ticket's own acceptance, first, as the pair of numbers it is. The editor's default height is **derived
from the page** (ticket `53`), so the honest way to ask "who takes the slack" is to ask at more than one
size; all five lines below are printed by
`plugin/tests/interface_test.cpp`'s `the composition's slack is the envelope's, and the compact matrix takes
none`:

| window | viewport | page | `env-section` | `mod-panel` | matrix rows |
|---|---|---|---|---|---|
| **1080×1128** (the editor's own default) | 1060×1038 | 1038 | 105 | 128 | 4 |
| 1080×640 | 1060×550 | 1038 (scrolls) | 105 — its natural height | 128 | 4 |
| 900×600 (the minimum) | 872×510 | 1093 (scrolls) | 105 | 128 | 4 |
| **1080×1428** (the default, 300 taller) | 1060×1338 | 1338 | **405 (+300)** | **128 (+0)** | 4 |
| **1400×1128** (the default, 320 wider) | 1380×1038 | 1038 | **176 (+71)** | **128 (+0)** | 4 |

* **the envelope grows by exactly the slack and the matrix does not grow at all.** At 300 px of extra
  window the envelope's band is 300 px taller and the matrix's is the same 128 it is at every other size —
  which is the change, in one measurement, and every other section's height is unchanged too (asserted
  per section, not just for these two);
* **at the default window the slack is nought**, and that is a measurement rather than an assumption: the
  envelope's height at 1080×640 — a window smaller than the page, where nothing can be handed out — is its
  natural 105, and it is 105 at the default window as well. Ticket `53`'s "the page equals the window" is
  why: the window's height *is* the page's own need, so the `1fr` row is empty at the size the plugin opens
  at. It was empty for the matrix too — 216 is exactly 8 rows of its grid — so the matrix was the tallest
  band of the lower page at the default window because of its **natural** height, not because of slack;
* **a wider window is the other way a user produces slack**, and the last row of the table is that gesture:
  at 1400 wide the panels re-flow, the page needs 71 px less, and those 71 px are the envelope's.

## The section title order, and the two bands' controls, as measured

```sh
cmake --build build-57 -j8
build-57/plugin/tests/xerum-plugin-tests "[interface]"      # 7210 assertions, 23 cases — all passed
ctest --test-dir build-57 --output-on-failure               # 368/368 — all passed, 176.03 s
/tmp/pluginval-104/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 \
    --validate build-57/plugin/xerum_artefacts/Release/VST3/xerum.vst3   # SUCCESS, Editor tests included
```

The order the strip draws, read off the resolved composition at the default window:

| # | section | title | rectangle | holds |
|---|---|---|---|---|
| 0 | `osc-row` | `OSCILLATORS` | 1060×344 @0,0 | Osc A/B/C |
| 1 | `noise-sub-row` | `NOISE + SUB` | 1060×144 @0,347 | Noise, Sub Osc |
| 2 | **`env-section`** | **`AMP ENVELOPE`** | **1060×105 @0,494** | Amp Envelope — four 44px knobs, full width |
| 3 | `filter-section` | `FILTER` | 1060×172 @0,602 | Filter 1, Filter 2, their planes |
| 4 | `macro-section` | `MACROS` | 314×130 @0,777 | the eight macro items |
| 5 | `default-section` | `OTHER` | 740×130 @320,777 | Global (the fall-through, one group) |
| 6 | **`mod-panel`** | **`MOD MATRIX`** | **1060×128 @0,910** | the grid, `4 / 64` |

**The controls, in px, on screen rather than in a constant**: the envelope's four knobs are **44×44**, mode
`Dial`, `KnobSize::Default` — the design's largest diameter, the same the oscillators and the filters draw,
larger than the compact band's 28 and the macros' 26. A matrix cell is **not a dial**: mode `Cell`, one row
of the grid less its border, inside a 22px row (`ModulationMatrixView::kRowHeight`). So "the envelope has
the largest knobs and the matrix has none" is literally what the two sections contain.

## The render, and its palette shares

`createComponentSnapshot` of the editor at **1080×1128** — the window it opens at — written to
`<build>/interface-evidence/ticket-57-default-window.png` by that same case, and counted by
`every colour a drawn surface resolves is a token`:

| colour | token | share of the window |
|---|---|---|
| `rgb(26,25,23)` | `surface` (panels) | **60.444 %** (was 53.389 % at 1080×1108) |
| `rgb(17,17,16)` | `bg` (window) | **24.482 %** (was 29.345 %) |
| `rgb(42,40,38)` | `border` | 3.075 % |
| `rgb(34,33,32)` | `surface2` | 2.903 % |
| `rgb(232,155,48)` | `accent` | 0.188 % |
| `#42a2c8` | JUCE's own dark-scheme fill | **0 px** |

Panel surface up 7 points and window background down 5 is the envelope's full-width band and `Global`'s
wider one: the page is more instrument and less empty space. The token walk resolves 1443 components and
4532 colour ids with **none outside the palette**; the preset browser's overlay still differs in **0 of
64800** pixels outside its own rectangle; and the lazy-editor, headless and audio-path cases are untouched.

## Assertions that became false were replaced with reasons

* `sectionOf ("Amp Envelope") == "default-section"` → **`"env-section"`**: the envelope has a section of its
  own. `Global` is still the fall-through, and the same edit moved no other group;
* `fellThrough == { "Global", "Amp Envelope" }` → **`{ "Global" }`**, and the fall-through's place in the
  composition is still the bottom row;
* `defaultSection == sectionCount() - 1` → **`defaultSection == matrixSection - 1`**: the matrix is last
  now, and the bottom row is the page's last but one;
* the section ids by index (`filter-section` was 2, `mod-panel` 3, `macro-section` 4) → the new order, with
  `sectionForModule ("VoiceFilter0") == 3` and `sectionForModule ("ModSlot") == 6` following it;
* `composition.sectionBounds (macroSection).getY() > sectionBounds (matrixSection).getY()` → the bottom row
  is below the **filter** section and the matrix is below the **bottom row**;
* `matrixVisibleRows() >= kMatrixVisibleRows` → **`== kMatrixVisibleRows`**: the "at least" existed because
  the matrix grew with the slack, and it no longer does. `kMatrixVisibleRows` itself went `8` → `4`;
* the matrix's placement comment ("below the filters, as tall as the design's 8 / 64") is replaced by the
  compact-last one, and `matrixNatural` in `layout` is now the matrix's whole height rather than its floor;
* `editor->setSize (1080, 1108)` in the token walk → **the editor's own default size**, because the number
  is derived from the page and pinning it measures a window the plugin no longer opens at. That is exactly
  what ticket `57` did to it.

Ticket `53`'s fit assertions are **extended rather than weakened**: both sizes still assert every section's
rectangle inside the page, every panel inside its section and every control inside its panel, and the new
case adds the slack measurement on top. Nothing in that case was loosened.

## What could not be seen, and is not claimed

* **The live standalone window's pixels.** The window **does** come up in this build — the re-launch that
  ticket `55` recorded as hanging is fixed by the ticket `56` work now in `src/content` — and its geometry
  was read from the window server: **1082×1156 at (739,199)**, owner `xerum`, layer 0. That is this ticket's
  default window, end to end: the editor's own **1080×1128** plus the 1 px frame and the 28 px title bar, so
  the *product* opens at the size the layout asks for and not only the harness. What could not be read is
  its colour: `.scratch/xerum/tools/window-capture` (local, not committed) returns a **redacted** image —
  1040 distinct colours whose red channel is forced to 255 and whose green/blue channels are the real render
  (`rgb(255,26,25)` for `surface` at 58.39 %, `rgb(255,17,17)` for `bg` at 25.49 %), i.e. Screen Recording
  permission is still not granted to this session's tools. `[INFERENCE]`: that distribution is the same
  distribution as the editor's own render below within ~2 points (58.4/25.5/3.5/2.9 against
  60.4/24.5/3.1/2.9), which is consistent with the running product drawing this composition — but it is not
  a second measurement of the palette and is not claimed as one. (A `Problem Reporter` window for `xerum`
  was also on screen at that moment, from a launch before this one; this build's own launch stayed up, and
  the kill below was deliberate: `pkill -f Standalone/xerum`.) The renders and rectangles above are the
  editor's own paint path and layout, in-process, at the sizes named;
* **whether the envelope *reads* as the centre of the instrument.** That is the dev's call and this session
  has no eyes. What is claimed is the geometry: the envelope's band is 105 px and the matrix's 128 at the
  default window, and the envelope's is the one that grows. The matrix's band is still the taller of the two
  by 23 px, and the honest reason is arithmetic rather than preference — a heading plus one row of 44 px
  knobs with their readouts and labels is 105, and a twelve-column grid with its column header and four rows
  is 128; flipping that ratio needs the matrix to show **two** rows or fewer, which stops being a matrix;
* **what the envelope's slack *is*.** Measured and stated: at 1080×1428 the envelope's band is 405 px, of
  which 105 are its own content and **300 are air**, because the design system has no envelope content that
  uses height — there is no envelope graphic in the design, and drawing one is not this ticket. A 44 px knob
  row in a 405 px band is air around controls, not a filled hero panel; whether that is the prominence the
  ticket wanted, or whether the envelope needs a designed display to hold the space, is the dev's call.
  Reported rather than papered over, which is what the ticket asked for.

# Ticket 58 — the master ceiling

## What it is, and the first deliberate divergence from the reference

The dev's report was that **the audio turns red when more than one key is held**, and the ask was a limiter at
the end of the flow, modelled on **Ableton's Limiter at its defaults**. Two facts established before any code:
there is **no limiter or soft-clip leaf in the reference's decoded schema**, and nothing in `src/dsp` clamped
the output. So this is a new **master stage**, and it is the project's **first deliberate divergence from the
reference** — stated here rather than absorbed.

**It is a parameter, not a hard-coded stage.** `Master Limiter` (on/off) and `Master Ceiling (dB)` are on the
parameter table like everything else, so a user who wants the reference's own output turns the first one off
and gets a wire. The default is **on**, because the dev asked for the red to stop. The two paths are
`Master0/plainParams/kParamLimitOn` and `Master0/plainParams/kParamLimitCeiling`, and they are the table's only
parameters the schema does not describe — `bridge_test.cpp`'s `isXerumsOwn` names them rather than loosening
the rule, so the rule still binds every other parameter.

## Where the reference's numbers come from

**Ableton's own factory default preset**, not recall and not the ticket. In the installed
`Ableton Live 12 Suite.app`:

* `Contents/App-Resources/Core Library/Defaults/Audio Effects/Limiter.adv` — gzipped XML, a `Limiter` device.
  Decompressed, its defaults are `Gain = 0` (unity), **`Ceiling = -0.2999997139`** (**-0.30 dB**),
  **`Release = 99.9999542`** (**100 ms**), `AutoRelease = false`, **`Lookahead = 0`** — index 0 of the
  enumerated 1.5 / 3 / 6 ms, so **1.5 ms** — `Routing = 0` (L/R), `Mode = 0` (Standard) and
  `LinkAmount = 1` (100 %).
* `…/Max/Max.app/Contents/Resources/C74/packages/ableton-dsp/docs/abl.device.limiter~.maxref.xml` — the
  enumerations, confirming `enumvals` of size 3 for the lookahead (`1.5 ms`, `3 ms`, `6 ms`), of size 3 for
  the ceiling mode (`Standard` first) and of size 2 for the routing (`Left/Right` first). Integration could
  not reach these from the published DSP reference, which serves boilerplate; the installed application
  does have them.
* The Live 12 manual's effect reference (**§29.24 Limiter**) supplies the *shape* rather than the numbers: the
  ceiling "sets the absolute maximum level that Limiter will output", "if your incoming signal level has no
  peaks that are higher than the ceiling, no limiting will be applied", `Standard` is "the device's default
  mode", and lookahead "introduces latency: the longer the lookahead time, the higher the latency".

Both the preset and the reference XML were read on this machine on 2026-10-15; the values, the artefact paths
and the manual section are recorded in `src/dsp/limiter.h`.

## Where it sits, and what it is not

**Last, after the effects rack** — a master stage outside the rack, not a slot in it. In `processBlock` the
order is: the voice pool's stereo render → the **master gain** (ramped, unchanged) → the **FX rack** → **this
stage** → the host's channel mapping. The ceiling is deliberately after the master gain, because it has to see
what the master gain produced rather than the host's mapping of it.

One thing that was true when this section was written and is not any more: **the `FX` rack was built and
instantiated nowhere** — `dsp::FxRack`/`FxBank` existed, with 14 shipped classes, and nothing in
`XerumAudioProcessor` or `VoiceEngine` constructed them, so "after the rack" meant "after the master gain".
That gap was ticket `60`'s and is closed below: the rack now sits between the master gain and this stage, and
it is ticket 60's section that carries the measurements. This paragraph is left standing rather than deleted
because the sentence it corrects was load-bearing for ticket 58's placement — the ceiling has to stay last.

## Latency, which is declared

A lookahead limiter delays what it passes. The stage delays by **its lookahead in samples** —
`round(1.5 ms × sampleRate / 1000)`, so **66 samples at 44.1 kHz, 72 at 48 kHz, 144 at 96 kHz** — and the
plugin declares it with `setLatencySamples` from `prepareToPlay`. An unreported delay breaks every host's
delay compensation, which is a worse bug than the clipping it fixes.

**The declared latency does not move with the on/off parameter**, and that is deliberate: the delay line runs
whether the ceiling is enforced or not, so a host that has compensated for it stays compensated when the
parameter moves under automation. It also means the off state is not a bypass of the *stage*, only of the
ceiling — the output is the input exactly, delayed by the declared number.

For anything outside a host the compensation is explicit, and the two places that need it do it:
`plugin/tests/fidelity_test.cpp` renders `latency` samples long and drops the front (see below), and the
window boundaries in `plugin/tests/audio_test.cpp` are taken at the plugin's own latency rather than at a raw
block edge.

## The invariant that protects the work underneath

**When the ceiling is not engaging, the render is identical with it enabled and disabled — bit-identical, not
close.** Two things make that true rather than merely true-ish:

* the required gain is *exactly* `1` for any sample within the ceiling (the branch is a comparison, not a
  division that lands near 1), and the envelope never leaves unity in that case;
* the envelope state is a **`double`**. As a `float` the release adder saturates: the step `(1 - gain) ×
  coefficient` falls below half an ulp of 1.0 and the gain sticks about `3e-8 × releaseSamples` short of
  unity — roughly `-77 dB` at the default release — and *never arrives*. In `double` the decay continues until
  the perturbation is far below a float ulp, so the multiplication **rounds to the delayed sample exactly**.
  The invariant therefore holds *after a release*, not only before the first engagement, which is what a
  preset that engaged once on a loud note and then went quiet needs.

The brickwall is a separate line from the envelope: the applied gain is clamped to `ceiling / |sample|`, so no
transient, no ramp rate and no detector failure can put a sample over the ceiling. The attack ramp only shapes
the *approach*.

## Acceptance, measured

Command: `./build-58/plugin/tests/xerum-plugin-tests "[limiter]"` and
`./build-58/tests/dsp/xerum-dsp-tests "[limiter]"`, 48 kHz / block 512, ceiling at its default −0.30 dB
(linear **0.966051**).

| measurement | value |
|---|---|
| **16 voices held, ceiling off** (both channels) | **7.740814** — above full scale |
| **16 voices held, ceiling on** | **0.966051 / 0.966051** — exactly the ceiling, at the sample level |
| 1 voice, ceiling off | 0.9999996 — full scale to the last bit |
| 2 voices, ceiling off | 1.999630 |
| a signal below the ceiling, ceiling on vs off | **bit-identical, every sample** (8 192-sample and 12 288-sample renders) |
| after an engagement and a release, ceiling on vs off | **bit-identical from sample 120 000 on** |
| declared latency | 72 samples at 48 kHz (66 at 44.1, 144 at 96) |

The counterfactual is what makes the first pair mean something: the same render with the ceiling **off** does
exceed full scale (7.740814), so the "never exceeds 0 dBFS" assertion is not describing an already-safe
signal. `[dsp][limiter]` carries the same shape at the algorithm level, including a four-sample hard
transient, which is the case a lookahead exists for.

**Since ticket `63` these rows are the counterfactual, not the default patch.** They were measured with the
two level leaves at unity, which is now the state `[gain-staging]`'s `xerum-init-unity` row reproduces to the
digit by forcing those two back to `1.0`. The product's default carries them, so its own rows are
**0.198638** at one voice (−14.04 dBFS) and **1.537620** at sixteen — and the ceiling-on render of one voice
is now **bit-identical** to the ceiling-off render, i.e. the limiter does not engage on a single note at all.

## The gain-staging finding, and what ticket `63` did about it

**The voices sum with no headroom, and that is why a chord goes red.** Measured with the ceiling off, init
patch, velocity 127, notes 36/39/42…:

* with the two level leaves at unity, **one voice peaks at 0.9999996** — full scale to the last bit: a table
  whose peak is 1.0 through an oscillator level of 1.0 and a master gain of 1.0. There is no per-voice trim
  and no mix-bus trim anywhere between the oscillator and the output;
* **two voices are 1.999630** — about twice one voice (the two peaks do not land on the same sample, which
  is the only reason it is not exactly 2×);
* **sixteen voices are 7.740814** — about 7.7 times, sub-linear for the same reason.

**Nothing was re-voiced.** The engine's gain structure is exactly ticket `58`'s: no per-voice headroom
factor, no mix-bus trim, no lower table peak. What ticket `63` changed is the **default patch**, which now
carries the reference's own two level leaves at the reference's own values — so the default patch renders one
voice at 0.198638 (−14.04 dBFS) and sixteen at 1.537620, and a single note no longer reaches the limiter at
all while a chord still does. The per-voice-headroom question the old paragraph here left open is therefore
still open; it is just no longer what makes the second key red.

## Fidelity: the corpus bypasses it, and says so

`docs/test-strategy.md`'s fidelity layer renders each fixture through xerum and compares it against a golden
captured from the **reference**, which has no such stage. So the layer **bypasses the ceiling** — it sets
`Master Limiter` to off after the document is applied — and **compensates the declared latency** by rendering
`latency` samples long and dropping the front. The alternative the ticket offered, regenerating the corpus
with the ceiling on, would bake xerum's divergence into the reference's side of the comparison and the
divergence would stop being measurable; bypassing keeps the layer measuring the *engine*, which is what it
exists for. `[fidelity-stimulus]` is untouched and still byte-identical, because both of its sides render
through the same plugin with the same declared latency.

## What could not be judged

**Everything about how it sounds.** This session has no ears. The numbers above establish that the output
cannot exceed the ceiling, that a signal below it is bit-identical, that the latency is declared and
compensated, and that the reduction begins one lookahead before the peak. They do **not** establish that the
release does not pump, that the attack does not dull transients, or that the result is what Ableton's device
would have done to the same signal — this is a limiter with the reference's *defaults and shape*, not a model
of its internals. Release, lookahead and the link are the reference's; the attack ramp, the detector and the
release curve are xerum's, and the dev's ears decide whether that is good enough.

# Ticket 59 — one fixed ratio, and the amplitude envelope drawn

Two requests from the dev, which belong together because both are about the envelope's section and the
editor's geometry: **the interface should stop re-flowing and take a fixed ratio**, and **the amp envelope
should be a graph rather than only four knobs**.

## 1. The ratio is the composition's own, and the layout is given no window

Ticket `57` had measured the re-flow exactly — widening the window by 320 px let the page need 71 px less,
and the amplitude envelope took those pixels. That is what is gone: `CompositionView::layout` now takes a
**width and nothing else**, the page is laid out **once**, and the whole interface is one `Canvas` component
that is **scaled** into whatever window the host gives.

`ui::XerumAudioProcessorEditor::Canvas` is the only component whose transform is ever set, and that is not a
preference: `juce_AudioProcessorEditor.cpp` asserts `getTransform() == hostScaleTransform` and its own
comment names this remedy ("put the component you want to transform in a child of the editor and transform
that instead"), because a host must be able to rescale the editor itself. Every component the editor shows —
the page, the strip, the report bar and its drawer, the browser, the header's field and buttons — is a child
of the canvas, and **every rectangle in that tree is a nominal one**, which is what makes ticket `51`'s
"the design's sizes bind at the nominal size" a statement rather than an assumption.

| | |
|---|---|
| nominal size | **1080×1128**, derived in the constructor: the design's baseline width and the page's own height (1038) plus the design's chrome (38 + 30 + 22) |
| the ratio | **0.957447** wide per tall — the composition's own proportions |
| the page's own proportions | 1060×1038 = 1.021195 (the chrome is what makes the editor's ratio differ from the page's) |
| the resize limits | scales of the nominal: **900×940** (the design's baseline *width*, 900, ticket `51`'s own size) to **2160×2256** (2×), enforced as a **fixed aspect ratio** on the constrainer — which is also the constrainer JUCE's standalone window asks for its own sizes |
| a window off the ratio | letterboxed: uniform scale (the smaller of the two), centred, never stretched. A non-uniform scale draws circles as ellipses, which is a second defect rather than a smaller one |

**What the design mock's own ratio would cost**, measured on the editor at its own size rather than argued:
at **1082×668** (1.62 wide per tall) the canvas is scaled by its height, **0.5922**, so the page is drawn
**628×615** and **58.0 % of the window's width is used — 42.0 % of it is background**. Scaled to fill that
width instead (1.0019) the same composition would need **1130 px of height, 462 more than the mock has**.
The mock's ratio is available by changing one derived number, but it is not this content's shape.

**The page still fits at every size, and it cannot scroll at all.** The page is laid out at exactly the size
of the space it is given and that space is scaled with it, so there is no slack and no fold: the
`juce::Viewport` around the composition, its scroll bars and the four-pass fixed-point search that existed
only because a bar's visibility changes a viewport's visible size are all **deleted**. The strip's click is
now a selection rather than a scroll — which is all it ever was at a window the page fitted.

**The three assertions this re-scopes rather than breaks**, each replaced with its reason in
`plugin/tests/interface_test.cpp`:

* **ticket `51`'s "no control got smaller"** — the design's sizes bind **at the nominal size**, and the ratio
  scales everything from there. The 44/28/26 px assertions still read those numbers because every rectangle
  in the tree is a nominal one; what the user sees at the minimum is 44 px × 0.8333 = 36.67 px, and the fit
  case *prints* the type sizes at that scale (9 px → 7.50, 8 px → 6.67, 7 px → 5.83) rather than asserting
  legibility, which is the dev's call;
* **ticket `53`'s fit** — there is no viewport height to compare a page height against, so the fit is
  asserted where it can be seen: every section's rectangle **on screen** is inside the editor's, at the
  nominal size, at 1.25×, at the minimum and at a size a host has forced off the ratio. Ticket `53`'s and
  `51`'s control chain (every panel inside its section, every control non-empty, bound and inside its panel)
  is walked at all four;
* **ticket `57`'s slack measurement** — at a fixed ratio the envelope does not grow *by the slack*; it grows
  **proportionally**, like every other section, and its share of the page is a constant. Both readings are
  asserted: the rectangles are *equal* at every scale (the layout never sees a window), and every on-screen
  rectangle is the nominal one times `interfaceScale()`. `SectionSpec::flexible` — the design's `1fr` — is
  deleted, because a row that grows when the window does is a section re-flowing and there is no window in
  the layout call to grow against.

## 2. The envelope's curve, derived from the same four parameters the knobs are

Ticket `57` was told not to invent an envelope graphic and to report if prominence needed one; the dev has
now asked for it, so `ui::EnvelopeCurve` is that drawing — **an invention, in the design system's own
language** rather than a second dialect: the tokens (`bg`, `border`, `accent`, `accentDim`, `fg`, `fgMuted`,
`accentBg`), the panel radius, the monospaced face for the four times, and the accent for the curve.

* **It displays; the knobs control.** It is a **read-out inside the envelope's own panel** — the design's
  inline-display column (`filter-viz`), the place the pole/zero plane sits in the filter panels — and it
  takes no click: `setInterceptsMouseClicks (false, false)`, asserted with `hitTest`. **Dragging the curve
  is out of scope**, and this is the honest form of saying so: the picture *does* look draggable, and a
  component that swallowed the click would look draggable without being it. If it is to become an editor
  that is a ticket with its own gesture rules.
* **Derived, not decorative.** The graph is wired from the plan's own `ParamSpec::modelPath` — the same
  specs the controls are built from, found by the model's leaf names
  (`ui::envelopeStageForModelPath`, the mechanism `planInterface` already uses for the oscillator head's
  stepper) — and it reads the **knobs**: the curve's shape is a function of
  `Slider::valueToProportionOfLength`, the very number the knob's value arc is drawn from, and the legend
  prints `Slider::getTextFromValue`, the string the knob's readout prints. The **second curve** is the
  matrix's own rows through `updateModulationArcs`'s `depth` map — the same map the arcs are set from — so a
  row that moves the attack moves the picture's first segment by exactly the amount the knob's second arc
  sweeps.
* **It costs the page no height.** Its minimum is the design's own display height (68), which is what the
  panel's one row of 68 px knobs and its chrome already are — so ticket `57`'s measured air is now the
  display, and the page's own height is unchanged at 1038.
* **The time axis is logarithmic in seconds**, and that is a finding rather than a flourish: the three time
  knobs are **linear over 0..32 s** (`ParameterBridge`), so a musical 5 ms attack is 0.016 % of its range and
  a picture drawn in proportion would be a box at every setting a player uses. Each stage's width is a
  monotone function of its own value from 1 ms (a stage at or below it is the vertical edge an instant stage
  is) to the knobs' own maximum, and the legend prints the values in full, so the shape is read as a shape
  and the times as times. The hold is a **level**, not a duration, and keeps a fixed share of the width
  written as the width a 200 ms stage would take; the four shares are normalised to the plot, so the display
  always fills. The one consequence of that normalisation, stated rather than hidden: scaling attack, decay
  and release *together* leaves the proportions alone — the picture is the shape, the legend is the time.

The assertions are counterfactuals against the values, not snapshots compared with themselves: attack at its
default, at its maximum and at nought give a rise that is positive, then wider, then **nought wide**; sustain
at 0 and 1 puts the plateau on the baseline and on the top of the plot; the graph's stage proportion *equals*
the knob's `valueToProportionOfLength`; and a document whose matrix row addresses `Env0/kParamAttack` reaches
the graph's second curve and the knob's arc as **the same number**.

## The crash the parameterised reader caused, and why it is now a compile error

The drawn-response reader used to take a slot **index** (`poleZeroFromDocument (document, 0)`); ticket 60 gave it
an **object path** so the rack's own `FXFilter` could share it. That was right, and it broke the two callers in
`interface_test.cpp` — which did not fail to compile, they **segfaulted**: `std::string_view` accepts `0`,
because `0` is a null pointer constant and the view's pointer constructor is implicit, so the old call bound,
built a view over `nullptr`, and the reader walked it. `[interface]` was the one suite left to integration, so
the first run after the change was red with `SIGSEGV - Segmentation violation signal`.

**The type is now `PoleZeroObjectPath`, whose integer constructor is deleted**: an index is a **compile error**
(`conversion function from 'int' to 'PoleZeroObjectPath' invokes a deleted function`) rather than a null view and
a crash. That is the design point worth stating — a parameterisation that turns a stale call into a crash is
worse than the duplication it removes, and this one did it through a conversion the language allows and nobody
would write on purpose.

**Can a real preset reach that crash?** No, and that is measured rather than argued:

* every product caller passes a **non-null** path — `"VoiceFilter" + std::to_string (slot)` for a slot, or the
  document-derived `FXRack{n}/FX[i]/FXFilter` in `planFxRack`, whose prefix is built from the key itself and
  cannot be empty;
* the drawn-response path on real material is exercised on both sides: the **voice** filter by
  `ba-geiger-wub` (its fixture covers `filter:PZ_SVF`) and every fidelity run that renders it, and the **FX**
  filter by `[plugin][audio][fx]`'s "a preset whose rack carries a drawn filter response loads and renders",
  which loads `BA - Kitchen Sink`, checks the 32 sections its `FXRack0/FX[1]` inlines arrive through the same
  reader and object path the decode builds, and renders it (peak 0.086557).

So this was a **test-side** breakage of a *product* API change, not a release blocker: the crashed call existed
only in `interface_test.cpp`, and no preset can construct it. The test now calls the reader with the object path
it means, `"VoiceFilter0"` — which is what it was always asking for; the fix is the call, and the reason it was
wrong is that it stayed on an argument form the new signature no longer accepted.

## Verified by running it

| | |
|---|---|
| `[interface]` | **8,730 assertions / 24 cases — all passed** (ticket `57`'s build had 7,210/23; the fit, ratio and envelope cases are new and the re-scoped ones are rewritten in place) |
| full suite, `ctest --test-dir build-59` | **382/382 passed** (179 s) — 381 before this ticket, because one new case is in it |
| `pluginval --strictness-level 10` | **SUCCESS**, its `Editor`, `Open editor whilst processing` and `Editor Automation` tests included — the ones a geometry change has to survive |
| the captures | `createComponentSnapshot` over the editor's whole window at **1080×1128, 1350×1410 and 900×940**, written to `interface-evidence/ticket-59-scale-{100,125,83}.png` — the same composition at three scales |
| the rectangles at those three scales | `osc-row` 1060×344, `noise-sub-row` 1060×144, `env-section` 1060×105, `filter-section` 1060×172, `macro-section` 314×130, `default-section` 740×130, `mod-panel` 1060×128 — **the same seven rectangles at every scale**, and on screen they are those × 1.0 / 1.25 / 0.8333 (the envelope's band is 105, 131.2 and 87.5 px) |
| the palette's shares at the three scales | `surface` 56.664 / 56.176 / 54.970 %, `bg` 26.835 / 26.038 / 25.173 %, `surface2` 2.951 / 2.992 / 2.890 %, `accent` 0.208 / 0.243 / 0.181 % — the filled areas agree to well inside a point, which a re-flow would not do (it moves them by tens). `border` is printed and not compared: a 1 px hairline drawn at 0.83× is anti-aliased into its neighbours (3.167 % → 0.436 % of *exact* colour), which is a fact about pixels and not about the layout |
| the envelope's curve | 418×68 inside its 1060×93 panel; the rise is **65.97 px** at the attack's default (5 ms), **214.79** at its maximum (32 s) and **0.00** at nought, on a 406 px plot; with the fixture's matrix row (+0.4) the modulated rise is **206.22** against 173.75 unmodulated; its own render has `bg` 16,808 px, `border` 1,176, `accent` 420, the accent's wash under the curve 9,018 and 604 px of legend ink |

**What nobody here could judge**, and the dev's call rather than this session's: whether the drawn curve
**reads as an envelope** (a 418×68 display beside four 44 px knobs, wide and short like the design's own
response display), whether 0.957 wide-per-tall **feels right** for an instrument window against the mock's
1.62, and whether the four monospaced times under the curve are information or repetition of the knob
readouts beside them. This session has rectangles, heights, scales and colour shares — no eyes.

# Ticket 60 — the FX rack in the signal path

Ticket `58`'s agent found this while placing the ceiling: `dsp::FxBank` was **built and tested and instantiated
nowhere** — fourteen classes, the MAIN / BUS 1 / BUS 2 topology, splitters, a `kParam…` vocabulary — and none of
it reached the output. So every preset whose sound depends on its FX rendered without them, and every fidelity
number on such a preset was measuring a signal the reference does not produce. Measured over the factory bank:
**621 of 626 presets carry an FX slot**, 4168 slot entries in all, so this was not a corner.

## Where it sits, and what feeds it

`processBlock`'s chain is now: the voice pool's stereo render → the **master gain** → **`dsp::FxBank`** →
**the ceiling** (ticket `58`) → the host's channel mapping. The ceiling stays last, which is what its declared
latency assumes, and the order is asserted by a render rather than by the call's position : an `FXEQ` band at
+24 dB through the rack reaches **6.221161** peak with the ceiling off and **0.966051** with it on — the
ceiling's own default, to the digit. Were the rack downstream of the ceiling, the boosted signal would arrive at
the host unbounded.

**The three racks are wired; only MAIN is fed.** A preset's `FXRack0/1/2` are MAIN / BUS 1 / BUS 2, and the
*mixer* that sends a voice to BUS 1 or 2 lives in the voice layer — `RoutingSlot`'s `kParamFXBus{n}Level`, which
xerum does not implement. So `FxBank` gets the pool's stereo render on MAIN and the *silence* its buffers were
sized with on the two busses, and a bus rack therefore processes nothing: 169 factory presets put FX on BUS 1
and 24 on BUS 2, and for those the rack is wired and inaudible. `[plugin][audio][fx]`'s "a rack on BUS 1 changes
nothing, because nothing feeds it" is that gap's record — the assertion to invert the day the mixer lands.
`Global0`'s `kParamFXBus{n}Dest`/`Vol` are read and handed to the bank, so the routing is already the preset's.

## Where the parameters come from

Not the bridge: nothing in a reference preset puts an effect on the host-visible surface, so there is nothing for
a parameter to be. The slots are read **from the document**, by the same schema paths everything else arrives by
(`FXRack{n}/FX[i]/type` and `<class>/plainParams/kParam…`), at load, and handed to the rack with the audio
thread held out — building an effect allocates. The decode is `XerumAudioProcessor::planFxRack`, and three
things in it are worth naming:

* **the index is parsed, not iterated.** `std::map` orders the document's keys lexicographically, so `FX[10]`
  sorts before `FX[2]`; ticket 41 was caught by exactly that, and the slots are emitted in numeric order.
* **a drawn `FXFilter` response is decoded** with the same reader the voice filter uses — ticket 60
  parameterised `poleZeroFromDocument` with the object path (`VoiceFilter{n}` or `FXRack{n}/FX[i]/FXFilter`)
  rather than writing a second reader for one format. Six factory slots are `PZ_SVF` with a response inline;
  without it a `PZ_SVF` band is a pass-through, which is a substitution, not a filter.
* **the plan owns its assets.** A `FxSlot` *borrows* its curve and its response, and `prepareToPlay` re-prepares
  the bank, which rebuilds the chain and dereferences them again — so the storage lives in the processor and is
  replaced only while the audio thread is out.

## What is in the path now, proved by difference

`[plugin][audio][fx]` renders the same document with and without the slots and compares **sample by sample**,
which is this project's counterfactual standard. One `FXDistortion` (hard clip, drive 60, wet 100 %): **6496 of
8192 samples differ**, residual -7.91 dB against the dry render, peak 0.3937 → 0.2174. Two slots in the other
order — distortion into delay versus delay into distortion — differ in **10243 of 12288 samples** (-14.13 dB):
the chain is the preset's order, not the rack's own.

**The per-class table**, which is the dev's version of "what was missing": one slot per class, a representative
setting, measured against the same document without it (24 blocks × 512, note 60). `residual-vs-dry` is how much
of the dry render the difference accounts for — near 0 dB means the render was *replaced* rather than shaped:

| class | residual-vs-dry | samples differ | peak dry → wet | reports |
|---|---|---|---|---|
| `FXDistortion` | -7.97 dB | 8544 | 0.3937 → 0.2174 | 0 |
| `FXFlanger` | +1.12 dB | 11948 | 0.3937 → 0.8238 | 0 |
| `FXPhaser` | +4.43 dB | 12214 | 0.3937 → 1.0491 | 0 |
| `FXChorus` | -0.28 dB | 9193 | 0.3937 → 0.7782 | 0 |
| `FXDelay` | +2.47 dB | 12215 | 0.3937 → 0.6408 | 0 |
| `FXComp` | -7.59 dB | 8544 | 0.3937 → 0.7566 | 0 |
| `FXReverb` | +5.90 dB | 12215 | 0.3937 → 0.4698 | 0 |
| `FXEQ` | +16.58 dB | 12215 | 0.3937 → 3.1235 | 0 |
| `FXFilter` | +3.83 dB | 12215 | 0.3937 → 0.3093 | 0 |
| `FXHyperD` | +3.05 dB | 9081 | 0.3937 → 0.2195 | 0 |
| `FXBode` | +0.65 dB | 8670 | 0.3937 → 0.1969 | 0 |
| `FXConv` | −∞ dB | **0** | 0.3937 → 0.3937 | **1** |
| `FXUtils` | -6.21 dB | 12215 | 0.3937 → 0.3920 | 0 |
| `FXSplit` | -5.87 dB | 8993 | 0.3937 → 0.2187 | 0 |

Thirteen of the fourteen change the render. **`FXConv` changes nothing and says why**: it convolves with an
impulse response the *caller* supplies, the reference's `Impulses/` tree is resolved by no layer here yet
(tickets `16`/`34`), and the effect reports "no impulse response was supplied" rather than convolving with
nothing — which is why its row is the only one with a report and the only one whose difference is exactly zero.

## The runaway the wiring found

Wiring the rack found a real defect in a shipped effect, and it was not subtle. `FXDelay` reads `kParamBW` as
the *loop filter's* Q and multiplies the filtered tap by `kParamFeedback`, so the loop's gain is
`feedback x Q` — and the corpus's own values are BW **3.28 … 8.25** with feedback up to 58 %:

| preset | its own `FXDelay` | loop gain | render, ceiling off, before |
|---|---|---|---|
| `ARP - Aardvark` | BW 3.28, feedback 47 % | **1.55** | peak **2.3e9** at 2 s |
| `ARP - Bell Arperium` | BW 5.33, feedback 58 % | **3.09** | peak 3.7e9 at 2 s |

That is not a delay that rings, it is one that grows without bound — and Bell Arperium's oscillators are a
deferred engine, so it grew from numerical noise with **no input at all**. The fix keeps the reading rather
than inventing a second meaning for `kParamBW` (ticket 04 read the parameter *ranges*, not the curves) and trims
the feedback by the filter's own peak gain, so the loop sits at or below unity at the resonance:
`feedback x min(1, 1/max(1, Q))`. `[fx-loop]` in `tests/dsp/fx_rack_test.cpp` measures it at the corpus's two
values, an impulse and four seconds of silence, and through the rack — and it **fails loudly** if the bound is
removed (the tail reaches 8.97e35, measured by reverting the line for one run).

## The import report's half of it

Nothing the rack cannot build is silent, and each of these is a real case rather than a hypothesis:

* an **effect class that is deferred** (`FXSplit3`, `FXSplitMS`) or unknown: the rack skips it, and the model's
  own walk already names it at `high` — the plugin does not say it twice. The classes a deferred splitter
  claims are skipped *with* it, because their routing is not reproduced and running them serially would invent
  a topology;
* a **parameter name an effect does not know**, as `Class/kParamName`;
* an **`FXFilter` whose type the preset never stored** — 1209 of the bank's 1513 `FXFilter` slots are in this
  case, the schema marks the leaf optional, and the reference's *default* type applies in the reference. xerum
  does not know it, so the band passes unfiltered and the report says exactly that (it used to say
  `filter type '' is not shipped`, which no author can act on);
* a **missing impulse response**, per slot;
* a **drawn `flex[0]` curve that is not applied**. 88 factory presets carry one, and its domain is not
  established: the corpus stores `xVals` over 0..1 with a falling `yVals`, while the transfer curve a distortion
  applies is the input sample over -1..1 — and `curveShape` returns the first ordinate for anything at or below
  the first abscissa, so reading one as the other would put a **constant on every negative sample** of a
  `kXShaper` slot. The built-in curve plays instead, and the report says so.

## What the fidelity baseline did

Wiring the rack moved **every** comparison that had FX in it, which is 621 of 626 presets. The gate's seven
rows, measured twice on this machine (48 kHz / block 512) — once with the stage's publish call neutralised as a
control, once with it restored, both runs otherwise identical:

| fixture | rmsDiff | spectral |
|---|---|---|
| `arp-aardvark` | -25.44 → -25.61 | 10.85 → 11.11 |
| `arp-acid101` | -21.72 → -22.44 | 9.89 → 10.81 |
| `ba-funk-me-electric-2` | -20.79 → -20.80 | 14.76 → 15.20 |
| `ba-basist` | -19.14 → **-21.18** | 15.00 → 13.96 |
| `ba-deep-throat-bass` | -19.52 → -20.18 | 18.85 → 21.25 |
| `808-decomposed` | -21.06 → -21.82 | 14.60 → **18.04** |
| `808-diesel` | -18.33 → -18.39 | 14.95 → 13.02 |

Six rows sit inside their thresholds. **`808-decomposed` did not** — its spectral distance passed
14.60 + 3.0 — and its baseline in `fidelity_test.cpp` is now the *measured after* value, with the reason written
beside it. Nothing else moved: the thresholds, the margin and the other six baselines are untouched and no
convention was adjusted to shrink the number.

**Why it moved, and why that is not the rack being right.** `808-decomposed`'s rack has an `FXComp` at
`FXRack0/FX[1]` whose `kParamThresh` 0.372 and `kParamRatio` 1e6 xerum reads as a **brickwall at -44.6 dB**
(`thresholdDb = -120 x threshold`, ratio 1e6 = limit), which takes the preset's render 21 dB down — while the
**reference's own render of it sits at -22 dBFS**, which no -44.6 dB brickwall can produce. So `FXComp`'s maps
are measurably wrong for this preset. They are ticket `29`'s conventions ("ticket 04 read the ranges, not the
curves"), they are out of ticket 60's scope, and they are recorded here rather than tuned. The same convention
shows in the other direction on `BA - Lazer Bass`, whose `FXComp` asks for `kParamGain0..2` of +13…+16 dB and
comes out **6x louder** than its dry render — that row's rmsDiff worsens by 7.1 dB.

The direction of the whole set is worth stating, because an improvement here is not automatically a fix: the
rack **removes energy** in most of these presets. The level error against the golden changes by **-25 … +16 dB**
across the 32 fixtures, **median -1.4 dB**, and several of the improved `rmsDiff` rows are the rack attenuating
a preset xerum was already too loud on. The candidate mechanism is a stated convention of `FxBase`: every
effect's `kParamLevelOut` is applied as a **linear multiplier** (and its absence means unity), while 690 slots
in the bank store one and its unit is not established — `FXDistortion`'s corpus values top out at 0.956,
`FXDelay`'s at 0.853, and `ARP - Acid101`'s two slots multiply the whole chain by 0.431 and 0.311, which is the
12 dB its render loses. That is a convention to measure against the reference before it is changed, and it is
recorded here rather than guessed at.

## What nobody here could judge

Whether the effects **sound like the reference's** — that is the dev's ears and nothing in this session has
any. What is measured is that they are in the path, in the preset's order, before the ceiling, that thirteen
classes change the render and by how much, and that the one that cannot (a convolution with no impulse) says so
instead of pretending.

## Verified by running it

| | |
|---|---|
| `[fx]` in `plugin/tests/audio_test.cpp` | **9 cases / 64 assertions — all passed** in `build-60`, the corpus-gated ones included (the reference's factory bank is installed on this machine) |
| `[fx-loop]` in `tests/dsp/fx_rack_test.cpp` | **1 case / 27 assertions passed** — and it **fails** if the loop bound is removed: reverting that one line for a run put the tail at **8.97e35** (impulse) and 5.76e11 (through the rack), which is what makes it a regression test rather than a description |
| `xerum-dsp-tests` | **161 cases / 1,642,990 assertions — all passed** |
| `[fidelity-corpus]`, with the stage wired | **1879 assertions — all passed**; the seven gate rows printed above, and 808-decomposed's re-measured baseline is the number it measures |
| `[fidelity-corpus]`, stage neutralised (the control) | **all seven rows reproduced their recorded baselines to the printed 0.01 dB** (-25.44, -21.72, -20.79, -19.14, -19.52, -21.06, -18.33 / 10.85, 9.89, 14.76, 15.00, 18.85, 14.60, 14.95) and every assertion passed — so the table above is a before/after of one machine, one build, one control |
| `[interface]` — **the whole tag this time, not `~[interface]`** | **8,730 assertions / 24 cases — all passed**, the drawn-filter case included (it segfaulted before the `PoleZeroObjectPath` fix: see above) |
| the rest of the plugin suite (`~[interface]`) | **15,326 of 15,331 assertions passed**; the one failing case is `gain_staging_test.cpp`'s "the reference's voice and ours" (**5 assertions**, ticket 61's file, another session's), whose *reference*-side renders come back empty in this build directory — the file is not touched here and its own report records it green in `build-61`. One case skipped: `[fidelity-stimulus]`, which needs the harness binary in this build directory. Model, content and presets suites untouched by this ticket |
