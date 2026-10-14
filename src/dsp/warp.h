// xerum::dsp — oscillator warp.
//
// Warp is a transformation applied to a wavetable oscillator's output, and it splits into two kinds:
//
//   * **phase-domain** — bend, PWM, sync, phase distortion, FM, flip. These rewrite the position the
//     oscillator reads from.
//   * **amplitude-domain** — distortion, filter, remap, ring and amplitude modulation. These shape the
//     sample that came out.
//
// Several of the most-used modes — PD, FM, AM, RM — are **driven by another source**: another oscillator,
// the sub, the noise generator or a filter. The engine has no cross-oscillator routing yet (that arrives
// with the voice, ticket `28`), so a warp takes its modulator as an input and the wiring lands later. That
// keeps the family implemented and testable now, against a known modulator, rather than stubbed.
//
// **On the curves.** Ticket `02` established what each mode *does* from the shipped manual and the enum,
// not what its exact curve is — that would need measuring the reference, which needs the plugin wrapper
// (ticket `30`) and the fidelity suite. So each curve here is a defensible monotonic
// implementation of the documented behaviour, and the tests assert the *characteristic* properties the
// ticket asks for: identity at zero, monotonicity where a reordering would alias, and a measurable effect
// on the spectrum. Aligning the curves themselves is the fidelity suite's job, and ticket `25` records
// that as the remaining distance.
//
// Modes that are documented but not implemented are not silently treated as no-warp: `warpModeFromName`
// reports them as `Unknown` and the caller reports *that*, carrying the **specific name** — `kRemap_3`,
// `kSpectralShift` — because "some warp was not applied" is not something a user can act on. The enum
// therefore holds only implemented modes: a member that no name maps onto would be a half-state, and the
// name is what the report needs to say.

#pragma once

#include <cstddef>
#include <string_view>

namespace xerum::dsp
{

enum class WarpMode
{
    None,

    // Phase domain.
    Sync,
    BendPos,
    BendNeg,
    BendPosNeg,
    AsymPos,
    AsymNeg,
    AsymPosNeg,
    PWM,
    Flip,
    Quantize,
    PhaseDistortion,
    FrequencyModulation,

    // Amplitude domain.
    Distortion,
    RingModulation,
    AmplitudeModulation,
    FilterLowPass,
    FilterHighPass,

    Unknown      ///< documented, not implemented — reported rather than ignored
};

/// Map a preset's mode name (`kSync`, `kPD_OSC`, …) onto a mode.
///
/// The names carry a **source** suffix — `kPD_OSC`, `kPD_SUB`, `kFM_NOISE` — which the engine cannot act
/// on yet, so every source variant maps onto its family. The suffix is not lost: it is in the preset and
/// in the report.
WarpMode warpModeFromName (std::string_view name);

std::string_view warpModeFamilyName (WarpMode mode);

/// **Which signal a source-driven warp reads.** The mode names carry their source as a suffix —
/// `kPD_OSC`, `kFM_OSC2`, `kAM_SUB`, `kRM_NOISE`, `kPD_FILT1` — which ticket `25` dropped when it mapped
/// a name onto its family, because there was no routing for it to name. The suffix is the whole of the
/// difference between `kFM_OSC` and `kFM_SUB`, so it is recovered here and the voice resolves it against
/// the samples it has just produced.
enum class WarpSource
{
    None,
    OtherOsc,        ///< `..._OSC`: the next tabled oscillator, wrapping past the last back to the first
    SecondOtherOsc,  ///< `..._OSC2`: the one after that
    Self,            ///< `kSelfPD`: the oscillator's own previous sample
    Sub,             ///< `..._SUB`
    Noise,           ///< `..._NOISE`
    Filter1,         ///< `..._FILT1`
    Filter2          ///< `..._FILT2`
};

/// The source a warp mode name encodes, or `None` for a mode that takes no modulator.
///
/// **A stated convention, on one point.** Serum 1's source choices name *the other* oscillator, and the
/// corpus's own suffixes (`_OSC`, `_OSC2`) name two of them with three oscillators present. Which
/// oscillator `_OSC` is for each slot is therefore read as the cyclic successor: `_OSC` is the slot after
/// the warp's own, `_OSC2` the one after that, both wrapping. That keeps a warp from modulating itself
/// by accident, which is the reading the alternative (a fixed slot A) would produce for the most-used
/// family — `kPD_OSC` is 12.6% of the corpus's warps.
WarpSource warpSourceFromName (std::string_view modeName);

/// The tabled oscillator slot a `WarpSource` resolves to for a warp on `slot`, given `tableCount`
/// oscillators. Returns -1 for a source that is not a tabled oscillator.
int warpSourceSlot (WarpSource source, int slot, int tableCount);

/// The warp's own parameter, and the modulator it is driven by.
class Warp
{
public:
    void setMode (WarpMode mode) { mode_ = mode; }
    WarpMode mode() const { return mode_; }

    /// The warp knob, 0..1, as the preset stores it.
    void setAmount (double amount);
    double amount() const { return amount_; }

    /// A distortion-family variant, because the family shares one curve shape parameterised differently.
    void setDistortionVariant (std::string_view name) { distortionVariant_ = name; }

    /// Rewrite the read phase, **before** the table is read. A no-op for amplitude-domain modes.
    double phase (double turns, double modulator = 0.0) const;

    /// Shape the produced sample, **after** the table is read.
    ///
    /// Takes the phase too, because some amplitude-domain warps are phase-dependent: `flip` inverts
    /// polarity after a duty point, which it cannot know from the sample alone.
    float sample (double turnsAtRead, float value, double modulator = 0.0);

    /// How many resonant stages the filter-family modes use, for their own tests and tuning.
    void setFilterStages (int stages) { filterStages_ = stages; }

    /// Filter state — the one warp that has memory. Reset when a voice is stolen.
    void reset();
    void prepare (double sampleRate);

private:
    WarpMode mode_ { WarpMode::None };
    double amount_ { 0.0 };
    double sampleRate_ { 48000.0 };
    int filterStages_ { 1 };
    std::string_view distortionVariant_ {};

    // One-pole state, shared by the low- and high-pass modes — the only warp with memory, and the reason
    // `sample` is not const.
    double lowPassState_ { 0.0 };
};

} // namespace xerum::dsp
