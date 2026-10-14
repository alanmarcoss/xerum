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
yardstick, not the target. Per fixture the comparison reports:

- **null depth** — how much of xerum's output the reference's cancels,
- **RMS difference**,
- **spectral distance**.

Thresholds are **set from measured values, not invented**: the first run records what the engine
actually achieves, and the tolerance is derived from that. A tolerance chosen before the first
measurement is a guess wearing a number.

Rendered at 44.1, 48 and 96 kHz across several block sizes — the same axes `pluginval` sweeps, for the
same reason: a synth that is stable at one rate and broken at another is a common and embarrassing
failure.

A named **gate set** of fixtures blocks a release. Everything else is diagnostic.

### Plugin

`pluginval` and `auval`. The `pluginval` strictness level is fixed once it first runs — starting strict
and relaxing with a recorded reason beats starting lax and never noticing.

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

The **format**, **DSP** and **plugin** layers all run. The **fidelity layer is specified and, since ticket
`20`, runnable** — that ticket found the load path (`<contentRoot>/Presets/User/Default.SerumPreset`, read at
instantiation) and the determinism flags (`--pre-roll 128 --settle-ms 1500`), and ticket `32` then drove it end
to end with byte-identical reference renders. The 32-fixture golden corpus is captured and the reference and
harness are both present.

What is missing is the **comparison itself**, which is ticket `46`. This paragraph previously said the layer
was "not runnable" because loading a preset was an open question; that reason expired unnoticed, and the note
is corrected rather than deleted so the sequence is visible.
