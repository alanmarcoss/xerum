#include "warp.h"

#include <algorithm>
#include <cmath>

namespace xerum::dsp
{

namespace
{

constexpr double kPi = 3.14159265358979323846;

double clamp01 (double value) { return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value); }

double wrap (double turns) { return turns - std::floor (turns); }

bool startsWith (std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr (0, prefix.size()) == prefix;
}

/// A monotonic two-piece power curve: identity at `g == 1`, steeper either side of it.
///
/// Monotonic matters more than the exact shape. A phase warp that is not monotonic reads the table out of
/// order, which folds the waveform and aliases — the kind of failure that sounds like a bug in the table
/// rather than in the warp.
double bendCurve (double turns, double g)
{
    if (g == 1.0)
        return turns;

    return turns < 0.5 ? 0.5 * std::pow (2.0 * turns, g)
                       : 1.0 - 0.5 * std::pow (2.0 * (1.0 - turns), g);
}

/// The bend family's knob, as a signed amount. The two-directional variants are neutral in the *middle*,
/// which is the whole point of them.
double bendAmount (WarpMode mode, double amount)
{
    switch (mode)
    {
        case WarpMode::BendPos:
        case WarpMode::AsymPos:    return amount;
        case WarpMode::BendNeg:
        case WarpMode::AsymNeg:    return -amount;
        case WarpMode::BendPosNeg:
        case WarpMode::AsymPosNeg: return 2.0 * amount - 1.0;
        default:                   return 0.0;
    }
}

/// The distortion family's variants. One family, one knob — the variants differ in transfer curve, and
/// ticket 02 records what each one is *for* even where it could not record its exact shape.
enum class DistortionCurve
{
    Soft,          ///< tanh: soft clip and soft saturation
    HardClip,
    LinearFold,    ///< triangle folding
    SineFold,
    SineShaper,
    Rectify,
    ZeroSquare,
    Diode1,
    Diode2,
    Asym,
    StompBox,
    TapeSat,
    Tube
};

DistortionCurve distortionCurveFor (std::string_view name)
{
    if (name == "kDistHardClip")   return DistortionCurve::HardClip;
    if (name == "kDistLinFold")    return DistortionCurve::LinearFold;
    if (name == "kDistSinFold")    return DistortionCurve::SineFold;
    if (name == "kDistSineShaper") return DistortionCurve::SineShaper;
    if (name == "kDistRectify")    return DistortionCurve::Rectify;
    if (name == "kDistZeroSquare") return DistortionCurve::ZeroSquare;
    if (name == "kDistDiode1")     return DistortionCurve::Diode1;
    if (name == "kDistDiode2")     return DistortionCurve::Diode2;
    if (name == "kDistAsym")       return DistortionCurve::Asym;
    if (name == "kDistStompBox")   return DistortionCurve::StompBox;
    if (name == "kDistTapeSat")    return DistortionCurve::TapeSat;
    if (name == "kDistTube")       return DistortionCurve::Tube;

    return DistortionCurve::Soft;   // soft clip, soft sat, and anything unnamed in the family
}

/// Linear (triangle) folding: the signal reflects off ±1 as many times as it needs to, instead of being
/// clipped flat. That reflection is what separates the fold variants from the clip variants.
double linearFold (double x)
{
    for (int i = 0; i < 64 && (x > 1.0 || x < -1.0); ++i)
        x = x > 1.0 ? 2.0 - x : -2.0 - x;

    return x;
}

double shape (DistortionCurve curve, double x)
{
    switch (curve)
    {
        case DistortionCurve::HardClip:   return std::clamp (x, -1.0, 1.0);
        case DistortionCurve::LinearFold: return linearFold (x);
        case DistortionCurve::SineFold:   return std::sin (x * kPi * 0.5);   // folds once |x| > 1
        case DistortionCurve::SineShaper: return std::sin (x);
        case DistortionCurve::Rectify:    return std::abs (std::tanh (x));   // even-order only
        case DistortionCurve::ZeroSquare: return x >= 0.0 ? 1.0 : -1.0;

        // One polarity driven harder than the other is what makes a diode asymmetric, and asymmetry is
        // what puts even harmonics in a distortion.
        case DistortionCurve::Diode1:     return x <= 0.0 ? std::tanh (x * 0.3) : std::tanh (x);
        case DistortionCurve::Diode2:     return std::sin (x * 2.0);
        case DistortionCurve::Asym:       return x <= 0.0 ? std::tanh (x * 0.6) : std::tanh (x * 1.2);
        case DistortionCurve::StompBox:
            return x <= 0.0 ? std::clamp (x * 0.7, -1.0, 1.0) * 0.9 : std::clamp (x * 1.4, -1.0, 1.0);

        case DistortionCurve::TapeSat:
        {
            const auto t = std::tanh (x);
            return t + 0.25 * t * t;      // soft, and slightly asymmetric, as tape is
        }

        case DistortionCurve::Tube:
        {
            const auto t = std::tanh (x);
            return t - 0.25 * t * t;
        }

        case DistortionCurve::Soft:       return std::tanh (x);
    }

    return std::tanh (x);
}

} // namespace

