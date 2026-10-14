// xerum::dsp — the NOISE oscillator: the reference's slot 3, a fixed single-engine slot.
//
// ## Why this is not a wavetable slot
//
// `Oscillator3` is not one of the three container slots. Ticket 05 measured that all 626 factory presets
// put exactly one engine in it — `NoiseOsc3` — and no `kParamType` to select with, which is also why
// ticket 26 found 434 leftover override maps "on an engine the slot has not selected": there is nothing
// to select. The engine's own parameters live under `Oscillator3/NoiseOsc3/…`, and the slot's power and
// level under `Oscillator3/plainParams/kParamEnable` / `kParamVolume`.
//
// ## What the reference's NOISE oscillator is, from evidence
//
// The shipped manual (p. 129–131, the reference's own documentation) says the noise oscillator "is
// actually a stereo sample player", and `kParamNoiseType`'s **four** values — the schema's `kValues510`,
// `{Brown, Geiger, Pink, White}` — are documented there as a *separate menu category* of noises "named
// after colors", with their own two extra controls (a stereo width and a filter). Measured over the
// corpus, the colour and the sample are **mutually exclusive**: all 93 presets that name a colour carry
// no `relativePathToNoiseSample`, and all 531 that name a sample carry no colour (0 exceptions).
//
// So the colour modes are the reference's *generated* noises and the file is the *sampled* path, and this
// engine implements the four colours. The four file-based presets' behaviour is an asset-resolution
// question (tickets 24/34 own it), and `XerumAudioProcessor` reports both that and
// `sampleFromAudioInput` by name rather than substituting a colour for a sample — a substitution the
// import contract forbids.
//
// ## The four colours, from the manual's own definitions, and what is *not* established
//
// The manual defines them by spectrum, in its own words:
//
//   * **White** — "equal energy at all frequencies";
//   * **Pink** — "3 dB less energy per octave as the frequency doubles";
//   * **Brown** — Brownian/red, "6 dB per octave", "deep, rumbling", "very little high-frequency
//     content";
//   * **Geiger** — "random clicks or bursts that are not evenly distributed across time or frequency…
//     sporadic, random bursts with unpredictable intervals between them".
//
// The first three are standard spectral definitions and are implemented as such, then *checked* by
// measuring their tilt (`noise_osc_test.cpp`). Geiger is a **named character**, not a spectrum, so it is
// implemented as the manual describes it and nothing more: a Poisson process of randomly-signed
// exponentially-decaying bursts. **What is not established** is the reference's own tick rate, burst
// shape, and buffer length — the manual gives the character, not the numbers — so the constants below
// are stated conventions (`[INFERENCE]` on the numbers, measurement on the character), and the test
// asserts the character (sparse impulses: crest factor and kurtosis) rather than a tick rate nobody has
// measured.
//
// ## The sample-playback controls are reported, not invented
//
// The reference exposes Phase, Rand Phase, Pitch, Fine and One Shot on this oscillator because it plays a
// sample. For a generated colour there is no sample buffer, no start position and no playback rate in
// xerum's reading of the model — the colour's own spectrum is the whole signal — so
// `kParamInitialPhase`, `kParamOneShot`, `kParamColor` and the slot's pitch are **reported by name** by
// the plugin rather than silently ignored. `kParamRandomPhase` is the one that *does* transfer: the
// manual says it "prevents the noise from being identical each time you press a note", which is exactly
// a per-note reseed of the generator (`trigger(true)`).
//
// ## Level: all four are calibrated to the same in-band RMS
//
// The four colours are calibrated to the same RMS **in the 100 Hz – 8 kHz band**, not over the whole
// spectrum, because Brown's energy sits mostly below the audible band and a full-band match would make it
// a near-silent rumble. The constants below were measured over 2^15 samples of this implementation's own
// predecessors (see the ticket's measurements); `noise_osc_test.cpp` asserts the match rather than the
// constants, so a change that breaks the calibration fails a test instead of sounding wrong.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace xerum::dsp
{

/// The four values of `kParamNoiseType` — the schema's `kValues510` — in the manual's table order.
enum class NoiseType
{
    White,
    Pink,
    Brown,
    Geiger
};

/// `kParamNoiseType`'s vocabulary. Returns `nullopt` for a name outside the schema's four — an absent or
/// unknown colour is the reference's *sample* mode, which this engine does not render, and the caller
/// reports it rather than choosing a colour on the preset's behalf.
std::optional<NoiseType> noiseTypeFromName (std::string_view name);

std::string_view noiseTypeName (NoiseType type);

/// One colour generator. No pitch input: the reference's colour modes are not key-tracked (measured from
/// its own init state — `Noise Pitch Track` is `Off`, while every wavetable slot's is `On`), and a
/// generated colour has no rate to track.
class NoiseOscillator
{
public:
    void prepare (double sampleRate);

    void setType (NoiseType type);
    NoiseType type() const { return type_; }

    /// Note on. `decorrelate` is `kParamRandomPhase != 0`: it advances the per-note seed, so this note's
    /// noise is not the previous note's. Without it every note replays the same stream from the same
    /// state, which is what the manual says the RAND control exists to prevent.
    void trigger (bool decorrelate);

    /// Silence and re-arm to the current note's starting state. The generator has no envelope, so this is
    /// all a voice reset needs.
    void reset();

    float next();
    void process (float* out, std::size_t count);

private:
    /// Splitmix64 (Steele et al.): deterministic, allocation-free, and small enough to live in a voice.
    std::uint64_t nextBits();
    double uniformUnit();      ///< [0, 1)
    double uniformBipolar();   ///< [-1, 1)

    void clearState();

    /// The base seed every note's stream is derived from, and splitmix64's own increment. A fresh
    /// instance sounds the same as every other fresh instance, which is what makes a test's measurement
    /// reproducible and a preset's "no RAND" note-on repeatable.
    static constexpr std::uint64_t kSeed = 0x243F6A8885A308D3ull;
    static constexpr std::uint64_t kIncrement = 0x9E3779B97F4A7C15ull;

    double sampleRate_ { 48000.0 };
    NoiseType type_ { NoiseType::White };

    std::uint32_t note_ { 0 };
    std::uint64_t state_ { kSeed };

    // Pink: Kellet's seven-pole pinking filter.
    double pink_[7] {};

    // Brown: a leaky integrator. The leak is a one-pole high pass at ~11 Hz, low enough that the
    // integration regime (and so the -6 dB/octave tilt) covers the whole audible band.
    double brown_ {};

    // Geiger: one decaying burst at a time, restarted by a Poisson event.
    double burst_ {};
    double burstDecay_ { 0.0 };
    double burstProbability_ { 0.0 };
    double burstGain_ { 0.0 };
};

} // namespace xerum::dsp
