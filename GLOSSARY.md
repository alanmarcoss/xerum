# xerum

xerum is a software synthesizer: an instrument that reads Serum 2 preset files and renders them with
its own engine. This glossary fixes the vocabulary. Where a word in this domain collides with a word
in general software, the domain wins and the collision is called out below.

## The instrument

**Module**:
A named, addressable part of a preset that owns parameters — `Oscillator0`, `Filter`, `LFO3`,
`FXRack1`. Modules nest, and a module instance is identified by its position among its siblings.
_Avoid_: component, node, block, unit. In these notes "module" never means a unit of source code.

**Slot**:
One of the five fixed positions a generator can occupy in a voice: `A`, `B`, `C`, `NOISE`, `SUB`.
The first three each choose an engine; `NOISE` and `SUB` are single-purpose and choose none.

**Engine**:
The selectable generator inside a slot that is not `NOISE` or `SUB` — wavetable, sample, multisample,
granular or spectral. An engine choice is a *type*, not a separate module.
_Avoid_: oscillator type, generator mode.

**Wavetable** (or **table**):
A stack of single-cycle waveforms that an oscillator scans through, rather than a sampled recording
that it plays back. Confusing the two is the most common error in this domain — a sample is played,
a table is scanned.

**Frame**:
One single-cycle waveform within a wavetable. A wavetable is many frames.

**Warp**:
A transformation applied to an oscillator's waveform before it is read out — bending, syncing,
phase-distorting. Warp is per-oscillator state, not an effect in the FX rack.
_Avoid_: shape, mode, distortion.

**PZ filter**:
A filter defined by explicit pole and zero positions rather than as a named type with sliders.

## Presets

**Preset**:
A saved instrument state: a sparse set of parameter values plus references to the assets it needs.

**Sparse override**:
A preset records only the parameter values that differ from the engine defaults. A parameter a
preset does not mention is not off, not zero, and not missing — it is *at its default*.
_Avoid_: diff, patch. The distinction matters: absence carries meaning and must never be read as a value.

**Default sentinel**:
The marker meaning "this whole module is at its schema defaults". It is a statement about every
parameter in the module at once, not a value in its own right.

**Module path**:
The address of a parameter: its module's instance position, then the parameter's name. Stable across
sessions and independent of the order parameters happen to be declared in.

**Parameter**:
A single named, addressable value in the instrument — the unit of automation, of preset storage, and
of modulation destinations.
_Avoid_: knob, control, setting.

**Model parameter** and **plugin parameter**:
Two different things that are both called "parameter". A *model parameter* is a value as the preset
model sees it, addressed by a module path. A *plugin parameter* is a value as a host sees it, with an
integer index and a normalised 0..1 range. They do not correspond one-to-one, and the bridge between
them is its own concern.
_Avoid_: using "parameter" unqualified when the distinction matters.

**Modulation slot** (or **mod slot**):
One row of the modulation matrix: a source, optionally a second *aux* source, an amount, and a
destination. Rows are independent; slot order carries no meaning.

**Source** and **aux source**:
The two signal inputs a modulation slot can read. The source drives the slot; the aux source modifies
how it does so.

**Destination**:
The parameter a modulation slot writes to, addressed by target class, instance, and parameter.

**Schema version**:
The version of the preset model a file was written against. Presets of different schema versions
differ in which modules and parameters exist at all.

## Content

**Content root**:
The directory tree a preset's asset references are resolved against — tables, samples,
multisamples and impulses all live beneath it.

**Factory** and **user**:
The two banks every category of content is split into. Factory content is what shipped with the
reference instrument; user content is everything the user added.

**Embedded asset**:
An asset stored inside the preset file itself rather than referenced by path. Both forms exist in the
wild, and a preset may carry either.

**Asset reference**:
A path in a preset pointing at content outside it. An asset reference is not a guarantee that the
asset is present.

## Development

**Reference instrument**:
The commercial synthesizer whose presets and behaviour xerum is measured against. xerum is not
affiliated with it and shares no code with it.

**Golden render**:
A recording captured from the reference instrument for a fixed preset, note and sample rate, kept as
the yardstick a fidelity test compares against.

**Fidelity test**:
A test that asserts xerum's output is close enough to a golden render. It measures similarity within
a tolerance; it does not claim identity.

**Harness**:
The small host program that can load a plugin, drive it and write audio to a file, used to produce
golden renders and to observe the reference instrument.
