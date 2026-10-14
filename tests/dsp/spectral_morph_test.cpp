// Tests for the spectral morph, and for the decision ticket 37 made about the `clm ` modes.
//
// Two different mechanisms share the name "spectral morph" in the reference, and this file keeps them
// apart on purpose:
//
//   * The `clm ` **interpolation mode** in a table file (0..4). Measured: 0 is discrete, and 1, 2, 3
//     and 4 all render as a plain linear crossfade — a two-frame probe built with deliberately
//     disagreeing phases rendered byte-identically for modes 1/2/3/4 (peak 0.075252, identical SHA-256)
//     while mode 0 rendered frame A. Four shipped tables carrying mode 2 or 4 matched a crossfade of
//     their neighbouring frames to <=0.008 normalised magnitude distance over 48 harmonics, against
//     2.4-4.2 for every spectral candidate. So `Wavetable::interpolated` crossfading a
//     `FrameInterpolation::SpectralMorph` table is not a fallback, it is the reference's behaviour, and
//     the corpus case at the end asserts it over all 288 factory tables.
//
//   * The preset field `WTOsc{N}/interpolateAfterLoad == 2`, an **embedded** table. That one really
//     morphs, and `SpectralMorph` below implements the rule measured from the reference: per harmonic,
//     magnitudes blend linearly and phases take the shorter arc. Fitted to the reference's own output at
//     0.022 magnitude / 0.022 rad, against 1.18 rad for interpolating the phase as stored.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "oscillator.h"
#include "spectral_morph.h"
#include "wavetable.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

using namespace xerum::dsp;
using Catch::Approx;

namespace
{

constexpr std::size_t kN = 2048;
constexpr double kPi = 3.14159265358979323846;

/// A cycle built from `(magnitude, phase)` pairs, index k = harmonic k.
std::vector<float> cycle (const std::vector<std::pair<double, double>>& harmonics, std::size_t size = kN)
{
    std::vector<float> out (size, 0.0f);

    for (std::size_t k = 1; k < harmonics.size(); ++k)
        for (std::size_t i = 0; i < size; ++i)
            out[i] += float (harmonics[k].first * std::cos (2.0 * kPi * double (k) * double (i) / double (size) + harmonics[k].second));

    return out;
}

/// Harmonic `k` of a cycle, scaled so a unit cosine reads back as 1 + 0i.
std::complex<double> harmonic (const std::vector<float>& x, std::size_t k)
{
    std::complex<double> sum { 0.0, 0.0 };

    for (std::size_t i = 0; i < x.size(); ++i)
        sum += double (x[i]) * std::polar (1.0, -2.0 * kPi * double (k) * double (i) / double (x.size()));

    return sum / (double (x.size()) / 2.0);
}

double peakAbove (const std::vector<float>& x, std::size_t firstHarmonic)
{
    double peak = 0.0;

    for (std::size_t k = firstHarmonic; k <= x.size() / 2; ++k)
        peak = std::max (peak, std::abs (harmonic (x, k)));

    return peak;
}

/// The `clm ` interpolation digit, read the way ticket 02 decoded it: `<!>AAAA BC000000 vendor`.
int clmMode (const std::vector<std::byte>& bytes)
{
    std::size_t offset = 12;

    while (offset + 8 <= bytes.size())
    {
        const std::string id (reinterpret_cast<const char*> (bytes.data() + offset), 4);

        std::uint32_t size = 0;
        std::memcpy (&size, bytes.data() + offset + 4, sizeof (size));

        const std::size_t body = offset + 8;

        if (id == "clm " && body < bytes.size())
        {
            const auto length = std::min<std::size_t> (size, bytes.size() - body);
            const std::string text (reinterpret_cast<const char*> (bytes.data() + body), length);

            const auto marker = text.find ("<!>");

            if (marker != std::string::npos)
            {
                std::size_t at = marker + 3;

                while (at < text.size() && ! std::isdigit (static_cast<unsigned char> (text[at])))
                    ++at;

                while (at < text.size() && std::isdigit (static_cast<unsigned char> (text[at])))
                    ++at;

                while (at < text.size() && text[at] == ' ')
                    ++at;

                if (at < text.size())
                    return std::isdigit (static_cast<unsigned char> (text[at])) ? text[at] - '0'
                                                                               : (text[at] | 0x20) - 'a' + 10;
            }

            return -1;
        }

        offset = body + size + (size % 2);
    }

    return -1;
}

std::vector<std::byte> readFile (const std::filesystem::path& path)
{
    std::ifstream in (path, std::ios::binary);
    std::vector<std::byte> bytes ((std::size_t) std::filesystem::file_size (path));
    in.read (reinterpret_cast<char*> (bytes.data()), (std::streamsize) bytes.size());
    return bytes;
}

} // namespace

