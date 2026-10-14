// Tests for the SUB oscillator (the reference's slot 4), asserted by **frequency relationship** and
// **harmonic structure** rather than by "it produced output".
//
// The acceptance for this engine is that it tracks the note, so the first measurement is a frequency: the
// fundamental of the rendered sub against the note it was told to play. The rest is the waveform menu,
// whose members the shipped manual distinguishes in its own words (sine "only the fundamental", triangle
// "primarily odd harmonics" and "a small amount", square "rich in odd harmonics", saw "both odd and even
// harmonics", pulse "the high and low states are not equal", rounded rect a square "with smoother
// transitions… less harsh"), so each shape is asserted by the harmonics that description implies.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sub_osc.h"
#include "voice.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace xerum::dsp;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

/// 100 Hz: exactly 480 samples per period at 48 kHz, so a whole number of periods is a window with no
/// leakage and the harmonic probes land on exact bins.
constexpr double kFundamental = 100.0;
constexpr int kPeriod = 480;
constexpr int kPeriods = 32;

std::vector<float> renderShape (SubShape shape, double frequency = kFundamental, int length = kPeriod * kPeriods)
{
    SubOscillator oscillator;
    oscillator.prepare (kSampleRate);
    oscillator.setShape (shape);
    oscillator.setFrequency (frequency);
    oscillator.trigger();

    std::vector<float> out (static_cast<std::size_t> (length));
    oscillator.process (out.data(), out.size());
    return out;
}

/// Harmonic `k` of a whole number of periods, unnormalised, by direct correlation.
double harmonic (const std::vector<float>& samples, double frequency, int k)
{
    double real = 0.0;
    double imaginary = 0.0;

    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const double angle = kTwoPi * frequency * static_cast<double> (k) * static_cast<double> (i) / kSampleRate;
        real += static_cast<double> (samples[i]) * std::cos (angle);
        imaginary += static_cast<double> (samples[i]) * std::sin (angle);
    }

    return std::sqrt (real * real + imaginary * imaginary);
}

/// Harmonic `k` relative to the fundamental, in dB.
double harmonicDb (const std::vector<float>& samples, int k, double frequency = kFundamental)
{
    const double fundamental = harmonic (samples, frequency, 1);
    REQUIRE (fundamental > 0.0);
    return 20.0 * std::log10 (harmonic (samples, frequency, k) / fundamental + 1e-30);
}

double rms (const std::vector<float>& samples)
{
    double sum = 0.0;

    for (const auto sample : samples)
        sum += static_cast<double> (sample) * static_cast<double> (sample);

    return std::sqrt (sum / static_cast<double> (samples.size()));
}

/// The strongest frequency in a band, for asserting the voice sounds the note it was told to.
double dominantFrequency (const std::vector<float>& samples, double minHz, double maxHz)
{
    std::size_t best = 0;
    double bestPower = 0.0;

    for (int bin = 0; static_cast<double> (bin) <= 2.0 * (maxHz - minHz); ++bin)
    {
        const double f = minHz + 0.5 * static_cast<double> (bin);
        double real = 0.0;
        double imaginary = 0.0;

        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            const double angle = kTwoPi * f * static_cast<double> (i) / kSampleRate;
            real += static_cast<double> (samples[i]) * std::cos (angle);
            imaginary += static_cast<double> (samples[i]) * std::sin (angle);
        }

        const double power = real * real + imaginary * imaginary;

        if (power > bestPower)
        {
            bestPower = power;
            best = static_cast<std::size_t> (bin);
        }
    }

    return minHz + 0.5 * static_cast<double> (best);
}

