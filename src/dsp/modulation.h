// xerum::dsp — the modulation matrix.
//
// **This header is a frozen contract while tickets `27` and `28` are built in parallel.** Ticket `27`
// owns the implementation and may ADD to this file; it may not change what is already declared, because
// the voice layer is being written against it at the same time. When both land, the contract thins out
// to whatever the voice actually uses.
//
// The shapes come from ticket `03`, which read them out of the presets rather than the manual:
//
//   * a row's `source` is a **two-element `[source, auxSource]` pair**, both from the reference's
//     1-based enum of 60 names (value 0 = no source, and no preset stores it);
//   * destinations are `(class, instance, parameter)`: `destModuleTypeString`, `destModuleID`,
//     `destModuleParamID` — with **FX instances encoding `rackIndex * 100 + slotIndex`**;
//   * `kParamAmount` is percent and signed, so polarity is the amount's sign, not a separate flag;
//   * the drawn transfer curves live beside the row, not in it.
//
// Evaluation follows ticket `13`: control rate per block with the result **interpolated across the
// block** for continuous destinations, held for discrete ones. The evaluator is a pure function of
// modulator state, so moving it per-sample later is a change of call site rather than a rewrite.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xerum::dsp
{

/// The reference's own count: 60 names, 1-based, value 0 unused. `modSourceName (1)` is `"Mod Wheel"`.
inline constexpr int kReferenceSourceCount = 60;

/// The guide name of a reference source value, or empty for 0/out of range.
std::string_view modSourceName (int referenceValue);

/// The reverse: a guide name to its reference value, or 0.
int modSourceFromName (std::string_view name);

/// Whether v1 evaluates this source. Ticket `12` cut the head; the rest are **named and reported**.
///
/// Added by ticket `27`, which is what actually chose the line: a source is shipped when
/// `ModulationInputs` carries a value for it, and shipped sources are therefore every one of the
/// reference's 60 **except `NoteOn Rand2` (22) and `ModSlot` (60)**. Reported, with the reason:
///
///   * 22 `NoteOn Rand2` — the manual says `NoteOn Rand 1` and `2` are "two separate random numbers
///     generated on a note on event"; the contract carries one (`noteOnRandom`), so the second is not
///     something this layer can invent. 15 corpus rows use it.
///   * 60 `ModSlot` — a matrix row used as a source is another row's output, i.e. an edge in a graph
///     the caller owns, not a value in `ModulationInputs`. No corpus row uses it.
///
/// `NoteOn Alt.2` (24) **is** shipped: the manual states it "is the inverse of Note-On Alt 1", so it is
/// `1 - noteOnAlternate` rather than a second input.
bool modSourceIsImplemented (int referenceValue);

/// One matrix row, as a preset stores it.
struct ModSlot
{
    bool used { false };
    int source { 0 };              ///< reference value 1..60; 0 = no source
    int auxSource { 0 };           ///< 0 = no aux, which is 90.4% of the corpus
    double amount { 0.0 };         ///< `kParamAmount`, percent, signed — the sign IS the polarity
    bool bipolar { false };
    bool bypassed { false };
    double curveIn { 1.0 };        ///< `kParamCurveIn`, a straight line until the curve is decoded
    double curveOut { 1.0 };

    std::string destinationClass;      ///< `destModuleTypeString`, e.g. "VoiceFilter", "FXEQ"
    int destinationInstance { 0 };     ///< `destModuleID`; FX: rackIndex * 100 + slotIndex
    int destinationParameter { 0 };    ///< `destModuleParamID`
    std::string destinationName;       ///< `destModuleParamName`, redundant with the id and kept for reports
};

/// Everything the evaluator reads. Values are already in their natural units — the voice fills this in,
/// the matrix does not reach for anything.
struct ModulationInputs
{
    double modWheel { 0.0 };
    double pitchBend { 0.0 };
    double expressionX { 0.0 };
    double expressionY { 0.0 };
    double expressionZ { 0.0 };
    double aftertouch { 0.0 };
    double polyAftertouch { 0.0 };
    double velocity { 0.0 };
    double releaseVelocity { 0.0 };
    double noteNumber { 0.0 };
    double noiseOsc { 0.0 };
    double noteOnRandom { 0.0 };
    double noteOnRandomDiscrete { 0.0 };
    double noteOnAlternate { 0.0 };
    double fixed { 1.0 };
    double activeVoices { 0.0 };
    double voiceIndex { 0.0 };
    double voiceMod1 { 0.0 };
    double voiceMod2 { 0.0 };

    double macro[8] {};
    double envelope[4] {};
    double lfo[10] {};          ///< `LFO 1`..`LFO 10`
    double lfoY[10] {};         ///< `LFO 1 Y`..`LFO 10 Y`, the unipolar secondary output
    double osc[3] {};           ///< `OSC A/B/C` as modulation sources
    double subOsc { 0.0 };
    double voiceFilter[2] {};   ///< `Filter 1/2` as sources
};

/// **The source-value convention** (added by ticket `27`, because it is the one thing a caller can get
/// silently wrong): every value above is the source's **output on the reference's own 0..1 range** —
/// the range its "OUT" graph draws — *not* a raw MIDI or unit quantity. `fixed` is the default 1.0 that
/// makes that concrete, and the manual's own description of the derived sources normalises them the
/// same way ("Active Voices, derived by dividing the number of active voices by the maximum number of
/// voices allowed by the POLY count setting"). So `noteNumber` is the scalars note curve's output, not
/// a MIDI note number, and `activeVoices` is the divided value, not a count.
///
/// The matrix turns that range into the signed one the destination receives: unidirectional rows use
/// the source as it is, and `bipolar` rows re-centre it, `2s - 1`. The manual is the evidence for what
/// `bipolar` means: "Specify whether the modulation is unidirectional or bi-directional ... it depends
/// on whether you prefer the destination control (the knob position) to be at the beginning
/// (unidirectional) or the middle (bi-directional)" — the beginning is 0, the middle is 0.5.

/// A destination's modulation for one block: where it starts, where it ends, and whether it may ramp.
struct ModulatedRamp
{
    double start { 0.0 };
    double end { 0.0 };
    bool discrete { false };

    /// The value at a frame offset within the block — interpolated, or held when discrete.
    double at (int frame, int frames) const;
};

class ModulationMatrix
{
public:
    void prepare (double sampleRate, int maximumBlockFrames);

    /// The rows, straight from a document. Unused rows are kept: the index is the slot's identity.
    void setSlots (std::vector<ModSlot> slots);
    const std::vector<ModSlot>& slots() const { return slots_; }

    /// Advance to a block. Sums every row onto its destination and prepares each destination's ramp.
    void beginBlock (const ModulationInputs& inputs, int frames);

    /// The summed modulation for one destination, ramped across the current block.
    ///
    /// One unit is the destination's full range: `kParamAmount` of 100 contributes 1.0, so a caller
    /// multiplies by the parameter's span. A destination nothing modulates reads as a zero ramp.
    ModulatedRamp rampFor (std::string_view destinationClass, int instance, int parameter) const;

    /// Every destination the current block wrote, for tests and for the report.
    std::size_t modulatedDestinationCount() const;

    /// The sources in use that v1 does not evaluate, by reference value. The caller reports these by
    /// name — "a modulation source was ignored" is not something a user can act on.
    std::vector<int> unimplementedSourcesInUse() const;

private:
    std::vector<ModSlot> slots_ {};
    double sampleRate_ { 48000.0 };
    int maximumBlockFrames_ { 0 };

    // --- added by ticket 27 -------------------------------------------------------------------
    //
    // The declared surface above is unchanged; everything below is state the implementation needs so
    // that `beginBlock` can be allocation-free. All of it is built once, in `setSlots`.

    /// One row, resolved once: a source index, a depth, and an index into `destinations_`.
    struct Row
    {
        int source { 0 };
        int auxSource { 0 };
        double depth { 0.0 };        ///< `kParamAmount` / 100 — the sign is the polarity
        bool bipolar { false };
        double curveIn { 0.0 };      ///< the reference's CRV control, percent, 0 = a straight line
        double curveOut { 0.0 };     ///< the reference's OUT curve, same scale
        int destination { -1 };
    };

    /// One destination, summed across every row that targets it.
    struct Destination
    {
        int classIndex { 0 };        ///< index into `destinationClasses_`
        int instance { 0 };
        int parameter { 0 };
        bool discrete { false };
        double previous { 0.0 };     ///< the value this destination had at the last block's boundary
        double current { 0.0 };
        bool hasPrevious { false };
    };

    std::vector<Row> rows_ {};
    std::vector<Destination> destinations_ {};
    std::vector<std::string> destinationClasses_ {};   ///< distinct class names, for comparison
    std::vector<double> blockSums_ {};                 ///< scratch, sized with `destinations_`
    std::vector<unsigned char> blockWritten_ {};
    std::vector<int> unimplementedInUse_ {};
};

/// Whether `className` is one of the reference's matrix destination classes.
///
/// The vocabulary is ticket `03`'s corpus-wide measurement: it is a **closed list of module class
/// names**, and an FX class is named by its own class string (`FXEQ`, `FXFilter`, …) with the rack
/// encoded in the instance. A name outside the list is not silently treated as "no modulation" — it is
/// what `modDestinationUnresolvedReason` exists to report.
bool modDestinationClassKnown (std::string_view className);

/// Whether `instance` is a valid instance id for that class.
///
/// `Env0..3`, `LFO0..9`, `Macro0..7`, `RoutingSlot0..6`, `LFOPointModBus0..15` and the rest are the
/// reference's own counts (ticket `03` §4, confirmed against every destination in the factory bank).
/// For an `FX*` class the id is `rackIndex * 100 + slotIndex`, so this checks the **encoding** — the
/// rack is one of the reference's three and the slot fits the two digits the encoding gives it. The
/// rack's *depth* is not a fact about the class: it is the length of that preset's `FXRack{r}/FX`
/// array, which the loader has and this layer does not.
bool modDestinationInstanceKnown (std::string_view className, int instance);

/// Whether the destination parameter is **discrete**, i.e. takes one value per block rather than being
/// ramped: the reference types it as an enum, so there is nothing to interpolate between.
///
/// Keyed by the parameter's **name**, not by its id, and that is a finding rather than a preference:
/// `destModuleParamID` is **version-dependent** — `Oscillator[10]` is `kParamDetune` at schema 3.0 and
/// `kParamStart` at 4.0, and `WTOsc[1]` is `kParamTablePos` at 3.0 and `kParamWarpVar` at 4.0 — so the
/// name is the only stable identity a row's destination has across the versions we accept.
bool modDestinationIsDiscrete (std::string_view className, std::string_view parameterName);

/// Why a `used` row's destination does not resolve, or empty when it does.
///
/// This is the **address** check — is this a class the reference addresses, with an instance and a named
/// parameter in range — and it is the whole of what the matrix needs. Two things it deliberately does
/// *not* claim, both of which the corpus walk measures rather than assumes:
///
///   * a class whose parameters the reference's own model does not carry is **not** rejected here. 135
///     corpus rows name such a parameter (`Global.kParamMasterTuning` 128 times, `RetriggerState.*` 7),
///     and the address still resolves: the matrix computes the value and a consumer that knows the
///     parameter can take it. Whether a user is *told* is the import report's job, and today it is
///     silent about them — `modulation_coverage_test.cpp` names them for exactly that reason;
///   * a row with no destination at all is not an error of wiring, it is missing data: the 6 rows in
///     `SY - Tetris` kept a source and an amount and lost their destination, and this is what reports
///     them.
std::string modDestinationUnresolvedReason (const ModSlot& slot);

} // namespace xerum::dsp
