// xerum::dsp — the modulation sources the matrix reads.
//
// `modulation.cpp` is the bus: it takes a `ModulationInputs` and sums rows onto destinations. **Nothing
// in the engine used to fill that struct's generated half** — the four matrix envelopes, the ten LFOs
// (and their second outputs), the eight macros and the two audio-rate families (`OSC A/B/C`,
// `Filter 1/2`) — so 37 of the matrix's 57 source cases evaluated to a constant zero while looking
// wired (ticket 48). This file is the generator for them.
//
// ## What is generated here, and what a voice supplies
//
//   * **the ten LFOs** (`LFO{N}`). Rate, delay, rise, smoothing, swing, direction, phase, one-shot vs
//     looping, the `Mono` flag, the drawn shape (`curveData`) and the second output (`pathData`, the
//     `LFO n Y` source).
//   * **the four matrix envelopes** (`Env0..3`): attack, hold, decay, sustain, release, the three
//     segment curves, start and end levels. They are separate generators from the amplitude envelope
//     ticket 28 built: different settings, different state, and nothing couples their stages.
//   * **the eight macros** — a value, so they live in `ModulationInputs::macro` and are the caller's.
//   * **the audio-rate sources** (`osc[3]`, `voiceFilter[2]`, `noiseOsc`, `subOsc`) are the voice's:
//     they are the samples the voice just produced, which this layer cannot know.
//
// ## Control rate, per ticket 13
//
// Every generator here advances **once per block**. That is not an approximation of the matrix's
// design, it is the design: `ModulationMatrix::beginBlock` reads a source at the block's end and ramps
// from the previous block's value, so a source that is exact at the boundaries is interpolated in
// between. Envelope segments are therefore evaluated *analytically* — a segment's level is a function of
// its progress, so advancing the progress by `frames / sampleRate` is exact, and no generator needs a
// per-sample loop. That is what keeps `advance` allocation-free and cheap enough for 32 voices.
//
// ## What the schema fixes and what is a stated convention
//
// The schema (ticket 21, corpus-wide in ticket 03's `research/01-serum2-param-model.schema.json`) is the
// specification: `LFO{N}/plainParams/` carries rate, mode, delay, phase, smoothing, swing, rise,
// direction, dotted/triplet and the `Mono` flag, and `curveData`/`pathData` carry the drawn point arrays.
// Where the schema gives a *name* but not a curve, the implementation says so at the point of use — see
// `lfoRateHz`, `dspRateToHz` and the `kParamType` handling below.

#pragma once

#include "modulation.h"

#include <array>
#include <cstddef>

namespace xerum::dsp
{

/// The reference's own counts: ten LFOs (`LFO0..9`), four envelopes (`Env0..3`), eight macros
/// (`Macro0..7`). They are the instance ranges `kDestinationClasses` already uses for `LFO`, `Env` and
/// `Macro` destinations, so a source index and a destination instance are the same number.
inline constexpr int kLfoCount = 10;
inline constexpr int kMatrixEnvelopeCount = 4;
inline constexpr int kMacroCount = 8;

/// The drawn-shape point ceiling. `LFO{N}/curveData/numPoints` is observed 1..212 and
/// `pathData/numPoints` 1..65 in the factory bank, so 256 holds every drawn shape in the corpus with
/// room for a preset that draws more.
inline constexpr int kMaxLfoPoints = 256;

/// A built-in LFO shape, for an LFO that ships no drawn curve. The names are the corpus's own
/// `curveDisplayName` values (`Sine`, `triangle`, `saw up`, `saw down`, `square`).
enum class LfoShape
{
    Sine,
    Triangle,
    SawUp,
    SawDown,
    Square
};

/// `LFO{N}/plainParams/kParamType` — the LFO's generator, not its shape. `Curve` (the field's absence)
/// is the drawn/built-in shape; `Path` traces `pathData`; `RandomSampleHold` holds a value per cycle;
/// `Lorenz`/`Rossler` are chaotic attractors.
enum class LfoType
{
    Curve,
    Path,
    RandomSampleHold,
    Lorenz,
    Rossler
};

/// One drawn point. `x` is its position (0..1 for a curve), `y` its value, and `curve` the bend of the
/// segment that *ends* at it — the reference's `curveVals[]`, whose own neutral is 0.5.
struct LfoPoint
{
    double x { 0.0 };
    double y { 0.0 };
    double curve { 0.5 };
};

/// A drawn shape: the reference's `curveData` (monotone x) or `pathData` (an arbitrary polyline).
///
/// The arrays in a preset carry `numPoints + 1` entries — the last repeats the first point's `y` at
/// `x == 1` to close the cycle — so `count` is what the preset's `numPoints` says plus the closing
/// point, and sampling walks the segments the preset authored.
struct LfoCurve
{
    std::array<LfoPoint, kMaxLfoPoints> points {};
    int count { 0 };

