// Tests for the voice layer: stealing, unison, mono/legato, and the real-time invariant.
//
// Every assertion here measures something. Stealing is asserted from the voice the allocator actually
// chose (the returned index and that voice's state), not from a parameter. Unison is asserted by
// *counting partials in the rendered spectrum*, not by reading the unison count back. And the
// real-time-safe invariant is asserted by replacing `operator new`/`delete` and observing that a full
// render — note-on, note-off, steal, process — performs zero heap allocations.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "voice.h"
#include "wavetable.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <vector>

using namespace xerum::dsp;

// --------------------------------------------------------------- tracking allocator
//
// A replacement for the global allocation functions. It is inert until a test turns `g_tracking` on, so
// Catch2's own allocations are not counted; when on, every allocation from any translation unit in this
// binary is counted. That is the point: it observes the audio thread's allocations, not one function's.

namespace
{
std::atomic<long> g_allocations { 0 };
std::atomic<bool> g_tracking { false };

void noteAllocation()
{
    if (g_tracking.load (std::memory_order_relaxed))
        g_allocations.fetch_add (1, std::memory_order_relaxed);
}

void* trackedAllocate (std::size_t size)
{
    noteAllocation();
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
} // namespace

void* operator new (std::size_t size) { return trackedAllocate (size); }
void* operator new[] (std::size_t size) { return trackedAllocate (size); }
void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    noteAllocation();
    return std::malloc (size == 0 ? 1 : size);
}
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    noteAllocation();
    return std::malloc (size == 0 ? 1 : size);
}
void* operator new (std::size_t size, std::align_val_t alignment)
{
    noteAllocation();
    void* p = nullptr;
    if (posix_memalign (&p, static_cast<std::size_t> (alignment), size == 0 ? 1 : size) != 0)
        throw std::bad_alloc();
    return p;
}
void* operator new[] (std::size_t size, std::align_val_t alignment)
{
    return operator new (size, alignment);
}

