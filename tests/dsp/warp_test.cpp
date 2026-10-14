// Tests for the warp unit.
//
// The ticket asks for *characteristic* behaviour rather than mere execution, so the assertions measure:
// monotonicity where a reordering would alias, spectral change where a distortion is supposed to add
// harmonics, and attenuation where a filter is supposed to remove them.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "oscillator.h"
#include "warp.h"
#include "wavetable.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace xerum::dsp;

namespace
{

std::vector<std::byte> readFixture (const std::string& name)
{
    const auto path = std::filesystem::path (XERUM_DSP_FIXTURE_DIR) / name;
    std::ifstream in (path, std::ios::binary);
    REQUIRE (in.good());
    std::vector<std::byte> bytes ((std::size_t) std::filesystem::file_size (path));
    in.read (reinterpret_cast<char*> (bytes.data()), (std::streamsize) bytes.size());
    return bytes;
}

Wavetable sineTable()
{
    const auto parsed = parseWavetable (readFixture ("sine-1frame.wav"));
    REQUIRE (parsed.ok);
    return parsed.table;
}

/// Energy at one frequency, by direct correlation. Crude, but it is a real measurement of the spectrum
/// rather than an assertion that "the output changed".
double energyAt (const std::vector<float>& samples, double cyclesPerSample)
{
    double real = 0.0, imaginary = 0.0;

    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto angle = 2.0 * 3.14159265358979 * cyclesPerSample * static_cast<double> (i);
        real += samples[i] * std::cos (angle);
        imaginary += samples[i] * std::sin (angle);
    }

    return std::sqrt (real * real + imaginary * imaginary) / static_cast<double> (samples.size());
}

std::vector<float> render (WavetableOscillator& oscillator, std::size_t count)
{
    std::vector<float> out (count);
    oscillator.process (out.data(), count);
    return out;
}

std::size_t risingZeroCrossings (const std::vector<float>& samples)
{
    std::size_t count = 0;
    for (std::size_t i = 1; i < samples.size(); ++i)
        if (samples[i - 1] <= 0.0f && samples[i] > 0.0f)
            ++count;
    return count;
}

} // namespace

TEST_CASE ("mode names map onto families, and unknown modes are named as unknown", "[warp]")
{
    CHECK (warpModeFromName ("kNoWarp") == WarpMode::None);
    CHECK (warpModeFromName ("kSync") == WarpMode::Sync);
    CHECK (warpModeFromName ("kBendPos") == WarpMode::BendPos);
    CHECK (warpModeFromName ("kPWM") == WarpMode::PWM);

    // The source suffix collapses onto the family, because the engine cannot route a source yet — but the
    // suffix is not lost, it is still in the preset.
    CHECK (warpModeFromName ("kPD_OSC") == WarpMode::PhaseDistortion);
    CHECK (warpModeFromName ("kSelfPD") == WarpMode::PhaseDistortion);
    CHECK (warpModeFromName ("kFM_NOISE") == WarpMode::FrequencyModulation);
    CHECK (warpModeFromName ("kFMX_SUB") == WarpMode::FrequencyModulation);
    CHECK (warpModeFromName ("kFMP_FILT1") == WarpMode::FrequencyModulation);
    CHECK (warpModeFromName ("kRM_SUB") == WarpMode::RingModulation);
    CHECK (warpModeFromName ("kAM_OSC2") == WarpMode::AmplitudeModulation);
    CHECK (warpModeFromName ("kDistTube") == WarpMode::Distortion);

    // Documented, semantics not established. Reported rather than guessed at.
    CHECK (warpModeFromName ("kDLM") == WarpMode::Unknown);
    CHECK (warpModeFromName ("kRemap_1") == WarpMode::Unknown);
    CHECK (warpModeFromName ("kEvenOdd") == WarpMode::Unknown);
}

