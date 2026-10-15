// Behavioural tests for the effect rack.
//
// Ticket 29's acceptance is stated in *measurements*, not in parameter values: a distortion must change
// harmonic content, an EQ must change the spectrum at the shaped band, a compressor must reduce measured
// dynamic range, a delay must put an echo at its delay time, and a splitter's bands must process
// independently. Each test below drives the real effect and measures the property, in the same style as
// `filter_test.cpp`: probes sit on exact DFT bins so the answer is the effect's response rather than a
// windowing artefact.
//
// The convention-dependent parts — where `kParamFreq` maps to hertz, what `kParamTone` does to a
// convolution — are *not* asserted, because ticket 04 measured the ranges and not the curves. What is
// asserted is independent of them. **Two exceptions arrived with ticket `62`**, which swept the knob
// against the reference and fitted it: `kParamLevelOut` (`gain = 4·s²`, unity at the stored `0.5`) and
// `kParamThresh` (`threshold amplitude = (1−s)³`, `0 dBFS` at the stored `0`). Tests that need a gain or a
// threshold state it through `levelOutFor` or `threshFor`, so the law is named where it is used rather
// than spelled as a bare number.

#include <catch2/catch_test_macros.hpp>

#include "fx_rack.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace xerum::dsp;

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr int kBlock = 512;

/// Ticket 62: the reference's FX `kParamLevelOut` knob is a **squared** taper, unity at the stored
/// `0.5` — `gain = 4·s²`, measured against the reference (see `levelTaperGain`). A test that wants a
/// given gain states it through this rather than by storing the gain itself.
double levelOutFor (double gain)
{
    return std::sqrt (std::max (gain, 0.0)) * 0.5;
}

/// The inverse of `fxCompThresholdDb`: the stored `kParamThresh` whose measured threshold is `db`.
/// Ticket 62 fitted `threshold amplitude = (1−s)³` against the reference, so this is
/// `1 − cbrt(amplitude)`.
double threshFor (double db)
{
    const double amplitude = std::pow (10.0, db / 20.0);
    return 1.0 - std::cbrt (std::max (amplitude, 0.0));
}

std::unique_ptr<FxEffect> make (FxClass cls, double sampleRate = 48000.0)
{
    auto effect = makeFxEffect (cls);
    REQUIRE (effect != nullptr);
    effect->prepare (sampleRate, kBlock);
    return effect;
}

void set (FxEffect& effect, std::string_view name, double value)
{
    REQUIRE (effect.setNumber (name, value));
}

void setText (FxEffect& effect, std::string_view name, std::string_view value)
{
    REQUIRE (effect.setText (name, value));
}

/// The effect's gain at one frequency, measured: settle on a sine, then correlate the output against
/// it. The probe sits on an exact DFT bin, so no window and no leakage.
double gainAt (FxEffect& effect, double hertz, double sampleRate, double amplitude = 0.25)
{
    constexpr std::size_t kMeasure = 1u << 15;
    const auto bin = static_cast<std::size_t> (std::lround (hertz * static_cast<double> (kMeasure) / sampleRate));
    REQUIRE (bin >= 1);
    REQUIRE (bin <= kMeasure / 2 - 1);

    const double omega = kTwoPi * static_cast<double> (bin) / static_cast<double> (kMeasure);
    effect.reset();
    float left[1] {}, right[1] {};

    for (std::size_t i = 0; i < kMeasure; ++i)
    {
        left[0] = right[0] = static_cast<float> (amplitude * std::sin (omega * static_cast<double> (i)));
        effect.process (left, right, 1);
    }

    double real = 0.0, imaginary = 0.0;

    for (std::size_t i = 0; i < kMeasure; ++i)
    {
        left[0] = right[0] = static_cast<float> (amplitude * std::sin (omega * static_cast<double> (i)));
        effect.process (left, right, 1);
        real += left[0] * std::cos (omega * static_cast<double> (i));
        imaginary += left[0] * std::sin (omega * static_cast<double> (i));
    }

    return std::sqrt (real * real + imaginary * imaginary) / (amplitude * static_cast<double> (kMeasure) / 2.0);
}

double decibels (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1.0e-12)); }

/// The output's magnitude at `hertz` when the input is a steady sine at `inputHz` — the tool for
/// "what came out at this frequency", which is how harmonic and image content is measured.
double outputMagnitudeAt (FxEffect& effect, double inputHz, double hertz, double sampleRate,
                          double amplitude = 0.25)
{
    constexpr std::size_t kMeasure = 1u << 15;
    const auto bin = static_cast<std::size_t> (std::lround (hertz * static_cast<double> (kMeasure) / sampleRate));

    if (bin < 1 || bin > kMeasure / 2 - 1)
        return 0.0;

    const double inputOmega = kTwoPi * inputHz / sampleRate;
    const double omega = kTwoPi * static_cast<double> (bin) / static_cast<double> (kMeasure);
    effect.reset();
    float left[1] {}, right[1] {};

    for (std::size_t i = 0; i < kMeasure * 2; ++i)
    {
        left[0] = right[0] = static_cast<float> (amplitude * std::sin (inputOmega * static_cast<double> (i)));
        effect.process (left, right, 1);
    }

    double real = 0.0, imaginary = 0.0;

    for (std::size_t i = 0; i < kMeasure; ++i)
    {
        left[0] = right[0] = static_cast<float> (amplitude * std::sin (inputOmega * static_cast<double> (i)));
        effect.process (left, right, 1);
        real += left[0] * std::cos (omega * static_cast<double> (i));
        imaginary += left[0] * std::sin (omega * static_cast<double> (i));
    }

    return std::sqrt (real * real + imaginary * imaginary) / (static_cast<double> (kMeasure) / 2.0);
}

struct HarmonicContent
{
    double fundamental { 0.0 };
    double harmonic { 0.0 };

    /// Total harmonic energy over the fundamental, harmonics 2…6.
    double ratio() const { return harmonic / std::max (fundamental, 1.0e-12); }
};

HarmonicContent measureHarmonics (FxEffect& effect, double inputHz, double sampleRate)
{
    HarmonicContent content;
    content.fundamental = outputMagnitudeAt (effect, inputHz, inputHz, sampleRate);
    double sum = 0.0;

    for (int n = 2; n <= 6; ++n)
    {
        const double magnitude = outputMagnitudeAt (effect, inputHz, inputHz * n, sampleRate);
        sum += magnitude * magnitude;
    }

    content.harmonic = std::sqrt (sum);
    return content;
}

/// Feed a loud half-second then a quiet one, and report the level of each half's settled portion.
struct DynamicRange
{
    double loudDb { 0.0 };
    double quietDb { 0.0 };
    double rangeDb() const { return loudDb - quietDb; }
};

DynamicRange measureDynamicRange (FxEffect& effect, double sampleRate, double loudAmplitude, double quietAmplitude)
{
    const long total = static_cast<long> (sampleRate * 1.0);
    const long half = total / 2;
    std::vector<float> left (kBlock), right (kBlock);
    double loudSum = 0.0, quietSum = 0.0;
    long loudCount = 0, quietCount = 0;

    for (long start = 0; start + kBlock <= total; start += kBlock)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const long n = start + i;
            const double amplitude = n < half ? loudAmplitude : quietAmplitude;
            const double value = amplitude * std::sin (kTwoPi * 1000.0 * static_cast<double> (n) / sampleRate);
            left[static_cast<std::size_t> (i)] = static_cast<float> (value);
            right[static_cast<std::size_t> (i)] = static_cast<float> (value);
        }

        effect.process (left.data(), right.data(), kBlock);

        for (int i = 0; i < kBlock; ++i)
        {
            const long n = start + i;

            // Skip the first quarter of each regime: the detector needs to settle before its output
            // is a statement about the steady signal rather than about the transition.
            if (n >= half / 2 && n < half)
            {
                loudSum += static_cast<double> (left[static_cast<std::size_t> (i)])
                         * static_cast<double> (left[static_cast<std::size_t> (i)]);
                ++loudCount;
            }
            else if (n >= half + half / 2)
            {
                quietSum += static_cast<double> (left[static_cast<std::size_t> (i)])
                          * static_cast<double> (left[static_cast<std::size_t> (i)]);
                ++quietCount;
            }
        }
    }

    DynamicRange out;
    out.loudDb = 20.0 * std::log10 (std::sqrt (loudSum / std::max (loudCount, 1L)) + 1.0e-12);
    out.quietDb = 20.0 * std::log10 (std::sqrt (quietSum / std::max (quietCount, 1L)) + 1.0e-12);
    return out;
}