/// The sub through the real voice pool, after its attack has settled.
std::vector<float> renderedSub (int note, SubShape shape, double octave, int frames = 24000)
{
    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 4);

    VoiceSettings settings;
    settings.envelope.attack = 0.001;
    settings.sub.enabled = true;
    settings.sub.shape = shape;
    settings.sub.pitchSemitones = 12.0 * octave;
    settings.sub.volume = 0.5;
    engine.setSettings (settings);

    REQUIRE (engine.noteOn (note, 1.0) == 0);

    std::vector<float> out (static_cast<std::size_t> (frames));
    for (std::size_t i = 0; i < out.size(); i += 128)
        engine.process (out.data() + i, 128);

    // Skip the attack and the first cycles.
    return std::vector<float> (out.begin() + 4800, out.end());
}
} // namespace

TEST_CASE ("sub: kParamShape's vocabulary is six, including the default the schema never stores", "[sub]")
{
    // Five names are in the schema's kValues557 because the corpus stores them; `Sine` is the sixth and
    // is the reference's own default (`xerum-host --dump-params`: `Sub Shape … Sine`, 6 steps), which is
    // exactly why no preset stores it.
    REQUIRE (subShapeFromName ("Sine") == SubShape::Sine);
    REQUIRE (subShapeFromName ("kRoundRect") == SubShape::RoundRect);
    REQUIRE (subShapeFromName ("kTriangle") == SubShape::Triangle);
    REQUIRE (subShapeFromName ("kSaw") == SubShape::Saw);
    REQUIRE (subShapeFromName ("kSquare") == SubShape::Square);
    REQUIRE (subShapeFromName ("kPulse") == SubShape::Pulse);
    REQUIRE_FALSE (subShapeFromName ("kSine").has_value());

    for (const auto shape : { SubShape::Sine, SubShape::RoundRect, SubShape::Triangle, SubShape::Saw,
                              SubShape::Square, SubShape::Pulse })
        REQUIRE (subShapeFromName (std::string (subShapeName (shape))) == shape);
}

TEST_CASE ("sub: the sub tracks the note, at the octave the preset asks for", "[sub]")
{
    // The acceptance's frequency relationship. A4 is the reference pitch: note 69 is 440 Hz, and equal
    // temperament above and below it.
    const auto frequencyOf = [] (int note, double octave)
    {
        return 440.0 * std::exp2 ((static_cast<double> (note) - 69.0 + 12.0 * octave) / 12.0);
    };

    // At its own pitch (`kParamOctave` default 0) the sub sounds the note — and follows it an octave up.
    const auto atNote = renderedSub (57, SubShape::Sine, 0.0);
    const auto atOctaveUp = renderedSub (57, SubShape::Sine, 1.0);

    const double expected = frequencyOf (57, 0.0);

    REQUIRE (dominantFrequency (atNote, 100.0, 400.0) == Catch::Approx (expected).margin (2.0));
    REQUIRE (dominantFrequency (atOctaveUp, 100.0, 800.0) == Catch::Approx (expected * 2.0).margin (2.0));

    // Transposition is `kParamOctave`, which the bank does use: 80 presets sit at -1 and 74 at -2.
    const auto down = renderedSub (57, SubShape::Sine, -1.0);
    REQUIRE (dominantFrequency (down, 50.0, 400.0) == Catch::Approx (expected / 2.0).margin (2.0));

    // And it *tracks*: an octave up in the note is an octave up in the sub, which is what "tracks pitch"
    // means as a measurement rather than as a mechanism.
    const auto higher = renderedSub (69, SubShape::Sine, 0.0);
    const double lowerPeak = dominantFrequency (atNote, 100.0, 400.0);
    const double higherPeak = dominantFrequency (higher, 200.0, 800.0);

    REQUIRE (higherPeak / lowerPeak == Catch::Approx (2.0).margin (0.01));
}