void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept { std::free (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { std::free (p); }
void operator delete (void* p, std::align_val_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { std::free (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { std::free (p); }

namespace
{
// ------------------------------------------------------------------------- helpers

constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;

std::vector<std::byte> readFixture (const std::string& name)
{
    const auto path = std::filesystem::path (XERUM_DSP_FIXTURE_DIR) / name;
    std::ifstream in (path, std::ios::binary);
    REQUIRE (in.good());
    std::vector<std::byte> bytes (static_cast<std::size_t> (std::filesystem::file_size (path)));
    in.read (reinterpret_cast<char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
    return bytes;
}

Wavetable sineTable()
{
    const auto parsed = parseWavetable (readFixture ("sine-1frame.wav"));
    REQUIRE (parsed.ok);
    return parsed.table;
}

/// A settings block with exactly one wavetable oscillator, so a test's spectrum is that oscillator's.
VoiceSettings sineSettings (const Wavetable* table)
{
    VoiceSettings settings;
    auto& oscillator = settings.oscillators[0];
    oscillator.enabled = true;
    oscillator.table = table;
    oscillator.volume = 1.0;
    oscillator.unison = 1;
    oscillator.unisonRangeSemitones = 0.0;
    return settings;
}

/// Render `frames` samples in `blockSize` chunks. The engine overwrites each block, so the chunks are
/// disjoint ranges of the output.
void render (VoiceEngine& engine, std::vector<float>& out, int frames, int blockSize = 128)
{
    out.assign (static_cast<std::size_t> (frames), 0.0f);
    for (int i = 0; i < frames; i += blockSize)
    {
        const int n = std::min (blockSize, frames - i);
        engine.process (out.data() + i, n);
    }
}

std::vector<float> render (VoiceEngine& engine, int frames, int blockSize = 128)
{
    std::vector<float> out;
    render (engine, out, frames, blockSize);
    return out;
}

/// The magnitude spectrum over [minHz, maxHz] at `stepHz`, by direct DFT with a Hann window and an
/// incremental rotation (so the cost is a handful of multiplies per sample, not a transcendental).
std::vector<double> magnitudeSpectrum (const std::vector<float>& samples, double sampleRate, double minHz,
                                       double maxHz, double stepHz)
{
    const std::size_t n = samples.size();
    std::vector<double> window (n);
    for (std::size_t i = 0; i < n; ++i)
        window[i] = 0.5 * (1.0 - std::cos (2.0 * kPi * static_cast<double> (i) / static_cast<double> (n - 1)));

    std::vector<double> magnitudes;
    for (double f = minHz; f <= maxHz + 1e-9; f += stepHz)
    {
        const double omega = 2.0 * kPi * f / sampleRate;
        const double cw = std::cos (omega);
        const double sw = std::sin (omega);

        double real = 0.0;
        double imaginary = 0.0;
        double cr = 1.0;
        double ci = 0.0;

        for (std::size_t i = 0; i < n; ++i)
        {
            const double v = static_cast<double> (samples[i]) * window[i];
            real += v * cr;
            imaginary += v * ci;

            const double nextCr = cr * cw - ci * sw;
            ci = cr * sw + ci * cw;
            cr = nextCr;
        }

        magnitudes.push_back (std::sqrt (real * real + imaginary * imaginary));
    }

    return magnitudes;
}

/// Spectral peaks: a bin that is above `thresholdRatio` of the maximum and is a local maximum. This is
/// a *count of partials*, which is what the unison test needs — not a parameter read back.
std::size_t countSpectralPeaks (const std::vector<float>& samples, double sampleRate, double minHz,
                                double maxHz, double stepHz, double thresholdRatio)
{
    const auto magnitudes = magnitudeSpectrum (samples, sampleRate, minHz, maxHz, stepHz);

    double maximum = 0.0;
    for (const auto magnitude : magnitudes)
        maximum = std::max (maximum, magnitude);

    const double threshold = maximum * thresholdRatio;

    std::size_t peaks = 0;
    for (std::size_t i = 1; i + 1 < magnitudes.size(); ++i)
        if (magnitudes[i] > threshold && magnitudes[i] > magnitudes[i - 1]
            && magnitudes[i] >= magnitudes[i + 1])
            ++peaks;

    return peaks;
}

/// The strongest frequency in a band, for asserting a voice sounds the note it was told to.
double dominantFrequency (const std::vector<float>& samples, double sampleRate, double minHz, double maxHz)
{
    const double stepHz = 1.0;
    const auto magnitudes = magnitudeSpectrum (samples, sampleRate, minHz, maxHz, stepHz);

    std::size_t best = 0;
    for (std::size_t i = 1; i < magnitudes.size(); ++i)
        if (magnitudes[i] > magnitudes[best])
            best = i;

    return minHz + stepHz * static_cast<double> (best);
}

std::vector<float> analyseWindow (const std::vector<float>& rendered, int skip, int length)
{
    return std::vector<float> (rendered.begin() + skip, rendered.begin() + skip + length);
}

/// A voice's spectrum after its attack has settled: skip 0.1 s, analyse 0.4 s.
std::vector<float> settledSpectrumSource (VoiceEngine& engine)
{
    const auto rendered = render (engine, 24000);
    return analyseWindow (rendered, 4800, 19200);
}
} // namespace

// --------------------------------------------------------------------- stealing

TEST_CASE ("voice: stealing, all released, reuses idle voices in order", "[voice]")
{
    const auto table = sineTable();

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 4);

    auto settings = sineSettings (&table);
    settings.envelope.attack = 0.001;
    settings.envelope.release = 0.05;
    engine.setSettings (settings);

    for (int note = 60; note < 64; ++note)
        REQUIRE (engine.noteOn (note, 1.0) == note - 60);
    REQUIRE (engine.activeVoiceCount() == 4);

    for (int note = 60; note < 64; ++note)
        engine.noteOff (note);

    render (engine, 4800);   // 0.1 s: well past the 0.05 s release
    REQUIRE (engine.activeVoiceCount() == 0);

    // Every voice is idle and reusable, in index order.
    REQUIRE (engine.noteOn (70, 1.0) == 0);
    REQUIRE (engine.noteOn (71, 1.0) == 1);
    REQUIRE (engine.noteOn (72, 1.0) == 2);
    REQUIRE (engine.noteOn (73, 1.0) == 3);
    REQUIRE (engine.voice (3).note() == 73);
}

TEST_CASE ("voice: stealing, some releasing, takes the quietest releasing voice", "[voice]")
{
    const auto table = sineTable();

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 2);

    auto settings = sineSettings (&table);
    settings.envelope.attack = 0.001;
    settings.envelope.sustain = 1.0;
    settings.envelope.release = 1.0;   // slow, so the two releasing voices have different levels
    engine.setSettings (settings);

    REQUIRE (engine.noteOn (60, 1.0) == 0);
    REQUIRE (engine.noteOn (62, 1.0) == 1);
    render (engine, 4800);              // 0.1 s: both reach sustain

    // Release the *newer* voice first, so the quieter releasing voice is not the oldest.
    engine.noteOff (62);
    render (engine, 2400);              // 0.05 s: voice 1 is now ~0.95
    engine.noteOff (60);
    render (engine, 480);               // 0.01 s: voice 0 is now ~0.99

    REQUIRE (engine.voice (0).releasing());
    REQUIRE (engine.voice (1).releasing());
    REQUIRE (engine.voice (1).level() < engine.voice (0).level());

    // The quieter releasing voice is stolen — voice 1, which is the *newer* one. A policy that simply
    // took the oldest releasing voice would take voice 0.
    REQUIRE (engine.noteOn (67, 1.0) == 1);
    REQUIRE (engine.voice (1).stage() == VoiceStage::Stealing);
    REQUIRE (engine.voice (0).releasing());
    REQUIRE (engine.voice (0).note() == 60);

    // The fade is 2 ms; once it is over the pending note is playing.
    render (engine, 256);
    REQUIRE (engine.voice (1).note() == 67);
    REQUIRE (engine.voice (1).stage() == VoiceStage::On);
}

TEST_CASE ("voice: stealing, none releasing, takes the quietest voice not the oldest", "[voice]")
{
    const auto table = sineTable();

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 2);

    auto settings = sineSettings (&table);
    settings.envelope.attack = 1.0;     // slow attack, so the two held voices sit at different levels
    settings.envelope.sustain = 1.0;
    engine.setSettings (settings);

    REQUIRE (engine.noteOn (60, 1.0) == 0);
    render (engine, 28800);             // 0.6 s: voice 0 at ~0.6
    REQUIRE (engine.noteOn (62, 1.0) == 1);
    render (engine, 4800);              // 0.1 s: voice 1 at ~0.1, voice 0 at ~0.7

    REQUIRE_FALSE (engine.voice (0).releasing());
    REQUIRE_FALSE (engine.voice (1).releasing());
    REQUIRE (engine.voice (0).level() > engine.voice (1).level());

    // Quietest wins; the oldest voice (0) is left alone.
    REQUIRE (engine.noteOn (64, 1.0) == 1);
    REQUIRE (engine.voice (0).note() == 60);
}

TEST_CASE ("voice: stealing, none releasing and levels tied, takes the oldest", "[voice]")
{
    const auto table = sineTable();

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 4);

    auto settings = sineSettings (&table);
    settings.envelope.attack = 0.001;
    settings.envelope.sustain = 1.0;
    engine.setSettings (settings);

    REQUIRE (engine.noteOn (60, 1.0) == 0);
    REQUIRE (engine.noteOn (62, 1.0) == 1);
    REQUIRE (engine.noteOn (64, 1.0) == 2);
    REQUIRE (engine.noteOn (65, 1.0) == 3);
    render (engine, 512);               // all four reach sustain

    for (int i = 0; i < 4; ++i)
        REQUIRE (engine.voice (i).level() == Catch::Approx (1.0));
    for (int i = 0; i < 4; ++i)
        REQUIRE_FALSE (engine.voice (i).releasing());

    // Equal levels: the age tie-break picks the oldest.
    REQUIRE (engine.noteOn (67, 1.0) == 0);
    REQUIRE (engine.voice (1).note() == 62);
}

// ------------------------------------------------------------------ mono / legato

TEST_CASE ("voice: monophonic mode uses one voice and falls back to the held note", "[voice]")
{
    const auto table = sineTable();

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 16);

    auto settings = sineSettings (&table);
    settings.envelope.attack = 0.001;
    settings.envelope.release = 0.1;
    settings.mode = voiceModeFor (/* mono */ true, /* legato */ false);
    REQUIRE (settings.mode == VoiceMode::Mono);
    engine.setSettings (settings);

    REQUIRE (engine.noteOn (60, 1.0) == 0);
    render (engine, 256);
    REQUIRE (engine.activeVoiceCount() == 1);
    REQUIRE (engine.voice (0).note() == 60);
    REQUIRE (engine.currentNote() == 60);

    // A second note still uses the single voice, and retriggers it.
    REQUIRE (engine.noteOn (64, 1.0) == 0);
    render (engine, 256);
    REQUIRE (engine.activeVoiceCount() == 1);
    REQUIRE (engine.voice (0).note() == 64);
    REQUIRE (engine.currentNote() == 64);

    // Releasing the top note falls back to the note still held.
    engine.noteOff (64);
    render (engine, 256);
    REQUIRE (engine.activeVoiceCount() == 1);
    REQUIRE (engine.voice (0).note() == 60);
    REQUIRE (engine.currentNote() == 60);

    engine.noteOff (60);
    render (engine, 9600);
    REQUIRE (engine.activeVoiceCount() == 0);
}

TEST_CASE ("voice: legato mode changes pitch without retriggering the envelope", "[voice]")
{
    const auto table = sineTable();

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 16);

    auto settings = sineSettings (&table);
    settings.envelope.attack = 1.0;     // slow, so a retrigger would be obvious as a level reset
    settings.envelope.sustain = 1.0;
    settings.mode = voiceModeFor (/* mono */ true, /* legato */ true);
    REQUIRE (settings.mode == VoiceMode::MonoLegato);
    engine.setSettings (settings);

    REQUIRE (engine.noteOn (60, 1.0) == 0);
    render (engine, 14400);             // 0.3 s of the 1 s attack
    const double levelBefore = engine.voice (0).level();
    REQUIRE (levelBefore == Catch::Approx (0.3).margin (0.02));

    REQUIRE (engine.noteOn (64, 1.0) == 0);

    // The envelope keeps running: a retrigger would have reset it to ~0.
    REQUIRE (engine.voice (0).level() > levelBefore - 0.01);
    REQUIRE (engine.voice (0).note() == 64);
    REQUIRE (engine.activeVoiceCount() == 1);

    // A mono (non-legato) engine would have restarted the envelope here.
    auto retriggering = sineSettings (&table);
    retriggering.envelope.attack = 1.0;
    retriggering.mode = VoiceMode::Mono;
    VoiceEngine mono;
    mono.prepare (kSampleRate, 128, 16);
    mono.setSettings (retriggering);
    REQUIRE (mono.noteOn (60, 1.0) == 0);
    render (mono, 14400);
    REQUIRE (mono.voice (0).level() > 0.25);
    REQUIRE (mono.noteOn (64, 1.0) == 0);
    REQUIRE (mono.voice (0).level() < 0.05);
}

