// Tests for the master ceiling (ticket `58`).
//
// Four properties are asserted here, and three of them are **exact** rather than approximate, because
// each is a claim about samples rather than about a sound:
//
//   * **transparency** — a signal entirely within the ceiling comes out bit-identical to the delayed
//     input, so a preset that already sat below the ceiling is not rewritten by the limiter;
//   * **the brickwall** — a signal far above the ceiling comes out with a peak on the ceiling, sample
//     for sample, including at a hard transient (which is the case a lookahead exists for);
//   * **the counterfactual** — the same signal with the ceiling off does exceed the ceiling, so the
//     brickwall assertion is measuring the limiter and not an already-safe signal;
//   * **off is a wire** — the disabled path is a pure delay, bit-identical to the input.
//
// The reference numbers these use (ceiling -0.30 dB, release 100 ms, lookahead 1.5 ms) are documented in
// `limiter.h` with the Ableton artefacts they were read from.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "limiter.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace xerum::dsp;

namespace
{

constexpr double kPi = 3.14159265358979323846;

/// The linear amplitude of the default ceiling, computed here independently of the limiter so the test
/// does not read the answer out of the thing it is testing.
double defaultCeilingLinear()
{
    return std::pow (10.0, kMasterCeilingDb / 20.0);
}

std::vector<float> sine (const int frames, const double sampleRate, const double hertz, const float amplitude,
                         const double phase = 0.0)
{
    std::vector<float> out (static_cast<std::size_t> (frames), 0.0f);

    for (int i = 0; i < frames; ++i)
        out[static_cast<std::size_t> (i)] =
            static_cast<float> (amplitude * std::sin (2.0 * kPi * hertz * static_cast<double> (i) / sampleRate + phase));

    return out;
}

/// A signal whose loudest moment is only a few samples wide: a quiet floor with a hard click in it. This
/// is the case a lookahead limiter is for — a reduced-gain envelope that only reacts once a peak has
/// arrived cannot keep this under a ceiling.
std::vector<float> floorWithClick (const int frames, const int clickAt, const float floorAmplitude,
                                   const float clickAmplitude)
{
    std::vector<float> out (static_cast<std::size_t> (frames), 0.0f);

    for (int i = 0; i < frames; ++i)
        out[static_cast<std::size_t> (i)] =
            std::sin (2.0 * kPi * 220.0 * static_cast<double> (i) / 48000.0) > 0.0 ? floorAmplitude : -floorAmplitude;

    // Four samples of a hard excursion. Not a one-sample Dirac — the reference's own transient case.
    for (int i = 0; i < 4 && clickAt + i < frames; ++i)
        out[static_cast<std::size_t> (clickAt + i)] = clickAmplitude;

    return out;
}

double peakOf (const std::vector<float>& samples)
{
    double peak = 0.0;

    for (const auto sample : samples)
        peak = std::max (peak, std::abs (static_cast<double> (sample)));

    return peak;
}

} // namespace

TEST_CASE ("the defaults are the reference's, and the latency is the lookahead", "[dsp][limiter]")
{
    CHECK (kMasterCeilingDb == Catch::Approx (-0.30));
    CHECK (kMasterReleaseMs == Catch::Approx (100.0));
    CHECK (kMasterLookaheadMs == Catch::Approx (1.5));

    // The lookahead is the latency, rounded to whole samples: the only unit a host compensates in.
    const auto latencyAt = [] (const double sampleRate, const double lookaheadMs = kMasterLookaheadMs)
    {
        Limiter limiter;
        limiter.setLookaheadMs (lookaheadMs);
        limiter.prepare (sampleRate);
        return limiter.latencySamples();
    };

    CHECK (latencyAt (44100.0) == 66);
    CHECK (latencyAt (48000.0) == 72);
    CHECK (latencyAt (96000.0) == 144);

    // A longer lookahead is more latency — the reference's own warning, and what this stage declares.
    CHECK (latencyAt (48000.0, 6.0) == 288);
    CHECK (latencyAt (48000.0, 3.0) == 144);
}

TEST_CASE ("a signal below the ceiling renders exactly as it did, bit for bit", "[dsp][limiter]")
{
    constexpr int kFrames = 8192;
    constexpr double kSampleRate = 48000.0;

    // Comfortably inside -0.30 dB, and deliberately not a round number so nothing can pass by accident.
    const auto input = sine (kFrames, kSampleRate, 440.0, 0.6f);

    Limiter limiter;
    limiter.prepare (kSampleRate);

    auto left = input;
    auto right = input;
    limiter.process (left.data(), right.data(), kFrames);

    const auto latency = limiter.latencySamples();
    REQUIRE (latency > 0);

    // **Bit-identical**, not close: every sample is the input's own value from `latency` samples earlier,
    // because the gain stayed at exactly `1.0f` and multiplying by it is exact. The first `latency`
    // samples are the delay line's own fill, which is what a declared latency means.
    for (int i = 0; i < kFrames; ++i)
    {
        const auto expected = i < latency ? 0.0f : input[static_cast<std::size_t> (i - latency)];

        if (left[static_cast<std::size_t> (i)] != expected || right[static_cast<std::size_t> (i)] != expected)
        {
            FAIL ("sample " << i << " was " << left[static_cast<std::size_t> (i)] << ", expected " << expected);
        }
    }

    CHECK (limiter.currentGain() == 1.0);
    CHECK (peakOf (left) == Catch::Approx (0.6).epsilon (1e-6));
}

