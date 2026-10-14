// Tickets 34 (+24 for the reader). The writer's own test.
//
// The load-bearing assertion is the round trip: everything encoded here is decoded through ticket 24's
// `parseWavetable` — a different reader, with no code in common — because a writer that only satisfies
// itself proves nothing.
//
// The rest measures behaviour rather than presence: the shapes are checked by the harmonics they are
// *supposed* to have (a saw's 1/k rolloff, a square's odd-only spectrum, a triangle's 1/k²), and the
// morphing tables by the property that makes them interesting (brightness rising across the frames, a
// duty cycle that tracks the formula, zero crossings that scale with the sync ratio).

#include <catch2/catch_test_macros.hpp>

#include "table_library.h"
#include "wavetable.h"
#include "wavetable_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace xerum::dsp;

constexpr double kPi = 3.14159265358979323846;

std::filesystem::path repoRoot()
{
    // XERUM_DSP_FIXTURE_DIR is <repo>/tests/dsp/fixtures (see tests/dsp/CMakeLists.txt).
    return std::filesystem::path (XERUM_DSP_FIXTURE_DIR).parent_path().parent_path().parent_path();
}

std::filesystem::path contentRoot()
{
    return repoRoot() / "content";
}

const TableSpec* findSpec (std::string_view name)
{
    for (const auto& spec : starterLibrary())
        if (spec.name == name)
            return &spec;

    return nullptr;
}

const TableSpec& specNamed (std::string_view name)
{
    const auto* spec = findSpec (name);
    REQUIRE (spec != nullptr);
    return *spec;
}

Wavetable tableNamed (std::string_view name)
{
    Wavetable table;
    std::string error;

    REQUIRE (buildTable (specNamed (name), table, error));
    REQUIRE (error.empty());
    return table;
}

std::vector<std::byte> readFile (const std::filesystem::path& path)
{
    std::ifstream in (path, std::ios::binary);
    REQUIRE (in.good());

    in.seekg (0, std::ios::end);
    const auto size = in.tellg();
    in.seekg (0, std::ios::beg);

    std::vector<std::byte> bytes (static_cast<std::size_t> (size));

    if (! bytes.empty())
        in.read (reinterpret_cast<char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));

    return bytes;
}

/// Amplitude of harmonics 1..count in one frame, by a plain DFT, normalised so a unit-amplitude sine
/// reads as 1.0.
std::vector<double> harmonics (const Wavetable& table, std::size_t frame, std::size_t count)
{
    const double n = static_cast<double> (table.frameSize);
    std::vector<double> amplitudes (count + 1, 0.0);

    for (std::size_t k = 1; k <= count; ++k)
    {
        double re = 0.0, im = 0.0;

        for (std::size_t i = 0; i < table.frameSize; ++i)
        {
            const double angle = 2.0 * kPi * static_cast<double> (k) * static_cast<double> (i) / n;
            const double sample = static_cast<double> (table.at (frame, i));
            re += sample * std::cos (angle);
            im -= sample * std::sin (angle);
        }

        amplitudes[k] = 2.0 * std::sqrt (re * re + im * im) / n;
    }

    return amplitudes;
}

/// How many harmonics carry at least `threshold` of the frame's strongest one — "how much spectrum is
/// this frame using".
std::size_t significantHarmonics (const Wavetable& table, std::size_t frame, double threshold = 0.01,
                                  std::size_t limit = 64)
{
    const auto amplitudes = harmonics (table, frame, limit);
    double peak = 0.0;

    for (std::size_t k = 1; k <= limit; ++k)
        peak = std::max (peak, amplitudes[k]);

    std::size_t count = 0;

    for (std::size_t k = 1; k <= limit; ++k)
        if (amplitudes[k] >= threshold * peak)
            ++count;

    return count;
}

double highFrequencyShare (const Wavetable& table, std::size_t frame, std::size_t above, std::size_t limit = 64)
{
    const auto amplitudes = harmonics (table, frame, limit);
    double total = 0.0, high = 0.0;

    for (std::size_t k = 1; k <= limit; ++k)
    {
        const double energy = amplitudes[k] * amplitudes[k];
        total += energy;

        if (k > above)
            high += energy;
    }

    return total > 0.0 ? high / total : 0.0;
}