/// Index of the first significant peak in a response — where a delay's echo lands.
long firstPeakIndex (const std::vector<float>& samples, long from, float threshold)
{
    for (std::size_t i = static_cast<std::size_t> (from); i < samples.size(); ++i)
        if (std::fabs (samples[i]) >= threshold)
            return static_cast<long> (i);

    return -1;
}

/// The envelope's rate of variation and its dominant frequency, measured on a steady input.
///
/// The window is a whole number of input periods (eight, here). That makes the measurement exact for
/// *any* steady-state effect: a linear effect turns one sine into one sine of the same frequency, and
/// the RMS of a whole number of its periods is the same number whatever the phase — so a static
/// flanger measures as static rather than as the RMS window's own ripple.
struct Envelope
{
    double variation { 0.0 };
    double peakHz { 0.0 };
};

Envelope measureEnvelope (FxEffect& effect, double sampleRate, double toneHz, double seconds = 2.0)
{
    const int window = std::max (1, static_cast<int> (std::lround (sampleRate / toneHz))) * 8;
    const double omega = kTwoPi * toneHz / sampleRate;
    const long total = static_cast<long> (sampleRate * seconds);
    std::vector<float> left (static_cast<std::size_t> (window)), right (static_cast<std::size_t> (window));
    std::vector<double> envelope;
    effect.reset();

    for (long start = 0; start + window <= total; start += window)
    {
        double energy = 0.0;

        for (int k = 0; k < window; ++k)
        {
            const double value = 0.3 * std::sin (omega * static_cast<double> (start + k));
            left[static_cast<std::size_t> (k)] = static_cast<float> (value);
            right[static_cast<std::size_t> (k)] = static_cast<float> (value);
        }

        effect.process (left.data(), right.data(), window);

        for (int k = 0; k < window; ++k)
            energy += static_cast<double> (left[static_cast<std::size_t> (k)]) * left[static_cast<std::size_t> (k)];

        envelope.push_back (std::sqrt (energy / window));
    }

    Envelope out;

    // Discard the first 0.1 s: the delay lines start empty, and the step as they fill is a trend the
    // envelope's lowest DFT bin happily reports as modulation.
    const auto skip = static_cast<std::size_t> (std::ceil (0.1 * sampleRate / window));

    if (envelope.size() > skip + 32)
        envelope.erase (envelope.begin(), envelope.begin() + static_cast<long> (skip));

    if (envelope.empty())
        return out;

    double mean = 0.0;

    for (const double value : envelope)
        mean += value;

    mean /= static_cast<double> (envelope.size());
    double variance = 0.0;

    for (const double value : envelope)
        variance += (value - mean) * (value - mean);

    out.variation = mean > 0.0 ? std::sqrt (variance / static_cast<double> (envelope.size())) / mean : 0.0;

    for (double& value : envelope)
        value -= mean;

    const std::size_t size = envelope.size();
    double best = -1.0;
    std::size_t bestBin = 1;

    for (std::size_t k = 1; k < size / 2; ++k)
    {
        double real = 0.0, imaginary = 0.0;

        for (std::size_t i = 0; i < size; ++i)
        {
            const double angle = kTwoPi * static_cast<double> (k * i) / static_cast<double> (size);
            real += envelope[i] * std::cos (angle);
            imaginary += envelope[i] * std::sin (angle);
        }

        const double magnitude = real * real + imaginary * imaginary;

        if (magnitude > best)
        {
            best = magnitude;
            bestBin = k;
        }
    }

    out.peakHz = (sampleRate / window) * static_cast<double> (bestBin) / static_cast<double> (size);
    return out;
}

/// A rack's gain at one frequency, measured the same way `gainAt` measures an effect's. The rack's
/// slots run real filters with real transients, so a settled sine is the honest probe.
double rackGain (FxRack& rack, double hertz, double sampleRate, double amplitude = 0.2)
{
    constexpr std::size_t kMeasure = 1u << 14;
    const auto bin = static_cast<std::size_t> (std::lround (hertz * static_cast<double> (kMeasure) / sampleRate));
    const double omega = kTwoPi * static_cast<double> (bin) / static_cast<double> (kMeasure);
    std::vector<float> left (kMeasure), right (kMeasure);
    rack.reset();

    for (std::size_t start = 0; start < kMeasure; start += static_cast<std::size_t> (kBlock))
    {
        const int frames = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (kBlock),
                                                                    kMeasure - start));

        for (int i = 0; i < frames; ++i)
        {
            const double value = amplitude * std::sin (omega * static_cast<double> (start + static_cast<std::size_t> (i)));
            left[start + static_cast<std::size_t> (i)] = static_cast<float> (value);
            right[start + static_cast<std::size_t> (i)] = static_cast<float> (value);
        }

        rack.process (left.data() + start, right.data() + start, frames);
    }

    double real = 0.0, imaginary = 0.0;

    for (std::size_t i = 0; i < kMeasure; ++i)
    {
        real += left[i] * std::cos (omega * static_cast<double> (i));
        imaginary += left[i] * std::sin (omega * static_cast<double> (i));
    }

    return std::sqrt (real * real + imaginary * imaginary) / (amplitude * static_cast<double> (kMeasure) / 2.0);
}

} // namespace

TEST_CASE ("the sixteen classes are named, id'd, and cut the way usage says", "[fx]")
{
    CHECK (fxClassName (FxClass::Distortion) == "FXDistortion");
    CHECK (fxClassName (FxClass::SplitMS) == "FXSplitMS");
    CHECK (fxTypeId (FxClass::Distortion) == 0);
    CHECK (fxTypeId (FxClass::SplitMS) == 15);
    CHECK (fxTypeId (FxClass::Unknown) == -1);

    for (int id = 0; id < 16; ++id)
    {
        const auto cls = fxClassFromTypeId (id);
        CHECK (static_cast<int> (cls) == id);
        CHECK (fxClassFromName (fxClassName (cls)) == cls);
    }

    CHECK (fxClassFromName ("FXConvolve") == FxClass::Unknown);   // the name ticket 09 guessed, and no
    CHECK (fxClassFromName ("FXFromTheFuture") == FxClass::Unknown);

    // The cut itself: 14 of 16, and the two outside are the two least-used classes in the corpus.
    CHECK (shippedFxClasses().size() == 14);
    REQUIRE (deferredFxClasses().size() == 2);
    CHECK (deferredFxClasses()[0] == FxClass::Split3);
    CHECK (deferredFxClasses()[1] == FxClass::SplitMS);

    for (const auto cls : deferredFxClasses())
    {
        CHECK_FALSE (fxClassIsShipped (cls));
        CHECK (makeFxEffect (cls) == nullptr);
    }

    for (const auto cls : shippedFxClasses())
    {
        CHECK (fxClassIsShipped (cls));

        // `FXSplit` is shipped but is not an effect node — it is the routing that owns bands, and the
        // rack builds it directly. Every other shipped class is a `FxEffect`.
        if (fxClassIsSplitter (cls))
            CHECK (makeFxEffect (cls) == nullptr);
        else
            CHECK (makeFxEffect (cls) != nullptr);
    }

    // A splitter's topology is the format's, not the cut's: a deferred splitter still claims slots.
    CHECK (fxClassIsSplitter (FxClass::Split));
    CHECK (fxClassIsSplitter (FxClass::Split3));
    CHECK (fxClassIsSplitter (FxClass::SplitMS));
    CHECK (fxSplitterBandCount (FxClass::Split) == 2);
    CHECK (fxSplitterBandCount (FxClass::Split3) == 3);
    CHECK (fxSplitterBandCount (FxClass::SplitMS) == 2);
    CHECK_FALSE (fxClassIsSplitter (FxClass::Reverb));
}