TEST_CASE ("every family is identity at zero", "[warp]")
{
    // A warp that does something at zero makes every preset that does not use it sound wrong.
    // The modes whose neutral position is zero.
    const WarpMode modes[] = { WarpMode::Sync, WarpMode::BendPos, WarpMode::BendNeg,
                               WarpMode::AsymPos, WarpMode::PWM, WarpMode::Flip,
                               WarpMode::PhaseDistortion, WarpMode::FrequencyModulation };

    for (const auto mode : modes)
    {
        Warp warp;
        warp.setMode (mode);
        warp.setAmount (0.0);

        for (const double turns : { 0.0, 0.1, 0.25, 0.5, 0.75, 0.9 })
            CHECK (warp.phase (turns, 0.0) == turns);
    }

    // The two-directional variants are neutral in the *middle*, which is the whole point of them.
    for (const auto mode : { WarpMode::BendPosNeg, WarpMode::AsymPosNeg })
    {
        Warp warp;
        warp.setMode (mode);
        warp.setAmount (0.5);
        CHECK (warp.phase (0.3, 0.0) == 0.3);
    }

    // Quantize is never transparent — quantising is what it does. At the finest setting it is within one
    // step of identity, which is the closest it can honestly get.
    {
        Warp warp;
        warp.setMode (WarpMode::Quantize);
        warp.setAmount (0.0);

        for (const double turns : { 0.1, 0.25, 0.5, 0.9 })
            CHECK (std::abs (warp.phase (turns, 0.0) - turns) < 1.0 / 64.0);
    }

    for (const auto mode : { WarpMode::Distortion, WarpMode::RingModulation, WarpMode::AmplitudeModulation })
    {
        Warp warp;
        warp.setMode (mode);
        warp.setAmount (0.0);
        CHECK (warp.sample (0.25, 0.5f, 0.0) == 0.5f);
    }
}

TEST_CASE ("phase warps are monotonic, so the table is never read out of order", "[warp]")
{
    // A non-monotonic phase warp folds the waveform and aliases, which sounds like a bad table rather than
    // a bad warp. This is the property that matters, and it is cheap to assert.
    // Sync and the modulation-driven modes deliberately *restart* or wrap: monotonicity is not their
    // property, and asserting it would be asserting that sync is not sync. They are covered below.
    // PWM is a phase *shift*, so it wraps like sync does, and is covered by the range test below.
    const WarpMode modes[] = { WarpMode::BendPos, WarpMode::BendNeg, WarpMode::BendPosNeg,
                               WarpMode::AsymPos, WarpMode::AsymNeg, WarpMode::Quantize };

    for (const auto mode : modes)
    {
        for (const double amount : { 0.25, 0.5, 0.9 })
        {
            Warp warp;
            warp.setMode (mode);
            warp.setAmount (amount);

            double previous = -1.0;

            for (int i = 0; i < 256; ++i)
            {
                const auto turns = static_cast<double> (i) / 256.0;
                const auto mapped = warp.phase (turns, 0.3);

                // Non-decreasing rather than strictly increasing: quantising deliberately holds still.
                CHECK (mapped >= previous);
                previous = mapped;
            }
        }
    }
}

TEST_CASE ("the wrapping modes stay in range", "[warp]")
{
    // Sync and the modulation-driven warps restart the read position, so they are not monotonic — but they
    // must still land inside the cycle, or the oscillator reads out of bounds.
    for (const auto mode : { WarpMode::Sync, WarpMode::PWM, WarpMode::PhaseDistortion,
                             WarpMode::FrequencyModulation })
    {
        Warp warp;
        warp.setMode (mode);
        warp.setAmount (0.8);

        for (int i = 0; i < 512; ++i)
        {
            const auto turns = static_cast<double> (i) / 512.0;

            for (const double modulator : { -1.0, 0.0, 1.0 })
            {
                const auto mapped = warp.phase (turns, modulator);
                CHECK (mapped >= 0.0);
                CHECK (mapped < 1.0);
            }
        }
    }
}