std::size_t zeroCrossings (const Wavetable& table, std::size_t frame)
{
    std::size_t crossings = 0;
    double previous = static_cast<double> (table.at (frame, 0));

    for (std::size_t i = 1; i < table.frameSize; ++i)
    {
        const double current = static_cast<double> (table.at (frame, i));

        if ((previous < 0.0 && current >= 0.0) || (previous >= 0.0 && current < 0.0))
            ++crossings;

        previous = current;
    }

    return crossings;
}

double dutyCycle (const Wavetable& table, std::size_t frame)
{
    std::size_t positive = 0;

    for (std::size_t i = 0; i < table.frameSize; ++i)
        if (table.at (frame, i) > 0.0f)
            ++positive;

    return static_cast<double> (positive) / static_cast<double> (table.frameSize);
}

/// Offset of a chunk body, or npos.
std::size_t findChunk (const std::vector<std::byte>& bytes, std::string_view id)
{
    for (std::size_t offset = 12; offset + 8 <= bytes.size();)
    {
        const std::string tag (reinterpret_cast<const char*> (bytes.data() + offset), 4);
        std::uint32_t size = 0;
        std::memcpy (&size, bytes.data() + offset + 4, 4);

        if (tag == id)
            return offset;

        offset += 8 + size + (size % 2);
    }

    return std::string::npos;
}

/// The value a chunk declares for its own length.
std::uint32_t declaredChunkSize (const std::vector<std::byte>& bytes, std::string_view id)
{
    const auto offset = findChunk (bytes, id);

    if (offset == std::string::npos)
        return 0;

    std::uint32_t size = 0;
    std::memcpy (&size, bytes.data() + offset + 4, 4);
    return size;
}

std::string chunkPayload (const std::vector<std::byte>& bytes, std::string_view id)
{
    const auto offset = findChunk (bytes, id);

    if (offset == std::string::npos)
        return {};

    const auto size = declaredChunkSize (bytes, id);
    return std::string (reinterpret_cast<const char*> (bytes.data() + offset + 8), size);
}

std::size_t countMismatches (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
        return std::max (a.size(), b.size());

    std::size_t mismatches = 0;

    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            ++mismatches;

    return mismatches;
}

} // namespace

// ================================================================== the formula language

TEST_CASE ("formula arithmetic follows the documented precedence", "[wavetable-write]")
{
    const FormulaContext context;

    CHECK (evaluateFormula ("1 + 2*3", context).value == 7.0);
    CHECK (evaluateFormula ("(1 + 2)*3", context).value == 9.0);
    CHECK (evaluateFormula ("2^3^2", context).value == 512.0);     // right-associative
    CHECK (evaluateFormula ("10 % 3", context).value == 1.0);
    CHECK (evaluateFormula ("1 - 2*abs(0.25)", context).value == 0.5);
    CHECK (evaluateFormula ("2 <= 3", context).value == 1.0);
    CHECK (evaluateFormula ("2 > 3", context).value == 0.0);
    CHECK (evaluateFormula ("2 == 2 && 1 == 1", context).value == 1.0);
    CHECK (evaluateFormula ("2 == 3 || 1 == 1", context).value == 1.0);
    CHECK (evaluateFormula ("(2 > 1) ? 10 : 20", context).value == 10.0);
    CHECK (evaluateFormula ("(2 < 1) ? 10 : 20", context).value == 20.0);
    CHECK (evaluateFormula ("(2 > 1) || (1/0 > 0)", context).value == 1.0);   // `||` short-circuits
    CHECK (evaluateFormula ("(2 > 3) && (1/0 > 0)", context).value == 0.0);   // and so does `&&`
}

TEST_CASE ("formula names bind to the evaluation context", "[wavetable-write]")
{
    FormulaContext context;
    context.x = -0.5;
    context.w = 0.25;
    context.i = 7.0;
    context.n = 3.0;
    context.y = 0.125;
    context.z = -0.75;
    context.N = 2048.0;
    context.M = 32.0;

    CHECK (evaluateFormula ("x", context).value == -0.5);
    CHECK (evaluateFormula ("w", context).value == 0.25);
    CHECK (evaluateFormula ("i", context).value == 7.0);
    CHECK (evaluateFormula ("n", context).value == 3.0);
    CHECK (evaluateFormula ("y", context).value == 0.125);
    CHECK (evaluateFormula ("z", context).value == -0.75);
    CHECK (evaluateFormula ("N", context).value == 2048.0);
    CHECK (evaluateFormula ("M", context).value == 32.0);
    CHECK (std::abs (evaluateFormula ("pi", context).value - kPi) < 1e-12);
    CHECK (std::abs (evaluateFormula ("e", context).value - std::exp (1.0)) < 1e-12);
    CHECK (std::abs (evaluateFormula ("sin(pi)", context).value) < 1e-12);
}