TEST_CASE ("distortion changes harmonic content, and does it more as drive rises", "[fx]")
{
    const auto flat = [] (FxEffect& effect)
    {
        set (effect, "kParamWet", 100.0);
        set (effect, "kParamLevelOut", levelOutFor (1.0));   // ticket 62: unity is the stored 0.5
    };

    auto clean = make (FxClass::Distortion);
    setText (*clean, "kParamMode", "kHardClip");
    set (*clean, "kParamDrive", 0.0);     // unity gain: a 0.25 input never reaches the clip
    flat (*clean);
    const auto cleanHarmonics = measureHarmonics (*clean, 1000.0, 48000.0);

    auto low = make (FxClass::Distortion);
    setText (*low, "kParamMode", "kHardClip");
    set (*low, "kParamDrive", 40.0);
    flat (*low);
    const auto lowHarmonics = measureHarmonics (*low, 1000.0, 48000.0);

    auto high = make (FxClass::Distortion);
    setText (*high, "kParamMode", "kHardClip");
    set (*high, "kParamDrive", 90.0);
    flat (*high);
    const auto highHarmonics = measureHarmonics (*high, 1000.0, 48000.0);

    UNSCOPED_INFO ("THD at drive 0/40/90: " << cleanHarmonics.ratio() << " / " << lowHarmonics.ratio() << " / "
                                            << highHarmonics.ratio());

    // A hard clipper produces real odd harmonics, and turning DRIVE up produces more of them.
    CHECK (cleanHarmonics.ratio() < 0.01);
    CHECK (lowHarmonics.ratio() > 0.02);
    CHECK (highHarmonics.ratio() > 0.2);
    CHECK (highHarmonics.ratio() > lowHarmonics.ratio());

    // The fundamental survives: this is distortion, not a filter.
    CHECK (highHarmonics.fundamental > 0.01);

    // Wet at zero is the identity — the arithmetic the mix knob promises.
    auto bypassed = make (FxClass::Distortion);
    setText (*bypassed, "kParamMode", "kHardClip");
    set (*bypassed, "kParamDrive", 80.0);
    set (*bypassed, "kParamWet", 0.0);
    const auto bypassedHarmonics = measureHarmonics (*bypassed, 1000.0, 48000.0);
    CHECK (bypassedHarmonics.ratio() < 0.001);

    // A disabled slot is a no-op even at full drive.
    auto disabled = make (FxClass::Distortion);
    setText (*disabled, "kParamMode", "kHardClip");
    set (*disabled, "kParamDrive", 80.0);
    set (*disabled, "kParamEnable", 0.0);
    CHECK (measureHarmonics (*disabled, 1000.0, 48000.0).ratio() < 0.001);
}

TEST_CASE ("the EQ moves the shaped band and leaves the other one alone", "[fx]")
{
    auto equalizer = make (FxClass::Equalizer);

    // Low band as a peak: +12 dB at 1 kHz. The high band is left at its shelf default with 0 dB gain,
    // which is the identity.
    set (*equalizer, "kParamType1", 2.0);
    set (*equalizer, "kParamFreq1", 1000.0);
    set (*equalizer, "kParamGain1", 12.0);
    set (*equalizer, "kParamReso1", 20.0);
    set (*equalizer, "kParamWet", 100.0);

    const double inBand = decibels (gainAt (*equalizer, 998.0, 48000.0));
    const double outOfBand = decibels (gainAt (*equalizer, 6000.0, 48000.0));

    UNSCOPED_INFO ("EQ gain in band " << inBand << " dB, out of band " << outOfBand << " dB");

    CHECK (inBand > 9.0);
    CHECK (inBand < 14.0);
    CHECK (std::fabs (outOfBand) < 1.5);

    // A cut of the same size moves it the other way, which rules out "it just got louder".
    auto cut = make (FxClass::Equalizer);
    set (*cut, "kParamType1", 2.0);
    set (*cut, "kParamFreq1", 1000.0);
    set (*cut, "kParamGain1", -12.0);
    set (*cut, "kParamReso1", 20.0);
    set (*cut, "kParamWet", 100.0);
    CHECK (decibels (gainAt (*cut, 998.0, 48000.0)) < -9.0);

    // The high band shapes the top and leaves the bottom alone.
    auto top = make (FxClass::Equalizer);
    set (*top, "kParamType2", 2.0);
    set (*top, "kParamFreq2", 6000.0);
    set (*top, "kParamGain2", 12.0);
    set (*top, "kParamReso2", 20.0);
    set (*top, "kParamWet", 100.0);
    CHECK (decibels (gainAt (*top, 6000.0, 48000.0)) > 9.0);
    CHECK (std::fabs (decibels (gainAt (*top, 998.0, 48000.0))) < 1.5);
}

TEST_CASE ("a compressor reduces measured dynamic range", "[fx]")
{
    // 20 dB between the loud and quiet halves of the input.
    constexpr double kLoud = 0.5;
    constexpr double kQuiet = 0.05;

    // **The threshold is `-20 dBFS`, and ticket `65` restored it.** Before ticket `62` re-expressed it
    // through `threshFor`, this case stored the raw `20.0 / 120.0`, which under the *old* `-120·s`
    // reading was exactly `-20 dB` — the test's own threshold, and the one its assertions were written
    // for. Ticket `62`'s rewrite wrote `threshFor (-10.0)`, moving the threshold 10 dB *up* while leaving
    // the expectations where they were, and that is what left this case red: at `-10 dBFS` the loud half
    // (`-6.02 dBFS`) is only 3.98 dB above the threshold, so the most any compressor can take from it is
    // `3.98·(1 − 1/ratio)` dB — 3.78 dB at ratio 20 — and the quiet half is 16 dB below the threshold and
    // untouched, so the 20 dB input keeps **16.02 dB** however the compressor is built.
    //
    // Settled on the reference rather than by argument. Its own `FXComp` at `threshFor (-10)` (stored
    // `0.318687`, the law's `-10.0 dBFS`), ratio at its limit, fed a `0.5` then a `0.05` sine — the same
    // stimulus this case uses — renders **0.505269 then 0.050527** through its own plugin
    // (`BA - Analog Classics 2` with one `FXComp` slot, `render-preset.sh`, steady peak from 0.30 s,
    // input level set by the leaf `Global0/kParamMasterVolume`): **20.00 dB remaining, nothing removed at
    // all**, because its detector reads RMS and a 0.5-peak sine sits at `-9.0 dBFS` RMS, one dB above a
    // `-10 dBFS` threshold. So at `-10 dBFS` this case's expectation is not merely stale, it is
    // unreachable: the old `< 14.0` came from reading stored `0.3187` as `-38.2 dBFS`, which is where the
    // `-120·s` law put it. At `-20 dBFS` — the threshold the case always meant — the loud half is
    // 13.98 dB above it, a ratio-20 compressor removes 13.3 dB, and the quiet half is still below and
    // untouched, so the range lands near 6.7 dB and every assertion below holds as written.
    //
    // No assertion and no margin was relaxed to make this pass; the law itself is what ticket `62`
    // certified against the corpus and `[fx-units]` re-measures against the reference.
    constexpr double kThresholdDb = -20.0;

    auto uncompressed = make (FxClass::Compressor);
    set (*uncompressed, "kParamRatio", 1.0);      // ratio 1 is "do nothing"
    set (*uncompressed, "kParamThresh", threshFor (kThresholdDb));
    set (*uncompressed, "kParamWet", 100.0);
    const auto open = measureDynamicRange (*uncompressed, 48000.0, kLoud, kQuiet);

    auto compressed = make (FxClass::Compressor);
    set (*compressed, "kParamRatio", 20.0);
    set (*compressed, "kParamThresh", threshFor (kThresholdDb));   // ticket 62's measured law
    set (*compressed, "kParamAttack", 1.0);
    set (*compressed, "kParamRelease", 100.0);
    set (*compressed, "kParamWet", 100.0);
    const auto squashed = measureDynamicRange (*compressed, 48000.0, kLoud, kQuiet);

    UNSCOPED_INFO ("range open " << open.rangeDb() << " dB, compressed " << squashed.rangeDb() << " dB");

    CHECK (open.rangeDb() > 18.0);
    CHECK (squashed.rangeDb() < open.rangeDb() - 5.0);
    CHECK (squashed.rangeDb() < 14.0);

    // Make-up gain lifts both halves; it does not put the dynamics back.
    auto withMakeup = make (FxClass::Compressor);
    set (*withMakeup, "kParamRatio", 20.0);
    set (*withMakeup, "kParamThresh", threshFor (kThresholdDb));
    set (*withMakeup, "kParamAttack", 1.0);
    set (*withMakeup, "kParamRelease", 100.0);
    set (*withMakeup, "kParamMakeup", 12.0);
    set (*withMakeup, "kParamWet", 100.0);
    const auto lifted = measureDynamicRange (*withMakeup, 48000.0, kLoud, kQuiet);
    CHECK (lifted.loudDb > squashed.loudDb + 8.0);
    CHECK (lifted.rangeDb() < 14.0);

    // A harder ratio squeezes the same signal further, which is the same statement on the ratio axis
    // rather than on a knob axis: compression is monotone in RATIO.
    auto limiter = make (FxClass::Compressor);
    set (*limiter, "kParamRatio", 1000.0);
    set (*limiter, "kParamThresh", threshFor (kThresholdDb));
    set (*limiter, "kParamAttack", 1.0);
    set (*limiter, "kParamRelease", 100.0);
    set (*limiter, "kParamWet", 100.0);
    const auto limited = measureDynamicRange (*limiter, 48000.0, kLoud, kQuiet);
    UNSCOPED_INFO ("range at ratio 1000 " << limited.rangeDb() << " dB");
    CHECK (limited.rangeDb() < squashed.rangeDb());
}

