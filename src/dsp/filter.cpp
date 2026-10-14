#include "filter.h"

#include <algorithm>
#include <cmath>

namespace xerum::dsp
{

namespace
{

constexpr double kPi = 3.14159265358979323846;

double clampUnit (double value) { return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value); }

bool startsWith (std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr (0, prefix.size()) == prefix;
}

/// `tan(pi * f / fs)`, clamped short of Nyquist.
///
/// This is where a naive design comes apart: as the cutoff approaches Nyquist the tangent goes to
/// infinity and the filter is emitting NaNs within a few samples. Stability at 44.1 and 96 kHz is an
/// acceptance criterion, and this clamp is the line that delivers it.
double tangent (double fractionOfSampleRate)
{
    return std::tan (kPi * std::clamp (fractionOfSampleRate, 0.0, 0.4999));
}

bool kindForLetter (char letter, FilterKind& out)
{
    switch (letter)
    {
        case 'L': out = FilterKind::LowPass;  return true;
        case 'H': out = FilterKind::HighPass; return true;
        case 'B': out = FilterKind::BandPass; return true;
        case 'N': out = FilterKind::Notch;    return true;
        case 'P': out = FilterKind::Peak;     return true;
        default:  return false;
    }
}

/// A run of digits as a number, or -1 when it is not a run of digits.
int parseNumber (std::string_view text)
{
    if (text.empty())
        return -1;

    int value = 0;

    for (const auto character : text)
    {
        if (character < '0' || character > '9')
            return -1;

        value = value * 10 + (character - '0');
    }

    return value;
}

/// `CombL6N` → a low pass in the loop and negative feedback. The suffix is
/// `<optional L><optional H><optional P|N>`, and no polarity letter means positive.
CombSpec combSpecFromSuffix (std::string_view suffix)
{
    CombSpec spec;
    spec.lowPassPoles = suffix.find ('L') != std::string_view::npos ? 1 : 0;
    spec.highPassPoles = suffix.find ('H') != std::string_view::npos ? 1 : 0;
    spec.negative = suffix.find ('N') != std::string_view::npos;
    return spec;
}

/// The reference's 0–1 "Q" as a pole radius.
///
/// Ticket `04` measured that this value is *not* a textbook Q, so this is a stated convention rather
/// than a derivation: 0 is broadly damped, 1 is almost undamped. What is exact is the *shape* — where
/// the poles and zeros sit, which is the preset's own geometry.
double radiusForQ (double frequencyOfNyquist, double q)
{
    const auto damping = 1.0 + 99.0 * clampUnit (q);
    return std::min (0.9999, std::exp (-kPi * clampUnit (frequencyOfNyquist) / damping));
}

constexpr double kButterworthDamping = 1.4142135623730951;   // 1/Q for Q = 0.7071

} // namespace

std::string_view toString (FilterKind kind)
{
    switch (kind)
    {
        case FilterKind::LowPass:   return "low pass";
        case FilterKind::HighPass:  return "high pass";
        case FilterKind::BandPass:  return "band pass";
        case FilterKind::Notch:     return "notch";
        case FilterKind::Peak:      return "peak";
        case FilterKind::LowShelf:  return "low shelf";
        case FilterKind::HighShelf: return "high shelf";
        case FilterKind::AllPass:   return "all pass";
    }

    return "unknown";
}

std::string_view toString (FilterModel model)
{
    switch (model)
    {
        case FilterModel::Svf:           return "state variable";
        case FilterModel::DualSvf:       return "dual state variable";
        case FilterModel::MorphSvf:      return "morphing state variable";
        case FilterModel::Ladder:        return "ladder";
        case FilterModel::Comb:          return "comb/flanger";
        case FilterModel::Allpass:       return "all pass cascade";
        case FilterModel::Diffuser:      return "diffuser";
        case FilterModel::PoleZero:      return "pole/zero";
        case FilterModel::Unimplemented: return "unimplemented";
    }

    return "unimplemented";
}

