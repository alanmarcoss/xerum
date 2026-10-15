// Tests for the NOISE oscillator (the reference's slot 3), asserted by **spectrum** rather than by "it
// produced output".
//
// The four colours are defined by the reference's own manual in spectral terms (White "equal energy at
// all frequencies", Pink "3 dB less energy per octave", Brown "6 dB per octave", Geiger "random clicks
// or bursts… unpredictable intervals"), so that is what is measured here: a least-squares fit of band
// power against log-frequency for the tilt, and crest factor / kurtosis for the one colour whose
// definition is temporal rather than spectral.
//
// Every threshold comes from a measurement of this implementation, taken in the ticket and repeated by
// this file's own run — none of them is a number chosen to make the test pass.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "noise_osc.h"
#include "voice.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace xerum::dsp;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

/// A run of the generator, `blocks` windows of `window` samples, so the statistics have real averaging
/// behind them rather than one realisation's luck.
std::vector<std::vector<float>> noiseWindows (NoiseType type, int blocks = 6, int window = 8192)
{
    NoiseOscillator oscillator;
    oscillator.prepare (kSampleRate);
    oscillator.setType (type);
    oscillator.trigger (false);

    std::vector<std::vector<float>> out (static_cast<std::size_t> (blocks));

    for (auto& block : out)
    {
        block.resize (static_cast<std::size_t> (window));
        oscillator.process (block.data(), block.size());
    }

    return out;
}

/// Power in `bands` octave-wide bands between `fLow` and `fLow * 2^bands`, by integrating a few narrow
/// DFT probes per band with a Hann window. The probe count matters: one probe is a chi-squared estimate
/// with almost no averaging, which is why the slope is averaged over windows as well.
std::vector<double> bandPowers (const std::vector<float>& samples, double fLow, int bands, int probes = 12)
{
    const std::size_t n = samples.size();
    std::vector<double> windowed (n);

    for (std::size_t i = 0; i < n; ++i)
        windowed[i] = static_cast<double> (samples[i])
                      * 0.5 * (1.0 - std::cos (kTwoPi * static_cast<double> (i) / static_cast<double> (n - 1)));

    std::vector<double> powers;
    powers.reserve (static_cast<std::size_t> (bands));

    for (int band = 0; band < bands; ++band)
    {
        const double low = fLow * std::exp2 (band);
        const double high = low * 2.0;
        double accumulated = 0.0;

        for (int probe = 0; probe < probes; ++probe)
        {
            const double f = low * std::pow (high / low, static_cast<double> (probe)
                                                             / static_cast<double> (probes - 1));
            const double omega = kTwoPi * f / kSampleRate;
            const double cw = std::cos (omega);
            const double sw = std::sin (omega);

            double real = 0.0;
            double imaginary = 0.0;
            double cr = 1.0;
            double ci = 0.0;

            for (std::size_t i = 0; i < n; ++i)
            {
                real += windowed[i] * cr;
                imaginary += windowed[i] * ci;

                const double nextCr = cr * cw - ci * sw;
                ci = cr * sw + ci * cw;
                cr = nextCr;
            }

            accumulated += real * real + imaginary * imaginary;
        }

        powers.push_back (accumulated / static_cast<double> (probes));
    }

    return powers;
}

/// dB per octave, least-squares over the band centres. The band centre in octaves is `log2` of the band's
/// geometric mean, which is what makes the fit's x-axis the octave axis the manual talks in.
double tiltDbPerOctave (const std::vector<float>& samples, double fLow = 250.0, int bands = 6)
{
    std::vector<double> xs;
    std::vector<double> ys;

    const auto powers = bandPowers (samples, fLow, bands);

    for (int band = 0; band < bands; ++band)
    {
        xs.push_back (std::log2 (fLow) + static_cast<double> (band) + 0.5);
        ys.push_back (10.0 * std::log10 (powers[static_cast<std::size_t> (band)] + 1e-30));
    }

    double meanX = 0.0;
    double meanY = 0.0;

    for (std::size_t i = 0; i < xs.size(); ++i)
    {
        meanX += xs[i];
        meanY += ys[i];
    }

    meanX /= static_cast<double> (xs.size());
    meanY /= static_cast<double> (ys.size());

    double numerator = 0.0;
    double denominator = 0.0;

    for (std::size_t i = 0; i < xs.size(); ++i)
    {
        numerator += (xs[i] - meanX) * (ys[i] - meanY);
        denominator += (xs[i] - meanX) * (xs[i] - meanX);
    }

    return numerator / denominator;
}