TEST_CASE ("voice: portamento glides the pitch over the portamento time", "[voice]")
{
    const auto table = sineTable();

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 16);

    auto settings = sineSettings (&table);
    settings.envelope.attack = 0.001;
    settings.mode = VoiceMode::MonoLegato;
    settings.portamentoTime = 0.2;
    engine.setSettings (settings);

    REQUIRE (engine.noteOn (60, 1.0) == 0);
    render (engine, 512);
    REQUIRE (engine.voice (0).pitchSemitones() == Catch::Approx (-9.0));   // 60 - 69

    // Glide to 72 (note-semitone +3).
    REQUIRE (engine.noteOn (72, 1.0) == 0);
    render (engine, 4800);   // 0.1 s, half the glide
    const double half = engine.voice (0).pitchSemitones();
    REQUIRE (half > -9.0);
    REQUIRE (half < 3.0);

    render (engine, 9600);   // the glide completes
    REQUIRE (engine.voice (0).pitchSemitones() == Catch::Approx (3.0).margin (0.01));
}

TEST_CASE ("voice: the sustain pedal defers note-off until it lifts", "[voice]")
{
    const auto table = sineTable();

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 4);

    auto settings = sineSettings (&table);
    settings.envelope.attack = 0.001;
    settings.envelope.release = 0.1;
    engine.setSettings (settings);

    engine.setSustainPedal (true);
    REQUIRE (engine.noteOn (60, 1.0) == 0);
    render (engine, 256);

    engine.noteOff (60);
    render (engine, 2400);
    REQUIRE (engine.voice (0).stage() == VoiceStage::On);   // held by the pedal, not releasing

    engine.setSustainPedal (false);
    REQUIRE (engine.voice (0).releasing());
    render (engine, 9600);
    REQUIRE (engine.activeVoiceCount() == 0);
}

