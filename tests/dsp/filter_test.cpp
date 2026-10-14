// Tests for the voice filter models.
//
// The acceptance criterion is a **measured slope in dB/oct**, not "it produces output", so these tests
// measure the magnitude response: drive the filter with a sine, let the resonant tail settle, and
// correlate the output against the input at that exact frequency. The probe frequencies are chosen on
// the FFT grid so there is no leakage and no window to argue about.
//
// The slope is a property of the *circuit*, not of the cutoff convention — which is why it is the right
// thing to assert while the mapping from `kParamFreq` to Hz is still a stated convention rather than a
// measurement.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "filter.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace xerum::dsp;

namespace
{

constexpr double kPi = 3.14159265358979323846;

/// The inverse of `cutoffHzFromNormalised`, so a test can ask for a cutoff in Hz.
double normalisedFor (double hertz) { return std::log (hertz / 20.0) / std::log (1000.0); }

/// The magnitude response at one frequency, measured rather than derived.
///
/// The probe is placed on an exact DFT bin, so the correlation is exact: no window, no leakage, and the
/// answer is the filter's gain at that frequency to within float precision.
double gainAt (VoiceFilter& filter, double frequency, double sampleRate)
{
    constexpr std::size_t kMeasure = 1 << 15;
    const auto bin = static_cast<std::size_t> (std::lround (frequency * kMeasure / sampleRate));
    const auto lastBin = kMeasure / 2 - 1;

    REQUIRE (bin >= 1);
    REQUIRE (bin <= lastBin);

    const auto omega = 2.0 * kPi * static_cast<double> (bin) / static_cast<double> (kMeasure);
    filter.reset();

    // Settle: a resonant filter takes a while to forget its zero state, and measuring during the
    // transient would report the transient's spectrum rather than the filter's response.
    for (std::size_t i = 0; i < kMeasure; ++i)
        (void) filter.process (static_cast<float> (0.5 * std::sin (omega * static_cast<double> (i))));

    double real = 0.0;
    double imaginary = 0.0;

    for (std::size_t i = 0; i < kMeasure; ++i)
    {
        const auto x = 0.5 * std::sin (omega * static_cast<double> (i));
        const auto y = static_cast<double> (filter.process (static_cast<float> (x)));

        real += y * std::cos (omega * static_cast<double> (i));
        imaginary += y * std::sin (omega * static_cast<double> (i));
    }

    // The input's own magnitude at that bin is amplitude * N / 2.
    const auto reference = 0.5 * static_cast<double> (kMeasure) / 2.0;
    return std::sqrt (real * real + imaginary * imaginary) / reference;
}

/// The measured slope between two frequencies, in dB per octave.
double slopeBetween (VoiceFilter& filter, double low, double high, double sampleRate)
{
    const auto lowGain = gainAt (filter, low, sampleRate);
    const auto highGain = gainAt (filter, high, sampleRate);

    REQUIRE (lowGain > 0.0);
    REQUIRE (highGain > 0.0);

    // Signed the way a filter spec reads: a low pass *falls*, so its slope is negative dB/oct.
    const auto octaves = std::log2 (high / low);
    return 20.0 * std::log10 (highGain / lowGain) / octaves;
}

/// `resonance` is 0..1 here and percent in the API, which is the difference between a resonant filter
/// and a flat one — the tests below measure both, so the conversion lives in one place.
VoiceFilter makeFilter (const std::string& type, double cutoffHz, double sampleRate = 48000.0,
                        double resonance = 0.0)
{
    VoiceFilter filter;
    filter.prepare (sampleRate);
    filter.setSpec (filterSpecFromName (type));
    filter.setCutoff (normalisedFor (cutoffHz));
    filter.setResonance (resonance * 100.0);
    return filter;
}

} // namespace