TEST_CASE ("the shortest arc is the shorter of the two ways round", "[spectral-morph]")
{
    CHECK (wrapToPi (0.5) == Approx (0.5));
    CHECK (wrapToPi (-0.5) == Approx (-0.5));
    CHECK (wrapToPi (3.0) == Approx (3.0));                    // inside half a turn, left alone
    CHECK (wrapToPi (3.5) == Approx (3.5 - 2.0 * kPi));
    CHECK (wrapToPi (-3.5) == Approx (-3.5 + 2.0 * kPi));
    CHECK (wrapToPi (7.0) == Approx (7.0 - 2.0 * kPi));

    for (double angle = -20.0; angle <= 20.0; angle += 0.25)
    {
        const double wrapped = wrapToPi (angle);
        CHECK (std::abs (wrapped) <= kPi + 1e-12);

        // Same direction, a whole number of turns away.
        CHECK (std::abs (std::sin (wrapped) - std::sin (angle)) < 1e-12);
        CHECK (std::abs (std::cos (wrapped) - std::cos (angle)) < 1e-12);
    }
}

TEST_CASE ("a morph meets the frames it sits between", "[spectral-morph]")
{
    // The endpoints have to be the frames themselves, not a transform that rounds to them: a table
    // scanned across frames would step audibly at every boundary otherwise.
    SpectralMorph morph (kN);

    const auto a = cycle ({ {}, { 1.0, 0.3 }, { 0.5, -2.0 }, { 0.25, 1.1 } });
    const auto b = cycle ({ {}, { 0.7, 2.9 }, { 0.9, 0.2 }, { 0.15, -1.4 } });
    std::vector<float> out (kN, 0.0f);

    morph.morph (a.data(), b.data(), 0.0f, out.data());
    CHECK (out == a);

    morph.morph (a.data(), b.data(), 1.0f, out.data());
    CHECK (out == b);
}

TEST_CASE ("a morphed cycle carries only the harmonics its neighbours had", "[spectral-morph]")
{
    // The anti-aliasing claim, as a measurement rather than an argument: harmonic k of the result is
    // built from harmonic k of the two neighbours and nothing else, so a pair band-limited at the 16th
    // harmonic cannot produce a 17th. A crossfade has the same property; a transform that moved energy
    // between harmonics would show up here immediately.
    SpectralMorph morph (kN);

    std::vector<std::pair<double, double>> specA (17), specB (17);

    for (std::size_t k = 1; k <= 16; ++k)
    {
        specA[k] = { 1.0 / double (k), 0.4 * double (k) };
        specB[k] = { 1.0 / double (k), -0.9 * double (k) + 1.0 };
    }

    const auto a = cycle (specA);
    const auto b = cycle (specB);
    std::vector<float> out (kN, 0.0f);

    morph.morph (a.data(), b.data(), 0.5f, out.data());

    const double fundamental = std::abs (harmonic (out, 1));

    CHECK (fundamental > 0.5);

    // 100 dB down: far above the float round-trip's own floor (~1e-8 absolute here) and far below the
    // 0.06-relative a real 17th harmonic would carry if the transform invented one.
    CHECK (peakAbove (out, 17) < fundamental * 1e-5);
}