TEST_CASE ("voice: a stolen voice is faded, not hard cut", "[voice]")
{
    const auto table = sineTable();

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 1);

    auto settings = sineSettings (&table);
    settings.envelope.attack = 0.001;
    settings.envelope.release = 1.0;
    engine.setSettings (settings);

    REQUIRE (engine.noteOn (60, 1.0) == 0);
    const auto sounding = render (engine, 4800);

    // Steal with a new note. Note 60 at 48 kHz has advanced ~26.16 cycles in 4800 samples, so the
    // waveform right before the steal is near a peak — a hard cut here would step by nearly full scale.
    REQUIRE (engine.noteOn (67, 1.0) == 0);
    const auto stolen = render (engine, 480);   // 10 ms, covering the 2 ms fade

    const double fullScale = *std::max_element (sounding.begin(), sounding.end());
    REQUIRE (fullScale > 0.1);

    // Continuity across the steal boundary as well as within the stolen block.
    double maxStep = std::abs (static_cast<double> (stolen[0] - sounding.back()));
    for (std::size_t i = 1; i < stolen.size(); ++i)
        maxStep = std::max (maxStep, std::abs (static_cast<double> (stolen[i] - stolen[i - 1])));

    REQUIRE (maxStep < fullScale * 0.25);
}

TEST_CASE ("voice: a note released before its steal completes never starts", "[voice]")
{
    const auto table = sineTable();

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 1);

    auto settings = sineSettings (&table);
    settings.envelope.attack = 0.001;
    settings.envelope.release = 0.05;
    engine.setSettings (settings);

    REQUIRE (engine.noteOn (60, 1.0) == 0);
    render (engine, 480);

    // Steal the only voice; 72 is pending behind the 2 ms fade and has not started yet.
    REQUIRE (engine.noteOn (72, 1.0) == 0);
    REQUIRE (engine.voice (0).pendingNote() == 72);

    // Its note-off arrives first: it must be cancelled rather than start and hang.
    engine.noteOff (72);
    REQUIRE (engine.voice (0).pendingNote() == -1);

    render (engine, 960);   // well past the fade
    REQUIRE_FALSE (engine.voice (0).active());
    REQUIRE (engine.activeVoiceCount() == 0);
}

