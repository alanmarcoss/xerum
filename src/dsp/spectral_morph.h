// xerum::dsp — spectral morphing between wavetable frames.
//
// Two mechanisms in the reference wear the same name, and ticket 37 had to measure both because
// guessing from the word "spectral" produced the wrong answer:
//
//   * **`clm ` mode 2, 3 or 4 in a table file — the mode this module does *not* implement, because the
//     reference does not implement it either.** 74 shipped tables carry one of those three digits, and
//     the header comment on `FrameInterpolation` used to read them as "morph in the frequency domain".
//     They are not: the reference renders all of modes 1..4 as a plain linear crossfade. Measured by
//     rendering a hand-built two-frame table whose frames were given deliberately different phases —
//     the sharpest possible discriminator between a crossfade and a spectral transform — with `clm `
//     mode 0, 1, 2, 3 and 4 in turn: modes 1, 2, 3 and 4 produced **byte-identical** WAVs (peak
//     0.075252, identical SHA-256), mode 0 produced a different one (frame A, no interpolation).
//     And four shipped factory tables carrying mode 2 or 4 matched a linear crossfade of their two
//     neighbouring frames to a normalised magnitude distance of <=0.008 over 48 harmonics, against
//     2.4-4.2 for every spectral-morph candidate. So `Wavetable::interpolated` crossfading a
//     `FrameInterpolation::SpectralMorph` table is not a fallback — it is the reference's behaviour,
//     and `TableLookup::needsUnimplementedInterpolation` was reporting a gap that did not exist.
//
//   * **`Oscillator{N}/WTOsc{N}/interpolateAfterLoad` in a preset — the real spectral morph, and the
//     transform this module implements.** It applies to an *embedded* table (the preset carries its
//     cycles in `embeddedWTData`), where 0 means "play the stored cycles as they are", 1 means
//     "crossfade between them" and 2 means "morph in the frequency domain". Measured on a two-frame
//     embedded table at `kParamTablePos` 127.5 (the midpoint), scored over 48 harmonics at note 24:
//
//         magnitude  |c_k| = (1-t)|a_k| + t|b_k|
//         phase      arg c_k = arg a_k + t * wrapToPi(arg b_k - arg a_k)
//
//     fitted the reference's own output to 0.022 magnitude and 0.022 rad, while interpolating the
//     wrapped phase without the shortest-arc rule scored 1.18 rad, discarding phase entirely 1.46 rad
//     and holding the lower frame 1.53 rad. Bin k stays bin k throughout: the transform moves energy
//     between neither harmonics nor frames, which is what makes the anti-aliasing answer below easy.
//     The field is ignored for a table referenced by path — the same preset with `relativePathToWT`
//     pointed at a factory table gave byte-identical renders for the field absent and set to 0, 1 and 2.
//
// **Where the cost is paid.** One forward FFT per source cycle, one inverse per morphed cycle, and two
// cycles of scratch. That is far too much to pay per sample, so this module's intended entry point is
// `expandWavetable`, which does the work once per table at bind time — the same place the reference does
// it, expanding a table to the 256 WT POS slots the manual and `numFrames` both describe. `morph()` on
// its own holds no state between calls and allocates nothing, so a caller that can afford an FFT at a
// frame crossing may also use it directly.
//
// **Anti-aliasing.** Both frames of a wavetable are band-limited individually, and the worry in the
// ticket was that a morph between two band-limited spectra might not be band-limited at the read rate.
// It is: the transform is diagonal in the harmonic basis — harmonic k of the result is built only from
// harmonic k of the two neighbours — so the result is band-limited to the union of the two neighbours'
// bands, exactly the band a crossfade produces. Neither operation can place energy at a harmonic the
// sources did not already have, so the morph cannot alias where the crossfade would not. (The aliasing
// that does exist in this system lives in the *read*: 2048 samples per cycle decimated to
// `sampleRate / frequency` of them. That is why a note-60 render of a bright table did not match the
// table's own harmonics above about 183 while a note-24 render matched to 0.001 — the read, not the
// interpolation.)

#pragma once

#include "wavetable.h"

#include <cstddef>
#include <vector>