TEST_CASE ("magnitudes blend linearly", "[spectral-morph]")
{
    // The measured rule, on a case with no phase ambiguity: |c_k| = (1-t)|a_k| + t|b_k|.
    SpectralMorph morph (kN);

    const auto a = cycle ({ {}, { 1.0, 0.0 } });
    const auto b = cycle ({ {}, { 0.5, 0.0 } });
    std::vector<float> out (kN, 0.0f);

    morph.morph (a.data(), b.data(), 0.5f, out.data());
    CHECK (std::abs (harmonic (out, 1)) == Approx (0.75).margin (1e-4));

    morph.morph (a.data(), b.data(), 0.25f, out.data());
    CHECK (std::abs (harmonic (out, 1)) == Approx (0.875).margin (1e-4));
}

TEST_CASE ("a frame size that is not a power of two is morphed, not refused", "[spectral-morph]")
{
    // The shipped format is 2048 and always a power of two, so this exercises the fallback path rather
    // than anything real — but a frame size the format does not produce must still be morphed correctly
    // instead of silently degrading, and the fallback is the one place where a transform has to be
    // written out by hand.
    constexpr std::size_t kOdd = 100;
    SpectralMorph morph (kOdd);

    const auto a = cycle ({ {}, { 1.0, 0.0 }, { 0.4, 1.0 } }, kOdd);
    const auto b = cycle ({ {}, { 0.5, 0.0 }, { 0.2, -2.0 } }, kOdd);
    std::vector<float> out (kOdd, 0.0f);

    morph.morph (a.data(), b.data(), 0.0f, out.data());
    CHECK (out == a);

    morph.morph (a.data(), b.data(), 1.0f, out.data());
    CHECK (out == b);

    morph.morph (a.data(), b.data(), 0.5f, out.data());

    CHECK (std::abs (harmonic (out, 1)) == Approx (0.75).margin (1e-4));
    CHECK (std::abs (harmonic (out, 2)) == Approx (0.3).margin (1e-4));

    const double fundamental = std::abs (harmonic (out, 1));
    CHECK (peakAbove (out, 3) < fundamental * 1e-4);
}

TEST_CASE ("phases travel the short way round, not through the stored angle", "[spectral-morph]")
{
    // +2.5 rad and -2.5 rad are 5.0 rad apart the way they are stored, but only 1.283 rad apart on the
    // circle. Interpolating the stored angles puts the midpoint at 0 (the cosine); the shortest arc puts
    // it at pi (minus the cosine). The reference does the latter — this is the rule that separated the
    // measured fit (0.022 rad) from the obvious implementation (1.18 rad).
    SpectralMorph morph (kN);

    const auto a = cycle ({ {}, { 1.0, 2.5 } });
    const auto b = cycle ({ {}, { 1.0, -2.5 } });
    std::vector<float> out (kN, 0.0f);

    morph.morph (a.data(), b.data(), 0.5f, out.data());

    const auto morphed = harmonic (out, 1);

    CHECK (std::abs (morphed) == Approx (1.0).margin (1e-4));
    CHECK (std::abs (std::arg (morphed)) == Approx (kPi).margin (1e-3));
    CHECK (std::abs (std::arg (morphed)) > 3.0);            // and emphatically not 0
}

TEST_CASE ("morphing a frame with itself changes nothing", "[spectral-morph]")
{
    SpectralMorph morph (kN);
    const auto a = cycle ({ {}, { 1.0, 0.3 }, { 0.5, -2.0 }, { 0.25, 1.1 }, { 0.1, 0.0 } });
    std::vector<float> out (kN, 0.0f);

    for (float t : { 0.1f, 0.37f, 0.9f })
    {
        morph.morph (a.data(), a.data(), t, out.data());

        for (std::size_t i = 0; i < kN; ++i)
            REQUIRE (out[i] == Approx (a[i]).margin (1e-5));
    }
}