TEST_CASE ("a delay puts its echo at the delay time", "[fx]")
{
    constexpr double kSampleRate = 48000.0;
    constexpr double kDelaySeconds = 0.125;

    auto delay = make (FxClass::Delay, kSampleRate);
    set (*delay, "kParamTimeL", kDelaySeconds);
    set (*delay, "kParamTimeR", kDelaySeconds);
    set (*delay, "kParamOffsetL", 1.0);
    set (*delay, "kParamOffsetR", 1.0);
    set (*delay, "kParamFeedback", 0.0);
    set (*delay, "kParamWet", 100.0);
    set (*delay, "kParamFreq", 18000.0);

    const auto expected = static_cast<long> (kDelaySeconds * kSampleRate);
    std::vector<float> left (static_cast<std::size_t> (kSampleRate * 1.0), 0.0f);
    std::vector<float> right (left.size(), 0.0f);
    left[0] = 1.0f;
    right[0] = 1.0f;

    for (std::size_t start = 0; start < left.size(); start += static_cast<std::size_t> (kBlock))
    {
        const int frames = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (kBlock),
                                                                    left.size() - start));
        delay->process (left.data() + start, right.data() + start, frames);
    }

    const long peak = firstPeakIndex (left, 10, 0.5f);
    UNSCOPED_INFO ("echo at " << peak << ", expected " << expected);
    REQUIRE (peak > 0);
    CHECK (std::abs (peak - expected) <= 2);

    // Wet at zero is the dry signal: no echo anywhere.
    auto dry = make (FxClass::Delay, kSampleRate);
    set (*dry, "kParamTimeL", kDelaySeconds);
    set (*dry, "kParamWet", 0.0);
    std::vector<float> dryLeft (4096, 0.0f), dryRight (4096, 0.0f);
    dryLeft[0] = dryRight[0] = 1.0f;
    dry->process (dryLeft.data(), dryRight.data(), 4096);
    CHECK (dryLeft[0] == 1.0f);
    CHECK (firstPeakIndex (dryLeft, 10, 0.5f) == -1);
}

TEST_CASE ("the rack filter measures the slope the voice filter does", "[fx]")
{
    // `FXFilter` is `VoiceFilter` (ticket 04 section 2.3), so an L24 measures 24 dB/oct here too.
    auto filter = make (FxClass::Filter);
    setText (*filter, "kParamType", "L24");
    set (*filter, "kParamFreq", std::log (1000.0 / 20.0) / std::log (1000.0));
    set (*filter, "kParamReso", 0.0);
    set (*filter, "kParamWet", 100.0);

    const double lowGain = gainAt (*filter, 4000.0, 48000.0);
    const double highGain = gainAt (*filter, 8000.0, 48000.0);
    const double slope = 20.0 * std::log10 (highGain / lowGain);

    UNSCOPED_INFO ("FXFilter L24 slope over 4k→8k: " << slope << " dB/oct");
    CHECK (slope < -18.0);
    CHECK (slope > -30.0);

    // A type the engine does not build reports, and passes the band through rather than substituting.
    auto unknown = make (FxClass::Filter);
    setText (*unknown, "kParamType", "Reverb1");
    CHECK_FALSE (unknown->problem().empty());
    CHECK (gainAt (*unknown, 2000.0, 48000.0) > 0.9);
}

TEST_CASE ("a reverb's tail lengthens with decay", "[fx]")
{
    constexpr double kSampleRate = 48000.0;

    const auto tailRatio = [&] (double decay)
    {
        auto reverb = make (FxClass::Reverb, kSampleRate);
        set (*reverb, "kParamDelay", decay);
        set (*reverb, "kParamSize", 50.0);
        set (*reverb, "kParamWet", 100.0);

        const auto total = static_cast<std::size_t> (kSampleRate * 0.9);
        std::vector<float> left (total, 0.0f), right (total, 0.0f);
        left[0] = 1.0f;
        right[0] = 1.0f;

        for (std::size_t start = 0; start < total; start += static_cast<std::size_t> (kBlock))
        {
            const int frames = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (kBlock),
                                                                        total - start));
            reverb->process (left.data() + start, right.data() + start, frames);
        }

        double early = 0.0, late = 0.0;

        for (std::size_t i = static_cast<std::size_t> (kSampleRate * 0.1); i < static_cast<std::size_t> (kSampleRate * 0.3); ++i)
            early += static_cast<double> (left[i]) * left[i];

        for (std::size_t i = static_cast<std::size_t> (kSampleRate * 0.6); i < static_cast<std::size_t> (kSampleRate * 0.8); ++i)
            late += static_cast<double> (left[i]) * left[i];

        return late / std::max (early, 1.0e-12);
    };

    const double shortTail = tailRatio (20.0);
    const double longTail = tailRatio (200.0);
    UNSCOPED_INFO ("late/early energy: decay 20 -> " << shortTail << ", decay 200 -> " << longTail);
    CHECK (longTail > shortTail * 20.0);

    // Wet at zero is dry, so a reverb that is "off" adds nothing.
    auto dry = make (FxClass::Reverb, kSampleRate);
    set (*dry, "kParamDelay", 200.0);
    set (*dry, "kParamWet", 0.0);
    std::vector<float> left (4096, 0.0f), right (4096, 0.0f);
    left[0] = right[0] = 1.0f;
    dry->process (left.data(), right.data(), 4096);
    CHECK (left[0] == 1.0f);
    CHECK (firstPeakIndex (left, 10, 0.5f) == -1);
}