    /// Whether the last point joins the first. `curveData` is always closed (a cycle), and `pathData`
    /// carries the reference's own `isOpen`.
    bool closed { true };

    void clear() { count = 0; }
    void add (double x, double y, double curve);
    bool empty() const { return count < 2; }

    /// Sample a monotone curve at `x` (0..1). A repeated `x` is a **step**: the segment is vertical and
    /// the later point's value wins, which is how the corpus draws a hard edge (measured: `LD - Artform`'s
    /// LFO 1 has `xVals 0.125, 0.125` with `yVals 0.097, 1.0`).
    double sampleCurve (double x) const;

    /// Sample a closed or open polyline by cycle position `t` (0..1), returning its x or its y.
    double samplePathX (double t) const;
    double samplePathY (double t) const;

private:
    double samplePath (double t, bool x) const;
};

/// One LFO's settings, from `LFO{N}`.
struct LfoSettings
{
    LfoType type { LfoType::Curve };
    LfoShape shape { LfoShape::Sine };
    double rateHz { 0.5 };          ///< resolved from `kParamRate` by `lfoRateHz`
    double phaseTurns { 0.0 };      ///< `kParamPhase` / 360
    double delaySeconds { 0.0 };    ///< `kParamDelay`: the output holds before it starts moving
    double riseSeconds { 0.0 };     ///< `kParamRise`: a fade-in from the held value
    double smooth { 0.0 };          ///< `kParamSmooth` percent / 100
    double swing { 0.0 };           ///< `kParamSwing` percent / 100
    double direction { 1.0 };       ///< `kParamDirection`: 1 = forward, -1 = reverse
    bool oneShot { false };         ///< `kParamMode == "Envelope"`: one cycle per note, then hold
    bool freeRunning { false };     ///< `kParamMode == "Free"`: a note-on does not reset the phase
    bool mono { false };            ///< `kParamMono`: one generator for the whole pool, not per voice

    LfoCurve curve {};              ///< `curveData` — the `LFO n` output
    LfoCurve path {};               ///< `pathData` — the `LFO n Y` output

    /// Whether this LFO has anything drawn to play. False means the built-in `shape` is used.
    bool hasDrawnCurve() const { return ! curve.empty(); }
};

/// One matrix envelope (`Env{N}`), distinct from the voice's amplitude envelope.
struct MatrixEnvelopeSettings
{
    double attack { 0.0 };
    double hold { 0.0 };
    double decay { 0.0 };
    double sustain { 1.0 };
    double release { 0.0 };
    double curve1 { 50.0 };   ///< `kParamCurve1`, percent; 50 is a straight line
    double curve2 { 50.0 };
    double curve3 { 50.0 };
    double start { 0.0 };     ///< `kParamStart`, the level the attack begins from
    double end { 0.0 };       ///< `kParamEnd`, the level the release ends at
    bool legatoInverted { false };   ///< `kParamLegatoInverted`: a legato retarget does not retrigger
};

/// Everything the generators need. Held by `VoiceEngine` and read (never copied) by each voice.
struct ModSourceSettings
{
    std::array<LfoSettings, kLfoCount> lfos {};
    std::array<MatrixEnvelopeSettings, kMatrixEnvelopeCount> envelopes {};
};

/// `kParamRate` percent to hertz.
///
/// **A stated convention.** The schema carries the field's range (0..100) and its name but not its unit,
/// and the corpus cannot settle it either: the observed rates are 0.00002..34.7 with a median of 2.5, and
/// no single interpretation is forced. What *is* measured is the distribution, so the map is chosen to
/// put the corpus's own median and ninetieth percentile in the range an LFO is used in: a linear percent
/// of a 20 Hz ceiling puts `kParamRate` 2.5 at 0.5 Hz and 13.4 at 2.7 Hz. `kParamRate10x` multiplies by
/// ten, which is the reference's own control on the same field.
double lfoRateHz (double ratePercent, bool rate10x);

/// A percent whose 50 is a straight line, as the reference's CRV and envelope-curve controls are: 0
/// bends the segment concave, 100 convex, 50 linear. Monotone in `progress` for every curve value, which
/// is what an envelope segment needs.
double shapedSegment (double progress, double curvePercent);

/// The value of one LFO at a phase, in the 0..1 range `ModulationInputs` documents (a bipolar shape such
/// as a sine therefore sits at 0.5 when it crosses zero). `cycleIndex` seeds the sample-and-hold type.
double lfoValue (const LfoSettings& settings, double phaseTurns, long cycleIndex);

/// The LFO's second output — `LFO n Y`. From `pathData`'s y when a path is drawn; otherwise it follows
/// the curve, so a preset that names `LFO n Y` without drawing a path still moves something.
double lfoSecondValue (const LfoSettings& settings, double phaseTurns, long cycleIndex);

/// One matrix envelope's state machine. Self-contained so a test can drive a single envelope.
class MatrixEnvelope
{
public:
    void prepare (double sampleRate);
    void reset();
    void trigger (const MatrixEnvelopeSettings& settings, bool legato);
    void release (const MatrixEnvelopeSettings& settings);