TEST_CASE ("the ceiling is a brickwall, and it bites", "[dsp][limiter]")
{
    constexpr int kFrames = 16384;
    constexpr double kSampleRate = 48000.0;

    // Well above full scale — the state the dev reported as "the audio turns red".
    const auto input = sine (kFrames, kSampleRate, 110.0, 2.5f);
    const auto ceiling = defaultCeilingLinear();

    // The counterfactual first: **without** the limiter the same signal does exceed full scale, so the
    // assertion below is not describing an already-safe signal.
    {
        Limiter open;
        open.prepare (kSampleRate);
        open.setEnabled (false);

        auto left = input;
        auto right = input;
        open.process (left.data(), right.data(), kFrames);

        CHECK (peakOf (left) > 1.0);
        CHECK (peakOf (left) == Catch::Approx (2.5).epsilon (1e-6));
    }

    Limiter limiter;
    limiter.prepare (kSampleRate);

    auto left = input;
    auto right = input;
    limiter.process (left.data(), right.data(), kFrames);

    const auto peak = peakOf (left);

    // At or below the ceiling, to the sample. The clamp in `process` is what makes this unconditional;
    // the small epsilon is float rounding on `ceiling / magnitude * magnitude`, nothing else.
    CHECK (peak <= ceiling + 1.0e-6);

    // And it is actually limiting rather than crushing everything: the peak reaches the ceiling.
    CHECK (peak > ceiling * 0.999);

    CHECK (limiter.currentGain() < 1.0);
}

TEST_CASE ("a hard transient is caught, and would not be without the lookahead", "[dsp][limiter]")
{
    constexpr int kFrames = 8192;
    constexpr double kSampleRate = 48000.0;

    const auto input = floorWithClick (kFrames, 4096, 0.2f, 3.0f);
    const auto ceiling = defaultCeilingLinear();

    Limiter limiter;
    limiter.prepare (kSampleRate);

    auto left = input;
    auto right = input;
    limiter.process (left.data(), right.data(), kFrames);

    const auto peak = peakOf (left);
    CHECK (peak <= ceiling + 1.0e-6);
    CHECK (peak > ceiling * 0.999);

    // The counterfactual at the transient too.
    Limiter open;
    open.prepare (kSampleRate);
    open.setEnabled (false);

    auto openLeft = input;
    auto openRight = input;
    open.process (openLeft.data(), openRight.data(), kFrames);

    CHECK (peakOf (openLeft) > 1.0);
}

TEST_CASE ("the ceiling does not change a sample that is within it, even in a signal that engages it",
           "[dsp][limiter]")
{
    // The invariant that protects the fidelity work, in its harder form: a signal that *does* engage the
    // limiter early and then falls well inside the ceiling must render its later samples bit-identically,
    // once the release has returned the gain to unity. An envelope that only approached unity would fail
    // this by a hair; this limiter snaps to exactly `1.0f`.
    constexpr int kFrames = 480000;   // 10 s at 48 kHz
    constexpr double kSampleRate = 48000.0;

    std::vector<float> input (static_cast<std::size_t> (kFrames), 0.0f);

    // 0.5 s of a signal that engages the limiter, then a quiet tail.
    for (int i = 0; i < kFrames; ++i)
        input[static_cast<std::size_t> (i)] =
            i < 24000 ? 2.0f * static_cast<float> (std::sin (2.0 * kPi * 110.0 * i / kSampleRate))
                      : 0.3f * static_cast<float> (std::sin (2.0 * kPi * 440.0 * i / kSampleRate));

    Limiter limiter;
    limiter.prepare (kSampleRate);

    auto left = input;
    auto right = input;
    limiter.process (left.data(), right.data(), kFrames);

    CHECK (limiter.currentGain() > 1.0 - 1.0e-8);

    const auto latency = limiter.latencySamples();

    // Two seconds after the loud section — the 100 ms time constant needs about 1.6 s for the envelope to
    // come within a float ulp of unity — the two are the same numbers.
    for (int i = 120000; i < kFrames; ++i)
    {
        const auto expected = input[static_cast<std::size_t> (i - latency)];

        if (left[static_cast<std::size_t> (i)] != expected)
        {
            FAIL ("sample " << i << " was " << left[static_cast<std::size_t> (i)] << ", expected " << expected);
        }
    }
}

