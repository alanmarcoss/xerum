# Test strategy

What is tested, where it runs, and what a passing build proves. Derived from
`.scratch/xerum/issues/15-test-strategy.md`.

## The split that shapes everything

No reference-derived artefact is committed to this repository — no factory presets, no wavetables, no
renders captured from the reference instrument. So:

> **CI proves the code is sound. Local runs prove it is faithful.**

Tests that need the reference instrument or the local content tree run where those exist and **skip
loudly** where they do not. A skip is reported; it is never allowed to look like a pass.

## Four layers

| Layer | What it asserts | Runs in CI | Needs locally |
|---|---|---|---|
| **Format** | containers decode; the schema resolves; the import contract's clauses hold | yes, on our own fixtures | the factory corpus, for the corpus half |
| **DSP** | oscillators, filters, modulation, voices and effects behave | yes | — |
| **Fidelity** | xerum's output is close enough to a golden render | **no** | the reference instrument |
| **Plugin** | `pluginval` and `auval` pass | yes | — |

### Format

Self-authored fixtures — presets **xerum itself writes** — are committed, so the suite means something
on a fresh clone. The factory corpus is resolved **by path and skipped when absent**.

Goldens for our own fixtures are small checked-in JSON: the canonical document and the import report.

The corpus test asserts the walk over **all 1,253 containers** and the report produced for each
preset. Per `docs/preset-import-contract.md`, every preset produces a report — **empty, not absent**.

### DSP

Assertions are behavioural and quantitative. Never "it ran", never "output was non-empty".

- Frame interpolation must be *observed to vary* between frames.
- A filter's slope must *measure* its stated dB/oct.
- A compressor must *reduce measured dynamic range*.
- A distortion must *change harmonic content* measurably.
- A modulator must actually reach its destination, with the amount, polarity and curve applied.

Three invariants are named because they are the ones broken by accident:

1. **No allocation on the audio thread** — enforced by a tracking allocator across a full render.
2. **No exception across the plugin boundary** — a throw into a host process is a crash in someone
   else's DAW.
3. **A clean import still produces a report** — empty, not absent.

### Fidelity

**The metric is not byte-identity.** xerum is an independent implementation; the reference is the
yardstick, not the target. Per fixture the comparison reports, never as an average:

- **null depth** — `20 log10(rms(golden − xerum) / rms(golden))`: how much of xerum's output the
  reference's cancels,
- **RMS difference** — `rms(golden − xerum)`, absolute, in dBFS,
- **spectral distance** — log-spectral distance: the mean |Δ magnitude| in dB over 2048-point Hann frames
  at 512-sample hop, taken over the bins within 60 dB of the reference's own peak. The window is what
  makes the number mean something: a mean over *every* bin is dominated by bins where neither signal has
  anything and reads ~100 dB for any two different synths.

The suite is `plugin/tests/fidelity_test.cpp` (ticket `46`), tagged `[fidelity]`. It renders through
xerum's own processor under the harness's render loop, **proved identical to it** by a case that renders
the same init patch both ways and requires byte-identity — so nothing here is a comparison against a
second protocol dressed up as a stimulus. The preset arrives the way the plugin's own code path delivers
it (`loadPresetFile` → `encodeStateBlock` → `setStateInformation`, all meeting in `applyDocument`), and
the applied document is compared with the loaded one, so a state that was silently dropped fails rather
than quietly measuring the init patch.

**The harness cannot be the xerum side**, measured rather than assumed: its `--load-state` hands bytes to
`AudioPluginInstance::setStateInformation`, which for a JUCE-hosted VST3 transports JUCE's *own* envelope
and discards a plugin's own block without a word — `--load-state` with a 216 370-byte block followed by
`--dump-state` writes the identical 318-byte init document that the same command writes with no
`--load-state` at all.

