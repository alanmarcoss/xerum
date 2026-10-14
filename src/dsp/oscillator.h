// xerum::dsp — the wavetable oscillator.
//
// Phase accumulator, position along the table's frames, and interpolation on both axes: across frames
// (which frame are we between) and within a frame (which sample are we between). The second one is easy
// to skip and audible when skipped — a 2048-sample table scanned at audio rate with no interpolation
// buzzes.
//
// **The oscillator does not invent a table.** With none set it produces silence and says so through
// `hasTable()`; substituting a default table is exactly the silent drop the import contract forbids, so
// the decision is left where it can be reported.

#pragma once

#include "warp.h"
#include "wavetable.h"

#include <array>

#include <cstddef>

namespace xerum::dsp
{

class WavetableOscillator
{
public:
    void prepare (double sampleRate);

    /// Bind a table. The table must outlive the oscillator; nothing is copied, because a factory table is
    /// up to 2 MB and the engine holds several.
    void setTable (const Wavetable* table);
    const Wavetable* table() const { return table_; }
    bool hasTable() const { return table_ != nullptr && ! table_->empty(); }

    void setFrequency (double hertz);
    double frequency() const { return frequency_; }

    void setPhase (double turns);
    double phase() const { return phase_; }

    /// Two warp slots, because the reference gives every wavetable oscillator two — `kParamWarpMenu` and
    /// `kParamWarpMenu2`, each with its own amount — and **174 of the 626 factory presets use both at
    /// once** (measured; see `warp_coverage_test.cpp`). One slot would be silently half a preset on those.
    static constexpr int kWarpSlotCount = 2;

    /// Bind a warp to a slot. `kNoWarp` is the default, and a warp at amount zero is transparent — so an
    /// oscillator that is not using one reads exactly as it did before this existed.
    void setWarp (int slot, WarpMode mode, double amount = 0.0);
    Warp& warp (int slot) { return warps_[wrapSlot (slot)]; }
    const Warp& warp (int slot) const { return warps_[wrapSlot (slot)]; }

    /// The modulator the source-driven warps (FM, PD, AM, RM) read: the voice supplies another slot's
    /// output here once routing exists. Until then it is zero, which makes those modes transparent rather
    /// than wrong — the alternative is inventing a source, which the import contract forbids.
    void setWarpModulator (int slot, double value) { warpModulators_[wrapSlot (slot)] = value; }

    /// Position along the frame axis, in frames and possibly fractional.
    void setFramePosition (double frames);
    double framePosition() const { return framePosition_; }

    /// One sample, phase advanced by one step.
    float next();

    /// `count` samples, phase advanced.
    void process (float* out, std::size_t count);

    /// Read the current position without advancing — for analysis and tests.
    ///
    /// Not const: the warp's read path may carry state (the filter-as-warp is a filter).
    float currentSample();

private:
    const Wavetable* table_ { nullptr };
    double sampleRate_ { 48000.0 };
    double frequency_ { 440.0 };
    double phase_ { 0.0 };            ///< in turns, 0..1
    double phaseIncrement_ { 0.0 };   ///< turns per sample
    double framePosition_ { 0.0 };

    // The warps sit *in* the read path rather than after it: sync and the phase warps rewrite the position
    // the table is read from, which no post-processing stage can express.
    //
    // The two slots are **serial**, slot 0 first. Phase rewrites therefore compose, and the amplitude
    // half of each slot shapes what the previous stage produced — each seeing the position *it* actually
    // read from, which is what `flip` needs.
    std::array<Warp, kWarpSlotCount> warps_;
    std::array<double, kWarpSlotCount> warpModulators_ { 0.0, 0.0 };

    static int wrapSlot (int slot) { return slot < 0 ? 0 : (slot >= kWarpSlotCount ? kWarpSlotCount - 1 : slot); }
};

} // namespace xerum::dsp