TEST_CASE ("the model table reads the names the way ticket 04 established", "[filter]")
{
    // The number in a name is the slope in dB/oct, and the letters are the responses to combine.
    const auto low24 = filterSpecFromName ("L24");
    CHECK (low24.model == FilterModel::Svf);
    CHECK (low24.kind == FilterKind::LowPass);
    CHECK (low24.poles == 4);

    CHECK (filterSpecFromName ("L6").poles == 1);
    CHECK (filterSpecFromName ("H18").poles == 3);
    CHECK (filterSpecFromName ("H18").kind == FilterKind::HighPass);
    CHECK (filterSpecFromName ("B12").kind == FilterKind::BandPass);
    CHECK (filterSpecFromName ("N24").kind == FilterKind::Notch);

    // The ladder, at its slope, and its flavours as whole circuits.
    CHECK (filterSpecFromName ("MgL24").model == FilterModel::Ladder);
    CHECK (filterSpecFromName ("MgL6").poles == 1);
    for (const auto* flavour : { "LadderMg", "LadderEMS", "LadderAcid", "DirtyMg" })
    {
        CHECK (filterSpecFromName (flavour).model == FilterModel::Ladder);
        CHECK (filterSpecFromName (flavour).kind == FilterKind::LowPass);
    }

    // Two letters are a dual filter: the letters name both halves.
    const auto dual = filterSpecFromName ("LH12");
    CHECK (dual.model == FilterModel::DualSvf);
    CHECK (dual.kind == FilterKind::LowPass);
    CHECK (dual.secondary == FilterKind::HighPass);

    // Three letters morph through all three, in order.
    const auto morph = filterSpecFromName ("LNH24");
    CHECK (morph.model == FilterModel::MorphSvf);
    CHECK (morph.kind == FilterKind::LowPass);
    CHECK (morph.secondary == FilterKind::Notch);
    CHECK (morph.tertiary == FilterKind::HighPass);

    // The comb's polarity and loop filters come out of the suffix, not out of a table.
    const auto comb = filterSpecFromName ("CombL6N");
    CHECK (comb.model == FilterModel::Comb);
    CHECK (comb.comb.lowPassPoles == 1);
    CHECK (comb.comb.highPassPoles == 0);
    CHECK (comb.comb.negative);

    CHECK (filterSpecFromName ("CombH6P").comb.highPassPoles == 1);
    CHECK_FALSE (filterSpecFromName ("CombH6P").comb.negative);
    CHECK (filterSpecFromName ("CombHL6N").comb.lowPassPoles == 1);
    CHECK (filterSpecFromName ("CombHL6N").comb.highPassPoles == 1);
    CHECK (filterSpecFromName ("DistComb1LP").comb.distorted);

    // Stage counts, and the PZ filter, which is not a circuit.
    CHECK (filterSpecFromName ("Phase24N").stages == 24);
    CHECK (filterSpecFromName ("Phase48P").stages == 48);
    CHECK (filterSpecFromName ("Allpasses").model == FilterModel::Allpass);
    CHECK (filterSpecFromName ("Diffuser").model == FilterModel::Diffuser);
    CHECK (filterSpecFromName ("PZ_SVF").model == FilterModel::PoleZero);
    CHECK (filterSpecFromName ("BandReject").kind == FilterKind::Notch);
    CHECK (filterSpecFromName ("HEQ12").kind == FilterKind::HighShelf);
}

TEST_CASE ("every shipped slope measures its stated dB/oct", "[filter]")
{
    // The heart of ticket 26's acceptance: a slope that is not 24 dB/oct is not an L24, whatever the
    // output looks like. Measured two octaves above the corner, with no resonance, so the measurement
    // reads the stopband rather than the peak.
    const double cutoff = 1000.0;
    const double low = 2000.0;
    const double high = 8000.0;

    struct Expectation
    {
        const char* type;
        double dbPerOctave;
    };

    for (const auto expectation : { Expectation { "L6", -6.0 }, Expectation { "L12", -12.0 },
                                    Expectation { "L18", -18.0 }, Expectation { "L24", -24.0 },
                                    Expectation { "H6", 6.0 }, Expectation { "H12", 12.0 },
                                    Expectation { "H18", 18.0 }, Expectation { "H24", 24.0 } })
    {
        auto filter = makeFilter (expectation.type, cutoff);

        // A low pass is measured *above* its corner, a high pass *below* it — outside the corner is the
        // passband, and a passband measures flat however steep the filter is.
        const auto measured = expectation.dbPerOctave < 0.0
                                ? slopeBetween (filter, low, high, 48000.0)
                                : slopeBetween (filter, low / 4.0, low / 16.0, 48000.0);

        UNSCOPED_INFO (expectation.type << " measured " << measured << " dB/oct");

        CHECK (measured == Catch::Approx (expectation.dbPerOctave).margin (3.0));
    }
}