Thresholds are **set from measured values, not invented**. The first full run (2026-10-14, 48 kHz / block
512, clean build) recorded, over the seven fixtures whose import is complete: null depth **0.02 to
6.91 dB**, RMS difference **−25.44 to −18.33 dBFS**, spectral distance **9.89 to 18.85 dB**. Across all 32
fixtures the ranges are −1.27 to +6.91 dB, −33.96 to −14.85 dBFS and 7.55 to 22.96 dB. The three metrics
agree that xerum is **not close to the reference yet** — a null of about 0 dB means the residuals do not
cancel at all, and a positive one means the residual is louder than the reference's own output — and that
is the baseline this layer exists to expose, not a defect it fixes. Tickets `25`, `26`, `29` and `37` each
recorded that their curves implement documented behaviour rather than measure the reference; closing that
distance is a separate effort.

A named **gate set** blocks a release, chosen by completeness and not by leniency: every fixture whose
import report is **empty** (nothing this build does not ship, no modulation row resolving to nothing) and
which is not one of ticket `09`'s negative fixtures — `arp-aardvark`, `arp-acid101`,
`ba-funk-me-electric-2`, `ba-basist`, `ba-deep-throat-bass`, `808-decomposed`, `808-diesel`. Each row's
recorded baseline and its threshold (baseline + 3 dB) are in the test and in the report; the gate also
asserts that a gate fixture renders finite, non-silent output and still imports cleanly, so it cannot
silently become a different measurement. Everything else is diagnostic.

Rendered at 44.1, 48 and 96 kHz across blocks 64–1024 — the same axes `pluginval` sweeps, for the same
reason: a synth that is stable at one rate and broken at another is a common and embarrassing failure. A
render at anything but the capture's own rate is moved onto the golden's grid by a phase-normalised
128-tap windowed-sinc resampler whose floor is **measured and recorded**, not assumed: −33.27 dB of null
at 44.1 kHz (inherent — the golden's content above the 44.1 kHz Nyquist cannot exist in a 44.1 kHz render,
so no resampler can cancel it) and −49.58 dB at 96 kHz. The observed spread across the fifteen
configurations is at most 1.1 dB, so the 3 dB sweep margin is over twice the largest move.

