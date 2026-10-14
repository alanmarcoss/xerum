# xerum — interface design brief

A self-contained brief to hand to a design-capable model. It exists as a file so it can be versioned and
re-used, and as a paste-in prompt so it can be handed over directly. Ticket `51` owns the work it describes.

---

## The prompt

You are designing the interface for **xerum**, an audio synthesiser plugin. I want a complete design system
and a set of ready-to-implement vector elements. Work in a dark, in-DAW aesthetic: this is a plugin window
inside someone's DAW, competing for screen space with their mixer.

**Deliver in this order, and stop between steps if I ask:**

1. **Two or three distinct visual directions**, each one paragraph plus a palette and a single composed
   screen. They must be genuinely different from each other, not shades of one idea — I need to react to
   *differences*.
2. **A design system**: palette (hex), type scale, spacing grid, control geometry with exact pixel sizes,
   and every control state.
3. **Individual elements** as SVG at exact pixel dimensions, plus a spec table for each.

### The one hard constraint — read this before anything else

**Do not reproduce Serum 2's visual identity.** xerum reads Serum 2's preset format and renders it with its
own engine, but it is an independent product distributed publicly, and a UI that reproduces the reference's
look invites a trade-dress claim. So:

| Wanted | Not wanted |
|---|---|
| A **conventional wavetable-synth grammar** a musician already knows: a device strip, panels, big rotary knobs, an arc showing value, a second arc showing modulation depth, a modulation matrix as rows with a source/amount/destination triple | Serum 2's **skin**: its panel shapes and bevels, its specific colour scheme, its control artwork, its typefaces, its logo, its window chrome |
| Dense, professional, legible — a working instrument | Anything sampled, traced or closely imitated from another synthesiser's artwork |

If you find yourself reaching for "make it look like Serum", stop and make it look like *a good synth*.

### What the product actually is

- A wavetable synthesiser: three oscillator slots (A/B/C), each with a **wavetable position** across up to
  256 frames, **two warp slots**, and **unison** up to 16 voices.
- Plus two **fixed** slots that are *not* selectable: **NOISE** (a four-colour generator: `Brown`, `Geiger`,
  `Pink`, `White`, plus a colour control) and **SUB** (six waveforms: Sine, Triangle, Saw, Square, Pulse,
  Rounded Rect, tracking the played note).
- **Two voice filters**, each with an **83-value** type vocabulary (a searchable picker, not a dropdown list),
  plus a **drawn pole/zero response** of up to **32 sections** that the user edits on a 2-D plane.
- A **modulation matrix of 64 rows**. Each row has: a source and an optional auxiliary source (60 named
  sources, e.g. `Env 1`, `LFO 3`, `Macro 2`, `Velo`, `Note#`), a **signed** amount (−100…+100, so zero must
  read as the centre), a polarity toggle, a bypass, in and out curves, smoothing, a delay, an output level,
  and a **destination addressed as three parts** — module class, instance, parameter name.
- **Ten LFOs**, each with rate, rate×10, dotted/triplet, phase, delay, rise, smooth, swing, direction, mono,
  a mode (one-shot "Envelope" vs free-running "Free"), and — importantly — **a drawn shape** made of
  draggable points, including vertical edges.
- **Four modulation envelopes**, separate from the amplitude envelope, each ADSR with hold, three
  independently **curved** segments, and start/end levels.
- **Eight macros**, three **FX racks** (16 effect classes; one of them is a convolution with an inlined
  impulse), and a global section (tuning 432–435 Hz, bend ranges, two bus volumes and routings).
- An **import report**. This matters: xerum imports Serum 2 presets and cannot reproduce everything, so any
  preset produces a report — entries with a severity (`high` = the sound is materially wrong, `medium` =
  something was not interpreted, `info` = how we coped), a module path, an item and a reason, plus counts per
  severity. A clean import must say **"Nothing to report"**, because an empty report and no report are
  different states and must not look alike.

### Aesthetic direction (my call — work inside it, not against it)

Dense, dark, flat with subtle depth. One accent hue for value and modulation; monospaced numerals for
readouts so numbers do not jitter as they change. Panels grouped by signal flow (source → shape → modulate →
effect). Grid-based, because the layout is *derived from a parameter table*, not hand-placed: controls come in
a small number of standard sizes and everything aligns to a baseline grid.

### What I need back

- **SVG per element**, at exact pixel dimensions, with the spec table (sizes, radii, stroke widths, colours,
  spacing). I will implement these in JUCE, so: vector paths, gradients and simple shapes only. **No HTML/CSS
  frameworks, no JavaScript, no bitmaps, no icon fonts** — and no icon or illustration I cannot legally ship.
- **A style-token list** I can paste into a single header: colour names and hex, type sizes, spacing steps,
  knob diameters, corner radii, stroke widths.
- **One composed screen** showing a device page, the modulation matrix, and the import-report bar, so I can
  judge the whole at once.
- **States for every control**: default, hover, active/dragging, disabled, focused (keyboard), and
  *modulated* (the second arc). A control with no modulated state is not finished.
- **Two window sizes**: roughly 900×600 as the baseline and a wider layout, since the window is resizable.
  Everything must stay legible at 100 % — no relying on Retina.

### How it will be judged

- A musician familiar with wavetable synths can find an oscillator's tuning and a filter's cutoff **without
  being taught**.
- Every element can be implemented from the spec alone, with no further design questions.
- It does not look like a utility panel, and it does not look like someone else's synthesiser.