TEST_CASE ("a band pass and a notch measure their order too", "[filter]")
{
    // A band pass rolls off on both sides, so its slope above the corner is the low-pass slope of the
    // same order. A notch does not roll off at all, which is the assertion that distinguishes it.
    const double cutoff = 1000.0;

    // A two-pole band pass has one zero at DC and one at infinity, so each skirt falls at **6** dB/oct —
    // the `12` in `B12` names the filter's order, not its skirt. A four-pole band pass is 12 dB/oct.
    auto bandPass = makeFilter ("B12", cutoff);
    const auto bandSlope = slopeBetween (bandPass, 2000.0, 8000.0, 48000.0);
    UNSCOPED_INFO ("B12 measured " << bandSlope << " dB/oct");
    CHECK (bandSlope == Catch::Approx (-6.0).margin (2.5));

    auto wideBandPass = makeFilter ("B24", cutoff);
    const auto wideSlope = slopeBetween (wideBandPass, 2000.0, 8000.0, 48000.0);
    UNSCOPED_INFO ("B24 measured " << wideSlope << " dB/oct");
    CHECK (wideSlope == Catch::Approx (-12.0).margin (3.0));

    auto notch = makeFilter ("N12", cutoff);
    const auto notchSlope = slopeBetween (notch, 2000.0, 8000.0, 48000.0);
    UNSCOPED_INFO ("N12 measured " << notchSlope << " dB/oct");
    CHECK (std::abs (notchSlope) < 3.0);      // flat well above the notch
}

TEST_CASE ("the ladder is a ladder: 24 dB/oct, and its flavours are not the same filter", "[filter]")
{
    const double cutoff = 1000.0;

    auto ladder = makeFilter ("MgL24", cutoff);
    const auto measured = slopeBetween (ladder, 2000.0, 8000.0, 48000.0);
    UNSCOPED_INFO ("MgL24 measured " << measured << " dB/oct");
    CHECK (measured == Catch::Approx (-24.0).margin (3.5));

    // Six and twelve are the same circuit with fewer stages, so the slope has to follow.
    auto shortLadder = makeFilter ("MgL12", cutoff);
    CHECK (slopeBetween (shortLadder, 2000.0, 8000.0, 48000.0) == Catch::Approx (-12.0).margin (3.0));

    // And the flavours differ in their saturation, which is a *level-dependent* difference: with the
    // drive up, `DirtyMg` must not produce the same samples as `LadderMg`.
    auto clean = makeFilter ("LadderMg", cutoff, 48000.0, 0.6);
    auto dirty = makeFilter ("DirtyMg", cutoff, 48000.0, 0.6);

    REQUIRE (filterSpecFromName ("LadderMg").flavourDrive
             < filterSpecFromName ("DirtyMg").flavourDrive);

    bool differs = false;

    for (int i = 0; i < 4096; ++i)
    {
        const auto x = static_cast<float> (0.7 * std::sin (0.01 * static_cast<double> (i)));

        if (clean.process (x) != dirty.process (x))
            differs = true;
    }

    // Four flavours, four circuits in the reference. They share the ladder structure and differ in where
    // and how hard the feedback stage saturates, so at a level that saturates they must not agree.
    CHECK (differs);
}

TEST_CASE ("a dual filter sums its halves rather than cascading them", "[filter]")
{
    const double cutoff = 1000.0;

    // `LH12` is low plus high, which is a wide band: it must pass both the low and the high end, where
    // a cascade of a low pass and a high pass at the same cutoff would pass only a narrow sliver.
    auto filter = makeFilter ("LH12", cutoff);
    filter.setResonance (0.0);

    const auto lowGain = gainAt (filter, 250.0, 48000.0);
    const auto highGain = gainAt (filter, 4000.0, 48000.0);

    UNSCOPED_INFO ("LH12 low " << lowGain << " high " << highGain);

    CHECK (lowGain > 0.1);
    CHECK (highGain > 0.1);
}