TEST_CASE ("formula functions do what they say", "[wavetable-write]")
{
    const FormulaContext context;

    CHECK (evaluateFormula ("sign(-4)", context).value == -1.0);
    CHECK (evaluateFormula ("sign(4)", context).value == 1.0);
    CHECK (evaluateFormula ("sign(0)", context).value == 0.0);
    CHECK (evaluateFormula ("frac(2.75)", context).value == 0.75);
    CHECK (evaluateFormula ("frac(-0.25)", context).value == 0.75);
    CHECK (evaluateFormula ("clamp(5, -1, 1)", context).value == 1.0);
    CHECK (evaluateFormula ("min(3, 1, 2)", context).value == 1.0);
    CHECK (evaluateFormula ("max(3, 1, 2)", context).value == 3.0);
    CHECK (evaluateFormula ("sum(1, 2, 3, 4)", context).value == 10.0);
    CHECK (evaluateFormula ("avg(1, 2, 3, 4)", context).value == 2.5);
    CHECK (evaluateFormula ("step(0.5, 0.25)", context).value == 0.0);
    CHECK (evaluateFormula ("step(0.5, 0.75)", context).value == 1.0);
    CHECK (evaluateFormula ("rint(2.5)", context).value == 2.0);      // half to even, like C's rint
    CHECK (evaluateFormula ("round(2.5)", context).value == 3.0);
    CHECK (std::abs (evaluateFormula ("log(e)", context).value - 1.0) < 1e-12);
    CHECK (std::abs (evaluateFormula ("tanh(0)", context).value) < 1e-12);
    CHECK (std::abs (evaluateFormula ("atan2(1, 1)", context).value - kPi / 4.0) < 1e-12);
}

TEST_CASE ("a malformed formula is an error, never a value", "[wavetable-write]")
{
    const FormulaContext context;

    for (const auto* text : { "", "   ", "1 +", "sin(", "sin(1, 2)", "nope(1)", "unknownName", "1 2",
                              "(1 + 2", "1 +* 2", "2 $ 3" })
    {
        const auto result = evaluateFormula (text, context);
        INFO ("formula: \"" << text << "\"");
        CHECK_FALSE (result.ok);
        CHECK_FALSE (result.error.empty());
    }

    // An uncompiled formula is inert rather than fatal.
    const auto formula = Formula::fromText ("1 +");
    CHECK_FALSE (formula.valid());
    CHECK (formula.evaluate (context) == 0.0);
}

// ================================================================== the basic shapes

TEST_CASE ("Sine is exactly one DFT bin", "[wavetable-write]")
{
    const auto table = tableNamed ("Sine");
    const auto amplitudes = harmonics (table, 0, 16);

    CHECK (std::abs (amplitudes[1] - 1.0) < 1e-6);

    for (std::size_t k = 2; k <= 16; ++k)
    {
        INFO ("harmonic " << k);
        CHECK (amplitudes[k] < 1e-6);
    }
}

TEST_CASE ("Saw rolls off as 1/k, Square is odd-only, Triangle falls as 1/k squared", "[wavetable-write]")
{
    const auto saw = harmonics (tableNamed ("Saw"), 0, 8);
    CHECK (std::abs (saw[2] / saw[1] - 0.5) < 0.02);
    CHECK (std::abs (saw[3] / saw[1] - 1.0 / 3.0) < 0.02);
    CHECK (std::abs (saw[5] / saw[1] - 0.2) < 0.02);

    const auto square = harmonics (tableNamed ("Square"), 0, 8);
    CHECK (square[2] < 0.02 * square[1]);
    CHECK (square[4] < 0.02 * square[1]);
    CHECK (std::abs (square[3] / square[1] - 1.0 / 3.0) < 0.03);
    CHECK (std::abs (square[5] / square[1] - 0.2) < 0.03);

    const auto triangle = harmonics (tableNamed ("Triangle"), 0, 8);
    CHECK (triangle[2] < 0.02 * triangle[1]);
    CHECK (std::abs (triangle[3] / triangle[1] - 1.0 / 9.0) < 0.01);
    CHECK (std::abs (triangle[5] / triangle[1] - 1.0 / 25.0) < 0.005);
}