    /// Advance `frames` samples and return the level at the end of them.
    double advance (const MatrixEnvelopeSettings& settings, int frames);

    double level() const { return value_; }
    bool idle() const { return stage_ == Stage::Idle; }

private:
    enum class Stage
    {
        Idle,
        Attack,
        Hold,
        Decay,
        Sustain,
        Release
    };

    /// Move to a stage and set the level and the progress rate it starts from. A stage whose duration is
    /// zero is *skipped* — `advance` completes it immediately and moves on — which is what makes
    /// `kParamHold` absent (0 s) mean "no hold" rather than "hold forever".
    void enterStage (const MatrixEnvelopeSettings& settings, Stage stage);

    /// Whether the current stage is one that runs on a clock rather than holding a level.
    bool segmented() const { return stage_ != Stage::Idle && stage_ != Stage::Sustain; }

    /// The level the current stage and progress describe.
    double sample (const MatrixEnvelopeSettings& settings) const;

    double sampleRate_ { 48000.0 };
    Stage stage_ { Stage::Idle };
    double value_ { 0.0 };
    double progress_ { 0.0 };     ///< 0..1 within the current segment
    double rate_ { 0.0 };         ///< progress per second over the current segment
    double releaseFrom_ { 0.0 };  ///< the level the release started from
};

/// One voice's generators: the ten LFOs and the four matrix envelopes, advanced together each block.
///
/// The macros and the audio-rate sources are not here — a macro is the caller's value and an oscillator's
/// output is the voice's — so `advance` writes only what it generates.
class ModulationGenerator
{
public:
    void prepare (double sampleRate);
    void reset();

    /// A note started. `legato` is a legato retarget rather than a new note.
    void noteOn (const ModSourceSettings& settings, bool legato);
    void noteOff (const ModSourceSettings& settings);

    /// Restart the `Mono` LFOs that are one-shot. Called by the engine when the *first* voice starts, so
    /// a one-shot LFO the whole pool shares runs once per phrase rather than once per voice.
    void restartOneShotLfos (const ModSourceSettings& settings);

    /// Advance `frames` samples and write every generated value into `inputs`.
    ///
    /// An LFO the preset marks `Mono` is left alone: the engine runs it once for the whole pool and its
    /// value arrives through the block inputs, so overwriting it here would fight the engine.
    void advance (const ModSourceSettings& settings, ModulationInputs& inputs, int frames);

    /// Advance only the `Mono` LFOs. The engine's generator uses this and nothing else; a voice's
    /// generator never does.
    void advanceMonoLfos (const ModSourceSettings& settings, ModulationInputs& inputs, int frames);

    /// The un-smoothed LFO phase, for tests and for the report.
    double lfoPhase (int index) const;

private:
    struct LfoState
    {
        double phase { 0.0 };
        double elapsedSeconds { 0.0 };
        double smoothed { 0.0 };
        double smoothedY { 0.0 };
        long cycles { 0 };
        bool started { false };
    };

    void advanceLfos (const ModSourceSettings& settings, ModulationInputs& inputs, int frames,
                      bool monoOnly);

    double sampleRate_ { 48000.0 };
    std::array<LfoState, kLfoCount> lfos_ {};
    std::array<MatrixEnvelope, kMatrixEnvelopeCount> envelopes_ {};
};

} // namespace xerum::dsp