TEST_CASE ("a morphing filter lands on the response it names at each end", "[filter]")
{
    const double cutoff = 1000.0;

    // `LNH24` morphs low → notch → high. At the ends it must measure like the plain responses, which is
    // what makes the morph a morph of *responses* rather than a crossfade between unrelated filters.
    auto morph = makeFilter ("LNH24", cutoff);

    morph.setMorph (0.0);
    const auto asLow = slopeBetween (morph, 2000.0, 8000.0, 48000.0);

    morph.setMorph (1.0);
    const auto asHigh = slopeBetween (morph, 125.0, 250.0, 48000.0);

    UNSCOPED_INFO ("LNH24 as low " << asLow << " dB/oct, as high " << asHigh << " dB/oct");

    CHECK (asLow == Catch::Approx (-24.0).margin (4.0));
    CHECK (asHigh == Catch::Approx (24.0).margin (4.0));
}

TEST_CASE ("the comb has peaks spaced by its delay and nulls between them", "[filter]")
{
    // A comb is not a slope, so its characteristic is the spacing: peaks at the delay's fundamental and
    // its multiples, nulls half way between. Measured at 1 kHz cutoff, so the delay is one millisecond
    // and the peaks sit a kilohertz apart.
    auto filter = makeFilter ("CombP", 1000.0, 48000.0, 0.8);

    const auto peak = gainAt (filter, 1000.0, 48000.0);
    const auto null = gainAt (filter, 500.0, 48000.0);
    const auto secondPeak = gainAt (filter, 2000.0, 48000.0);
    const auto secondNull = gainAt (filter, 1500.0, 48000.0);

    UNSCOPED_INFO ("comb: peak " << peak << " null " << null << " second peak " << secondPeak
                                 << " second null " << secondNull);

    CHECK (peak > null * 2.0);
    CHECK (secondPeak > secondNull * 2.0);

    // A kilohertz spacing means the two peaks are comparable; that is what makes the series periodic.
    CHECK (secondPeak == Catch::Approx (peak).epsilon (0.5));

    // And negative feedback moves the peaks by half the spacing — that is the difference `N` makes.
    auto negative = makeFilter ("CombN", 1000.0, 48000.0, 0.8);
    const auto negativeAtPeak = gainAt (negative, 1000.0, 48000.0);
    const auto negativeAtNull = gainAt (negative, 500.0, 48000.0);

    UNSCOPED_INFO ("CombN: at 1 kHz " << negativeAtPeak << ", at 500 Hz " << negativeAtNull);
    CHECK (negativeAtNull > negativeAtPeak);
}

TEST_CASE ("the allpass cascade and the diffuser pass every frequency equally", "[filter]")
{
    // This is what makes them allpasses. A cascade whose magnitude moved would be colouring the sound,
    // which is exactly what a phaser must not do — its notches come from mixing it with the dry signal.
    for (const auto* type : { "Allpasses", "Phase24N", "Diffuser" })
    {
        auto filter = makeFilter (type, 1000.0);

        for (const auto frequency : { 200.0, 1000.0, 5000.0 })
        {
            const auto gain = gainAt (filter, frequency, 48000.0);
            UNSCOPED_INFO (type << " at " << frequency << " Hz: " << gain);
            CHECK (gain == Catch::Approx (1.0).margin (0.05));
        }
    }
}

TEST_CASE ("a pole/zero section whose pole and zero agree is exactly transparent", "[filter]")
{
    // Ticket 04 found that the reference's section type 2 occurs when `fp == fz` **and** `qp == qz`, and
    // read that as identity. Written as a pole/zero transfer function it is identity by construction —
    // the numerator and denominator polynomials are the same — so the finding is not just consistent,
    // it is forced. Asserting it checks the geometry reading and the biquad maths at once.
    double poleFrequency[PoleZeroDescription::kSectionsPerObject] {};
    double zeroFrequency[PoleZeroDescription::kSectionsPerObject] {};
    double poleQ[PoleZeroDescription::kSectionsPerObject] {};
    double zeroQ[PoleZeroDescription::kSectionsPerObject] {};
    double gain[PoleZeroDescription::kSectionsPerObject] {};
    int type[PoleZeroDescription::kSectionsPerObject] {};

    for (std::size_t i = 0; i < PoleZeroDescription::kSectionsPerObject; ++i)
    {
        poleFrequency[i] = 0.0625 + 0.0625 * static_cast<double> (i);
        zeroFrequency[i] = poleFrequency[i];
        poleQ[i] = 0.4 + 0.05 * static_cast<double> (i);
        zeroQ[i] = poleQ[i];
        gain[i] = 1.0;
        type[i] = 2;
    }

    PoleZeroDescription description;
    appendPoleZeroObject (description, poleFrequency, zeroFrequency, poleQ, zeroQ, gain, type);

    auto spec = filterSpecFromName ("PZ_SVF");
    CHECK (spec.model == FilterModel::PoleZero);

    VoiceFilter filter;
    filter.prepare (48000.0);
    filter.setSpec (spec);
    filter.setPoleZero (description);

    bool identical = true;

    for (int i = 0; i < 8192; ++i)
    {
        const auto x = static_cast<float> (0.5 * std::sin (0.017 * static_cast<double> (i)));

        if (filter.process (x) != x)
            identical = false;
    }

    CHECK (identical);
}