TEST_CASE ("Rectified is the octave-up companion of Sine", "[wavetable-write]")
{
    const auto rectified = harmonics (tableNamed ("Rectified"), 0, 8);

    CHECK (rectified[1] < 1e-6);      // |sin| has no fundamental
    CHECK (rectified[2] > 0.4);
    CHECK (rectified[3] < 0.01 * rectified[2]);
}

// ================================================================== the authored tables

TEST_CASE ("FM Morph gets brighter with every frame", "[wavetable-write]")
{
    const auto table = tableNamed ("FM Morph");
    REQUIRE (table.frameCount == 32);

    // At frame 0 the modulation index is zero, so it is a pure sine; then the index rises, and with it
    // the spectrum: nothing the earlier frames had disappears, and more harmonics cross the threshold.
    CHECK (std::abs (harmonics (table, 0, 8)[1] - 1.0) < 1e-6);
    CHECK (significantHarmonics (table, 0) == 1);
    CHECK (significantHarmonics (table, 31) >= 8);

    for (std::size_t frame = 1; frame < table.frameCount; ++frame)
    {
        INFO ("frame " << frame);
        CHECK (significantHarmonics (table, frame) >= significantHarmonics (table, frame - 1));
    }

    // The share of energy above the eighth harmonic is at denormal level for the first frames and rises
    // from there; it is not monotone frame by frame (the sidebands move), so it is asserted end-to-end.
    CHECK (highFrequencyShare (table, 31, 8) > highFrequencyShare (table, 0, 8));
}

TEST_CASE ("PWM Morph's duty cycle tracks its formula", "[wavetable-write]")
{
    const auto table = tableNamed ("PWM Morph");
    REQUIRE (table.frameCount == 32);

    double previous = -1.0;

    for (std::size_t frame = 0; frame < table.frameCount; ++frame)
    {
        const double y = static_cast<double> (frame) / static_cast<double> (table.frameCount - 1);
        const double expected = 0.05 + 0.9 * y;
        const double measured = dutyCycle (table, frame);

        INFO ("frame " << frame << " expected " << expected << " measured " << measured);
        CHECK (std::abs (measured - expected) <= 1.5 / static_cast<double> (table.frameSize));
        CHECK (measured > previous);
        previous = measured;
    }
}

TEST_CASE ("Sync Sweep's zero crossings scale with the sync ratio", "[wavetable-write]")
{
    const auto table = tableNamed ("Sync Sweep");
    REQUIRE (table.frameCount == 32);

    CHECK (zeroCrossings (table, 0) == 1);        // one cycle at ratio 1 (the frame starts at zero)
    CHECK (zeroCrossings (table, 31) >= 14);      // eight cycles at ratio 8
    CHECK (zeroCrossings (table, 31) >= 8 * zeroCrossings (table, 0));

    for (std::size_t frame = 1; frame < table.frameCount; ++frame)
    {
        INFO ("frame " << frame);
        CHECK (zeroCrossings (table, frame) >= zeroCrossings (table, frame - 1));
    }
}

TEST_CASE ("Fold Drive adds high-frequency content as the drive rises", "[wavetable-write]")
{
    const auto table = tableNamed ("Fold Drive");
    REQUIRE (table.frameCount == 32);

    const double quiet = highFrequencyShare (table, 0, 8);
    const double loud = highFrequencyShare (table, table.frameCount - 1, 8);

    CHECK (quiet < 1e-6);       // at drive 1x, tanh(sin) is a sine to within 1e-6
    CHECK (loud > 0.01);        // at drive 13x, 1.7% of the energy sits above the eighth harmonic
    CHECK (loud > 1e4 * quiet); // and it is a monotone rise from nothing: see the per-frame check below

    for (std::size_t frame = 1; frame < table.frameCount; ++frame)
    {
        INFO ("frame " << frame);
        CHECK (highFrequencyShare (table, frame, 8) > highFrequencyShare (table, frame - 1, 8));
    }
}

