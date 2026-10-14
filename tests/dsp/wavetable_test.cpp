// Tests for the wavetable parser and the oscillator.
//
// The parser is tested against fixtures we wrote, so it verifies on a bare clone; the corpus walk at the
// end reads all 288 factory tables and skips loudly when the reference is not installed.

#include <catch2/catch_test_macros.hpp>

#include "oscillator.h"
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

Wavetable loadFixture (const std::string& name)
{
    const auto result = parseWavetable (readFixture (name));
    REQUIRE (result.ok);
    return result.table;
}

/// Zero crossings, for counting cycles without assuming a perfect sine.
std::size_t countRisingZeroCrossings (const std::vector<float>& samples)
{
    std::size_t crossings = 0;

    for (std::size_t i = 1; i < samples.size(); ++i)
        if (samples[i - 1] <= 0.0f && samples[i] > 0.0f)
            ++crossings;

    return crossings;
}

} // namespace

TEST_CASE ("a table's shape comes from its file, and its frame count is computed", "[wavetable]")
{
    const auto single = loadFixture ("sine-1frame.wav");
    CHECK (single.frameSize == 2048);
    CHECK (single.frameCount == 1);          // computed, not assumed to be 256
    CHECK_FALSE (single.sixteenBit);
    CHECK (single.interpolation == FrameInterpolation::Linear);

    const auto four = loadFixture ("mixed-4frames.wav");
    CHECK (four.frameCount == 4);
    CHECK (four.totalSamples() == 4 * 2048);
}

TEST_CASE ("16-bit tables are scaled into the same range as float ones", "[wavetable]")
{
    // Fourteen factory tables are 16-bit. A loader that assumes float works on most of the corpus and
    // fails quietly on the rest.
    const auto table = loadFixture ("sine-16bit.wav");

    REQUIRE (table.sixteenBit);
    CHECK (table.frameCount == 1);
    CHECK (table.sampleRate == 44100.0);

    float peak = 0.0f;

    for (const auto sample : table.samples)
        peak = std::max (peak, std::abs (sample));

    // Scaled to -1..1, so a full-scale sine lands near 1 rather than near 32767.
    CHECK (peak > 0.99f);
    CHECK (peak <= 1.0f);
}

TEST_CASE ("the interpolation mode comes from the clm chunk", "[wavetable]")
{
    CHECK (loadFixture ("ramp-2frames-linear.wav").interpolation == FrameInterpolation::Linear);
    CHECK (loadFixture ("ramp-2frames-snap.wav").interpolation == FrameInterpolation::None);

    // Mode 2 in the file, which is a spectral morph — a real gap, and one the library reports rather
    // than letting the engine pass off as linear.
    CHECK (loadFixture ("mixed-4frames.wav").interpolation == FrameInterpolation::SpectralMorph);
}

TEST_CASE ("frame interpolation happens, rather than snapping", "[wavetable]")
{
    // Two frames: the first all zeros, the second all ones. Half way between them every sample of a
    // linear table is 0.5 — which no snapping reader can produce, on any sample.
    const auto table = loadFixture ("ramp-2frames-linear.wav");

    for (std::size_t index : { std::size_t {0}, std::size_t {1}, std::size_t {1024}, std::size_t {2047} })
        CHECK (table.interpolated (0.5, index) == 0.5f);

    CHECK (table.interpolated (0.0, 0) == 0.0f);
    CHECK (table.interpolated (1.0, 0) == 1.0f);
    CHECK (table.interpolated (0.25, 0) == 0.25f);
}

TEST_CASE ("a snapping table steps instead of blending", "[wavetable]")
{
    const auto table = loadFixture ("ramp-2frames-snap.wav");

    CHECK (table.interpolated (0.25, 0) == 0.0f);
    CHECK (table.interpolated (0.75, 0) == 1.0f);
}

TEST_CASE ("the oscillator scans a table at the frequency it was asked for", "[wavetable]")
{
    const auto table = loadFixture ("sine-1frame.wav");

    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        WavetableOscillator oscillator;
        oscillator.prepare (rate);
        oscillator.setTable (&table);
        oscillator.setFrequency (440.0);

        const std::size_t samples = static_cast<std::size_t> (rate);   // one second
        std::vector<float> out (samples);
        oscillator.process (out.data(), samples);

        const auto cycles = countRisingZeroCrossings (out);

        // Within a cycle or two of 440. A sample-rate bug shows up here as a wildly wrong count.
        CHECK (cycles >= 439);
        CHECK (cycles <= 441);

        float peak = 0.0f;
        for (const auto sample : out)
            peak = std::max (peak, std::abs (sample));

        CHECK (peak > 0.95f);
        CHECK (peak <= 1.01f);
    }
}