TEST_CASE ("the eight pole/zero sections each shape the response", "[filter]")
{
    // Eight sections, and each one carries its own geometry: a section with its zero well below its
    // pole is a high-pass-ish shelf, so the assembled filter must not be transparent.
    double poleFrequency[PoleZeroDescription::kSectionsPerObject] {};
    double zeroFrequency[PoleZeroDescription::kSectionsPerObject] {};
    double poleQ[PoleZeroDescription::kSectionsPerObject] {};
    double zeroQ[PoleZeroDescription::kSectionsPerObject] {};
    double gain[PoleZeroDescription::kSectionsPerObject] {};
    int type[PoleZeroDescription::kSectionsPerObject] {};

    for (std::size_t i = 0; i < PoleZeroDescription::kSectionsPerObject; ++i)
    {
        poleFrequency[i] = 0.5;
        zeroFrequency[i] = 0.0625;
        poleQ[i] = 0.9;
        zeroQ[i] = 0.1;
        gain[i] = 1.0;
        type[i] = 4;
    }

    PoleZeroDescription description;
    appendPoleZeroObject (description, poleFrequency, zeroFrequency, poleQ, zeroQ, gain, type);

    VoiceFilter filter;
    filter.prepare (48000.0);
    filter.setSpec (filterSpecFromName ("PZ_SVF"));
    filter.setPoleZero (description);

    const auto lowGain = gainAt (filter, 250.0, 48000.0);
    const auto highGain = gainAt (filter, 8000.0, 48000.0);

    UNSCOPED_INFO ("PZ sections: low " << lowGain << " high " << highGain);

    CHECK (highGain > lowGain * 2.0);
}

TEST_CASE ("the filter is stable at 44.1 and 96 kHz, including at the edges", "[filter]")
{
    // A naive coefficient calculation misbehaves exactly here: the tangent approaches infinity as the
    // cutoff approaches Nyquist, and a cutoff of 0 with high resonance is where a ladder blows up.
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        for (const auto* type : { "L24", "H24", "MgL24", "CombN", "Phase24N", "LNH24" })
        {
            for (const auto cutoff : { 0.0, 0.5, 1.0 })
            {
                auto filter = makeFilter (type, 1000.0, sampleRate, 100.0);
                filter.setCutoff (cutoff);

                double peak = 0.0;

                for (int i = 0; i < 20000; ++i)
                {
                    const auto y = static_cast<double> (
                        filter.process (static_cast<float> (0.8 * std::sin (0.05 * static_cast<double> (i)))));

                    REQUIRE (std::isfinite (y));
                    peak = std::max (peak, std::abs (y));
                }

                UNSCOPED_INFO (type << " at " << sampleRate << " Hz, cutoff " << cutoff << ": peak " << peak);

                // Bounded, not merely finite: a self-oscillating ladder at maximum resonance is allowed
                // to ring, but it may not run away.
                CHECK (peak < 100.0);
            }
        }
    }
}

TEST_CASE ("the types we do not build are named, never substituted", "[filter]")
{
    // The long tail: individually authored circuits whose behaviour is not derivable from a name, and
    // the modelled hybrids whose second half we do not build. All of them report.
    for (const auto* type : { "RM", "RMT", "Reverb1", "Wsp", "Exp", "ExpBPF", "FormantONE",
                              "FormantTWO", "FormantTWB", "Scream", "Scream3LP", "DJMixer",
                              "ADD_BASS", "ZDF_A", "SNH1", "FlangePhase12HL6P", "Phase48H6P" })
    {
        CHECK (filterSpecFromName (type).model == FilterModel::Unimplemented);
        CHECK_FALSE (VoiceFilter::implements (type));
        CHECK (filterSpecFromName (type).poles == 2);      // the default, and nothing was set
    }

    // And an unbuildable filter passes audio rather than silencing the voice: the report is what tells
    // the user, and a silent slot would be a second, quieter failure.
    VoiceFilter filter;
    filter.prepare (48000.0);
    filter.setSpec (filterSpecFromName ("Wsp"));

    for (int i = 0; i < 64; ++i)
    {
        const auto x = static_cast<float> (i) / 64.0f;
        CHECK (filter.process (x) == x);
    }
}