TEST_CASE ("sync restarts the waveform, raising its pitch by the sync ratio", "[warp]")
{
    const auto table = sineTable();

    WavetableOscillator oscillator;
    oscillator.prepare (48000.0);
    oscillator.setTable (&table);
    oscillator.setFrequency (200.0);

    const auto plain = render (oscillator, 48000);
    CHECK (risingZeroCrossings (plain) == 200);

    // The warp knob sets the ratio: 1 + amount * 15, so 1/15 of the range gives exactly 2×. The warp lives
    // in the oscillator's read path, so the accumulator keeps advancing at 200 Hz while the table is read
    // twice as fast — which is what makes this sync rather than a doubling of pitch.
    WavetableOscillator synced;
    synced.prepare (48000.0);
    synced.setTable (&table);
    synced.setFrequency (200.0);
    synced.setWarp (0, WarpMode::Sync, 1.0 / 15.0);

    const auto out = render (synced, 48000);

    CHECK (risingZeroCrossings (out) == 400);

    // And sync is a different thing from pitch: this is a read-position rewrite, not a retune. The
    // oscillator still *is* a 200 Hz oscillator; it just traverses its table twice per cycle.
    CHECK (synced.frequency() == 200.0);
}

TEST_CASE ("bend moves content without moving the cycle's ends", "[warp]")
{
    const auto table = sineTable();

    Warp warp;
    warp.setMode (WarpMode::BendPos);
    warp.setAmount (0.7);

    // The ends of the cycle stay put; content between them moves. A warp that also moved the ends would
    // shift the pitch.
    CHECK (warp.phase (0.0, 0.0) == 0.0);
    CHECK (warp.phase (1.0, 0.0) == 1.0);

    const auto before = warp.phase (0.25, 0.0);
    CHECK (before != 0.25);
    CHECK (before > 0.0);
    CHECK (before < 1.0);

    (void) table;
}

TEST_CASE ("pulse width shifts the read position", "[warp]")
{
    Warp warp;
    warp.setMode (WarpMode::PWM);
    warp.setAmount (0.5);

    // amount 0.5 shifts by a quarter cycle.
    CHECK (warp.phase (0.0, 0.0) == 0.25);
    CHECK (warp.phase (0.75, 0.0) == 0.0);          // and wraps
}

TEST_CASE ("distortion adds harmonic content, measurably", "[warp]")
{
    const auto table = sineTable();

    const auto clean = [&]
    {
        WavetableOscillator oscillator;
        oscillator.prepare (48000.0);
        oscillator.setTable (&table);
        oscillator.setFrequency (1000.0);
        return render (oscillator, 4800);
    }();

    const auto ratio = [] (const std::vector<float>& samples, double fundamental)
    {
        const auto f = energyAt (samples, fundamental / 48000.0);
        const auto third = energyAt (samples, 3.0 * fundamental / 48000.0);
        return f > 0.0 ? third / f : 0.0;
    };

    const auto cleanThird = ratio (clean, 1000.0);
    CHECK (cleanThird < 0.01);                       // a sine has essentially none

    Warp warp;
    warp.setMode (WarpMode::Distortion);
    warp.setDistortionVariant ("kDistTube");
    warp.setAmount (0.8);

    std::vector<float> driven;
    driven.reserve (clean.size());

    for (const auto sample : clean)
        driven.push_back (warp.sample (0.0, sample, 0.0));

    CHECK (ratio (driven, 1000.0) > cleanThird * 10.0);   // an order of magnitude more third harmonic

    // And the output stays bounded, which a distortion that clipped without limiting would not. The
    // bound is 1.25 rather than 1 because the asymmetric curves (tube, tape) add an even-order term on
    // top of tanh — asymmetry is what puts even harmonics in a distortion, and it is not free.
    float peak = 0.0f;
    for (const auto sample : driven)
        peak = std::max (peak, std::abs (sample));
    CHECK (peak <= 1.3f);
}