double meanTilt (NoiseType type)
{
    const auto windows = noiseWindows (type);
    double total = 0.0;

    for (const auto& window : windows)
        total += tiltDbPerOctave (window);

    return total / static_cast<double> (windows.size());
}

/// The in-band RMS the four colours are calibrated to match, 100 Hz - 8 kHz (five octaves' worth of
/// bands, summed as power).
double bandRms (const std::vector<float>& samples)
{
    const auto powers = bandPowers (samples, 100.0, 6, 24);
    double total = 0.0;

    for (const auto power : powers)
        total += power;

    // dB of power summed over 28 probe slots' worth of bins: the scale is arbitrary, so the *match*
    // between colours is what is asserted, not the absolute number.
    return std::sqrt (total);
}

double rms (const std::vector<float>& samples)
{
    double sum = 0.0;

    for (const auto sample : samples)
        sum += static_cast<double> (sample) * static_cast<double> (sample);

    return std::sqrt (sum / static_cast<double> (samples.size()));
}

double peak (const std::vector<float>& samples)
{
    double maximum = 0.0;

    for (const auto sample : samples)
        maximum = std::max (maximum, std::abs (static_cast<double> (sample)));

    return maximum;
}

/// How impulsive a signal is: peak over RMS. White noise sits near 3-4, a click train far above it.
double crestFactor (const std::vector<float>& samples)
{
    return peak (samples) / rms (samples);
}

/// Fourth moment over squared second moment. Gaussian noise is 3; a sparse burst train is orders of
/// magnitude above it, which is the measurable form of "random bursts… unpredictable intervals".
double kurtosis (const std::vector<float>& samples)
{
    double sum4 = 0.0;
    double sum2 = 0.0;

    for (const auto sample : samples)
    {
        const double value = static_cast<double> (sample);
        sum4 += value * value * value * value;
        sum2 += value * value;
    }

    const double n = static_cast<double> (samples.size());
    return (sum4 / n) / ((sum2 / n) * (sum2 / n));
}

/// Spectral flatness: geometric mean over arithmetic mean of the band powers. 1 is flat, 0 is one band.
double spectralFlatness (const std::vector<float>& samples)
{
    const auto powers = bandPowers (samples, 100.0, 7, 24);

    double logSum = 0.0;
    double sum = 0.0;

    for (const auto power : powers)
    {
        logSum += std::log (power + 1e-30);
        sum += power;
    }

    const double count = static_cast<double> (powers.size());
    return std::exp (logSum / count) / (sum / count);
}

/// One window per type, long enough for the statistics.
std::vector<float> oneRun (NoiseType type, int length = 1 << 15)
{
    NoiseOscillator oscillator;
    oscillator.prepare (kSampleRate);
    oscillator.setType (type);
    oscillator.trigger (false);

    std::vector<float> out (static_cast<std::size_t> (length));
    oscillator.process (out.data(), out.size());
    return out;
}
} // namespace

TEST_CASE ("noise: kParamNoiseType's vocabulary is the schema's four colours", "[noise]")
{
    // The schema's kValues510, in the manual's table order. A name outside it is the reference's *sample*
    // mode, not a colour, and the caller reports it instead of choosing on the preset's behalf.
    REQUIRE (noiseTypeFromName ("White") == NoiseType::White);
    REQUIRE (noiseTypeFromName ("Pink") == NoiseType::Pink);
    REQUIRE (noiseTypeFromName ("Brown") == NoiseType::Brown);
    REQUIRE (noiseTypeFromName ("Geiger") == NoiseType::Geiger);
    REQUIRE_FALSE (noiseTypeFromName ("Snow").has_value());
    REQUIRE_FALSE (noiseTypeFromName ("").has_value());

    // And the names round-trip, so a report can name what it called the preset's value.
    for (const auto type : { NoiseType::White, NoiseType::Pink, NoiseType::Brown, NoiseType::Geiger })
        REQUIRE (noiseTypeFromName (std::string (noiseTypeName (type))) == type);
}

TEST_CASE ("noise: White, Pink and Brown have the tilt the reference's manual defines", "[noise]")
{
    const double white = meanTilt (NoiseType::White);
    const double pink = meanTilt (NoiseType::Pink);
    const double brown = meanTilt (NoiseType::Brown);

    CAPTURE (white, pink, brown);

    REQUIRE (std::abs (white) < 1.0);            // "equal energy at all frequencies"
    REQUIRE (pink < -2.0);                       // "3 dB less energy per octave"
    REQUIRE (pink > -4.2);
    REQUIRE (brown < -4.5);                      // "6 dB per octave"
    REQUIRE (brown > -7.5);

    // The manual's own ordering, as a measurement: pink is three dB per octave darker than white, brown
    // three more.
    REQUIRE (pink - white == Catch::Approx (-3.0).margin (1.2));
    REQUIRE (brown - pink == Catch::Approx (-3.0).margin (1.2));
}