TEST_CASE ("the self-conjugate bins stay real and blend by sign", "[spectral-morph]")
{
    // DC has no partner bin to mirror, so a rounded phase arc would push a real bin off the real axis and
    // the inverse transform would answer with an imaginary DC. It is blended by sign instead. The
    // reference cannot arbitrate this — its own output carries no DC at all (an embedded frame whose mean
    // was +0.300 emerged at 0.058, and a signed crossfade of -0.050 emerged at -0.009) — so this is the
    // documented choice, not a measured one.
    SpectralMorph morph (kN);

    auto a = cycle ({ {}, { 0.5, 0.0 } });
    auto b = cycle ({ {}, { 0.5, 0.0 } });

    for (auto& sample : a) sample += 0.6f;
    for (auto& sample : b) sample -= 0.2f;

    std::vector<float> out (kN, 0.0f);
    morph.morph (a.data(), b.data(), 0.5f, out.data());

    const double mean = std::accumulate (out.begin(), out.end(), 0.0) / double (kN);
    CHECK (mean == Approx (0.5 * (0.6 - 0.2)).margin (1e-5));
}

TEST_CASE ("expanding a table fills the slots between its cycles", "[spectral-morph]")
{
    Wavetable table;
    table.frameSize = kN;
    table.frameCount = 2;
    table.sampleRate = 44100.0;
    table.interpolation = FrameInterpolation::SpectralMorph;
    table.samples = cycle ({ {}, { 1.0, 0.0 } });
    for (float sample : cycle ({ {}, { 0.5, -2.0 } }))
        table.samples.push_back (sample);

    const auto expanded = expandWavetable (table);

    CHECK (expanded.frameCount == kExpandedFrameCount);
    CHECK (expanded.frameSize == kN);
    CHECK (expanded.interpolation == FrameInterpolation::Linear);
    CHECK (expanded.samples.size() == kExpandedFrameCount * kN);   // 2 MB at float32, per table

    // The stored cycles land on the first and last slot untouched; everything between is a morph.
    for (std::size_t i = 0; i < kN; ++i)
    {
        REQUIRE (expanded.at (0, i) == table.at (0, i));
        REQUIRE (expanded.at (kExpandedFrameCount - 1, i) == table.at (1, i));
    }

    // Slot i sits at position i/255, and the magnitude rule is visible through it.
    for (std::size_t slot : { std::size_t (64), std::size_t (128), std::size_t (192) })
    {
        const auto row = std::vector<float> (expanded.samples.begin() + slot * kN,
                                             expanded.samples.begin() + (slot + 1) * kN);
        const double position = double (slot) / double (kExpandedFrameCount - 1);
        CHECK (std::abs (harmonic (row, 1)) == Approx (1.0 - 0.5 * position).margin (1e-3));
    }

    // Adjacent slots differ, or the expansion is not doing anything.
    bool differs = false;
    for (std::size_t i = 0; i < kN && ! differs; ++i)
        differs = expanded.at (64, i) != expanded.at (128, i);

    CHECK (differs);
}

TEST_CASE ("expanding a single-cycle table duplicates it rather than inventing neighbours", "[spectral-morph]")
{
    Wavetable table;
    table.frameSize = kN;
    table.frameCount = 1;
    table.samples = cycle ({ {}, { 1.0, 0.7 }, { 0.3, -1.0 } });

    const auto expanded = expandWavetable (table);

    REQUIRE (expanded.frameCount == kExpandedFrameCount);
    for (std::size_t slot = 0; slot < kExpandedFrameCount; ++slot)
        for (std::size_t i = 0; i < kN; ++i)
            REQUIRE (expanded.at (slot, i) == table.at (0, i));
}

TEST_CASE ("a table already at full resolution is returned as it is", "[spectral-morph]")
{
    Wavetable table;
    table.frameSize = 4;
    table.frameCount = 4;
    table.samples = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.75f, 0.5f, 0.25f,
                      0.0f, -0.25f, -0.5f, -0.75f, -1.0f, -0.75f, -0.5f, -0.25f };

    const auto expanded = expandWavetable (table, table.frameCount);

    CHECK (expanded.frameCount == table.frameCount);
    CHECK (expanded.samples == table.samples);
}