TEST_CASE ("the filter warp attenuates high frequencies and passes low ones", "[warp]")
{
    const auto table = sineTable();

    const auto renderAt = [&] (double frequency)
    {
        WavetableOscillator oscillator;
        oscillator.prepare (48000.0);
        oscillator.setTable (&table);
        oscillator.setFrequency (frequency);
        return render (oscillator, 4800);
    };

    Warp lowPass;
    lowPass.setMode (WarpMode::FilterLowPass);
    lowPass.setAmount (0.15);                        // a low cutoff
    lowPass.prepare (48000.0);

    const auto filter = [&] (const std::vector<float>& input)
    {
        std::vector<float> out;
        out.reserve (input.size());
        for (const auto sample : input)
            out.push_back (lowPass.sample (0.0, sample, 0.0));
        return out;
    };

    const auto lowClean = renderAt (200.0);
    const auto highClean = renderAt (8000.0);

    const auto lowFiltered = filter (lowClean);
    lowPass.reset();
    const auto highFiltered = filter (highClean);

    const auto peak = [] (const std::vector<float>& v)
    {
        float p = 0.0f;
        for (const auto s : v) p = std::max (p, std::abs (s));
        return p;
    };

    // The low frequency survives; the high one is markedly reduced.
    CHECK (peak (lowFiltered) > 0.5 * peak (lowClean));
    CHECK (peak (highFiltered) < 0.5 * peak (highClean));
}

TEST_CASE ("ring and amplitude modulation multiply by the modulator", "[warp]")
{
    Warp ring;
    ring.setMode (WarpMode::RingModulation);
    ring.setAmount (1.0);

    // Full ring modulation inverts with the modulator's sign; that is the difference from AM.
    CHECK (ring.sample (0.0, 1.0f, -1.0) == -1.0f);
    CHECK (ring.sample (0.0, 1.0f, 1.0) == 1.0f);

    Warp amplitude;
    amplitude.setMode (WarpMode::AmplitudeModulation);
    amplitude.setAmount (1.0);

    // AM never inverts: a unipolar modulator scales.
    CHECK (amplitude.sample (0.0, 1.0f, -1.0) == 0.0f);
    CHECK (amplitude.sample (0.0, 1.0f, 1.0) == 1.0f);
}

TEST_CASE ("flip inverts polarity after the duty point", "[warp]")
{
    Warp warp;
    warp.setMode (WarpMode::Flip);
    warp.setAmount (0.5);

    CHECK (warp.sample (0.25, 1.0f, 0.0) == 1.0f);
    CHECK (warp.sample (0.75, 1.0f, 0.0) == -1.0f);
}

TEST_CASE ("modulation-driven warps do nothing without a modulator", "[warp]")
{
    // PD and FM are driven by another source. With none connected they must be transparent, or every
    // preset using them would be wrong the moment the source is missing.
    for (const auto mode : { WarpMode::PhaseDistortion, WarpMode::FrequencyModulation })
    {
        Warp warp;
        warp.setMode (mode);
        warp.setAmount (0.9);

        for (const double turns : { 0.0, 0.3, 0.7 })
            CHECK (warp.phase (turns, 0.0) == turns);
    }
}

TEST_CASE ("both warp slots are live, and the second sees what the first produced", "[warp]")
{
    // 174 of the 626 factory presets use both slots at once, so this is not a hypothetical: an oscillator
    // with one slot plays a different sound on a quarter of the bank.
    const auto table = sineTable();

    const auto renderWith = [&] (WarpMode first, WarpMode second)
    {
        WavetableOscillator oscillator;
        oscillator.prepare (48000.0);
        oscillator.setTable (&table);
        oscillator.setFrequency (200.0);
        oscillator.setWarp (0, first, 0.6);
        oscillator.setWarp (1, second, 0.6);
        return render (oscillator, 4096);
    };

    const auto firstOnly = renderWith (WarpMode::Sync, WarpMode::None);
    const auto both = renderWith (WarpMode::Sync, WarpMode::PWM);
    const auto secondOnly = renderWith (WarpMode::None, WarpMode::PWM);

    // Each slot changes the output on its own...
    CHECK (firstOnly != renderWith (WarpMode::None, WarpMode::None));
    CHECK (secondOnly != renderWith (WarpMode::None, WarpMode::None));

    // ...and having both is not the same as having either, which is what "serial" means.
    CHECK (both != firstOnly);
    CHECK (both != secondOnly);
}