TEST_CASE ("Fallback Shapes holds sine, triangle, saw and square", "[wavetable-write]")
{
    const auto fallback = tableNamed ("Fallback Shapes");
    REQUIRE (fallback.frameCount == 4);

    const auto sine = tableNamed ("Sine");
    const auto triangle = tableNamed ("Triangle");
    const auto saw = tableNamed ("Saw");
    const auto square = tableNamed ("Square");

    for (std::size_t frame = 0; frame < 4; ++frame)
    {
        const Wavetable* expected = frame == 0 ? &sine : frame == 1 ? &triangle : frame == 2 ? &saw : &square;
        INFO ("frame " << frame);

        for (std::size_t i = 0; i < fallback.frameSize; ++i)
            if (fallback.at (frame, i) != expected->at (0, i))
                FAIL ("frame " << frame << " sample " << i << ": " << fallback.at (frame, i)
                               << " != " << expected->at (0, i));
    }
}

// ================================================================== the round trip

TEST_CASE ("float32 tables round-trip through ticket 24's reader", "[wavetable-write]")
{
    const auto table = tableNamed ("FM Morph");

    EncodeOptions options;
    options.interpolation = FrameInterpolation::Linear;

    const auto encoded = encodeWavetable (table, options);
    REQUIRE (encoded.ok);
    REQUIRE_FALSE (encoded.bytes.empty());

    const auto decoded = parseWavetable (encoded.bytes);
    REQUIRE (decoded.ok);

    CHECK (decoded.table.frameSize == table.frameSize);
    CHECK (decoded.table.frameCount == table.frameCount);
    CHECK (decoded.table.interpolation == FrameInterpolation::Linear);
    CHECK_FALSE (decoded.table.factory);
    CHECK_FALSE (decoded.table.sixteenBit);
    CHECK (decoded.table.sampleRate == 44100.0);
    CHECK (countMismatches (decoded.table.samples, table.samples) == 0);

    // Every size is declared truthfully: 32-bit float, mono, and the frame count derived from data.
    CHECK (declaredChunkSize (encoded.bytes, "data") == table.totalSamples() * 4);
    CHECK (declaredChunkSize (encoded.bytes, "fmt ") == 16);

    std::uint32_t riffSize = 0;
    std::memcpy (&riffSize, encoded.bytes.data() + 4, 4);
    CHECK (riffSize == encoded.bytes.size() - 8);
}

TEST_CASE ("16-bit tables round-trip within one quantisation step", "[wavetable-write]")
{
    const auto table = tableNamed ("Sine");

    EncodeOptions options;
    options.sixteenBit = true;

    const auto encoded = encodeWavetable (table, options);
    REQUIRE (encoded.ok);

    // Ours declares the real byte count. The reference's own 16-bit writer declares twice it
    // (ticket 24); the next case proves that form is still readable, this one proves we do not write it.
    CHECK (declaredChunkSize (encoded.bytes, "data") == table.totalSamples() * 2);

    const auto decoded = parseWavetable (encoded.bytes);
    REQUIRE (decoded.ok);
    CHECK (decoded.table.sixteenBit);
    CHECK (decoded.table.frameCount == table.frameCount);
    REQUIRE (decoded.table.samples.size() == table.samples.size());

    double worst = 0.0;

    for (std::size_t i = 0; i < table.samples.size(); ++i)
        worst = std::max (worst, std::abs (static_cast<double> (decoded.table.samples[i] - table.samples[i])));

    // Half a quantisation step (1/32767 = 3.05e-5) plus the scale difference between the writer's
    // full-scale constant and ticket 24's decoder, which divides by 32768: measured worst case 4.45e-5
    // at full scale. Bounded, systematic and inaudible — but stated, not waved through.
    CHECK (worst > 0.0);
    CHECK (worst < 4.7e-5);
}

