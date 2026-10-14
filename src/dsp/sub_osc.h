// xerum::dsp — the SUB oscillator: the reference's slot 4, a fixed single-engine slot.
//
// ## The shape vocabulary is six, not five
//
// `Oscillator4/SubOsc4/plainParams/kParamShape` is a string leaf whose **schema** vocabulary (ticket 21's
// generated table, `kValues557`) holds five values — `{kPulse, kRoundRect, kSaw, kSquare, kTriangle}` —
// and 118 of the 626 factory presets store one. The reference's own init state says the vocabulary is
// **six**: `xerum-host --dump-params` against Serum 2 reports `SUB OSC | Sub Shape | 0.000000000 | Sine`
// with **6 steps**, and the generated defaults table (`Oscillator{N}/SubOsc4/plainParams/kParamShape`)
// records the default as the string `Sine`. So `Sine` is the sixth member and it is *never stored*,
// because the writer omits a value equal to the default — which is exactly why the schema's vocabulary
// does not contain it. The shipped manual's waveform table lists the same six.
//
// The five stored names and their distinction are all measurable, and the manual states each one's
// harmonic character in its own words, which is what `sub_osc_test.cpp` asserts: sine "only the
// fundamental", triangle "primarily odd harmonics" and "a small amount", square "rich in odd harmonics",
// saw "both odd and even harmonics", pulse "the high and low states are not equal", and rounded rect a
// square "with smoother transitions… less harsh, more rounded". The exact edges are stated curves, not
// measured ones — the fidelity suite's business, as with the warp families (ticket 25).
//
// ## Pitch: this is the one engine whose whole point is a frequency relationship
//
// The reference's own init state reports `SUB OSC | Sub Pitch Track | On`, and `Sub Octave` at 0 with a
// nine-step range (-4..+4) — so the sub sounds the note it is played at, transposed by `kParamOctave`,
// and 198 of the 626 presets transpose it (80 at -1, 74 at -2). `kParamCoarsePit` is "continuous (no
// snap) semitones" per the manual, and `kParamPitch`/`kParamFine` are the slot's own controls, so the
// engine's single input is a frequency in hertz and the caller sums them.
//
// ## Anti-aliasing
//
// The discontinuities (saw, square, pulse) are corrected with PolyBLEP rather than left naive: a naive
// 100 Hz saw has harmonics above Nyquist folding back into the audible band, and the sub is a low
// oscillator whose whole job is a clean low end. Sine, triangle and rounded rect have no discontinuity
// and need no correction (the triangle's harmonics fall at 1/k², the rounded rect's faster still).

#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace xerum::dsp
{

/// The six waveforms: the schema's five stored names plus the default the schema never sees.
enum class SubShape
{
    Sine,
    RoundRect,
    Triangle,
    Saw,
    Square,
    Pulse
};

/// `kParamShape`'s vocabulary, including the `Sine` default. Returns `nullopt` for anything else, so a
/// caller can report an out-of-vocabulary name rather than being handed a substitute.
std::optional<SubShape> subShapeFromName (std::string_view name);

std::string_view subShapeName (SubShape shape);

/// The pulse's duty cycle. The manual says only that the two states are "not equal"; 0.25 is the classic
/// pulse and is a stated convention, not a measured one.
inline constexpr double kSubPulseDuty = 0.25;

class SubOscillator
{
public:
    void prepare (double sampleRate);

    void setShape (SubShape shape);
    SubShape shape() const { return shape_; }

    /// The fundamental, in hertz. The engine does not know about notes; the caller converts.
    void setFrequency (double hertz);

    /// `kParamInitialPhase`, in turns (degrees / 360).
    void setInitialPhase (double turns);
    double initialPhase() const { return initialPhase_; }

    /// `kParamContiguousPhase`: a new note continues the previous note's phase instead of restarting at
    /// the initial phase. The manual: "New notes continue with the phase of the previous note."
    void setContiguousPhase (bool on) { contiguous_ = on; }
    bool contiguousPhase() const { return contiguous_; }

    /// Note on.
    void trigger();

    /// Silence and re-arm to the initial phase.
    void reset();

    double phase() const { return phase_; }

    float next();
    void process (float* out, std::size_t count);

private:
    /// The PolyBLEP correction for a step at `turns == 0`, in units of the step's own height.
    double polyBlep (double turns) const;

    double sampleRate_ { 48000.0 };
    double frequency_ { 55.0 };
    double phase_ { 0.0 };          ///< turns, [0, 1)
    double phaseIncrement_ { 0.0 };
    double initialPhase_ { 0.0 };

    bool contiguous_ { false };
    SubShape shape_ { SubShape::Sine };

    static constexpr double kTwoPi = 6.28318530717958647692;
};

} // namespace xerum::dsp