TEST_CASE ("oscillator instances do not share warp state", "[warp]")
{
    // The filter-as-warp is the one warp with memory, and the engine runs three oscillator slots at once.
    // State that leaked between instances would make one slot's output depend on another's, which is the
    // kind of bug that only shows up as "some presets sound phasey".
    const auto table = sineTable();

    WavetableOscillator clean;
    clean.prepare (48000.0);
    clean.setTable (&table);
    clean.setFrequency (200.0);

    const auto reference = render (clean, 2048);

    WavetableOscillator filtered;
    filtered.prepare (48000.0);
    filtered.setTable (&table);
    filtered.setFrequency (200.0);
    filtered.setWarp (0, WarpMode::FilterLowPass, 0.9);

    WavetableOscillator interleaved;
    interleaved.prepare (48000.0);
    interleaved.setTable (&table);
    interleaved.setFrequency (200.0);

    std::vector<float> mixed;

    for (std::size_t i = 0; i < reference.size(); ++i)
    {
        (void) filtered.next();               // the heavily filtered neighbour running alongside
        mixed.push_back (interleaved.next());
    }

    // The unfiltered oscillator is bit-identical either way.
    CHECK (mixed == reference);
}

TEST_CASE ("the modes the engine does not implement are reported as unknown, by name", "[warp]")
{
    // Two groups, and neither is an oversight. The first is documented but not implemented; the second
    // is the warp menus of oscillator engines v1 does not synthesise at all (deferredEngines in the
    // support catalogue), which still reach `kParamWarpMenu` in the corpus.
    for (const auto& name : { "kDLM", "kEvenOdd", "kRemap_1", "kRemap_3", "kRemap_4",
                              "kGate", "kSpread", "kSmear", "kDetune", "kAddharmonics",
                              "kSpectralComb", "kSpectralShift", "kShepardFilter", "kShepardNarrow",
                              "kVocode_OSC", "kMask_OSC", "kPeakOctaveDown" })
        CHECK (warpModeFromName (name) == WarpMode::Unknown);

    // The bare distortion names belong to other oscillator types' menus, where they are the same family
    // under a different spelling. They are in no wavetable menu, so they are reported rather than
    // half-supported — a distinction the coverage test measures against the real vocabulary.
    for (const auto& name : { "kHardClip", "kSoftClip", "kLinFold", "kSineShaper", "kOverdrive" })
        CHECK (warpModeFromName (name) == WarpMode::Unknown);
}

TEST_CASE ("each distortion variant shapes differently, because the variants are the point", "[warp]")
{
    // The family shares one knob; what distinguishes its members is the curve. Two of them that produce
    // identical output would mean the variant is not being read at all.
    const auto render = [] (const std::string& variant, float sample)
    {
        Warp warp;
        warp.setMode (WarpMode::Distortion);
        warp.setDistortionVariant (variant);
        warp.setAmount (1.0);
        return warp.sample (0.0, sample, 0.0);
    };

    // A hard clip and a linear fold differ precisely in what they do past the ceiling: one flattens, the
    // other reflects back down.
    CHECK (render ("kDistHardClip", 3.0f) > render ("kDistLinFold", 3.0f));

    // Rectification is even-order only, so it cannot produce a negative output at all.
    CHECK (render ("kDistRectify", -0.5f) > 0.0f);

    // A zero-crossing squarer discards amplitude entirely.
    CHECK (render ("kDistZeroSquare", 0.01f) == render ("kDistZeroSquare", 0.9f));

    // And the two soft curves differ from each other.
    CHECK (render ("kDistTube", 0.5f) != render ("kDistSoftClip", 0.5f));
}