TEST_CASE ("the modulated effects really modulate, and stop when depth does", "[fx]")
{
    constexpr double kSampleRate = 48000.0;

    SECTION ("flanger")
    {
        auto active = make (FxClass::Flanger, kSampleRate);
        set (*active, "kParamRate", 3.0);
        set (*active, "kParamDepth", 60.0);
        set (*active, "kParamWet", 100.0);
        const auto modulated = measureEnvelope (*active, kSampleRate, 1000.0);

        auto still = make (FxClass::Flanger, kSampleRate);
        set (*still, "kParamDepth", 0.0);
        set (*still, "kParamWet", 100.0);
        const auto static_ = measureEnvelope (*still, kSampleRate, 1000.0);

        UNSCOPED_INFO ("flanger envelope variation " << modulated.variation << " at " << modulated.peakHz
                                                     << " Hz; static " << static_.variation);
        CHECK (modulated.variation > 0.05);
        CHECK (modulated.variation > static_.variation * 3.0);
        CHECK (static_.variation < 0.005);

        const double multiple = modulated.peakHz / 3.0;
        CHECK (multiple > 0.5);
        CHECK (std::fabs (multiple - std::round (multiple)) < 0.25);
    }

    SECTION ("phaser")
    {
        auto active = make (FxClass::Phaser, kSampleRate);
        set (*active, "kParamRate", 3.0);
        set (*active, "kParamDepth", 80.0);
        set (*active, "kParamNumPoles", 6.0);
        set (*active, "kParamFreq", 1000.0);
        set (*active, "kParamWet", 100.0);
        const auto modulated = measureEnvelope (*active, kSampleRate, 1000.0);

        auto still = make (FxClass::Phaser, kSampleRate);
        set (*still, "kParamDepth", 0.0);
        set (*still, "kParamNumPoles", 6.0);
        set (*still, "kParamFreq", 1000.0);
        set (*still, "kParamWet", 100.0);
        const auto static_ = measureEnvelope (*still, kSampleRate, 1000.0);

        UNSCOPED_INFO ("phaser envelope variation " << modulated.variation << "; static " << static_.variation);
        // A phaser's notches are narrow, so the amplitude at one probe frequency moves less than a
        // flanger's comb does; the statement is that it moves, and a still one does not.
        CHECK (modulated.variation > 0.001);
        CHECK (modulated.variation > static_.variation * 50.0);
        CHECK (static_.variation < 0.005);
    }

    SECTION ("chorus")
    {
        auto active = make (FxClass::Chorus, kSampleRate);
        set (*active, "kParamRate", 3.0);
        set (*active, "kParamDepth", 60.0);
        set (*active, "kParamDelay", 4.0);
        set (*active, "kParamDelay2", 9.0);
        set (*active, "kParamWet", 100.0);
        const auto modulated = measureEnvelope (*active, kSampleRate, 1000.0);

        auto still = make (FxClass::Chorus, kSampleRate);
        set (*still, "kParamDepth", 0.0);
        set (*still, "kParamDelay", 4.0);
        set (*still, "kParamWet", 100.0);
        const auto static_ = measureEnvelope (*still, kSampleRate, 1000.0);

        UNSCOPED_INFO ("chorus envelope variation " << modulated.variation << "; static " << static_.variation);
        CHECK (modulated.variation > 0.02);
        CHECK (modulated.variation > static_.variation * 3.0);
        CHECK (static_.variation < 0.005);
    }
}

TEST_CASE ("Bode shifts frequency one way, not two", "[fx]")
{
    constexpr double kSampleRate = 48000.0;

    auto bode = make (FxClass::Bode, kSampleRate);
    set (*bode, "kParamShift", 20.0);       // 20 % of the range
    set (*bode, "kParamRange", 1000.0);     // → +200 Hz
    set (*bode, "kParamWet", 100.0);
    set (*bode, "kParamOutputWidth", 0.0);

    const double shifted = outputMagnitudeAt (*bode, 1000.0, 1200.0, kSampleRate);
    const double image = outputMagnitudeAt (*bode, 1000.0, 800.0, kSampleRate);

    UNSCOPED_INFO ("Bode +200 Hz: magnitude at 1200 Hz " << shifted << ", at 800 Hz " << image);

    CHECK (shifted > 0.1);
    CHECK (decibels (shifted / std::max (image, 1.0e-12)) > 20.0);

    // A negative shift moves it the other way.
    auto down = make (FxClass::Bode, kSampleRate);
    set (*down, "kParamShift", -20.0);
    set (*down, "kParamRange", 1000.0);
    set (*down, "kParamWet", 100.0);
    set (*down, "kParamOutputWidth", 0.0);
    CHECK (outputMagnitudeAt (*down, 1000.0, 800.0, kSampleRate) > 0.1);
    CHECK (outputMagnitudeAt (*down, 1000.0, 1200.0, kSampleRate)
           < outputMagnitudeAt (*down, 1000.0, 800.0, kSampleRate) * 0.1);
}

TEST_CASE ("convolution convolves, and a missing impulse is reported rather than ignored", "[fx]")
{
    constexpr double kSampleRate = 48000.0;
    constexpr int kIrFrames = 256;
    constexpr int kSpikeAt = 100;

    std::vector<float> leftIr (kIrFrames, 0.0f), rightIr (kIrFrames, 0.0f);
    leftIr[static_cast<std::size_t> (kSpikeAt)] = 1.0f;
    rightIr[static_cast<std::size_t> (kSpikeAt)] = 1.0f;
    const float* channels[2] { leftIr.data(), rightIr.data() };
    ImpulseResponse impulse { channels, 2, kIrFrames, kSampleRate };

    auto convolver = make (FxClass::Convolve, kSampleRate);
    set (*convolver, "kParamWet", 100.0);
    convolver->setImpulse (impulse);
    REQUIRE (convolver->problem().empty());

    std::vector<float> left (2048, 0.0f), right (2048, 0.0f);
    left[0] = 1.0f;
    right[0] = 1.0f;

    for (std::size_t start = 0; start < left.size(); start += static_cast<std::size_t> (kBlock))
    {
        const int frames = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (kBlock),
                                                                    left.size() - start));
        convolver->process (left.data() + start, right.data() + start, frames);
    }

    const long peak = firstPeakIndex (left, 1, 0.9f);
    UNSCOPED_INFO ("convolution peak at " << peak << ", impulse's spike at " << kSpikeAt);
    CHECK (peak == kSpikeAt);
    CHECK (std::fabs (left[static_cast<std::size_t> (kSpikeAt)]) > 0.9f);

    // No impulse: it must say so, and it must not convolve with nothing.
    auto missing = make (FxClass::Convolve, kSampleRate);
    set (*missing, "kParamWet", 100.0);
    CHECK_FALSE (missing->problem().empty());
    UNSCOPED_INFO ("missing impulse reports: " << missing->problem());

    std::vector<float> dryLeft (512, 0.0f), dryRight (512, 0.0f);
    dryLeft[3] = 0.75f;
    dryRight[3] = -0.5f;
    missing->process (dryLeft.data(), dryRight.data(), 512);
    CHECK (dryLeft[3] == 0.75f);
    CHECK (dryRight[3] == -0.5f);
}

