#include "sub_osc.h"

#include <cmath>

namespace xerum::dsp
{

namespace
{
/// The rounded rectangle's softness: `tanh(g·sin)` is a square whose edges are a smooth transition
/// rather than a jump, and `g = 3` is far enough from a sine to read as a rectangle while staying smooth.
/// A stated curve, per the header.
constexpr double kRoundRectSoftness = 3.0;

/// The peak of `tanh(kRoundRectSoftness · sin(φ))`, so the shape reaches ±1 like every other one.
double roundRectPeak()
{
    return std::tanh (kRoundRectSoftness);
}

double wrapTurns (double turns)
{
    return turns - std::floor (turns);
}
} // namespace

std::optional<SubShape> subShapeFromName (std::string_view name)
{
    if (name == "Sine")       return SubShape::Sine;
    if (name == "kRoundRect") return SubShape::RoundRect;
    if (name == "kTriangle")  return SubShape::Triangle;
    if (name == "kSaw")       return SubShape::Saw;
    if (name == "kSquare")    return SubShape::Square;
    if (name == "kPulse")     return SubShape::Pulse;

    return std::nullopt;
}

std::string_view subShapeName (SubShape shape)
{
    switch (shape)
    {
        case SubShape::Sine:      return "Sine";
        case SubShape::RoundRect: return "kRoundRect";
        case SubShape::Triangle:  return "kTriangle";
        case SubShape::Saw:       return "kSaw";
        case SubShape::Square:    return "kSquare";
        case SubShape::Pulse:     return "kPulse";
    }

    return "Sine";
}

void SubOscillator::prepare (double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    setFrequency (frequency_);
    reset();
}

void SubOscillator::setShape (SubShape shape)
{
    shape_ = shape;
}

void SubOscillator::setFrequency (double hertz)
{
    frequency_ = hertz;
    phaseIncrement_ = hertz / sampleRate_;
}

void SubOscillator::setInitialPhase (double turns)
{
    initialPhase_ = wrapTurns (turns);
}

void SubOscillator::trigger()
{
    if (! contiguous_)
        phase_ = initialPhase_;
}

void SubOscillator::reset()
{
    phase_ = initialPhase_;
}

double SubOscillator::polyBlep (double turns) const
{
    const auto dt = phaseIncrement_;

    if (dt <= 0.0)
        return 0.0;

    if (turns < dt)
    {
        const auto t = turns / dt;
        return t + t - t * t - 1.0;
    }

    if (turns > 1.0 - dt)
    {
        const auto t = (turns - 1.0) / dt;
        return t * t + t + t + 1.0;
    }

    return 0.0;
}

float SubOscillator::next()
{
    const auto position = phase_;
    auto value = 0.0;

    switch (shape_)
    {
        case SubShape::Sine:
            value = std::sin (kTwoPi * position);
            break;

        case SubShape::Triangle:
            // No discontinuity, so no correction: the corners are a slope change, not a jump.
            value = position < 0.5 ? (4.0 * position - 1.0) : (3.0 - 4.0 * position);
            break;

        case SubShape::Saw:
            value = 2.0 * position - 1.0 - polyBlep (position);
            break;

        case SubShape::Square:
            value = position < 0.5 ? 1.0 : -1.0;
            value += polyBlep (position) - polyBlep (wrapTurns (position + 0.5));
            break;

        case SubShape::Pulse:
        {
            // Zero-mean, then peak-normalised: a 25 % duty pulse straight up would sit at a constant
            // -0.5, which is DC in the audio path rather than a waveform.
            const auto duty = kSubPulseDuty;
            const auto raw = (position < duty ? 1.0 : 0.0) - duty;
            const auto corrected = raw + polyBlep (position) - polyBlep (wrapTurns (position + 1.0 - duty));
            value = corrected / (1.0 - duty);
            break;
        }

        case SubShape::RoundRect:
            value = std::tanh (kRoundRectSoftness * std::sin (kTwoPi * position)) / roundRectPeak();
            break;
    }

    phase_ = wrapTurns (phase_ + phaseIncrement_);

    return static_cast<float> (value);
}

void SubOscillator::process (float* out, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        out[i] = next();
}

} // namespace xerum::dsp