namespace xerum::dsp
{

/// The shortest signed angle between two directions, in (-pi, pi].
///
/// This is the whole of the difference between the reference's morph and the obvious implementation:
/// interpolating the phases as stored (in (-pi, pi]) takes the long way round whenever two neighbouring
/// frames disagree by more than half a turn, and measured 1.18 rad away from the reference against this
/// rule's 0.022.
double wrapToPi (double radians);

/// The number of WT POS slots the reference expands a table to when it interpolates after load.
/// Measured, not assumed: `KY - Delicacy`'s first oscillator stores four cycles (8192 samples of
/// `embeddedWTData`) and declares `numFrames` 524288 = 256 x 2048, and the User Guide describes the
/// WT POS selector as holding 256 frames once a morph has been applied.
inline constexpr std::size_t kExpandedFrameCount = 256;

/// The morph between two neighbouring cycles, in the frequency domain.
///
/// Construction sizes the FFT working set; `morph()` then allocates nothing, so one instance can live
/// on an audio thread as long as it was built somewhere else. One instance per reader: the scratch is
/// held in the object, so two threads sharing one is a data race.
class SpectralMorph
{
public:
    /// `frameSize` is the table's samples-per-cycle. 2048 is what the format produces and the only size
    /// the shipped corpus uses; any other size is handled by a direct transform so that no frame size is
    /// silently refused, at the cost of being slower.
    explicit SpectralMorph (std::size_t frameSize = kDefaultFrameSize);

    std::size_t frameSize() const { return frameSize_; }
    bool ready() const { return frameSize_ >= 2; }

    /// Morph `lower` and `upper` (each `frameSize()` samples) into `out`, at position `t`.
    ///
    /// `t` is clamped to [0, 1], and the endpoints are exact copies rather than a transform that happens
    /// to round to them — the morph has to meet the frames it sits between, or a scanned table would
    /// step at every frame boundary.
    ///
    /// The DC and Nyquist bins are self-conjugate: they have no partner bin to mirror, so the arc rule
    /// does not apply to them and a rounded arc would push a real bin off the real axis. They are
    /// blended by sign instead, `(1-t) a + t b`. The reference cannot arbitrate this: its own output
    /// carries no DC at all (an embedded frame with mean +0.300 emerged with 0.058, and the same table
    /// crossfaded emitted -0.009 against a signed blend of -0.050 — so DC is not merely morphed, it is
    /// removed somewhere downstream), and a cycle's Nyquist bin lands above the audio band at any
    /// playable pitch. [MEASURED for DC, UNVERIFIED for Nyquist]
    void morph (const float* lower, const float* upper, float t, float* out);

private:
    void transform (double* re, double* im, bool inverse);

    std::size_t frameSize_ { 0 };
    bool powerOfTwo_ { false };

    /// Radix-2 twiddles, `frameSize/2` of them, or the cos/sin table the direct transform indexes.
    std::vector<double> cosTable_;
    std::vector<double> sinTable_;

    std::vector<double> lowerRe_, lowerIm_;   ///< spectrum of the lower frame, then the result
    std::vector<double> upperRe_, upperIm_;   ///< spectrum of the upper frame
    std::vector<double> dftRe_, dftIm_;       ///< scratch for the non-power-of-two transform only
};

/// Expand a table's real cycles across `targetFrames` WT POS slots, morphing the slots in between.
///
/// Slot `i` sits at frame position `i * (frameCount - 1) / (targetFrames - 1)`, which is what makes the
/// stored cycles land on the first and last slots unchanged. The result's frames are all real, so its
/// `interpolation` is `Linear` — reading between them is a crossfade of two already-morphed cycles, the
/// same two-step the reference performs.
///
/// Cost, stated as numbers because it is what decides whether eager expansion is the right default: one
/// FFT pair per interpolated slot and `targetFrames * frameSize` floats — 2 MB for the reference's 256
/// slots of 2048, i.e. 2 MB *per table*, not per oscillator, and paid once at bind. A table that is
/// already at `targetFrames` cycles is returned unchanged.
Wavetable expandWavetable (const Wavetable& table, std::size_t targetFrames = kExpandedFrameCount);

} // namespace xerum::dsp