TEST_CASE ("utility filters, inverts polarity and widens", "[fx]")
{
    constexpr double kSampleRate = 48000.0;

    auto filtered = make (FxClass::Utility, kSampleRate);
    set (*filtered, "kParamHPF", 300.0);
    set (*filtered, "kParamLPF", 20000.0);
    set (*filtered, "kParamWet", 100.0);
    const double low = gainAt (*filtered, 50.0, kSampleRate);
    const double high = gainAt (*filtered, 3000.0, kSampleRate);
    UNSCOPED_INFO ("utility HPF at 300 Hz: gain at 50 Hz " << low << ", at 3 kHz " << high);
    CHECK (low < 0.25);
    CHECK (high > 0.9);

    auto inverted = make (FxClass::Utility, kSampleRate);
    set (*inverted, "kParamPolarityL", 1.0);
    set (*inverted, "kParamWet", 100.0);
    std::vector<float> left (64, 0.0f), right (64, 0.0f);
    left[0] = 0.5f;
    right[0] = 0.5f;
    inverted->process (left.data(), right.data(), 64);
    CHECK (left[0] < 0.0f);
    CHECK (right[0] > 0.0f);

    // Width scales the side signal; fed a hard-left input, the right channel gains energy.
    auto width = [&] (double percent)
    {
        auto utility = make (FxClass::Utility, kSampleRate);
        set (*utility, "kParamWidth", percent);
        set (*utility, "kParamWet", 100.0);
        utility->reset();
        std::vector<float> l (2048), r (2048);
        double side = 0.0;

        for (std::size_t i = 0; i < l.size(); ++i)
        {
            l[i] = static_cast<float> (0.5 * std::sin (kTwoPi * 500.0 * static_cast<double> (i) / kSampleRate));
            r[i] = 0.0f;
        }

        utility->process (l.data(), r.data(), static_cast<int> (l.size()));

        for (std::size_t i = 0; i < l.size(); ++i)
            side += static_cast<double> (r[i]) * r[i];

        return std::sqrt (side / static_cast<double> (l.size()));
    };

    const double narrow = width (50.0);
    const double wide = width (300.0);
    UNSCOPED_INFO ("side energy at width 50 %: " << narrow << ", at 300 %: " << wide);
    CHECK (wide > narrow * 2.0);
}

TEST_CASE ("Hyper/Dimension widens, and only when asked", "[fx]")
{
    constexpr double kSampleRate = 48000.0;

    const auto sideEnergy = [] (double unison, double detune, double dimensionSize)
    {
        auto effect = make (FxClass::HyperDimension, kSampleRate);
        set (*effect, "kParamUnison", unison);
        set (*effect, "kParamDetune", detune);
        set (*effect, "kParamRate", 30.0);
        set (*effect, "kParamDimESize", dimensionSize);
        set (*effect, "kParamDimEWet", 100.0);
        set (*effect, "kParamWet", 100.0);

        const std::size_t total = 8192;
        std::vector<float> left (total), right (total);

        for (std::size_t i = 0; i < total; ++i)
        {
            left[i] = static_cast<float> (0.3 * std::sin (kTwoPi * 500.0 * static_cast<double> (i) / kSampleRate));
            right[i] = left[i];
        }

        for (std::size_t start = 0; start < total; start += static_cast<std::size_t> (kBlock))
        {
            const int frames = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (kBlock),
                                                                        total - start));
            effect->process (left.data() + start, right.data() + start, frames);
        }

        double side = 0.0;

        for (std::size_t i = total / 2; i < total; ++i)
        {
            const double difference = 0.5 * (static_cast<double> (left[i]) - right[i]);
            side += difference * difference;
        }

        return std::sqrt (side / static_cast<double> (total / 2));
    };

    const double off = sideEnergy (0.0, 0.0, 0.0);
    const double dimension = sideEnergy (0.0, 0.0, 100.0);
    const double unison = sideEnergy (5.0, 80.0, 0.0);

    UNSCOPED_INFO ("side energy: off " << off << ", dimension " << dimension << ", unison " << unison);
    CHECK (off < 1.0e-4);
    CHECK (dimension > 1.0e-3);
    CHECK (unison > 1.0e-3);
}

TEST_CASE ("a rack runs its slots in series", "[fx]")
{
    FxRack rack;
    rack.prepare (48000.0, kBlock);

    FxSlot first;
    first.cls = FxClass::Utility;
    first.numbers = { { "kParamLevelOut", levelOutFor (0.5) }, { "kParamWet", 100.0 } };
    FxSlot second = first;

    rack.setSlots ({ first, second });

    REQUIRE (rack.nodeCount() == 2);
    CHECK (rack.unresolved().empty());

    // Two half-gain slots in series is a quarter, not a half — which is what "serial" means.
    const double gain = rackGain (rack, 1000.0, 48000.0);
    UNSCOPED_INFO ("two half-gain slots in series: gain " << gain);
    CHECK (std::fabs (gain - 0.25) < 0.01);
}

TEST_CASE ("a splitter's bands process independently, measured", "[fx][fx-topology]")
{
    constexpr double kSampleRate = 48000.0;

    // One splitter at 1 kHz, a quarter-gain utility in the LOW band and a unity one in the HIGH band.
    FxSlot split;
    split.cls = FxClass::Split;
    split.numbers = { { "kParamFreq", 1000.0 }, { "kParamModuleCount1", 1.0 }, { "kParamModuleCount2", 1.0 } };
    FxSlot lowBand;
    lowBand.cls = FxClass::Utility;
    lowBand.numbers = { { "kParamLevelOut", levelOutFor (0.25) }, { "kParamWet", 100.0 } };
    FxSlot highBand = lowBand;
    highBand.numbers = { { "kParamLevelOut", levelOutFor (1.0) }, { "kParamWet", 100.0 } };

    FxRack rack;
    rack.prepare (kSampleRate, kBlock);
    rack.setSlots ({ split, lowBand, highBand });

    // The splitter itself is not a node: two band chains are.
    REQUIRE (rack.nodeCount() == 2);
    CHECK (rack.unresolved().empty());

    // A two-tone probe, both tones on **exact DFT bins** (199.2 Hz and 5001.9 Hz): a tone a hertz off
    // its bin leaks, and correlating at the bin then measures the leakage rather than the band.
    constexpr std::size_t kMeasure = 1u << 14;
    constexpr std::size_t kLowBin = 68;
    constexpr std::size_t kHighBin = 1707;
    const double lowOmega = kTwoPi * static_cast<double> (kLowBin) / static_cast<double> (kMeasure);
    const double highOmega = kTwoPi * static_cast<double> (kHighBin) / static_cast<double> (kMeasure);

    const auto bandGain = [&] (double omega)
    {
        std::vector<float> left (kMeasure), right (kMeasure);

        for (std::size_t i = 0; i < kMeasure; ++i)
        {
            const double value = 0.3 * std::sin (lowOmega * static_cast<double> (i))
                               + 0.3 * std::sin (highOmega * static_cast<double> (i));
            left[i] = static_cast<float> (value);
            right[i] = static_cast<float> (value);
        }

        rack.reset();

        for (std::size_t start = 0; start < kMeasure; start += static_cast<std::size_t> (kBlock))
        {
            const int frames = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (kBlock),
                                                                        kMeasure - start));
            rack.process (left.data() + start, right.data() + start, frames);
        }

        double real = 0.0, imaginary = 0.0;

        for (std::size_t i = 0; i < kMeasure; ++i)
        {
            real += left[i] * std::cos (omega * static_cast<double> (i));
            imaginary += left[i] * std::sin (omega * static_cast<double> (i));
        }

        // The probe's own amplitude in the output's DFT is 0.3 * N / 2.
        return std::sqrt (real * real + imaginary * imaginary) / (0.3 * static_cast<double> (kMeasure) / 2.0);
    };

    const double lowGain = bandGain (lowOmega);
    const double highGain = bandGain (highOmega);
    UNSCOPED_INFO ("splitter bands: 200 Hz gain " << lowGain << ", 5 kHz gain " << highGain);

    CHECK (std::fabs (lowGain - 0.25) < 0.06);
    CHECK (std::fabs (highGain - 1.0) < 0.15);

    // The contrast: the *same* two utilities in series scale both tones by a quarter. That is the
    // difference between parallel bands and a serial chain, and it is what the numbers above mean.
    FxRack serial;
    serial.prepare (kSampleRate, kBlock);
    FxSlot quarter;
    quarter.cls = FxClass::Utility;
    quarter.numbers = { { "kParamLevelOut", levelOutFor (0.25) }, { "kParamWet", 100.0 } };
    serial.setSlots ({ quarter, quarter });
    REQUIRE (serial.nodeCount() == 2);

    const auto serialGain = [&] (double hertz)
    {
        constexpr std::size_t kMeasure = 1u << 14;
        std::vector<float> left (kMeasure), right (kMeasure);

        for (std::size_t i = 0; i < kMeasure; ++i)
        {
            const double value = 0.3 * std::sin (kTwoPi * hertz * static_cast<double> (i) / kSampleRate);
            left[i] = static_cast<float> (value);
            right[i] = static_cast<float> (value);
        }

        serial.reset();

        for (std::size_t start = 0; start < kMeasure; start += static_cast<std::size_t> (kBlock))
        {
            const int frames = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (kBlock),
                                                                        kMeasure - start));
            serial.process (left.data() + start, right.data() + start, frames);
        }

        const auto bin = static_cast<std::size_t> (std::lround (hertz * static_cast<double> (kMeasure) / kSampleRate));
        const double omega = kTwoPi * static_cast<double> (bin) / static_cast<double> (kMeasure);
        double real = 0.0, imaginary = 0.0;

        for (std::size_t i = 0; i < kMeasure; ++i)
        {
            real += left[i] * std::cos (omega * static_cast<double> (i));
            imaginary += left[i] * std::sin (omega * static_cast<double> (i));
        }

        return std::sqrt (real * real + imaginary * imaginary) / (0.3 * static_cast<double> (kMeasure) / 2.0);
    };

    CHECK (std::fabs (serialGain (200.0) - 0.0625) < 0.02);
    CHECK (std::fabs (serialGain (5000.0) - 0.0625) < 0.02);
}