WarpMode warpModeFromName (std::string_view name)
{
    if (name.empty() || name == "kNoWarp")
        return WarpMode::None;

    // The shipped families. Everything here is a claim the engine can honour, and the import report holds
    // the other side of it: a mode not listed here is *named* in the report rather than substituted.
    //
    // The mode names carry a **source** suffix — `kPD_OSC`, `kFM_NOISE`, `kRM_SUB` — which the engine
    // cannot act on yet (routing arrives with the voice, ticket `28`), so every source variant maps onto
    // its family. The suffix is not lost: it is still in the preset and in the report.
    if (name == "kSync")       return WarpMode::Sync;
    if (name == "kBendPos")    return WarpMode::BendPos;
    if (name == "kBendNeg")    return WarpMode::BendNeg;
    if (name == "kBendPosNeg") return WarpMode::BendPosNeg;
    if (name == "kASYMPos")    return WarpMode::AsymPos;
    if (name == "kASYMNeg")    return WarpMode::AsymNeg;
    if (name == "kASYMPosNeg") return WarpMode::AsymPosNeg;
    if (name == "kPWM")        return WarpMode::PWM;
    if (name == "kFlip")       return WarpMode::Flip;
    if (name == "kQuantize")   return WarpMode::Quantize;

    if (startsWith (name, "kPD_") || name == "kSelfPD") return WarpMode::PhaseDistortion;
    if (startsWith (name, "kFM"))                       return WarpMode::FrequencyModulation;
    if (startsWith (name, "kAM_"))                      return WarpMode::AmplitudeModulation;
    if (startsWith (name, "kRM_"))                      return WarpMode::RingModulation;
    if (startsWith (name, "kDist"))                     return WarpMode::Distortion;
    if (name == "kFilterLPF")                           return WarpMode::FilterLowPass;
    if (name == "kFilterHPF")                           return WarpMode::FilterHighPass;

    // Everything else is documented and deliberately not implemented, and it is *reported* rather than
    // guessed at. The set is not one thing:
    //
    //   * `kDLM` — in the enum, undocumented in the shipped guide (ticket 02).
    //   * `kEvenOdd`, `kRemap_1/3/4` — the remap modes need an editable curve graph the engine has not
    //     built, and even/odd's exact semantics were not established.
    //   * `kGate`, `kSpread`, `kSmear`, `kDetune`, `kAddharmonics`, `kSpectral*`, `kShepard*`,
    //     `kVocode_*`, `kMask_*`, `kPeak*` — the warp menus of the deferred oscillator engines
    //     (granular, sample, spectral), which v1 does not synthesise at all.
    return WarpMode::Unknown;
}

std::string_view warpModeFamilyName (WarpMode mode)
{
    switch (mode)
    {
        case WarpMode::None:                return "none";
        case WarpMode::Sync:                return "sync";
        case WarpMode::BendPos:
        case WarpMode::BendNeg:
        case WarpMode::BendPosNeg:          return "bend";
        case WarpMode::AsymPos:
        case WarpMode::AsymNeg:
        case WarpMode::AsymPosNeg:          return "asymmetry";
        case WarpMode::PWM:                 return "pulse width";
        case WarpMode::Flip:                return "flip";
        case WarpMode::Quantize:            return "quantize";
        case WarpMode::PhaseDistortion:     return "phase distortion";
        case WarpMode::FrequencyModulation: return "frequency modulation";
        case WarpMode::Distortion:          return "distortion";
        case WarpMode::RingModulation:      return "ring modulation";
        case WarpMode::AmplitudeModulation: return "amplitude modulation";
        case WarpMode::FilterLowPass:       return "filter (low pass)";
        case WarpMode::FilterHighPass:      return "filter (high pass)";
        case WarpMode::Unknown:             return "unknown";
    }

    return "unknown";
}