namespace
{

/// The biquad `VoiceFilter::processPoleZero` cascades, re-implemented here so a test can run the same
/// transfer function without the engine. That independent run is what makes "the recovery touched
/// nothing a float can hold" a checkable claim rather than a comment.
struct LocalBiquad
{
    double b0 { 1.0 }, b1 { 0.0 }, b2 { 0.0 }, a1 { 0.0 }, a2 { 0.0 };
    double x1 { 0.0 }, x2 { 0.0 }, y1 { 0.0 }, y2 { 0.0 };

    double process (double input)
    {
        const auto output = b0 * input + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;

        x2 = x1; x1 = input;
        y2 = y1; y1 = output;

        return output;
    }
};

/// `radiusForQ` from `filter.cpp`, restated: the reference's 0–1 "Q" as a pole radius.
double localRadius (double frequencyOfNyquist, double q)
{
    return std::min (0.9999, std::exp (-kPi * frequencyOfNyquist / (1.0 + 99.0 * q)));
}

} // namespace

TEST_CASE ("a drawn response at the interface's extreme renders finite output, and the voice recovers",
           "[pz-stability]")
{
    // **The extreme the pole/zero plane can produce.** Four `PZ_SVF` objects (the `Multi` form), each
    // eight second-order sections — 32 in series. `fp` is the pole as a fraction of Nyquist, `fz` the
    // zero; the objects alternate a pole at DC against a zero at Nyquist and the inverse, and `qp`/`qz`
    // are at the top of their range, which puts each pole radius at `radiusForQ`'s clamp of 0.9999.
    //
    // Measured (ticket 50): `setPoleZero` writes these as the preset's own geometry with no gain bound,
    // and a section with its pole at DC and its zero at Nyquist has a DC gain of
    //   (1 + 2·0.96907 + 0.96907²) / (1 - 2·0.9999 + 0.9999²) = 3.877 / 1e-8 ≈ 3.9e8.
    // The cascade accumulates in `double`; the interface's own extreme drives the *final* sample to
    // 3.49546e38 — `3.403e38` is the largest a `float` holds, so `2.7%` over, which is why the failure
    // is a knife edge: a compiler that reassociates or fuses differently flips it. Without the guard in
    // `processPoleZero` the sample is an `inf`, the `inf` enters `y1`/`y2`, and every later sample is a
    // `NaN` — the "went silent and never came back" of ticket 46.
    double poleFrequency[PoleZeroDescription::kSectionsPerObject] {};
    double zeroFrequency[PoleZeroDescription::kSectionsPerObject] {};
    double poleQ[PoleZeroDescription::kSectionsPerObject] {};
    double zeroQ[PoleZeroDescription::kSectionsPerObject] {};
    double gain[PoleZeroDescription::kSectionsPerObject] {};
    int type[PoleZeroDescription::kSectionsPerObject] {};

    PoleZeroDescription extreme;

    for (int object = 0; object < 4; ++object)
    {
        for (std::size_t i = 0; i < PoleZeroDescription::kSectionsPerObject; ++i)
        {
            poleFrequency[i] = object % 2 == 0 ? 0.0 : 1.0;
            zeroFrequency[i] = object % 2 == 0 ? 1.0 : 0.0;
            poleQ[i] = 1.0;
            zeroQ[i] = 1.0;
            gain[i] = 1.0;
            type[i] = static_cast<int> (i) % 7;
        }

        appendPoleZeroObject (extreme, poleFrequency, zeroFrequency, poleQ, zeroQ, gain, type);
    }

    REQUIRE (extreme.count == PoleZeroDescription::kMaxSections);

    VoiceFilter filter;
    filter.prepare (48000.0);
    filter.setSpec (filterSpecFromName ("PZ_SVF"));
    filter.setPoleZero (extreme);

    // Driven with a saw at the note `interface_test` plays, so the input is the same shape.
    double phase = 0.0;
    double peak = 0.0;
    bool finite = true;

    for (int i = 0; i < 96000; ++i)
    {
        phase += 261.625565 / 48000.0;
        phase -= std::floor (phase);

        const auto y = static_cast<double> (filter.process (static_cast<float> (0.5 * (2.0 * phase - 1.0))));
        finite = finite && std::isfinite (y);
        peak = std::max (peak, std::abs (y));
    }

    UNSCOPED_INFO ("interface extreme: peak " << peak);
    CHECK (finite);

    // The voice recovers, rather than being poisoned for the rest of the note: the filter is still
    // finite under silence, and still responsive to a fresh input.
    finite = true;

    for (int i = 0; i < 24000; ++i)
        finite = finite && std::isfinite (filter.process (0.0f));

    CHECK (finite);

    auto responded = false;

    for (int i = 0; i < 256; ++i)
    {
        const auto y = filter.process (0.5f);
        responded = responded || (std::isfinite (y) && std::abs (y) > 0.0f);
    }

    CHECK (responded);

    // And a second shape of the same extreme — every pole at DC, every zero at Nyquist, so there is no
    // cancelling half to bring the cascade back — is bounded the same way rather than assumed to be.
    for (std::size_t i = 0; i < PoleZeroDescription::kSectionsPerObject; ++i)
    {
        poleFrequency[i] = 0.0;
        zeroFrequency[i] = 1.0;
    }

    PoleZeroDescription allPolesAtDc;

    for (int object = 0; object < 4; ++object)
        appendPoleZeroObject (allPolesAtDc, poleFrequency, zeroFrequency, poleQ, zeroQ, gain, type);

    filter.setPoleZero (allPolesAtDc);
    finite = true;

    for (int i = 0; i < 48000; ++i)
        finite = finite && std::isfinite (filter.process (0.25f));

    CHECK (finite);
}

