#include "oscillator.h"

#include <cmath>

namespace xerum::dsp
{

void WavetableOscillator::prepare (double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
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

float WavetableOscillator::currentSample() const
{
    if (! hasTable())
        return 0.0f;

    const auto& table = *table_;

    // Within the frame, linearly between the two neighbouring samples. Without this a 2048-point table
    // scanned fast produces the buzz of a nearest-neighbour reader.
    const auto x = phase_ * static_cast<double> (table.frameSize);
    const auto i0 = static_cast<std::size_t> (x);
    const auto i1 = (i0 + 1) % table.frameSize;
    const auto fraction = static_cast<float> (x - static_cast<double> (i0));

    const auto a = table.interpolated (framePosition_, i0);
    const auto b = table.interpolated (framePosition_, i1);

    return a + (b - a) * fraction;
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
