#include "oscillator.h"

#include <cmath>

namespace xerum::dsp
{

void WavetableOscillator::prepare (double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    for (auto& warp : warps_)
        warp.prepare (sampleRate_);
    setFrequency (frequency_);
}

void WavetableOscillator::setTable (const Wavetable* table)
{
    table_ = table;

    // A table with one frame cannot be positioned between frames; keep the position inside whatever the
    // new table offers rather than carrying a stale one.
    if (table_ != nullptr && table_->frameCount > 0)
        setFramePosition (framePosition_);
}

void WavetableOscillator::setFrequency (double hertz)
{
    frequency_ = hertz;
    phaseIncrement_ = hertz / sampleRate_;
}

void WavetableOscillator::setPhase (double turns)
{
    phase_ = turns - std::floor (turns);
}

void WavetableOscillator::setFramePosition (double frames)
{
    if (table_ == nullptr || table_->frameCount == 0)
    {
        framePosition_ = 0.0;
        return;
    }

    const auto last = static_cast<double> (table_->frameCount - 1);
    framePosition_ = frames < 0.0 ? 0.0 : (frames > last ? last : frames);
}

void WavetableOscillator::setWarp (int slot, WarpMode mode, double amount)
{
    warps_[wrapSlot (slot)].setMode (mode);
    warps_[wrapSlot (slot)].setAmount (amount);
}

float WavetableOscillator::currentSample()
{
    if (! hasTable())
        return 0.0f;

    const auto& table = *table_;

    // The warps rewrite *where* the table is read, before the read: a hard sync at 2x has to make the
    // oscillator traverse its table twice, which shifting the output afterwards cannot do. Two slots, in
    // series — the second warp sees the position the first one produced.
    const auto phaseAfterFirst = warps_[0].phase (phase_, warpModulators_[0]);
    const auto readPhase = warps_[1].phase (phaseAfterFirst, warpModulators_[1]);

    // Within the frame, linearly between the two neighbouring samples. Without this a 2048-point table
    // scanned fast produces the buzz of a nearest-neighbour reader.
    const auto x = readPhase * static_cast<double> (table.frameSize);
    const auto i0 = static_cast<std::size_t> (x);
    const auto i1 = (i0 + 1) % table.frameSize;
    const auto fraction = static_cast<float> (x - static_cast<double> (i0));

    const auto a = table.interpolated (framePosition_, i0);
    const auto b = table.interpolated (framePosition_, i1);

    const auto read = a + (b - a) * fraction;

    // The amplitude-domain half of each warp: distortion, filtering, the modulation families, flip.
    // Slot 0 first, then slot 1, which is the order the two slots are wired in.
    const auto afterFirst = warps_[0].sample (phaseAfterFirst, read, warpModulators_[0]);

    return warps_[1].sample (readPhase, afterFirst, warpModulators_[1]);
}

float WavetableOscillator::next()
{
    const auto value = currentSample();

    phase_ += phaseIncrement_;

    // Wrap with fmod rather than a single subtraction: a frequency above the sample rate — which a
    // modulation source can ask for — would otherwise leave the phase above 1 forever.
    if (phase_ >= 1.0 || phase_ < 0.0)
        phase_ -= std::floor (phase_);

    return value;
}

void WavetableOscillator::process (float* out, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        out[i] = next();
}

} // namespace xerum::dsp