TEST_CASE ("the reference's inflated 16-bit chunk size is still readable", "[wavetable-write]")
{
    const auto table = tableNamed ("Triangle");

    EncodeOptions options;
    options.sixteenBit = true;

    auto bytes = encodeWavetable (table, options).bytes;
    REQUIRE_FALSE (bytes.empty());

    const std::uint32_t trueDataSize = declaredChunkSize (bytes, "data");

    // Reproduce the reference's bug: `data` is declared with the 32-bit byte count — twice the bytes
    // present — and the RIFF size is inflated to match. Ticket 24's reader accepts exactly this
    // arithmetic and nothing looser, so a truncated file is still refused; this asserts both halves.
    const auto dataHeader = findChunk (bytes, "data");
    REQUIRE (dataHeader != std::string::npos);

    const std::uint32_t inflated = trueDataSize * 2;
    std::memcpy (bytes.data() + dataHeader + 4, &inflated, 4);

    std::uint32_t riffSize = 0;
    std::memcpy (&riffSize, bytes.data() + 4, 4);
    riffSize += trueDataSize;
    std::memcpy (bytes.data() + 4, &riffSize, 4);

    const auto decoded = parseWavetable (bytes);
    REQUIRE (decoded.ok);
    CHECK (decoded.table.sixteenBit);
    CHECK (decoded.table.frameCount == table.frameCount);

    auto truncated = bytes;
    truncated.resize (truncated.size() - 2048);
    CHECK_FALSE (parseWavetable (truncated).ok);
}

TEST_CASE ("the interpolation digit and the factory flag survive the round trip", "[wavetable-write]")
{
    const auto table = tableNamed ("Sine");

    struct Case
    {
        FrameInterpolation interpolation;
        std::uint8_t subMode;
        bool factory;
        char expectedDigit;
    };

    const Case cases[] =
    {
        { FrameInterpolation::None, 0, false, '0' },
        { FrameInterpolation::Linear, 0, false, '1' },
        { FrameInterpolation::SpectralMorph, 2, false, '2' },
        { FrameInterpolation::SpectralMorph, 3, true, '3' },
        { FrameInterpolation::SpectralMorph, 4, true, '4' },
    };

    for (const auto& test : cases)
    {
        EncodeOptions options;
        options.interpolation = test.interpolation;
        options.spectralSubMode = test.subMode;
        options.factory = test.factory;

        const auto encoded = encodeWavetable (table, options);
        REQUIRE (encoded.ok);

        // The raw `clm ` payload, as ticket 02 decoded it: `<!>2048 BC000000 wavetable (…)`.
        const auto payload = chunkPayload (encoded.bytes, "clm ");
        INFO ("payload: " << payload);
        REQUIRE (payload.size() >= 10);
        CHECK (payload.substr (0, 3) == "<!>");
        CHECK (payload.substr (3, 4) == "2048");
        CHECK (payload[7] == ' ');
        CHECK (payload[8] == test.expectedDigit);
        CHECK (payload[9] == (test.factory ? '1' : '0'));
        CHECK (payload.find ("wavetable (xerum)") != std::string::npos);
        CHECK (payload.find ("xferrecords") == std::string::npos);

        const auto decoded = parseWavetable (encoded.bytes);
        REQUIRE (decoded.ok);
        CHECK (decoded.table.interpolation == test.interpolation);
        CHECK (decoded.table.factory == test.factory);
    }
}

TEST_CASE ("frame counts are computed by the reader, so the writer must not pad", "[wavetable-write]")
{
    for (const std::size_t frames : { 1u, 3u, 5u, 256u })
    {
        Wavetable table;
        table.frameSize = kDefaultFrameSize;
        table.frameCount = frames;
        table.samples.assign (frames * kDefaultFrameSize, 0.25f);

        const auto encoded = encodeWavetable (table, {});
        REQUIRE (encoded.ok);
        CHECK (declaredChunkSize (encoded.bytes, "data") == frames * kDefaultFrameSize * 4);

        const auto decoded = parseWavetable (encoded.bytes);
        REQUIRE (decoded.ok);
        INFO ("frames " << frames);
        CHECK (decoded.table.frameCount == frames);
        CHECK (countMismatches (decoded.table.samples, table.samples) == 0);
    }
}

TEST_CASE ("encoding is deterministic and refuses malformed tables", "[wavetable-write]")
{
    const auto table = tableNamed ("PWM Morph");

    const auto first = encodeWavetable (table, {});
    const auto second = encodeWavetable (table, {});
    REQUIRE (first.ok);
    REQUIRE (second.ok);
    CHECK (first.bytes == second.bytes);

    EncodeOptions unknown;
    unknown.interpolation = FrameInterpolation::Unknown;
    CHECK_FALSE (encodeWavetable (table, unknown).ok);

    EncodeOptions badSubMode;
    badSubMode.interpolation = FrameInterpolation::SpectralMorph;
    badSubMode.spectralSubMode = 5;
    CHECK_FALSE (encodeWavetable (table, badSubMode).ok);

    auto wrongCount = table;
    wrongCount.frameCount += 1;
    CHECK_FALSE (encodeWavetable (wrongCount, {}).ok);

    Wavetable empty;
    CHECK_FALSE (encodeWavetable (empty, {}).ok);
}