TEST_CASE ("a bounded drawn response passes the recovery untouched", "[pz-stability]")
{
    // **What a float can hold must not move.** The recovery in `processPoleZero` is a bound on *output*,
    // not a change to the response, so a drawn response inside the range a `float` holds must come back
    // bit-for-bit identical to the transfer function `setPoleZero` wrote. The expected samples are
    // computed here, independently of the engine, from the same coefficients the header documents.
    constexpr std::size_t kSections = PoleZeroDescription::kSectionsPerObject;
    double poleFrequency[kSections] {};
    double zeroFrequency[kSections] {};
    double poleQ[kSections] {};
    double zeroQ[kSections] {};
    double gain[kSections] {};
    int type[kSections] {};
    LocalBiquad expected[kSections];

    for (std::size_t i = 0; i < kSections; ++i)
    {
        poleFrequency[i] = 0.2;
        zeroFrequency[i] = 0.5;
        poleQ[i] = 0.7;
        zeroQ[i] = 0.5;
        gain[i] = 1.0;
        type[i] = static_cast<int> (i) % 7;

        const auto poleRadius = localRadius (poleFrequency[i], poleQ[i]);
        const auto zeroRadius = localRadius (zeroFrequency[i], zeroQ[i]);
        const auto poleAngle = kPi * poleFrequency[i];
        const auto zeroAngle = kPi * zeroFrequency[i];

        expected[i].b0 = gain[i];
        expected[i].b1 = -2.0 * zeroRadius * std::cos (zeroAngle) * gain[i];
        expected[i].b2 = zeroRadius * zeroRadius * gain[i];
        expected[i].a1 = -2.0 * poleRadius * std::cos (poleAngle);
        expected[i].a2 = poleRadius * poleRadius;
    }

    PoleZeroDescription description;
    appendPoleZeroObject (description, poleFrequency, zeroFrequency, poleQ, zeroQ, gain, type);

    VoiceFilter filter;
    filter.prepare (48000.0);
    filter.setSpec (filterSpecFromName ("PZ_SVF"));
    filter.setPoleZero (description);

    auto identical = true;

    for (int i = 0; i < 8192; ++i)
    {
        const auto x = 0.5f * std::sin (0.011f * static_cast<float> (i));
        auto local = static_cast<double> (x);

        for (auto& section : expected)
            local = section.process (local);

        identical = identical && filter.process (x) == static_cast<float> (local);
    }

    CHECK (identical);
}