TEST_CASE ("a splitter's flat array is decoded the way the corpus encodes it", "[fx][fx-topology]")
{
    FxRack rack;
    rack.prepare (48000.0, kBlock);

    // [split{c1=1,c2=1}, low, high, serial] — the splitter claims two slots, then serial resumes.
    FxSlot split;
    split.cls = FxClass::Split;
    split.numbers = { { "kParamFreq", 800.0 }, { "kParamModuleCount1", 1.0 }, { "kParamModuleCount2", 1.0 } };
    FxSlot low;
    low.cls = FxClass::Equalizer;
    FxSlot high;
    high.cls = FxClass::Reverb;
    FxSlot trailing;
    trailing.cls = FxClass::Delay;

    rack.setSlots ({ split, low, high, trailing });
    // Two band chains plus the trailing serial slot; the splitter claims its two slots and is not one.
    CHECK (rack.nodeCount() == 3);
    CHECK (rack.unresolved().empty());

    SECTION ("an absent count means zero, so a one-band splitter claims one slot")
    {
        // Measured on the corpus: with 1 as the default, 14 racks overrun their own array.
        FxSlot oneBand;
        oneBand.cls = FxClass::Split;
        oneBand.numbers = { { "kParamFreq", 800.0 }, { "kParamModuleCount1", 2.0 } };
        rack.setSlots ({ oneBand, low, high, trailing });
        // `kParamModuleCount1` is 2 and `kParamModuleCount2` is absent, so the second band is empty and
        // the splitter claims exactly two slots. Trailing serial processing still runs.
        CHECK (rack.nodeCount() == 3);
        CHECK (rack.unresolved().empty());
    }

    SECTION ("a deferred splitter is reported and its slots are not run as if they were serial")
    {
        FxSlot deferred;
        deferred.cls = FxClass::Split3;
        deferred.numbers = { { "kParamFreq", 400.0 },
                             { "kParamFreq2", 2000.0 },
                             { "kParamModuleCount1", 1.0 },
                             { "kParamModuleCount2", 1.0 },
                             { "kParamModuleCount3", 1.0 } };

        rack.setSlots ({ deferred, low, high, trailing });
        REQUIRE (rack.unresolved().size() == 1);
        CHECK (rack.unresolved()[0] == "FXSplit3");
        // The deferred splitter claims three slots and we do not reproduce that routing, so it and its
        // whole group are skipped rather than being run as if they were serial. Nothing is built.
        CHECK (rack.nodeCount() == 0);
    }

    SECTION ("an unknown class is reported, not substituted")
    {
        FxSlot unknown;
        unknown.cls = FxClass::Unknown;
        rack.setSlots ({ unknown, trailing });
        REQUIRE (rack.unresolved().size() == 1);
        CHECK (rack.unresolved()[0] == "unknown");
        CHECK (rack.nodeCount() == 1);
    }

    SECTION ("a parameter the effect does not know is reported by name")
    {
        FxSlot odd;
        odd.cls = FxClass::Utility;
        odd.numbers = { { "kParamFromTheFuture", 1.0 } };
        rack.setSlots ({ odd });
        REQUIRE (rack.unresolved().size() == 1);
        CHECK (rack.unresolved()[0] == "FXUtils/kParamFromTheFuture");
    }
}

TEST_CASE ("the three racks are wired MAIN, BUS 1 and BUS 2", "[fx]")
{
    FxBank bank;
    bank.prepare (48000.0, kBlock);

    FxSlot half;
    half.cls = FxClass::Utility;
    half.numbers = { { "kParamLevelOut", levelOutFor (0.5) }, { "kParamWet", 100.0 } };

    for (int index = 0; index < FxBank::kRackCount; ++index)
        bank.rack (index).setSlots ({ half });

    // A settled 1 kHz sine sent to BUS 1 only; the output is MAIN's, measured at that frequency.
    const auto outputAmplitude = [&] (double destination, double volume)
    {
        bank.setBusRouting (destination, volume, destination, volume);

        constexpr std::size_t kMeasure = 1u << 14;
        const auto bin = static_cast<std::size_t> (std::lround (1000.0 * static_cast<double> (kMeasure) / 48000.0));
        const double omega = kTwoPi * static_cast<double> (bin) / static_cast<double> (kMeasure);
        std::vector<float> mainLeft (kMeasure, 0.0f), mainRight (kMeasure, 0.0f);
        std::vector<float> busLeft (kMeasure), busRight (kMeasure);
        std::vector<float> silentLeft (kMeasure, 0.0f), silentRight (kMeasure, 0.0f);

        for (std::size_t i = 0; i < kMeasure; ++i)
        {
            const double value = 0.5 * std::sin (omega * static_cast<double> (i));
            busLeft[i] = static_cast<float> (value);
            busRight[i] = static_cast<float> (value);
        }

        bank.reset();

        for (std::size_t start = 0; start < kMeasure; start += static_cast<std::size_t> (kBlock))
        {
            const int frames = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (kBlock),
                                                                        kMeasure - start));
            bank.process (mainLeft.data() + start, mainRight.data() + start, busLeft.data() + start,
                          busRight.data() + start, silentLeft.data() + start, silentRight.data() + start, frames);
        }

        double real = 0.0, imaginary = 0.0;

        for (std::size_t i = 0; i < kMeasure; ++i)
        {
            real += mainLeft[i] * std::cos (omega * static_cast<double> (i));
            imaginary += mainLeft[i] * std::sin (omega * static_cast<double> (i));
        }

        return std::sqrt (real * real + imaginary * imaginary) / (0.5 * static_cast<double> (kMeasure) / 2.0);
    };

    const double throughMain = outputAmplitude (1.0, 1.0);
    const double aroundMain = outputAmplitude (2.0, 1.0);
    const double scaled = outputAmplitude (1.0, 0.5);

    UNSCOPED_INFO ("bus 1 through MAIN " << throughMain << ", around MAIN " << aroundMain << ", half volume "
                                         << scaled);

    // Destination 1: BUS 1's rack halves the send and MAIN's halves it again.
    CHECK (std::fabs (throughMain - 0.25) < 0.01);
    // Destination 2: BUS 1's rack halves it and it is summed after MAIN, so MAIN's gain is not applied.
    CHECK (std::fabs (aroundMain - 0.5) < 0.01);
    // Volume scales the send.
    CHECK (std::fabs (scaled - 0.125) < 0.01);
}