TEST_CASE ("an oscillator with no table is silent, and says so", "[wavetable]")
{
    // Silent, but not pretending: `hasTable()` is how a caller knows to report a missing table rather
    // than shipping the silence as if it were the preset.
    WavetableOscillator oscillator;
    oscillator.prepare (48000.0);

    CHECK_FALSE (oscillator.hasTable());

    std::vector<float> out (128, 1.0f);
    oscillator.process (out.data(), out.size());

    for (const auto sample : out)
        CHECK (sample == 0.0f);
}

TEST_CASE ("phase and frame position are clamped into the table", "[wavetable]")
{
    const auto table = loadFixture ("mixed-4frames.wav");

    WavetableOscillator oscillator;
    oscillator.prepare (48000.0);
    oscillator.setTable (&table);

    oscillator.setFramePosition (1000.0);      // past the end
    CHECK (oscillator.framePosition() == 3.0);

    oscillator.setFramePosition (-5.0);
    CHECK (oscillator.framePosition() == 0.0);

    oscillator.setPhase (2.75);
    CHECK (oscillator.phase() == 0.75);

    // A frequency above the sample rate must not leave the phase permanently out of range.
    oscillator.setFrequency (100000.0);
    for (int i = 0; i < 64; ++i)
        oscillator.next();

    CHECK (oscillator.phase() >= 0.0);
    CHECK (oscillator.phase() < 1.0);
}

TEST_CASE ("a malformed table is refused with a reason", "[wavetable]")
{
    SECTION ("not a RIFF file")
    {
        const auto bytes = std::vector<std::byte> (64, std::byte { 0 });
        const auto result = parseWavetable (bytes);
        CHECK_FALSE (result.ok);
        CHECK (result.error.find ("RIFF") != std::string::npos);
    }

    SECTION ("too short")
    {
        CHECK_FALSE (parseWavetable (std::vector<std::byte> (4, std::byte { 0 })).ok);
    }

    SECTION ("a truncated data chunk")
    {
        auto bytes = readFixture ("sine-1frame.wav");
        bytes.resize (bytes.size() / 2);          // cut the file in half mid-chunk
        CHECK_FALSE (parseWavetable (bytes).ok);
    }
}

TEST_CASE ("every factory table loads", "[wavetable][corpus]")
{
    const std::filesystem::path tables =
        "/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Tables";

    if (! std::filesystem::exists (tables))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    std::size_t loaded = 0;
    std::size_t sixteenBit = 0;
    std::size_t notFixedFrameCount = 0;
    std::vector<std::string> failures;
    std::vector<std::string> spectral;

    for (const auto& entry : std::filesystem::recursive_directory_iterator (tables))
    {
        if (! entry.is_regular_file() || entry.path().extension() != ".wav")
            continue;

        std::ifstream in (entry.path(), std::ios::binary);
        std::vector<std::byte> bytes ((std::size_t) std::filesystem::file_size (entry.path()));
        in.read (reinterpret_cast<char*> (bytes.data()), (std::streamsize) bytes.size());

        const auto result = parseWavetable (bytes);

        if (! result.ok)
        {
            failures.push_back (entry.path().filename().string() + ": " + result.error);
            continue;
        }

        ++loaded;

        if (result.table.sixteenBit)
            ++sixteenBit;

        if (result.table.frameCount != 256)
            ++notFixedFrameCount;

        if (result.table.interpolation == FrameInterpolation::SpectralMorph)
            spectral.push_back (entry.path().filename().string());
    }

    for (const auto& failure : failures)
        UNSCOPED_INFO (failure);

    CHECK (failures.empty());
    CHECK (loaded == 288);

    // Ticket 02's two corrections, asserted rather than trusted: some tables are 16-bit, and the frame
    // count is not a constant.
    CHECK (sixteenBit == 14);
    CHECK (notFixedFrameCount > 0);

    UNSCOPED_INFO ("tables needing spectral morphing: " << spectral.size());
}
