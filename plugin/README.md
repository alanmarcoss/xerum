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
(75), and `InterfacePlan` turns it into device pages; `ParameterBridge::modulationSpecs()` is the
matrix surface, which `InterfacePlan` deliberately never sees — a 512-row page is not a matrix.
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
  dynamic choice list, which hosts dislike; recorded rather than half-built.
* **Pitch bend is ±2 semitones**, the reference's init state's bend range (ticket `38`'s table), and
  `kParamBendRangeUp/Dn` are not exposed. Stated, not measured.
* **The editor is minimal on purpose**: the parameter surface plus the import report. The report is
  what ticket `30` needed the interface for. Ticket `44` implemented the *design* of ticket `43` on
  top of that surface — device pages, the matrix, the pole/zero plane, the report bar and drawer — and
  that is the section at the end of this file.

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

```sh
cmake -S . -B build-44 -DCMAKE_BUILD_TYPE=Release && cmake --build build-44 -j8
build-44/plugin/tests/xerum-plugin-tests "[interface]"             # 12 cases, 1,746 assertions
build-44/plugin/tests/xerum-plugin-tests "[interface][headless]"   # the no-editor assertion
build-44/plugin/tests/xerum-plugin-tests "[interface][ui]"         # the cases that open the interface
```

## One device per page, behind a strip that always shows the others

A **device is a panel**, and a panel is a `ParamSpec::group` — the model's own grouping, which is also
the granularity the reference uses for its tabs. So the pages are *derived* from the parameter table
and nothing is hand-listed: `Osc A`, `Osc B`, `Osc C`, `Noise`, `Sub Osc`, `Filter 1`, `Filter 2`,
`Global`, `Amp Envelope`. `ui::DeviceStrip` draws one tab per device, always all of them, with a
severity dot on any device whose module the import report named — so a half-loaded preset is located
where it happened and not only collected in the drawer.

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
`XerumAudioProcessorEditor`/`ParameterPanel`/`ModulationMatrixView`/`PoleZeroPlane`/`DeviceStrip`/
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