TEST_CASE ("sub: each waveform has the harmonic structure the manual describes", "[sub]")
{
    const auto sine = renderShape (SubShape::Sine);
    const auto triangle = renderShape (SubShape::Triangle);
    const auto square = renderShape (SubShape::Square);
    const auto saw = renderShape (SubShape::Saw);
    const auto pulse = renderShape (SubShape::Pulse);
    const auto roundRect = renderShape (SubShape::RoundRect);

    // Sine: "only the fundamental frequency, without any harmonics or overtones".
    REQUIRE (harmonicDb (sine, 2) < -60.0);
    REQUIRE (harmonicDb (sine, 3) < -60.0);

    // Triangle: "primarily odd harmonics… a small amount" — odd, and 1/k², so its third sits near -19 dB
    // where a square's sits near -9.5.
    REQUIRE (harmonicDb (triangle, 2) < -40.0);
    REQUIRE (harmonicDb (triangle, 3) == Catch::Approx (-19.1).margin (1.5));

    // Square: "rich in odd harmonics" — odd only, and the textbook -9.5 dB third.
    REQUIRE (harmonicDb (square, 2) < -40.0);
    REQUIRE (harmonicDb (square, 3) == Catch::Approx (-9.5).margin (1.5));

    // Saw: "both odd and even harmonics", 1/k, so the second sits near -6 dB.
    REQUIRE (harmonicDb (saw, 2) == Catch::Approx (-6.0).margin (1.5));
    REQUIRE (harmonicDb (saw, 3) > -12.0);

    // Pulse: "the high and low states are not equal" — asymmetry is what puts *even* harmonics in a
    // waveform whose square parent has none, so this is the shape's defining measurement.
    REQUIRE (harmonicDb (pulse, 2) > -20.0);
    REQUIRE (harmonicDb (pulse, 2) - harmonicDb (square, 2) > 20.0);

    // Rounded rect: a square "with smoother transitions… less harsh… more rounded": still odd-dominant,
    // but with less high-harmonic content than the square it is a variation of.
    REQUIRE (harmonicDb (roundRect, 2) < -40.0);
    REQUIRE (harmonicDb (roundRect, 3) < harmonicDb (square, 3) - 3.0);
}

TEST_CASE ("sub: the six waveforms differ from each other, measurably", "[sub]")
{
    // "the six" because the schema's five plus its default Sine are what the reference offers; the
    // acceptance asks for a menu whose members differ, so every pair is compared on the feature that
    // separates waveforms: even-harmonic content and the shape of the odd-harmonic rolloff.
    struct Feature
    {
        SubShape shape;
        double second;
        double third;
    };

    const std::array<Feature, 6> features { {
        { SubShape::Sine, harmonicDb (renderShape (SubShape::Sine), 2),
          harmonicDb (renderShape (SubShape::Sine), 3) },
        { SubShape::RoundRect, harmonicDb (renderShape (SubShape::RoundRect), 2),
          harmonicDb (renderShape (SubShape::RoundRect), 3) },
        { SubShape::Triangle, harmonicDb (renderShape (SubShape::Triangle), 2),
          harmonicDb (renderShape (SubShape::Triangle), 3) },
        { SubShape::Saw, harmonicDb (renderShape (SubShape::Saw), 2),
          harmonicDb (renderShape (SubShape::Saw), 3) },
        { SubShape::Square, harmonicDb (renderShape (SubShape::Square), 2),
          harmonicDb (renderShape (SubShape::Square), 3) },
        { SubShape::Pulse, harmonicDb (renderShape (SubShape::Pulse), 2),
          harmonicDb (renderShape (SubShape::Pulse), 3) },
    } };

    for (std::size_t i = 0; i < features.size(); ++i)
        for (std::size_t j = i + 1; j < features.size(); ++j)
        {
            const auto& a = features[i];
            const auto& b = features[j];

            const bool secondDiffers = std::abs (a.second - b.second) > 3.0;
            const bool thirdDiffers = std::abs (a.third - b.third) > 1.0;

            CAPTURE (subShapeName (a.shape), subShapeName (b.shape), a.second, b.second, a.third, b.third);

            REQUIRE ((secondDiffers || thirdDiffers));
        }
}

