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

#include "wavetable.h"

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

    /// Position along the frame axis, in frames and possibly fractional.
    void setFramePosition (double frames);
    double framePosition() const { return framePosition_; }

    /// One sample, phase advanced by one step.
    float next();

    /// `count` samples, phase advanced.
    void process (float* out, std::size_t count);

    /// Read the current position without advancing — for analysis and tests.
    float currentSample() const;

private:
    const Wavetable* table_ { nullptr };
    double sampleRate_ { 48000.0 };
    double frequency_ { 440.0 };
    double phase_ { 0.0 };            ///< in turns, 0..1
    double phaseIncrement_ { 0.0 };   ///< turns per sample
    double framePosition_ { 0.0 };
};

} // namespace xerum::dsp