FilterSpec filterSpecFromName (std::string_view name)
{
    FilterSpec spec;

    if (name.empty() || name == "default")
        return spec;

    // ---------------------------------------------------------------------------------------------
    // The names the letter rule cannot read, listed one at a time because they are irregular.
    // ---------------------------------------------------------------------------------------------

    // The ladder. `MgL24` is the Moog ladder at 24 dB/oct — the number is the slope, as everywhere.
    if (startsWith (name, "MgL"))
    {
        const auto slope = parseNumber (name.substr (3));

        if (slope != 6 && slope != 12 && slope != 18 && slope != 24)
            return spec;

        spec.model = FilterModel::Ladder;
        spec.kind = FilterKind::LowPass;
        spec.poles = polesForSlope (slope);
        return spec;
    }

    // The newer ladder flavours: whole circuits rather than one circuit at a slope, all 24 dB/oct.
    if (name == "LadderMg" || name == "LadderEMS" || name == "LadderAcid" || name == "DirtyMg")
    {
        spec.model = FilterModel::Ladder;
        spec.kind = FilterKind::LowPass;
        spec.poles = 4;

        // Ordered by how hard each one is: the plain Moog ladder, then EMS and the acid ladder, and
        // `DirtyMg` dirtiest — which is what its name says.
        spec.flavourDrive = name == "LadderMg"   ? 1.0
                          : name == "LadderEMS"  ? 1.5
                          : name == "LadderAcid" ? 2.0
                                                 : 3.0;
        return spec;
    }

    if (name == "Allpasses")
    {
        spec.model = FilterModel::Allpass;
        spec.stages = 12;
        return spec;
    }

    if (name == "Diffuser")
    {
        spec.model = FilterModel::Diffuser;
        spec.stages = 4;
        return spec;
    }

    // A notch under a name that does not spell it: the state-variable family (ticket 12).
    if (name == "BandReject")
    {
        spec.model = FilterModel::Svf;
        spec.kind = FilterKind::Notch;
        spec.poles = 2;
        return spec;
    }

    // Not a circuit at all: a response drawn in the preset as eight pole/zero sections.
    if (name == "PZ_SVF")
    {
        spec.model = FilterModel::PoleZero;
        return spec;
    }

    // The flanger/phaser hybrid puts a filter in the allpass path as well as the comb's, and only the
    // comb half is modelled here. A half-modelled circuit is not the circuit the preset asked for, so
    // it is reported rather than approximated.
    if (startsWith (name, "FlangePhase"))
        return spec;

    // The comb/flanger family: one delay line, an optional filter in the loop, and a polarity.
    for (const auto* prefix : { "DistComb", "Comb", "Flange" })
    {
        if (! startsWith (name, prefix))
            continue;

        spec.model = FilterModel::Comb;
        spec.comb = combSpecFromSuffix (name.substr (std::string_view (prefix).size()));
        spec.comb.distorted = startsWith (name, "DistComb");
        return spec;
    }

    // `Phase24N` puts the stage count *before* the polarity, so the trailing-number rule cannot read it.
    if (startsWith (name, "Phase"))
    {
        const auto rest = name.substr (5);
        const auto stages = parseNumber (rest.substr (0, rest.find_first_of ("NPHL")));

        // `Phase48H6P` and friends also filter the allpass path — reported for the same reason as the
        // flanger/phaser hybrid above.
        if (stages < 0 || rest.find ('H') != std::string_view::npos)
            return spec;

        spec.model = FilterModel::Allpass;
        spec.stages = stages;
        return spec;
    }

    // `HEQ6`/`HEQ12`: the high shelf, on the same state-variable core, with the number as its slope.
    if (startsWith (name, "HEQ"))
    {
        const auto slope = parseNumber (name.substr (3));

        if (slope != 6 && slope != 12)
            return spec;

        spec.model = FilterModel::Svf;
        spec.kind = FilterKind::HighShelf;
        spec.poles = polesForSlope (slope);
        return spec;
    }

    // ---------------------------------------------------------------------------------------------
    // The letter rule: `<responses><slope in dB/oct>`. One rule, three families — the letters are
    // literally the responses to combine.
    // ---------------------------------------------------------------------------------------------

    auto digitsAt = name.size();

    while (digitsAt > 0 && name[digitsAt - 1] >= '0' && name[digitsAt - 1] <= '9')
        --digitsAt;

    if (digitsAt == name.size() || digitsAt == 0)
        return spec;      // no slope, or nothing but a slope

    const auto letters = name.substr (0, digitsAt);
    const auto slope = parseNumber (name.substr (digitsAt));

    if (slope != 6 && slope != 12 && slope != 18 && slope != 24)
        return spec;

    for (const auto letter : letters)
    {
        FilterKind ignored {};

        if (! kindForLetter (letter, ignored))
            return spec;      // `Reverb1`, `SNH1`, `ADD_BASS` — named, and not one of ours
    }

    if (letters.size() == 1)
    {
        spec.model = FilterModel::Svf;
        kindForLetter (letters[0], spec.kind);
        spec.poles = polesForSlope (slope);
        return spec;
    }

    if (letters.size() == 2)
    {
        // Two SVFs summed: `LH12` low plus high, `PP12` two peaks, `BN12` band plus notch.
        spec.model = FilterModel::DualSvf;
        kindForLetter (letters[0], spec.kind);
        kindForLetter (letters[1], spec.secondary);
        spec.poles = polesForSlope (slope);
        return spec;
    }

    if (letters.size() == 3)
    {
        // Three responses to morph between, in the order written: `LBH24` low→band→high,
        // `LNH24` low→notch→high, `BPN12` band→peak→notch. MORPH is the position.
        spec.model = FilterModel::MorphSvf;
        kindForLetter (letters[0], spec.kind);
        kindForLetter (letters[1], spec.secondary);
        kindForLetter (letters[2], spec.tertiary);
        spec.poles = polesForSlope (slope);
        return spec;
    }

    return spec;
}