// ------------------------------------------------------------------------ unison

TEST_CASE ("voice: unison produces N measured partials", "[voice]")
{
    const auto table = sineTable();

    struct Case
    {
        int unison;
        double range;
    };

    for (const Case test : { Case { 1, 12.0 }, Case { 2, 12.0 }, Case { 4, 12.0 }, Case { 8, 12.0 } })
    {
        VoiceEngine engine;
        engine.prepare (kSampleRate, 128, 8);

        auto settings = sineSettings (&table);
        settings.envelope.attack = 0.005;
        settings.envelope.sustain = 1.0;
        settings.oscillators[0].unison = test.unison;
        settings.oscillators[0].unisonRangeSemitones = test.range;
        engine.setSettings (settings);

        REQUIRE (engine.noteOn (57, 1.0) == 0);   // 57 = A3 = 220 Hz
        const auto spectrum = settledSpectrumSource (engine);

        const auto peaks = countSpectralPeaks (spectrum, kSampleRate, 60.0, 700.0, 1.0, 0.25);

        INFO ("unison " << test.unison << " measured " << peaks << " partials");
        REQUIRE (peaks == static_cast<std::size_t> (test.unison));

        if (test.unison == 1)
            REQUIRE (dominantFrequency (spectrum, kSampleRate, 100.0, 400.0) == Catch::Approx (220.0).margin (1.0));
    }
}