// ================================================================== the shipped content

TEST_CASE ("every shipped table is exactly what the tool generates", "[wavetable-write]")
{
    REQUIRE (starterLibrary().size() == 10);
    REQUIRE_FALSE (fallbackTableReference().empty());

    // Both encodings the format supports ship, so the reader's 16-bit path is exercised by our own
    // content and not only by the fourteen factory tables.
    const auto sixteenBit = std::count_if (starterLibrary().begin(), starterLibrary().end(),
                                           [] (const TableSpec& s) { return s.sixteenBit; });
    CHECK (sixteenBit == 1);

    for (const auto& spec : starterLibrary())
    {
        const auto path = contentRoot() / "Tables" / spec.relativePath;
        INFO ("spec " << spec.name << " at " << path.string());
        REQUIRE (std::filesystem::exists (path));

        Wavetable built;
        std::string error;
        REQUIRE (buildTable (spec, built, error));

        const auto encoded = encodeWavetable (built, encodeOptionsFor (spec));
        REQUIRE (encoded.ok);

        // What ships is what the tool generated: byte for byte, not merely "equivalent".
        const auto onDisk = readFile (path);
        REQUIRE (onDisk.size() == encoded.bytes.size());
        CHECK (std::memcmp (onDisk.data(), encoded.bytes.data(), onDisk.size()) == 0);

        // And the shipped bytes decode through ticket 24 to the samples the formula generates.
        const auto decoded = parseWavetable (onDisk);
        REQUIRE (decoded.ok);
        CHECK (decoded.table.frameSize == kDefaultFrameSize);
        CHECK (decoded.table.frameCount == spec.frameCount);
        CHECK (decoded.table.interpolation == spec.interpolation);
        CHECK (decoded.table.sixteenBit == spec.sixteenBit);
        CHECK_FALSE (decoded.table.factory);

        if (spec.sixteenBit)
        {
            // 16-bit is quantised: the samples agree to well under one step, not bit for bit.
            double worst = 0.0;

            for (std::size_t i = 0; i < built.samples.size(); ++i)
                worst = std::max (worst, std::abs (static_cast<double> (decoded.table.samples[i] - built.samples[i])));

            CHECK (worst > 0.0);
            CHECK (worst < 4.7e-5);
        }
        else
        {
            CHECK (countMismatches (decoded.table.samples, built.samples) == 0);
        }
    }
}

TEST_CASE ("the fallback resolves through the real library, and a miss is still reported", "[wavetable-write]")
{
    xerum::content::TableLibrary library;
    library.addRoot (contentRoot());
    REQUIRE (library.roots().size() == 1);

    for (const auto& spec : starterLibrary())
    {
        const auto lookup = library.loadTable (spec.relativePath);
        INFO ("reference " << spec.relativePath);
        CHECK (lookup.found());
        CHECK (lookup.resolution == xerum::content::Resolution::Exact);
        CHECK (lookup.table.frameCount == spec.frameCount);
        // Ours uses modes none and linear only — the same thing the shipped-content test asserts from
        // the decoded `clm ` digit — so nothing here needs an interpolation the engine cannot do.
    }

    // The fallback is a real, named table of ours — never the reference's own default table.
    const auto fallback = library.loadTable (fallbackTableReference());
    CHECK (fallback.found());
    CHECK (fallback.resolution == xerum::content::Resolution::Exact);
    CHECK (fallback.table.frameCount == 4);
    CHECK (fallbackTableReference() == std::string_view ("xerum/Fallback Shapes.wav"));
    CHECK (fallbackTableReference() != std::string_view ("S2 Tables/Default Shapes.wav"));

    // A reference that cannot be resolved is still *reported*, not quietly substituted. Ticket 24 fixed
    // that behaviour; this pins it, so the fallback cannot be smuggled in behind it.
    const auto missing = library.loadTable ("xerum/No Such Table.wav");
    CHECK_FALSE (missing.found());
    CHECK (missing.resolution == xerum::content::Resolution::Missing);
    CHECK_FALSE (missing.error.empty());
}