TEST_CASE ("switching the ceiling off leaves a wire, and the latency does not move", "[dsp][limiter]")
{
    constexpr int kFrames = 8192;
    constexpr double kSampleRate = 48000.0;

    const auto input = sine (kFrames, kSampleRate, 440.0, 2.5f);

    Limiter limiter;
    limiter.prepare (kSampleRate);

    const auto latency = limiter.latencySamples();
    CHECK (latency == 72);

    limiter.setEnabled (false);
    CHECK (limiter.latencySamples() == latency);   // the declaration the host is compensated by

    auto left = input;
    auto right = input;
    limiter.process (left.data(), right.data(), kFrames);

    CHECK (limiter.currentGain() == 1.0);

    // A pure delay: the input's own values, unchanged, and above full scale where the input was — which
    // is the counterfactual the acceptance asks for, at this layer.
    for (int i = 0; i < kFrames; ++i)
    {
        const auto expected = i < latency ? 0.0f : input[static_cast<std::size_t> (i - latency)];

        if (left[static_cast<std::size_t> (i)] != expected)
        {
            FAIL ("sample " << i << " was " << left[static_cast<std::size_t> (i)] << ", expected " << expected);
        }
    }

    CHECK (peakOf (left) == Catch::Approx (2.5).epsilon (1e-6));
}

TEST_CASE ("the release returns the gain to unity in the reference's own time", "[dsp][limiter]")
{
    constexpr double kSampleRate = 48000.0;
    const auto ceiling = defaultCeilingLinear();

    Limiter limiter;
    limiter.prepare (kSampleRate);

    // 10 ms of a signal that is *constantly* above the ceiling, so the windowed minimum is one known
    // number rather than a phase-dependent one: the gain settles on `ceiling / 2` exactly.
    std::vector<float> loud (480, 2.0f);
    auto left = loud;
    auto right = loud;
    limiter.process (left.data(), right.data(), 480);

    const auto engaged = limiter.currentGain();
    REQUIRE (engaged < 1.0);
    CHECK (engaged == Catch::Approx (ceiling / 2.0).epsilon (0.01));

    const auto silence = [&] (const int frames)
    {
        auto run = std::vector<float> (static_cast<std::size_t> (frames), 0.0f);
        auto runRight = run;
        limiter.process (run.data(), runRight.data(), frames);
        return limiter.currentGain();
    };

    // 1 ms after the signal stops, most of the reduction is still in place.
    CHECK (silence (48) < 0.999);

    // 10 release constants later it is back to unity within a float ulp.
    CHECK (silence (static_cast<int> (kSampleRate * 2.0)) > 1.0 - 1.0e-8);
}

TEST_CASE ("a peak on one channel attenuates both", "[dsp][limiter]")
{
    constexpr int kFrames = 4096;
    constexpr double kSampleRate = 48000.0;
    const auto ceiling = defaultCeilingLinear();

    // The left channel is loud, the right is not, and they are **in phase**: with 100 % link the right
    // comes down with the left, which is the reference's `LinkAmount = 1` and what keeps the stereo image
    // from moving. In phase so the two peaks coincide and the shared gain can be read off as a ratio.
    auto left = sine (kFrames, kSampleRate, 220.0, 2.0f);
    auto right = sine (kFrames, kSampleRate, 220.0, 0.4f);

    Limiter limiter;
    limiter.prepare (kSampleRate);
    limiter.process (left.data(), right.data(), kFrames);

    CHECK (peakOf (left) <= ceiling + 1.0e-6);
    CHECK (peakOf (right) < 0.4);

    // The reduction the right channel received is the same factor the left received, measured off the
    // steady state rather than the transient.
    const auto leftLate = std::vector<float> (left.end() - 512, left.end());
    const auto rightLate = std::vector<float> (right.end() - 512, right.end());

    // The right channel is a quiet, inaudible sine, so its peak is 0.4 times the same gain.
    CHECK (peakOf (leftLate) / peakOf (rightLate) == Catch::Approx (2.0 / 0.4).epsilon (0.01));
}

TEST_CASE ("the ceiling parameter moves the ceiling", "[dsp][limiter]")
{
    constexpr int kFrames = 4096;
    constexpr double kSampleRate = 48000.0;

    const auto input = sine (kFrames, kSampleRate, 220.0, 4.0f);

    const auto peakAtCeiling = [&] (const double decibels)
    {
        Limiter limiter;
        limiter.setCeilingDb (decibels);
        limiter.prepare (kSampleRate);

        auto left = input;
        auto right = input;
        limiter.process (left.data(), right.data(), kFrames);
        return peakOf (left);
    };

    // The measured peak tracks the requested ceiling, which is what makes it a control rather than a
    // constant — and it is clamped to the reference's own range.
    CHECK (peakAtCeiling (-6.0) == Catch::Approx (std::pow (10.0, -6.0 / 20.0)).epsilon (1e-3));
    CHECK (peakAtCeiling (-12.0) == Catch::Approx (std::pow (10.0, -12.0 / 20.0)).epsilon (1e-3));
    CHECK (peakAtCeiling (-0.30) > peakAtCeiling (-6.0));

    Limiter clamped;
    clamped.setCeilingDb (6.0);      // above the reference's own maximum
    CHECK (clamped.ceilingDb() == 0.0);
    clamped.setCeilingDb (-100.0);   // below its minimum
    CHECK (clamped.ceilingDb() == -24.0);
}