TEST_CASE ("voice: unison detunes the partials across the range", "[voice]")
{
    const auto table = sineTable();

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 8);

    auto settings = sineSettings (&table);
    settings.envelope.attack = 0.005;
    settings.envelope.sustain = 1.0;
    settings.oscillators[0].unison = 4;
    settings.oscillators[0].unisonRangeSemitones = 12.0;
    engine.setSettings (settings);

    REQUIRE (engine.noteOn (57, 1.0) == 0);
    const auto spectrum = settledSpectrumSource (engine);

    // The four partials of a 12-semitone spread around 220 Hz: 155.6, 196.0, 246.9, 311.1.
    const double expected[] = { 220.0 * std::exp2 (-6.0 / 12.0), 220.0 * std::exp2 (-2.0 / 12.0),
                                220.0 * std::exp2 (2.0 / 12.0), 220.0 * std::exp2 (6.0 / 12.0) };

    for (const double frequency : expected)
    {
        const auto magnitudes = magnitudeSpectrum (spectrum, kSampleRate, frequency - 2.0, frequency + 2.0, 0.5);
        const double atPeak = *std::max_element (magnitudes.begin(), magnitudes.end());
        const auto midpoint = magnitudeSpectrum (spectrum, kSampleRate, 220.0, 220.0, 1.0).front();
        REQUIRE (atPeak > midpoint * 5.0);
    }
}

// -------------------------------------------------------------------- modulation

TEST_CASE ("voice: the matrix reaches the voice's pitch, measured", "[voice]")
{
    const auto table = sineTable();

    // One row: Mod Wheel -> OSC A pitch, 50%. With the wheel at 1.0 the row contributes 0.5 of the
    // destination's range, and the voice takes a pitch unit as 12 semitones, so the note moves up six
    // semitones: 220 Hz -> 311.1 Hz.
    const auto spectrumFor = [&table] (double modWheel)
    {
        auto engine = std::make_unique<VoiceEngine>();
        engine->prepare (kSampleRate, 128, 4);

        auto settings = sineSettings (&table);
        settings.envelope.attack = 0.005;
        settings.envelope.sustain = 1.0;
        engine->setSettings (settings);

        ModSlot slot;
        slot.used = true;
        slot.source = modSourceFromName ("Mod Wheel");
        REQUIRE (slot.source > 0);
        slot.amount = 50.0;
        slot.destinationClass = std::string (voiceDestination::kOscillatorClass);
        slot.destinationInstance = 0;
        slot.destinationParameter = voiceDestination::kParamPitch;
        slot.destinationName = "kParamPitch";
        engine->setModulationSlots (std::vector<ModSlot> { slot });

        ModulationInputs globals;
        globals.modWheel = modWheel;
        engine->setGlobalInputs (globals);

        REQUIRE (engine->noteOn (57, 1.0) == 0);   // A3, 220 Hz
        return settledSpectrumSource (*engine);
    };

    const double atRest = dominantFrequency (spectrumFor (0.0), kSampleRate, 100.0, 600.0);
    const double fullyModulated = dominantFrequency (spectrumFor (1.0), kSampleRate, 100.0, 600.0);

    REQUIRE (atRest == Catch::Approx (220.0).margin (1.0));
    REQUIRE (fullyModulated == Catch::Approx (220.0 * std::exp2 (6.0 / 12.0)).margin (2.0));
}