TEST_CASE ("noise: Geiger is a sparse burst train, not a fourth colour", "[noise]")
{
    // The manual defines Geiger temporally — "random clicks or bursts that are not evenly distributed
    // across time or frequency… unpredictable intervals" — so its defining measurement is the amplitude
    // distribution, not the tilt. Its *tilt* is deliberately not asserted: with a 0.2 ms burst it lands
    // near pink's, and claiming otherwise would be measuring the burst's decay, not the colour.
    const auto geiger = oneRun (NoiseType::Geiger);
    const auto white = oneRun (NoiseType::White);

    const double geigerCrest = crestFactor (geiger);
    const double geigerKurtosis = kurtosis (geiger);
    const double whiteKurtosis = kurtosis (white);

    CAPTURE (geigerCrest, geigerKurtosis, whiteKurtosis);

    REQUIRE (geigerCrest > 5.0);        // mostly silence, with rare tall bursts
    REQUIRE (geigerKurtosis > 10.0);    // far from Gaussian
    REQUIRE (whiteKurtosis < 5.0);      // and the reference's densest colour is not

    // Its spectrum is still broadband — "not evenly distributed across time or frequency" is not a claim
    // of a resonance, and a click train is flat-ish.
    REQUIRE (spectralFlatness (geiger) > 0.3);
}

TEST_CASE ("noise: the four colours differ from each other, measurably", "[noise]")
{
    // The acceptance asks for four types that differ rather than for four names that do. Each colour gets
    // a feature vector — spectral tilt, and how impulsive it is — and every pair must differ in at least
    // one feature by more than the measurement's own noise.
    struct Feature
    {
        NoiseType type;
        double tilt;
        double kurtosis;
    };

    const std::array<Feature, 4> features { {
        { NoiseType::White, meanTilt (NoiseType::White), kurtosis (oneRun (NoiseType::White)) },
        { NoiseType::Pink, meanTilt (NoiseType::Pink), kurtosis (oneRun (NoiseType::Pink)) },
        { NoiseType::Brown, meanTilt (NoiseType::Brown), kurtosis (oneRun (NoiseType::Brown)) },
        { NoiseType::Geiger, meanTilt (NoiseType::Geiger), kurtosis (oneRun (NoiseType::Geiger)) },
    } };

    for (std::size_t i = 0; i < features.size(); ++i)
        for (std::size_t j = i + 1; j < features.size(); ++j)
        {
            const auto& a = features[i];
            const auto& b = features[j];

            const double tiltGap = std::abs (a.tilt - b.tilt);
            const bool kurtosisDiffers = std::max (a.kurtosis, b.kurtosis)
                                         > 2.0 * std::min (a.kurtosis, b.kurtosis);
            const bool tiltDiffers = tiltGap > 1.0;

            CAPTURE (noiseTypeName (a.type), noiseTypeName (b.type), tiltGap, a.kurtosis, b.kurtosis);

            REQUIRE ((tiltDiffers || kurtosisDiffers));
        }
}

TEST_CASE ("noise: the four colours are calibrated to the same in-band level", "[noise]")
{
    // So that a preset's `kParamColor`-less level means the same thing whichever colour is chosen. Brown's
    // energy sits mostly below the audible band, so this is the 100 Hz - 8 kHz band and not the whole
    // spectrum.
    const auto reference = bandRms (oneRun (NoiseType::White));

    for (const auto type : { NoiseType::Pink, NoiseType::Brown, NoiseType::Geiger })
    {
        const double ratioDb = 20.0 * std::log10 (bandRms (oneRun (type)) / reference);
        CAPTURE (noiseTypeName (type), ratioDb);
        REQUIRE (std::abs (ratioDb) < 1.5);
    }
}