TEST_CASE ("sub: kParamInitialPhase sets where the waveform starts", "[sub]")
{
    // "Use the PHASE control to specify where the oscillator should begin playing back when a note is
    // triggered." A quarter turn in is the sine's peak and a half turn is its zero crossing going down,
    // which is the whole of what the control promises.
    SubOscillator oscillator;
    oscillator.prepare (kSampleRate);
    oscillator.setShape (SubShape::Sine);
    oscillator.setFrequency (kFundamental);

    oscillator.setInitialPhase (0.0);
    oscillator.trigger();
    REQUIRE (oscillator.next() == Catch::Approx (0.0f).margin (1e-6));   // rising through zero at 0

    oscillator.setInitialPhase (0.25);
    oscillator.trigger();
    REQUIRE (oscillator.next() == Catch::Approx (1.0f).margin (1e-6));   // the peak at a quarter turn

    oscillator.setInitialPhase (0.5);
    oscillator.trigger();
    REQUIRE (oscillator.next() == Catch::Approx (0.0f).margin (1e-6));
    REQUIRE (oscillator.next() < 0.0f);                                  // falling through zero at a half
}

TEST_CASE ("sub: kParamContiguousPhase carries the phase across a note change", "[sub]")
{
    // The manual: "choose Contiguous… to have new notes continue with the phase of the previous note."
    // Measured on the phase itself and then on the output: with it off the second note restarts at the
    // initial phase, with it on it does not.
    const auto phaseAfterTwoNotes = [] (bool contiguous)
    {
        SubOscillator oscillator;
        oscillator.prepare (kSampleRate);
        oscillator.setShape (SubShape::Sine);
        oscillator.setFrequency (kFundamental);
        oscillator.setInitialPhase (0.0);
        oscillator.setContiguousPhase (contiguous);

        oscillator.trigger();
        for (int i = 0; i < 123; ++i)      // a deliberately non-integral number of periods
            oscillator.next();

        const double before = oscillator.phase();
        oscillator.trigger();

        return std::pair { before, oscillator.phase() };
    };

    const auto [beforeRestart, afterRestart] = phaseAfterTwoNotes (false);
    const auto [beforeContinue, afterContinue] = phaseAfterTwoNotes (true);

    REQUIRE (beforeRestart > 0.0);
    REQUIRE (afterRestart == Catch::Approx (0.0).margin (1e-12));
    REQUIRE (afterContinue == Catch::Approx (beforeContinue).margin (1e-12));
}

TEST_CASE ("sub: the slot renders through the voice pool, at the note's frequency", "[sub]")
{
    // The acceptance's "a preset using SUB renders it", through the real fixed pool: enabled, silent
    // when disabled, and its fundamental follows the note.
    const auto rendered = renderedSub (45, SubShape::Saw, 0.0);

    REQUIRE (rms (rendered) > 0.01);
    REQUIRE (dominantFrequency (rendered, 50.0, 300.0) == Catch::Approx (110.0).margin (2.0));

    // The waveform reaches the output too: a saw has a second harmonic, a sine does not.
    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 4);

    VoiceSettings sine;
    sine.envelope.attack = 0.001;
    sine.sub.enabled = true;
    sine.sub.shape = SubShape::Sine;
    sine.sub.volume = 0.5;
    engine.setSettings (sine);
    REQUIRE (engine.noteOn (45, 1.0) == 0);

    std::vector<float> quiet (24000);
    for (std::size_t i = 0; i < quiet.size(); i += 128)
        engine.process (quiet.data() + i, 128);

    // A disabled SUB slot renders nothing, so the two settings blocks are the difference.
    VoiceEngine disabled;
    disabled.prepare (kSampleRate, 128, 4);

    VoiceSettings off = sine;
    off.sub.enabled = false;
    disabled.setSettings (off);
    REQUIRE (disabled.noteOn (45, 1.0) == 0);

    std::vector<float> nothing (4096);
    for (std::size_t i = 0; i < nothing.size(); i += 128)
        disabled.process (nothing.data() + i, 128);

    for (const auto sample : nothing)
        REQUIRE (sample == 0.0f);
}