void Warp::prepare (double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
}

void Warp::setAmount (double amount) { amount_ = clamp01 (amount); }

void Warp::reset() { lowPassState_ = 0.0; }

double Warp::phase (double turns, double modulator) const
{
    switch (mode_)
    {
        case WarpMode::Sync:
        {
            // Hard sync: read the table faster and let it restart. The warp knob sets the ratio.
            const auto ratio = 1.0 + amount_ * 15.0;
            return wrap (turns * ratio);
        }

        case WarpMode::BendPos:
        case WarpMode::BendNeg:
        case WarpMode::BendPosNeg:
        {
            const auto g = std::exp (bendAmount (mode_, amount_) * 1.5);
            return bendCurve (turns, g);
        }

        case WarpMode::AsymPos:
        case WarpMode::AsymNeg:
        case WarpMode::AsymPosNeg:
        {
            // The same curve, applied to the second half only: the asymmetry is the point.
            const auto g = std::exp (bendAmount (mode_, amount_) * 1.5);

            if (g == 1.0 || turns < 0.5)
                return turns;

            return 0.5 + 0.5 * std::pow (2.0 * (turns - 0.5), g);
        }

        case WarpMode::PWM:
            // Push the read position, which is what pulse-width does to a wave.
            return wrap (turns + amount_ * 0.5);

        case WarpMode::Quantize:
        {
            // Non-decreasing, so no reordering: coarse steps rather than a folded waveform.
            const auto steps = 2.0 + (1.0 - amount_) * 62.0;
            return std::floor (turns * steps) / steps;
        }

        case WarpMode::PhaseDistortion:
            // Classic Casio-style PD: the modulator bulges the phase, scaled by the warp knob.
            return wrap (turns + modulator * amount_ * 0.5);

        case WarpMode::FrequencyModulation:
            // Phase modulation, which is what FM on an oscillator amounts to. The through-zero and
            // exponential variants share this curve: their differences are real but small, and the
            // fidelity suite is where they get distinguished.
            return wrap (turns + modulator * amount_);

        default:
            return turns;
    }
}

float Warp::sample (double turnsAtRead, float value, double modulator)
{
    switch (mode_)
    {
        case WarpMode::Distortion:
        {
            if (amount_ == 0.0)
                return value;      // the knob is depth: at zero the mode is transparent

            const auto drive = 1.0 + amount_ * 24.0;
            const auto shaped = shape (distortionCurveFor (distortionVariant_), static_cast<double> (value) * drive);

            // Blended by the knob rather than by drive alone, so that "amount 0 is identity" holds for
            // every family — a warp that colours the sound at zero would make every preset that does not
            // use it wrong.
            return static_cast<float> (value + amount_ * (shaped - value));
        }

        case WarpMode::RingModulation:
            // Ring modulation is multiplication by a bipolar modulator; amplitude modulation by a
            // unipolar one. That is the whole difference, and the only one worth preserving.
            return static_cast<float> (value * (1.0 - amount_ + amount_ * modulator));

        case WarpMode::AmplitudeModulation:
            return static_cast<float> (value * (1.0 - amount_ + amount_ * (modulator * 0.5 + 0.5)));

        case WarpMode::Flip:
            // Polarity inversion after a duty point — which is why this needs the phase.
            return amount_ > 0.0 && turnsAtRead >= amount_ ? -value : value;

        case WarpMode::FilterLowPass:
        case WarpMode::FilterHighPass:
        {
            // One-pole, applied `filterStages_` times. The cutoff follows the warp knob, because that is
            // what "filter as a warp" means: the knob is the cutoff, not a depth.
            //
            // Squared, so the low end of the knob actually gets low — a cutoff curve that is linear in the
            // knob never attenuates anything by enough to be recognisable as a filter.
            const auto cutoff = 0.002 + amount_ * amount_ * 0.9;
            const auto coefficient = 1.0 - std::exp (-2.0 * kPi * cutoff);

            auto filtered = static_cast<double> (value);

            for (int stage = 0; stage < filterStages_; ++stage)
            {
                lowPassState_ += coefficient * (filtered - lowPassState_);

                filtered = mode_ == WarpMode::FilterLowPass ? lowPassState_
                                                            : filtered - lowPassState_;
            }

            return static_cast<float> (filtered);
        }

        default:
            return value;
    }
}

} // namespace xerum::dsp