TEST_CASE ("noise: kParamRandomPhase decides whether two notes are the same noise", "[noise]")
{
    // The manual: RAND "prevents the noise from being identical each time you press a note". So with it
    // off, two consecutive notes replay the *same* stream (the generator re-arms to the note's start
    // state), and with it on they are decorrelated.
    const auto runTwoNotes = [] (bool randomPhase)
    {
        NoiseOscillator oscillator;
        oscillator.prepare (kSampleRate);
        oscillator.setType (NoiseType::White);

        std::vector<float> first (4096);
        std::vector<float> second (4096);

        oscillator.trigger (randomPhase);
        oscillator.process (first.data(), first.size());

        oscillator.trigger (randomPhase);
        oscillator.process (second.data(), second.size());

        return std::pair { first, second };
    };

    const auto [same1, same2] = runTwoNotes (false);
    const auto [different1, different2] = runTwoNotes (true);

    REQUIRE (std::equal (same1.begin(), same1.end(), same2.begin()));

    // Correlation is the measure, not equality: two independent white streams agree to ~1/sqrt(n).
    double dot = 0.0;
    double energy = 0.0;

    for (std::size_t i = 0; i < different1.size(); ++i)
    {
        dot += static_cast<double> (different1[i]) * static_cast<double> (different2[i]);
        energy += static_cast<double> (different1[i]) * static_cast<double> (different1[i]);
    }

    const double correlation = std::abs (dot) / energy;
    CAPTURE (correlation);
    REQUIRE (correlation < 0.1);
}

TEST_CASE ("noise: the colour is not a wavetable — its spectrum has no partials", "[noise]")
{
    // The acceptance's "it is not a wavetable", as a measurement: a wavetable oscillator's spectrum is a
    // harmonic series, so it is *not* flat — one band carries far more than the rest. White noise's is
    // flat to the estimator's precision. The comparison is against a real wavetable oscillator rather
    // than against an argument.
    const auto noise = oneRun (NoiseType::White, 1 << 14);

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 4);

    VoiceSettings settings;
    settings.envelope.attack = 0.001;
    settings.sub.enabled = true;      // a sub oscillator is a *periodic* source, the opposite case
    settings.sub.shape = SubShape::Saw;
    settings.sub.volume = 1.0;
    engine.setSettings (settings);
    REQUIRE (engine.noteOn (60, 1.0) == 0);

    std::vector<float> periodic (1 << 14);
    std::vector<float> periodicRight (periodic.size());
    for (std::size_t i = 0; i < periodic.size(); i += 128)
        engine.process (periodic.data() + i, periodicRight.data() + i, {}, 128);

    const double noiseFlatness = spectralFlatness (noise);
    const double periodicFlatness = spectralFlatness (periodic);

    CAPTURE (noiseFlatness, periodicFlatness);

    REQUIRE (noiseFlatness > 0.4);
    REQUIRE (periodicFlatness < 0.2);
}

TEST_CASE ("noise: the slot renders through the voice pool", "[noise]")
{
    // The acceptance's "a preset using NOISE renders it": the settings a preset maps to, through the real
    // fixed pool, measured. The slot must be in the signal path — not merely an engine class that exists.
    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 4);

    VoiceSettings settings;
    settings.envelope.attack = 0.001;
    settings.noise.enabled = true;
    settings.noise.type = NoiseType::Pink;
    settings.noise.volume = 0.5;
    engine.setSettings (settings);
    REQUIRE (engine.noteOn (55, 1.0) == 0);

    std::vector<float> rendered (1 << 14);
    std::vector<float> renderedRight (rendered.size());
    for (std::size_t i = 0; i < rendered.size(); i += 128)
        engine.process (rendered.data() + i, renderedRight.data() + i, {}, 128);

    double energy = 0.0;
    for (const auto sample : rendered)
        energy += static_cast<double> (sample) * static_cast<double> (sample);

    REQUIRE (energy > 0.0);
    REQUIRE (tiltDbPerOctave (rendered) == Catch::Approx (-3.0).margin (1.5));

    // And an *enabled* NOISE slot with no colour is the reference's sample mode: silence, because xerum
    // does not render a sample it cannot resolve, and a colour it was not asked for would be a
    // substitution the contract forbids.
    VoiceEngine silent;
    silent.prepare (kSampleRate, 128, 4);

    VoiceSettings sampleMode;
    sampleMode.envelope.attack = 0.001;
    sampleMode.noise.enabled = true;
    sampleMode.noise.type = std::nullopt;
    sampleMode.noise.volume = 1.0;
    silent.setSettings (sampleMode);
    REQUIRE (silent.noteOn (55, 1.0) == 0);

    std::vector<float> rendered2 (4096);
    std::vector<float> rendered2Right (rendered2.size());
    for (std::size_t i = 0; i < rendered2.size(); i += 128)
        silent.process (rendered2.data() + i, rendered2Right.data() + i, {}, 128);

    for (const auto sample : rendered2)
        REQUIRE (sample == 0.0f);
}