// ---------------------------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------------------------

void VoiceFilter::SvfSection::set (double cutoffHz, double sampleRate, double damping)
{
    g = tangent (cutoffHz / sampleRate);
    k = damping;
    a1 = 1.0 / (1.0 + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
}

void VoiceFilter::SvfSection::update (double input)
{
    // TPT: the state is updated in the order that keeps the integrators consistent, which is why the
    // cutoff can go anywhere below Nyquist without the filter coming apart.
    const auto v3 = input - ic2;
    const auto v1 = a1 * ic1 + a2 * v3;
    const auto v2 = ic2 + a2 * ic1 + a3 * v3;

    ic1 = 2.0 * v1 - ic1;
    ic2 = 2.0 * v2 - ic2;

    lowpass = v2;
    bandpass = v1;
    highpass = input - k * v1 - v2;
}

double VoiceFilter::SvfSection::take (FilterKind kind, double shelfGain, double dry) const
{
    switch (kind)
    {
        case FilterKind::LowPass:   return lowpass;
        case FilterKind::HighPass:  return highpass;
        case FilterKind::BandPass:  return bandpass;
        case FilterKind::Notch:     return lowpass + highpass;
        case FilterKind::Peak:      return lowpass - highpass;
        case FilterKind::AllPass:   return lowpass + highpass - k * bandpass;
        case FilterKind::LowShelf:  return dry + (shelfGain - 1.0) * lowpass;
        case FilterKind::HighShelf: return dry + (shelfGain - 1.0) * highpass;
    }

    return lowpass;
}

void VoiceFilter::OnePole::set (double cutoffHz, double sampleRate)
{
    coefficient = 1.0 - std::exp (-2.0 * kPi * std::clamp (cutoffHz / sampleRate, 0.0, 0.49));
}

double VoiceFilter::OnePole::lowpass (double input)
{
    state += coefficient * (input - state);
    return state;
}

void VoiceFilter::FirstOrderAllpass::set (double coefficient)
{
    this->coefficient = std::clamp (coefficient, -0.9999, 0.9999);
}

double VoiceFilter::FirstOrderAllpass::process (double input)
{
    const auto output = -coefficient * input + state;
    state = input + coefficient * output;
    return output;
}

void VoiceFilter::DelayLine::resize (std::size_t samples)
{
    buffer.assign (std::max<std::size_t> (samples, 4), 0.0);
    write = 0;
}

void VoiceFilter::DelayLine::clear()
{
    std::fill (buffer.begin(), buffer.end(), 0.0);
    write = 0;
}

double VoiceFilter::DelayLine::read (double delaySamples) const
{
    if (buffer.empty())
        return 0.0;

    const auto size = static_cast<double> (buffer.size());
    auto position = static_cast<double> (write) - delaySamples;

    while (position < 0.0)
        position += size;

    const auto i0 = static_cast<std::size_t> (position) % buffer.size();
    const auto i1 = (i0 + 1) % buffer.size();
    const auto fraction = position - std::floor (position);

    return buffer[i0] + (buffer[i1] - buffer[i0]) * fraction;
}

void VoiceFilter::DelayLine::writeSample (double value)
{
    if (buffer.empty())
        return;

    buffer[write] = value;
    write = (write + 1) % buffer.size();
}

double VoiceFilter::Biquad::process (double input)
{
    // `a1`/`a2` already carry the signs of the pole polynomial, so they are subtracted.
    const auto output = b0 * input + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;

    x2 = x1;
    x1 = input;
    y2 = y1;
    y1 = output;

    return output;
}

// ---------------------------------------------------------------------------------------------
// The filter
// ---------------------------------------------------------------------------------------------

void VoiceFilter::prepare (double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    // The lowest comb frequency allowed is 20 Hz, so the line holds one period of it. The diffuser's
    // longest stage is 8.1 ms, which is far shorter at any sane rate.
    comb_.resize (static_cast<std::size_t> (sampleRate_ / 20.0) + 4);

    for (auto& line : diffuserLines_)
        line.resize (static_cast<std::size_t> (sampleRate_ * 0.01) + 4);

    reset();
    rebuild();
}

void VoiceFilter::setSpec (const FilterSpec& spec)
{
    spec_ = spec;
    reset();
    rebuild();
}

double VoiceFilter::cutoffHzFromNormalised (double normalised)
{
    // Logarithmic, 20 Hz to 20 kHz. Ticket `04` established that the stored value is normalised and
    // logarithmic but not what the endpoints are; this is a stated convention, and the slope tests are
    // deliberately independent of it, so measuring the real endpoints later moves a scale and nothing
    // else.
    return 20.0 * std::pow (1000.0, clampUnit (normalised));
}

void VoiceFilter::setCutoff (double normalised)
{
    cutoffHz_ = cutoffHzFromNormalised (normalised);
    secondaryCutoffHz_ = cutoffHz_;
    rebuild();
}

void VoiceFilter::setSecondaryCutoff (double normalised)
{
    secondaryCutoffHz_ = cutoffHzFromNormalised (normalised);
    rebuild();
}

void VoiceFilter::setResonance (double percent)
{
    resonance_ = clampUnit (percent / 100.0);
    rebuild();
}

void VoiceFilter::setDrive (double percent)
{
    // Deliberately not a rebuild: the drive is read per sample by the models that saturate.
    drive_ = clampUnit (percent / 100.0);
}

void VoiceFilter::setMorph (double position)
{
    morph_ = clampUnit (position);
    rebuild();
}

bool VoiceFilter::implements (std::string_view name)
{
    return filterSpecFromName (name).model != FilterModel::Unimplemented;
}

double VoiceFilter::shelfGain() const
{
    // A shelf's gain control is the type's VAR in the reference, whose mapping is not established. Until
    // it is, RES carries it, over ±12 dB, so the shelf is controllable rather than fixed.
    return 0.25 + resonance_ * 3.75;
}

void VoiceFilter::reset()
{
    for (auto& section : sections_)
        section.clear();

    for (auto& pole : ladder_)
        pole.clear();

    sparePole_.clear();

    for (auto& stage : allpasses_)
        stage.clear();

    for (auto& line : diffuserLines_)
        line.clear();

    for (auto& pole : loopFilters_)
        pole.clear();

    for (auto& biquad : pz_)
        biquad.clear();

    comb_.clear();
    ladderFeedbackState_ = 0.0;
}

void VoiceFilter::rebuild()
{
    // Q from 0.5 to 8 as resonance goes 0 to 100 percent — the damping term is 1/Q.
    const auto damping = 1.0 / (0.5 + resonance_ * 7.5);

    switch (spec_.model)
    {
        case FilterModel::Svf:
        case FilterModel::DualSvf:
        case FilterModel::MorphSvf:
        {
            // Section 0 carries the resonance; the second section of a 24 dB/oct cascade is damped to
            // Butterworth. Cascading two resonant sections would put the peak 6 dB too high and move
            // the corner, which is exactly what the slope test measures.
            sections_[0].set (cutoffHz_, sampleRate_, damping);
            sections_[1].set (cutoffHz_, sampleRate_, kButterworthDamping);
            sections_[2].set (secondaryCutoffHz_, sampleRate_, damping);
            sections_[3].set (secondaryCutoffHz_, sampleRate_, kButterworthDamping);
            sparePole_.set (cutoffHz_, sampleRate_);

            for (auto& pole : ladder_)
                pole.set (cutoffHz_, sampleRate_);
            break;
        }

        case FilterModel::Ladder:
        {
            for (auto& pole : ladder_)
                pole.set (cutoffHz_, sampleRate_);
            break;
        }

        case FilterModel::Comb:
        {
            // The loop filter's own cutoff follows the same control: a comb with a low pass in its
            // feedback loses its high harmonics, which is what `CombL6*` sounds like.
            loopFilters_[0].set (cutoffHz_, sampleRate_);
            loopFilters_[1].set (cutoffHz_, sampleRate_);
            break;
        }

        case FilterModel::Allpass:
        {
            const auto fraction = cutoffHz_ / sampleRate_;
            const auto coefficient = (1.0 - tangent (fraction)) / (1.0 + tangent (fraction));
            const auto stages = std::min<int> (spec_.stages, static_cast<int> (allpasses_.size()));

            for (int i = 0; i < stages; ++i)
                allpasses_[static_cast<std::size_t> (i)].set (coefficient);
            break;
        }

        case FilterModel::Diffuser:
        case FilterModel::PoleZero:
        case FilterModel::Unimplemented:
            break;
    }
}

float VoiceFilter::process (float input)
{
    switch (spec_.model)
    {
        case FilterModel::Svf:           return processSvf (input);
        case FilterModel::DualSvf:       return processDualSvf (input);
        case FilterModel::MorphSvf:      return processMorphSvf (input);
        case FilterModel::Ladder:        return processLadder (input);
        case FilterModel::Comb:          return processComb (input);
        case FilterModel::Allpass:       return processAllpass (input);
        case FilterModel::Diffuser:      return processDiffuser (input);
        case FilterModel::PoleZero:      return processPoleZero (input);
        case FilterModel::Unimplemented: return input;
    }

    return input;
}

float VoiceFilter::processSvf (float input)
{
    if (spec_.poles <= 1)
    {
        // 6 dB/oct is a single pole, which the state-variable core cannot give on its own.
        const auto pole = sparePole_.lowpass (input);
        return static_cast<float> (spec_.kind == FilterKind::HighPass ? input - pole : pole);
    }

    const auto gain = shelfGain();
    const auto count = spec_.poles >= 4 ? 2 : 1;
    auto value = static_cast<double> (input);

    for (int i = 0; i < count; ++i)
    {
        sections_[static_cast<std::size_t> (i)].update (value);
        value = sections_[static_cast<std::size_t> (i)].take (spec_.kind, gain, value);
    }

    if (spec_.poles == 3)
    {
        // The third pole takes the same shape as the response: cascading a low pass onto a high pass
        // would attenuate the top end and give a *falling* high pass, which is how this read 10 dB/oct
        // instead of 18 the first time it was measured.
        const auto pole = sparePole_.lowpass (value);
        value = spec_.kind == FilterKind::HighPass ? value - pole : pole;
    }

    return static_cast<float> (value);
}

float VoiceFilter::processDualSvf (float input)
{
    const auto gain = shelfGain();
    const auto perBranch = spec_.poles >= 4 ? 2 : 1;
    auto primary = static_cast<double> (input);
    auto secondary = static_cast<double> (input);

    for (int i = 0; i < perBranch; ++i)
    {
        auto& section = sections_[static_cast<std::size_t> (i)];
        section.update (primary);
        primary = section.take (spec_.kind, gain, primary);
    }

    for (int i = 0; i < perBranch; ++i)
    {
        auto& section = sections_[static_cast<std::size_t> (2 + i)];
        section.update (secondary);
        secondary = section.take (spec_.secondary, gain, secondary);
    }

    // Summed, which is what makes a dual filter a shelf or a band rather than a cascade.
    return static_cast<float> (0.5 * (primary + secondary));
}

float VoiceFilter::processMorphSvf (float input)
{
    const auto gain = shelfGain();
    const auto count = spec_.poles >= 4 ? 2 : 1;
    auto value = static_cast<double> (input);

    for (int i = 0; i < count; ++i)
    {
        auto& section = sections_[static_cast<std::size_t> (i)];
        section.update (value);

        // One update, three responses read out of the same state — advancing the section once per
        // response would triple the filter's order and change its cutoff.
        const auto first = section.take (spec_.kind, gain, value);
        const auto middle = section.take (spec_.secondary, gain, value);
        const auto last = section.take (spec_.tertiary, gain, value);

        value = morph_ < 0.5 ? first + (middle - first) * (morph_ * 2.0)
                             : middle + (last - middle) * ((morph_ - 0.5) * 2.0);
    }

    return static_cast<float> (value);
}

float VoiceFilter::processLadder (float input)
{
    // The Moog ladder: one-pole stages with the resonance taken from the last one. Four stages means
    // the feedback that reaches self-oscillation is 4.
    const auto feedback = resonance_ * 4.0;
    auto value = static_cast<double> (input) - feedback * ladderFeedbackState_;

    for (int i = 0; i < spec_.poles; ++i)
        value = ladder_[static_cast<std::size_t> (i)].lowpass (value);

    // Saturation in the feedback path is what makes a ladder a ladder rather than four one-poles, and
    // it is where the flavours differ. It also bounds the loop, which is why a ladder at full resonance
    // rings instead of exploding.
    ladderFeedbackState_ = std::tanh (value * (1.0 + drive_ * 4.0) * spec_.flavourDrive);

    return static_cast<float> (value);
}

float VoiceFilter::processComb (float input)
{
    const auto delaySamples = std::clamp (sampleRate_ / std::max (20.0, cutoffHz_), 2.0,
                                          static_cast<double> (comb_.buffer.size()) - 2.0);

    const auto delayed = comb_.read (delaySamples);
    auto loop = delayed;

    if (spec_.comb.lowPassPoles > 0)
        loop = loopFilters_[0].lowpass (loop);

    if (spec_.comb.highPassPoles > 0)
        loop = loop - loopFilters_[1].lowpass (loop);

    if (spec_.comb.distorted)
        loop = std::tanh (loop * (1.0 + drive_ * 4.0));

    // 0.97 rather than 1.0: at unity this is a resonator with infinite ringing, and every preset that
    // sets resonance to its maximum would hang the voice.
    auto feedback = resonance_ * 0.97;

    if (spec_.comb.negative)
        feedback = -feedback;

    comb_.writeSample (static_cast<double> (input) + feedback * loop);

    return static_cast<float> (delayed);
}

float VoiceFilter::processAllpass (float input)
{
    auto value = static_cast<double> (input);
    const auto stages = std::min<int> (spec_.stages, static_cast<int> (allpasses_.size()));

    for (int i = 0; i < stages; ++i)
        value = allpasses_[static_cast<std::size_t> (i)].process (value);

    return static_cast<float> (value);
}

float VoiceFilter::processDiffuser (float input)
{
    // Schroeder allpasses with real delays — `y = -g·x + xᴅ + g·yᴅ`, whose magnitude response is flat
    // and whose phase is not. Delays are mutually prime so the chain does not reinforce one period.
    static constexpr double kDelays[] = { 0.00273, 0.00431, 0.00601, 0.00811 };
    static constexpr double kCoefficients[] = { 0.62, 0.58, 0.55, 0.5 };

    auto value = static_cast<double> (input);
    const auto stages = std::min<int> (spec_.stages, static_cast<int> (diffuserLines_.size()));

    for (int i = 0; i < stages; ++i)
    {
        auto& line = diffuserLines_[static_cast<std::size_t> (i)];
        const auto g = kCoefficients[i];

        // Whole samples, deliberately: the delay line interpolates for the *comb*, where a fractional
        // delay is what makes it tunable, but interpolation is a low pass and would break the allpass
        // property a diffuser is defined by. Rounded delays stay allpass exactly, which the test asserts.
        const auto delayed = line.read (std::round (kDelays[i] * sampleRate_));
        const auto output = -g * value + delayed;

        line.writeSample (value + g * output);
        value = output;
    }

    return static_cast<float> (value);
}

void VoiceFilter::setPoleZero (const PoleZeroDescription& description)
{
    poleZeroCount_ = std::min (description.count, PoleZeroDescription::kMaxSections);

    for (std::size_t i = 0; i < poleZeroCount_; ++i)
    {
        const auto& section = description.sections[i];
        const auto poleRadius = radiusForQ (section.poleFrequency, section.poleQ);
        const auto zeroRadius = radiusForQ (section.zeroFrequency, section.zeroQ);
        const auto poleAngle = kPi * clampUnit (section.poleFrequency);
        const auto zeroAngle = kPi * clampUnit (section.zeroFrequency);
        const auto gain = section.gain != 0.0 ? section.gain : 1.0;

        auto& biquad = pz_[i];

        // The transfer function of the poles and zeros the preset describes, written directly:
        //   gain * (1 - 2·rz·cos θz·z⁻¹ + rz²·z⁻²) / (1 - 2·rp·cos θp·z⁻¹ + rp²·z⁻²)
        biquad.b0 = gain;
        biquad.b1 = -2.0 * zeroRadius * std::cos (zeroAngle) * gain;
        biquad.b2 = zeroRadius * zeroRadius * gain;
        biquad.a1 = -2.0 * poleRadius * std::cos (poleAngle);
        biquad.a2 = poleRadius * poleRadius;
        biquad.clear();
    }

    // Any biquads beyond this description are neutral rather than stale: a slot that draws eight
    // sections must not keep a previous preset's last twenty-four.
    for (auto i = poleZeroCount_; i < pz_.size(); ++i)
    {
        pz_[i] = Biquad {};
        pz_[i].clear();
    }
}

void appendPoleZeroObject (PoleZeroDescription& description, const double* poleFrequencies,
                           const double* zeroFrequencies, const double* poleQs, const double* zeroQs,
                           const double* gains, const int* types)
{
    if (description.count + PoleZeroDescription::kSectionsPerObject > PoleZeroDescription::kMaxSections)
        return;

    const auto base = description.count;

    for (std::size_t i = 0; i < PoleZeroDescription::kSectionsPerObject; ++i)
    {
        auto& section = description.sections[base + i];
        section.poleFrequency = poleFrequencies != nullptr ? poleFrequencies[i] : 0.0;
        section.zeroFrequency = zeroFrequencies != nullptr ? zeroFrequencies[i] : 0.0;
        section.poleQ = poleQs != nullptr ? poleQs[i] : 0.0;
        section.zeroQ = zeroQs != nullptr ? zeroQs[i] : 0.0;
        section.gain = gains != nullptr ? gains[i] : 1.0;
        section.type = types != nullptr ? types[i] : -1;
    }

    description.count = base + PoleZeroDescription::kSectionsPerObject;
}

float VoiceFilter::processPoleZero (float input)
{
    auto value = static_cast<double> (input);

    for (std::size_t i = 0; i < poleZeroCount_; ++i)
        value = pz_[i].process (value);

    // **A cascade can amplify without bound, and 32 sections of it overflow a float.** `radiusForQ`
    // clamps a section's pole radius at 0.9999, so one section is stable — but a pole at DC at Q 1 has
    // that radius, and eight of those in series have a DC gain near 1e64: finite in a `double`, an
    // infinity in the `float` this returns, and `inf` in the biquad state poisons every later sample.
    // Ticket `46` measured the corpus's drawn-filter fixture going non-finite once (and finite since), so
    // this is now the one place it can be caught.
    //
    // The check is exactly "what a float cannot hold" — every finite render passes it untouched, so no
    // existing measurement moves — and the recovery is to clear the sections, which brings the filter
    // back on the next sample instead of leaving it emitting NaN for the rest of the note.
    if (! std::isfinite (static_cast<float> (value)))
    {
        for (auto& section : pz_)
            section.clear();

        return 0.0f;
    }

    return static_cast<float> (value);
}

} // namespace xerum::dsp