TEST_CASE ("the engine crossfades a table that declares spectral morphing", "[spectral-morph]")
{
    // Ticket 37's decision, pinned in the DSP layer. The reference offers a *spectral* morph through the
    // preset's `interpolateAfterLoad` for embedded tables, but for a table file's own `clm ` mode 2/3/4 it
    // crossfades — measured, not assumed (see this file's header). So the engine's read of such a table is
    // not an approximation of the reference, it is the reference; and there is no mode left that the
    // engine cannot perform, which is what makes ticket 37's `needsUnimplementedInterpolation` removable.
    Wavetable spectral;
    spectral.frameSize = 4;
    spectral.frameCount = 2;
    spectral.interpolation = FrameInterpolation::SpectralMorph;
    spectral.samples = { 1.0f, 0.5f, -0.5f, -1.0f, 0.25f, 0.75f, 0.0f, -0.5f };

    Wavetable linear = spectral;
    linear.interpolation = FrameInterpolation::Linear;

    for (std::size_t index = 0; index < spectral.frameSize; ++index)
    {
        const float a = spectral.at (0, index);
        const float b = spectral.at (1, index);

        CHECK (spectral.interpolated (0.5, index) == Approx (0.5f * (a + b)).margin (1e-6));
        CHECK (spectral.interpolated (0.5, index) == linear.interpolated (0.5, index));
    }
}

TEST_CASE ("the morph is in the signal path: an embedded table asking for it renders differently from the crossfade",
           "[morph-wired]")
{
    // The acceptance is asserted by **output**, not by a call existing. A two-cycle embedded table is
    // bound twice the way the plugin binds it — once with `interpolateAfterLoad = 2` (expanded once, at
    // bind) and once with `1` (left to the read's crossfade) — and the two renders are measured against
    // each other.
    //
    // The cycles are given deliberately disagreeing magnitudes *and* phases, so the two mechanisms cannot
    // coincide at the midpoint: a morph blends the fundamental's magnitude as
    // `(1-t)|a_1| + t|b_1| = 0.5*1.0 + 0.5*0.8 = 0.90`, while a crossfade adds the two complex harmonics
    // with their 2.6 rad of disagreement and lands near 0.26.
    const auto a = cycle ({ {}, { 1.0, 0.9 }, { 0.6, 2.4 }, { 0.4, -1.1 } });
    const auto b = cycle ({ {}, { 0.8, -1.7 }, { 0.7, 0.4 }, { 0.5, 2.2 } });

    Wavetable embedded;
    embedded.frameSize = kN;
    embedded.frameCount = 2;
    embedded.interpolation = FrameInterpolation::Linear;
    embedded.samples = a;
    embedded.samples.insert (embedded.samples.end(), b.begin(), b.end());

    // **A table referenced by path is untouched whatever the field says** — ticket 37 measured the
    // reference ignoring it there, byte for byte across the field's every value. Morphing one would make
    // xerum differ from the reference on every preset built that way.
    CHECK (tableForInterpolateAfterLoad (embedded, /*embedded*/ false, 2).frameCount == 2);
    CHECK (tableForInterpolateAfterLoad (embedded, /*embedded*/ false, 2).samples == embedded.samples);

    const auto morphed = tableForInterpolateAfterLoad (embedded, /*embedded*/ true, 2);
    const auto crossfaded = tableForInterpolateAfterLoad (embedded, /*embedded*/ true, 1);

    REQUIRE (morphed.frameCount == kExpandedFrameCount);
    REQUIRE (crossfaded.frameCount == 2);
    REQUIRE (crossfaded.samples == embedded.samples);

    // Frequency chosen so one render is exactly one table cycle: 48000/2048 samples per cycle means the
    // render is periodic over its own length and a bare DFT of it is the cycle's own spectrum, with no
    // window and no leakage.
    const auto renderCycle = [] (const Wavetable& table, double position)
    {
        WavetableOscillator oscillator;
        oscillator.prepare (48000.0);
        oscillator.setTable (&table);
        oscillator.setFramePosition (position);
        oscillator.setFrequency (48000.0 / double (kN));

        std::vector<float> out (kN, 0.0f);
        oscillator.process (out.data(), out.size());
        return out;
    };

    // Slot 127 of the expanded table sits at t = 127/255 and slot 128 at 128/255, so position 127.5 is
    // the same place in the table the two-frame version answers at 0.5.
    const auto morphedRender = renderCycle (morphed, 127.5);
    const auto crossfadeRender = renderCycle (crossfaded, 0.5);

    const double morphedFundamental = std::abs (harmonic (morphedRender, 1));
    const double crossfadeFundamental = std::abs (harmonic (crossfadeRender, 1));

    CHECK (morphedFundamental == Approx (0.90).margin (0.02));   // the magnitude blend
    CHECK (crossfadeFundamental < 0.40);                          // the complex sum, phases apart
    CHECK (morphedFundamental > 2.0 * crossfadeFundamental);
    CAPTURE (morphedFundamental, crossfadeFundamental);

    // And they are not the same signal sample for sample, which is the blunt version of the same fact.
    double maxDifference = 0.0;

    for (std::size_t i = 0; i < kN; ++i)
        maxDifference = std::max (maxDifference,
                                  std::abs (static_cast<double> (morphedRender[i] - crossfadeRender[i])));

    CHECK (maxDifference > 0.05);
    CAPTURE (maxDifference);

    // **The anti-aliasing property, measured the way ticket 37 measured it.** The transform is diagonal
    // in the harmonic basis — harmonic k of the result is built only from harmonic k of the two
    // neighbours — so it cannot place energy at a harmonic neither neighbour had. The probe is
    // band-limited at the 3rd harmonic, and nothing may appear at the 4th.
    CHECK (peakAbove (morphedRender, 4) < morphedFundamental * 1e-5);
}