TEST_CASE ("a rack processes a block longer than its prepare size", "[fx]")
{
    // `prepare`'s block sizes the scratch. A caller that hands over a longer block must get its audio
    // processed, not its tail dropped — so a delay whose echo lands *past* the first chunk must still
    // echo. Truncation is exactly what this test would catch.
    constexpr double kSampleRate = 48000.0;
    constexpr int kFrames = 4096;

    FxRack rack;
    rack.prepare (kSampleRate, kBlock);

    FxSlot delay;
    delay.cls = FxClass::Delay;
    delay.numbers = { { "kParamTimeL", 0.05 },   // 2400 samples: inside the block, past 512
                      { "kParamTimeR", 0.05 },
                      { "kParamOffsetL", 1.0 },
                      { "kParamOffsetR", 1.0 },
                      { "kParamFeedback", 0.0 },
                      { "kParamWet", 100.0 },
                      { "kParamFreq", 18000.0 } };
    rack.setSlots ({ delay });

    std::vector<float> left (kFrames, 0.0f), right (kFrames, 0.0f);
    left[0] = 1.0f;
    right[0] = 1.0f;
    rack.process (left.data(), right.data(), kFrames);

    const long peak = firstPeakIndex (left, 10, 0.5f);
    UNSCOPED_INFO ("echo at " << peak << " in a " << kFrames << "-frame block, prepare block " << kBlock);
    CHECK (peak == 2400);
}

TEST_CASE ("a delay does not run away at the corpus's own numbers", "[fx][fx-loop]")
{
    // **Found by wiring the rack into the plugin (ticket 60).** The corpus's own `FXDelay` slots store the
    // filter's `kParamBW` between 0.75 and 8.25, and this effect reads that as the loop filter's Q while
    // the loop multiplies the filtered tap by `kParamFeedback` — so `feedback x Q` was **1.5** at
    // `ARP - Aardvark` (BW 3.28, feedback 47 %) and **3.1** at `ARP - Bell Arperium` (BW 5.33, 58 %). A loop
    // above unity does not ring, it grows: measured through the plugin with the ceiling off — the way the
    // fidelity layer renders, the golden being the reference's unlimited output — Aardvark reached
    // **2.3e9** and Bell Arperium 3.7e9 by the end of a 2 s render, and Bell Arperium's oscillators are a
    // deferred engine, so the second one grew from numerical noise with no input at all.
    //
    // The two cases below are the same measurement at the two levels that matter: the effect in isolation,
    // and the rack it lives in — because the runaway needed a *chain* to become audible in a render.
    constexpr double kSampleRate = 48000.0;

    const auto setCorpusNumbers = [] (FxEffect& delay, double bw, double feedback)
    {
        set (delay, "kParamTimeL", 0.04933726956749931);
        set (delay, "kParamTimeR", 0.04933726956749931);
        set (delay, "kParamLink", 1.0);
        set (delay, "kParamFeedback", feedback);
        set (delay, "kParamFreq", 2301.2985114436656);
        set (delay, "kParamBW", bw);
        set (delay, "kParamWet", 100.0);
    };

    for (const auto& [bw, feedback] : { std::pair { 3.2793322652578354, 47.19850718975067 },
                                        std::pair { 5.329449445009232, 58.13401937484741 } })
    {
        auto delay = make (FxClass::Delay, kSampleRate);
        setCorpusNumbers (*delay, bw, feedback);

        // A one-sample impulse, then four seconds of silence. An impulse is the worst legal input for a
        // loop: every frequency at once, including the resonance.
        const auto seconds = 4.0;
        std::vector<float> left (static_cast<std::size_t> (kSampleRate * seconds), 0.0f);
        std::vector<float> right (left.size(), 0.0f);
        left[0] = 1.0f;
        right[0] = 1.0f;

        for (std::size_t start = 0; start < left.size(); start += static_cast<std::size_t> (kBlock))
        {
            const int frames = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (kBlock),
                                                                        left.size() - start));
            delay->process (left.data() + start, right.data() + start, frames);
        }

        const auto peakIn = [&left] (double from, double to)
        {
            double peak = 0.0;

            for (auto i = static_cast<std::size_t> (from * kSampleRate);
                 i < static_cast<std::size_t> (to * kSampleRate) && i < left.size(); ++i)
                peak = std::max (peak, std::abs (static_cast<double> (left[i])));

            return peak;
        };

        const auto first = peakIn (0.5, 1.5);
        const auto last = peakIn (3.0, 4.0);

        UNSCOPED_INFO ("BW " << bw << ", feedback " << feedback << " %: peak " << first << " in 0.5-1.5 s, "
                             << last << " in 3-4 s");

        // Bounded, and decaying: the tail of the second half is below the first, and neither is a runaway.
        CHECK (std::isfinite (first));
        CHECK (first < 4.0);
        CHECK (last < first);
        CHECK (last < 1.0);
    }

    // And through the rack, with the corpus's chain around it — a delay *after* a distortion, which is
    // what Aardvark's rack does two slots earlier.
    FxRack rack;
    rack.prepare (kSampleRate, kBlock);

    FxSlot distortion;
    distortion.cls = FxClass::Distortion;
    distortion.numbers = { { "kParamDrive", 36.74789369106293 }, { "kParamWet", 56.083983182907104 } };
    distortion.texts = { { "kParamMode", "kOverdrive" } };

    FxSlot delay;
    delay.cls = FxClass::Delay;
    delay.numbers = { { "kParamTimeL", 0.04933726956749931 },
                      { "kParamTimeR", 0.04933726956749931 },
                      { "kParamLink", 1.0 },
                      { "kParamFeedback", 47.19850718975067 },
                      { "kParamFreq", 2301.2985114436656 },
                      { "kParamBW", 3.2793322652578354 },
                      { "kParamWet", 22.105523943901062 } };

    rack.setSlots ({ distortion, delay });
    CHECK (rack.unresolved().empty());

    std::vector<float> left (static_cast<std::size_t> (kSampleRate * 4.0), 0.0f);
    std::vector<float> right (left.size(), 0.0f);

    for (std::size_t i = 0; i < static_cast<std::size_t> (kSampleRate * 0.5); ++i)
        left[i] = right[i] = 0.5f * static_cast<float> (std::sin (kTwoPi * 220.0 * static_cast<double> (i)
                                                                 / kSampleRate));

    for (std::size_t start = 0; start < left.size(); start += static_cast<std::size_t> (kBlock))
    {
        const int frames = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (kBlock),
                                                                    left.size() - start));
        rack.process (left.data() + start, right.data() + start, frames);
    }

    double lastSecondPeak = 0.0;

    for (auto i = static_cast<std::size_t> (kSampleRate * 3.0); i < left.size(); ++i)
        lastSecondPeak = std::max (lastSecondPeak, std::abs (static_cast<double> (left[i])));

    UNSCOPED_INFO ("rack tail, peak in the last second: " << lastSecondPeak);
    CHECK (std::isfinite (lastSecondPeak));
    CHECK (lastSecondPeak < 2.0);
}

TEST_CASE ("the level knob is the squared curve measured against the reference", "[fx]")
{
    // Ticket 62 swept `kParamLevelOut` against the reference's own audio (a preset differing only in
    // this leaf, rendered on both sides) and fitted `gain = 4·s²`: `0.25 → 0.25`, `0.5 → 1.0` (which is
    // why a slot that omits the leaf is unity), `1.0 → 4.0`. Three stored values, three measured gains —
    // an affine reading of the leaf would fail the middle one and a linear one the ends.
    const std::pair<double, double> points[] = { { 0.25, 0.25 }, { 0.5, 1.0 }, { 0.75, 2.25 }, { 1.0, 4.0 } };

    for (const auto& [stored, gain] : points)
    {
        auto effect = make (FxClass::Utility);
        set (*effect, "kParamWet", 100.0);
        set (*effect, "kParamLevelOut", stored);
        UNSCOPED_INFO ("stored " << stored << " -> gain " << gain);

        CHECK (std::fabs (gainAt (*effect, 1000.0, 48000.0) - gain) < 0.01 * gain);
    }

    // And the leaf really is the gain rather than a correlate of it: zero is silence.
    auto silent = make (FxClass::Utility);
    set (*silent, "kParamWet", 100.0);
    set (*silent, "kParamLevelOut", 0.0);
    CHECK (gainAt (*silent, 1000.0, 48000.0) < 1.0e-6);
}
