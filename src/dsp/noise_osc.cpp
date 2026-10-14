#include "noise_osc.h"

#include <cmath>

namespace xerum::dsp
{

namespace
{
/// In-band (100 Hz - 8 kHz) RMS calibration. Each constant was measured as the reciprocal of the
/// generator's own in-band RMS on a 2^15-sample run, so all four colours arrive at the same level in the
/// band that matters. `noise_osc_test.cpp` asserts the resulting match; these are the numbers it starts
/// from, not a claim about the reference's own gain staging.
constexpr double kWhiteGain = 0.727;
constexpr double kPinkGain = 0.0973;
constexpr double kBrownGain = 18.6;
constexpr double kGeigerGain = 1.33;

/// Brown's leak: a one-pole high pass at ~11 Hz at 48 kHz, so integration (and the -6 dB/octave tilt)
/// covers the audible band while DC cannot wander.
constexpr double kBrownLeak = 0.0015;

/// Geiger's burst process. The manual establishes the *character* (sporadic random bursts at
/// unpredictable intervals) and not the numbers, so these two are stated conventions — see the header.
constexpr double kGeigerTicksPerSecond = 250.0;
constexpr double kGeigerDecaySeconds = 0.0002;
} // namespace

std::optional<NoiseType> noiseTypeFromName (std::string_view name)
{
    if (name == "White")  return NoiseType::White;
    if (name == "Pink")   return NoiseType::Pink;
    if (name == "Brown")  return NoiseType::Brown;
    if (name == "Geiger") return NoiseType::Geiger;

    return std::nullopt;
}

std::string_view noiseTypeName (NoiseType type)
{
    switch (type)
    {
        case NoiseType::White:  return "White";
        case NoiseType::Pink:   return "Pink";
        case NoiseType::Brown:  return "Brown";
        case NoiseType::Geiger: return "Geiger";
    }

    return "White";
}

void NoiseOscillator::prepare (double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    burstDecay_ = std::exp (-1.0 / (kGeigerDecaySeconds * sampleRate_));
    burstProbability_ = kGeigerTicksPerSecond / sampleRate_;
    burstGain_ = kGeigerGain;

    reset();
}

void NoiseOscillator::setType (NoiseType type)
{
    // The generator's state is shared by the four colours, and no colour's filter means anything to
    // another: a change of type starts from a clean state rather than continuing the previous colour's
    // pink history.
    if (type != type_)
    {
        type_ = type;
        clearState();
    }
}

void NoiseOscillator::trigger (bool decorrelate)
{
    if (decorrelate)
        ++note_;

    clearState();
}

void NoiseOscillator::reset()
{
    clearState();
}

void NoiseOscillator::clearState()
{
    state_ = kSeed + kIncrement * static_cast<std::uint64_t> (note_);

    for (auto& pole : pink_)
        pole = 0.0;

    brown_ = 0.0;
    burst_ = 0.0;
}

std::uint64_t NoiseOscillator::nextBits()
{
    state_ += kIncrement;

    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;

    return z ^ (z >> 31);
}

double NoiseOscillator::uniformUnit()
{
    // The top 53 bits of a 64-bit word, scaled by 2^-53: every representable double in [0, 1) is
    // reachable and none above it, which is what a uniform draw is.
    return static_cast<double> (nextBits() >> 11) * 0x1.0p-53;
}

double NoiseOscillator::uniformBipolar()
{
    return 2.0 * uniformUnit() - 1.0;
}

float NoiseOscillator::next()
{
    switch (type_)
    {
        case NoiseType::White:
            return static_cast<float> (uniformBipolar() * kWhiteGain);

        case NoiseType::Pink:
        {
            const double white = uniformBipolar();

            // Kellet's seven-pole pinking filter: a staircase of one-poles whose summed response is
            // -3 dB/octave across the audio band to within a fraction of a dB.
            auto& b = pink_;
            b[0] = 0.99886 * b[0] + white * 0.0555179;
            b[1] = 0.99332 * b[1] + white * 0.0750759;
            b[2] = 0.96900 * b[2] + white * 0.1538520;
            b[3] = 0.86650 * b[3] + white * 0.3104856;
            b[4] = 0.55000 * b[4] + white * 0.5329522;
            b[5] = -0.7616 * b[5] - white * 0.0168980;
            const double sum = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362;
            b[6] = white * 0.115926;

            return static_cast<float> (sum * kPinkGain);
        }

        case NoiseType::Brown:
            brown_ = (brown_ + kBrownLeak * uniformBipolar()) / (1.0 + kBrownLeak);
            return static_cast<float> (brown_ * kBrownGain);

        case NoiseType::Geiger:
        {
            // A Poisson event restarts the burst; between events it decays exponentially. The mean
            // interval is 1/rate and the intervals are memoryless, which is the "unpredictable
            // intervals" the manual describes.
            if (uniformUnit() < burstProbability_)
                burst_ = uniformBipolar();

            const auto value = burst_;
            burst_ *= burstDecay_;

            return static_cast<float> (value * burstGain_);
        }
    }

    return 0.0f;
}

void NoiseOscillator::process (float* out, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        out[i] = next();
}

} // namespace xerum::dsp