**Finiteness is the sweep's one hard invariant**: every render at every configuration must exist and be
finite, asserted rather than eyeballed. It needs no threshold — a finite render is finite — and it is the
assertion that catches a rate- or block-dependent breakdown. It is there because the layer first met that
failure with nothing to fail on: a `VoiceFilter::processPoleZero` cascade whose `double` accumulation
overflowed the `float` it returns showed up as a column reading `nan` and nothing else. That defect is
fixed and guarded (`src/dsp/filter.cpp`, ticket 45's plane case reproduces it), and the assertion fails if
it returns. Silence is *not* asserted: a deferred engine or a recorded gap can legitimately render
nothing, so silence is reported per fixture with its import report as the explanation.

Because that guard answers an overflow with **silence for one sample** rather than a NaN, the report also
carries a **discontinuity diagnostic** — the largest sample-to-sample step, for xerum and for the golden —
since a dropout leaves every sample finite and moves an RMS difference by about −40 dB, so neither the
assertion nor the three metrics would see it. It is reported, never gated (a sharper transient than the
reference's is not a defect). Measured over the corpus: no fixture's step exceeds the reference's by more
than **5.2×** (`arp-etheric-aluminium`), and that excess is *not* an isolated click — 3 671 of its samples
carry a step above 0.02 against 2 in the reference — so the column is reading waveform sharpness on the
bright patches, which is what it should read. A click would appear as a single-sample excursion, and
`[fidelity-diagnostics]` shows the sensitivity rather than asserting it: one zeroed sample in a real render
raises the figure 2.4× while moving the peak by 0.7 %.

**Negative fixtures** — ticket `09`'s four deferred engines (granular, spectral, multisample, sample) —
are measured and reported like everything else and **excluded from the gate with that as the stated
reason**: xerum reports the missing engine rather than substituting something, so their render is expected
to diverge. The report names each one and prints its numbers.

Two limitations are recorded rather than left implicit. The manifest's `uncoveredTargets` —
`fx:FXConvolve`, `fx:FXHyper`, `fx:FXUtility` — appear in none of the 626 factory presets, so no fixture
can cover them: three of the sixteen FX classes are unexercised by this layer. And a golden exists only at
48 kHz / block 512, so every other configuration is a measurement *through* the resampler.

**Ticket `58`'s master ceiling is bypassed here, and its latency is compensated.** This layer is the one
place the divergence had to be decided, because the golden is a capture of the **reference**, which has no
such stage — the reference's schema has no limiter leaf at all. So each render sets `Master Limiter` to off
*after* the fixture's document is applied (a reference preset carries no `Master0` leaf, so applying one
resolves the ceiling back to its on default), and then compensates the plugin's declared latency: the plugin
is a lookahead limiter and delays its output by `getLatencySamples()` whether the ceiling is enforced or not,
so the render is taken that many samples long and its front is dropped, putting xerum's output on the
stimulus's own clock. The other option the ticket offered — regenerating the corpus with the ceiling on —
would bake xerum's divergence into the reference's side of the comparison, and the divergence would stop
being measurable; bypassing keeps this layer measuring the *engine*, which is what it exists for. The
`[fidelity-stimulus]` case is untouched by the compensation and still requires byte-identity, because both of
its sides render through the same plugin with the same declared latency. The stage itself, its defaults and
their sources, and the measured numbers are in `plugin/README.md` under ticket `58`.

The whole report is written to `~/.xerum-fixtures/fidelity/fidelity-report.tsv` — outside the repository,
per ticket `10` — and names the executable that produced it;
`XERUM_FIDELITY_DUMP=<dir>` additionally writes the exact signal pair each metric was computed on, so a
reading can be listened to and re-analysed rather than only read. The layer **skips loudly** when the
corpus, the harness or xerum's bundle is absent: a fresh clone and CI stay green, and the manifest case
still runs there.

### Plugin

`pluginval` and `auval`. The `pluginval` strictness level is fixed once it first runs — starting strict
and relaxing with a recorded reason beats starting lax and never noticing.

**`[gain-staging]` is a measurement, not a gate** (ticket `61`). It renders **one reference preset's own
container on both sides** — xerum through `loadPresetFile`/`encodeStateBlock`/`setStateInformation`, the
reference through the harness and ticket `20`'s `Default.SerumPreset` route — under ticket `58`'s own chord
(`36 + 3i`, velocity 127, held), with xerum's ceiling off and its declared latency compensated, and reports
**peak and RMS at one voice and sixteen**, the vocabulary ticket `54` fixed. Its preset is chosen for an
**empty FX rack** in the reference's own container, so ticket `60`'s stage cannot reach the numbers, and the
test asserts that property on xerum's decoded document rather than trusting the file. Every row renders twice
and byte-identically, and the numbers go to `~/.xerum-fixtures/fidelity/gain-staging/`. Its assertions encode
the measured finding — that a single full-scale voice is our **default patch's** level, not a gain-staging
fault, because the reference's own default is attenuated 14 dB by two of its own controls — and it **changes
no engine level**; a fix would be a defaults ticket, not this one. The harness gained `--notes`/`--velocity`
for it, additively: `[fidelity-stimulus]` still requires byte-identity, so the capture protocol did not move.

**`[gain-staging][fx-units]` is the same instrument pointed at the two leaves ticket `63` could not reach**
(ticket `62`): `kParamLevelOut`, the FX "level" knob, and `FXComp`'s `kParamThresh`. Neither is on the
reference's parameter surface — its 2623 parameters carry sixteen generic `FX Main Param`s per rack and all
of them read zero with a real rack loaded — so the case **authors a preset that differs from a factory one
only in the leaf under test** (`patchReferencePreset`: decode the payload tree, set the leaf in the tree,
re-wrap the container, so no structural quirk is lost), renders it on both sides and compares the ratios
*between stored values*, which is what makes the measurement independent of everything else the two engines
do differently. The fitted models are `gain = 4·s²` (unity at `s = 0.5`, the leaf's own default: a preset
that omits it renders byte-identically to one storing `0.5`) and `threshold amplitude = (1−s)³` (the
stored `0` is `0 dBFS`), the latter measured with the ratio at its limit so the compressor pins its
output to the threshold, and its **exponent** — `ln(amplitude)/ln(1−s) = 2.999` across six stored
values — is the part the sweep establishes; the *absolute pin* is a stimulus-dependent factor above the
threshold (1.10× on one stimulus, 1.41× on another), so the case asserts the ratio between stored values,
which is exact, rather than a peak that would be asserting the reference's detector. The case asserts
each model against the reference and each output against the other side's at four stored values, so a
model fitted to two points could not pass.

**Ticket `65` extended the taper to the two leaves the host surface *does* carry, and read them off it.**
`VoiceFilter{N}/kParamLevelOut` ("Filter 1 Level") and `Global{N}/kParamFXBus{n}Vol` ("Bus 1/2 Vol") were
read by nothing / fed raw; both are the same `4·s²` family, and both were established the way ticket `64`
recommended — sweep the reference's own **host parameter** and read its **display strings**, which is the
whole stored↔dB relation in one pass rather than a curve fitted to a proxy. The `Filter 1 Level` display reads
`-∞ / -40.0 / -36.5 / -28.0 / -16.0 / -12.1 / -8.9 / -3.9 / -0.0 / 3.1 / 5.8 / 7.0 / 8.1 / 10.2 / 12.0 dB` at
stored `0 / 0.05 / 0.0614035 / 0.1 / 0.2 / 0.25 / 0.3 / 0.4 / 0.5 / 0.6 / 0.7 / 0.75 / 0.8 / 0.9 / 1.0`, and
`Bus 1 Vol`'s is `-∞ / -28.0 / -12.1 / -0.0 / 7.0 / 12.0 dB` at `0 / 0.1 / 0.25 / 0.5 / 0.75 / 1.0` — `4·s²` to
0.1 dB at every point of both. Each is cross-checked on **presets that actually store one** (`BA - 303 Die
Treppe` `9.9` / `4.1 dB` against the law's `9.94` / `4.18`; `ARP - Bell Arperium` `-1.2` against `-1.20`; `808 -
Diesel` `-36.5` against `-36.43`), which is the check that caught a law fitted to a default. The filter leaf is
also a **modulation destination** — every preset that stores a non-default one modulates it, and `808 - Diesel`'s
`Velo` row at `+51.19` is why the leaf cannot be applied as a static gain — so `kLevelModulationSpan` is the
span, read the same way `kCutoffModulationSpan` reads a `0..1` destination.

## Where fixtures live

Outside the repository, in a documented location (`~/.xerum-fixtures/`), covered by `.gitignore`.

**The capture script and the manifest are committed; the renders are not.** If a licence is ever bought
or a claim ever arrives, re-capturing is a single script run — which is why the script is the durable
artefact and the output is not.

## What evidence a change carries

| Change | Evidence required |
|---|---|
| DSP | the measured fidelity delta, not merely a green suite |
| Format or import | the corpus walk result: 1,253 containers, and a report for each preset |
| Plugin surface | `pluginval` and `auval` clean at the fixed strictness |

## Current state

**All four layers run.** The **fidelity layer** arrived with ticket `46`: it renders every fixture through
xerum, measures the three metrics per fixture across the rate and block sweep, records the baseline into a
report, and gates a named set of cleanly-imported fixtures against thresholds derived from that first
measurement. Its prerequisite came from ticket `20` (the reference's load path and the determinism flags) and
ticket `09` (the 32-fixture golden corpus itself); ticket `32` proved the reference renders byte-identically
from these materials. The paragraph that used to sit here said the layer was "specified and runnable" but not
built — the reason it gave for not building it expired when ticket `20` landed, unnoticed, and it is corrected
rather than deleted so the sequence stays visible.

Run it alone with:

```
ctest --test-dir build-46 -R xerum-plugin-tests          # or
build-46/plugin/tests/xerum-plugin-tests "[fidelity]" -s
```

It skips loudly (exit 0, four skipped cases) on a machine without the corpus, the harness or xerum's bundle,
and the manifest case still runs there.