TEST_CASE ("the factory tables declare no interpolation the engine cannot perform", "[spectral-morph][corpus]")
{
    // The ticket's third acceptance criterion, asserted against all 288 shipped tables rather than
    // counted once in a report. `SKIP`s loudly when the reference is not installed (docs/test-strategy.md).
    const std::filesystem::path root = "/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Tables";

    if (! std::filesystem::exists (root))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    std::size_t census[5] = {};
    std::size_t total = 0;
    std::size_t unknown = 0;
    std::vector<std::string> wrong;

    for (const auto& entry : std::filesystem::recursive_directory_iterator (root))
    {
        if (! entry.is_regular_file() || entry.path().extension() != ".wav")
            continue;

        ++total;

        const auto bytes = readFile (entry.path());
        const int mode = clmMode (bytes);

        if (mode >= 0 && mode <= 4)
            ++census[mode];
        else
            ++unknown;

        const auto parsed = parseWavetable (bytes);

        if (! parsed.ok)
        {
            wrong.push_back (entry.path().filename().string() + ": " + parsed.error);
            continue;
        }

        // A mode the parser could not name would be the "unimplemented interpolation" this ticket exists
        // to remove: refuse it loudly rather than let the engine crossfade under a flag.
        if (parsed.table.interpolation == FrameInterpolation::Unknown)
            wrong.push_back (entry.path().filename().string() + ": mode " + std::to_string (mode));

        // And every named mode has to be *performed*: modes 1..4 crossfade, mode 0 is discrete.
        if (parsed.table.frameCount > 1)
        {
            for (std::size_t index : { std::size_t (0), std::size_t (511), std::size_t (2047) })
            {
                const float a = parsed.table.at (0, index);
                const float b = parsed.table.at (1, index);

                if (mode == 0)
                {
                    if (parsed.table.interpolated (0.25, index) != a
                        || parsed.table.interpolated (0.75, index) != b)
                        wrong.push_back (entry.path().filename().string()
                                         + ": mode 0 is meant to be discrete and stepped instead");
                }
                else if (std::abs (parsed.table.interpolated (0.5, index) - (a + (b - a) * 0.5f)) > 1e-6f)
                {
                    wrong.push_back (entry.path().filename().string() + ": mode " + std::to_string (mode)
                                     + " reads back something other than the crossfade");
                }
            }
        }
    }

    for (const auto& failure : wrong)
        UNSCOPED_INFO (failure);

    CHECK (wrong.empty());
    CHECK (unknown == 0);
    CHECK (total == 288);

    // Ticket 02's counts, re-measured here because 74 tables in three different modes is the premise of
    // the whole ticket and was never itself asserted.
    CHECK (census[0] == 142);
    CHECK (census[1] == 72);
    CHECK (census[2] == 51);
    CHECK (census[3] == 7);
    CHECK (census[4] == 16);
}