// ------------------------------------------------------ the real-time-safety invariant

TEST_CASE ("voice: no heap allocation on the audio thread", "[voice]")
{
    const auto table = sineTable();

    // Constructed and prepared with tracking off: the pool is allowed to allocate, off the audio thread.
    auto engine = std::make_unique<VoiceEngine>();
    engine->prepare (kSampleRate, 128, 16);

    auto settings = sineSettings (&table);
    settings.envelope.attack = 0.001;
    settings.envelope.release = 0.05;
    engine->setSettings (settings);

    // A live row, so the render really moves through `beginBlock`/`rampFor`: Mod Wheel -> OSC A pitch.
    ModSlot slot;
    slot.used = true;
    slot.source = modSourceFromName ("Mod Wheel");
    REQUIRE (slot.source > 0);
    slot.amount = 50.0;
    slot.destinationClass = std::string (voiceDestination::kOscillatorClass);
    slot.destinationInstance = 0;
    slot.destinationParameter = voiceDestination::kParamPitch;
    slot.destinationName = "kParamPitch";
    engine->setModulationSlots (std::vector<ModSlot> { slot });

    ModulationInputs globals;
    globals.modWheel = 0.5;
    engine->setGlobalInputs (globals);

    std::array<float, 128> block {};

    g_allocations.store (0, std::memory_order_relaxed);
    g_tracking.store (true, std::memory_order_relaxed);

    for (int i = 0; i < 400; ++i)
    {
        if (i % 32 == 0)
            engine->noteOn (48 + (i % 24), 1.0);
        if (i % 48 == 0)
            engine->noteOff (48 + ((i + 16) % 24));

        engine->process (block.data(), static_cast<int> (block.size()));
    }

    // Change settings and the sustain pedal during the render too: neither may allocate.
    engine->setSettings (settings);
    engine->setSustainPedal (true);
    engine->noteOn (60, 1.0);
    engine->process (block.data(), static_cast<int> (block.size()));
    engine->setSustainPedal (false);

    g_tracking.store (false, std::memory_order_relaxed);

    REQUIRE (g_allocations.load (std::memory_order_relaxed) == 0);

    // The row really reached its destination, so the zero above is the whole modulated render path.
    REQUIRE (engine->modulation().modulatedDestinationCount() > 0);
    const auto ramp = engine->modulation().rampFor (voiceDestination::kOscillatorClass, 0,
                                                    voiceDestination::kParamPitch);
    REQUIRE ((ramp.start != 0.0 || ramp.end != 0.0));

    // Positive control: the hook really does see allocations, so the zero above is not a broken counter.
    // The size comes from a volatile and the call is made directly, so the optimiser cannot elide the
    // allocation away.
    {
        g_allocations.store (0, std::memory_order_relaxed);
        g_tracking.store (true, std::memory_order_relaxed);
        volatile std::size_t positiveControlSize = 64;
        void* deliberate = ::operator new (positiveControlSize);
        g_tracking.store (false, std::memory_order_relaxed);
        ::operator delete (deliberate);
        REQUIRE (g_allocations.load (std::memory_order_relaxed) > 0);
    }
}

TEST_CASE ("voice: a rendered note carries the fundamental of the note played", "[voice]")
{
    const auto table = sineTable();

    VoiceEngine engine;
    engine.prepare (kSampleRate, 128, 8);
    engine.setSettings (sineSettings (&table));

    REQUIRE (engine.noteOn (69, 1.0) == 0);   // A4
    const auto spectrum = settledSpectrumSource (engine);

    REQUIRE (dominantFrequency (spectrum, kSampleRate, 300.0, 600.0) == Catch::Approx (440.0).margin (1.0));
}
