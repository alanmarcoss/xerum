// Ticket 48: the modulation generators — the ten LFOs, the four matrix envelopes, and the audio-rate
// sources the voice supplies.
//
// This file is the generators' own arithmetic: a drawn shape sampled as the preset drew it, the envelope
// state machine's stages and their curves, the rate/direction/swing/delay/rise of an LFO, the source a
// warp mode's name encodes. The *end-to-end* assertions — a preset driving each of these through the
// plugin and changing its render — live in `plugin/tests/mod_sources_test.cpp`, and the real-time
// invariant lives in `voice_test.cpp` beside the allocation counter it reuses.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mod_sources.h"
#include "voice.h"
#include "wavetable.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace xerum::dsp;

namespace
{
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

/// One LFO reading a row onto the filter cutoff, so a render difference is measurable.
ModSlot cutoffRow (const int source)
{
    ModSlot slot;
    slot.used = true;
    slot.source = source;
    slot.amount = 100.0;
    slot.destinationClass = std::string (voiceDestination::kVoiceFilterClass);
    slot.destinationInstance = 0;
    slot.destinationParameter = voiceDestination::kParamFreq;
    slot.destinationName = "kParamFreq";
    return slot;
}

VoiceSettings filteredSettings (const Wavetable* table)
{
    VoiceSettings settings;
    auto& oscillator = settings.oscillators[0];
    oscillator.enabled = true;
    oscillator.table = table;
    oscillator.volume = 1.0;
    oscillator.unison = 1;
    settings.envelope.attack = 0.001;
    settings.envelope.sustain = 1.0;
    settings.filters[0].enabled = true;
    settings.filters[0].spec = filterSpecFromName ("L12");
    settings.filters[0].cutoff = 0.3;
    settings.filters[0].resonance = 30.0;
    return settings;
}

std::vector<float> renderEngine (VoiceEngine& engine, const int totalFrames, const int block = 128)
{
    std::vector<float> left (static_cast<std::size_t> (totalFrames));
    std::vector<float> right (static_cast<std::size_t> (totalFrames));

    for (int done = 0; done < totalFrames; done += block)
        engine.process (left.data() + done, right.data() + done, {}, std::min (block, totalFrames - done));

    return left;
}

double difference (const std::vector<float>& a, const std::vector<float>& b)
{
    double worst = 0.0;

    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        worst = std::max (worst, std::abs (static_cast<double> (a[i]) - static_cast<double> (b[i])));

    return worst;
}

LfoCurve rampCurve()
{
    // A fast rise followed by a flat top: distinct from every built-in shape, which is what makes the
    // "a drawn shape differs from a built-in one" assertion below mean something (a plain ramp *is* the
    // built-in `saw up`).
    LfoCurve curve;
    curve.add (0.0, 0.0, 0.5);
    curve.add (0.25, 1.0, 0.5);
    curve.add (1.0, 1.0, 0.5);
    return curve;
}
} // namespace

// ---------------------------------------------------------------- the drawn shape

TEST_CASE ("a drawn LFO shape plays back as drawn", "[modsources][lfo]")
{
    LfoCurve curve;
    curve.add (0.0, 0.0, 0.5);
    curve.add (0.5, 0.5, 0.5);
    curve.add (1.0, 1.0, 0.5);

    // The points are the shape: the value at each drawn point is the value the preset stored.
    CHECK (curve.sampleCurve (0.0) == Catch::Approx (0.0));
    CHECK (curve.sampleCurve (0.5) == Catch::Approx (0.5));
    CHECK (curve.sampleCurve (1.0) == Catch::Approx (1.0));

    // ... and between them it is the segment the preset drew, not a step.
    CHECK (curve.sampleCurve (0.25) == Catch::Approx (0.25));
    CHECK (curve.sampleCurve (0.75) == Catch::Approx (0.75));

    // A **repeated x is a vertical edge**, which is how the factory bank draws a hard step: `LD - Artform`'s
    // LFO 1 carries `xVals 0.125, 0.125` with `yVals 0.0966, 1.0`. The later point is the value after it.
    LfoCurve stepped;
    stepped.add (0.0, 0.0, 0.5);
    stepped.add (0.5, 0.2, 0.5);
    stepped.add (0.5, 0.8, 0.5);
    stepped.add (1.0, 1.0, 0.5);

    CHECK (stepped.sampleCurve (0.4999) == Catch::Approx (0.2).margin (0.001));
    CHECK (stepped.sampleCurve (0.5) == Catch::Approx (0.8));
    CHECK (stepped.sampleCurve (0.75) == Catch::Approx (0.9));

    // The segment's own curve bends it, and 50 is the straight line the factory bank stores for almost
    // every shape (`kParamCurve1...3` are 50; `curveVals` matches).
    LfoCurve straight;
    straight.add (0.0, 0.0, 0.5);
    straight.add (1.0, 1.0, 0.5);

    LfoCurve bowed;
    bowed.add (0.0, 0.0, 100.0);
    bowed.add (1.0, 1.0, 100.0);

    LfoCurve sagging;
    sagging.add (0.0, 0.0, 0.0);
    sagging.add (1.0, 1.0, 0.0);

    CHECK (straight.sampleCurve (0.5) == Catch::Approx (0.5));
    CHECK (bowed.sampleCurve (0.5) == Catch::Approx (std::sqrt (0.5)));
    CHECK (sagging.sampleCurve (0.5) == Catch::Approx (0.25));

    // The bend moves the midpoint and not the endpoints, which is what makes it a curve rather than a
    // different line.
    CHECK (bowed.sampleCurve (1.0) == Catch::Approx (1.0));
    CHECK (sagging.sampleCurve (0.0) == Catch::Approx (0.0));
}

TEST_CASE ("the schema's own point arrays are read as the reference stores them", "[modsources][lfo]")
{
    // A preset stores `numPoints + 1` entries: the last repeats the first point's y at x == 1 to close the
    // cycle. `LD - Artform`'s LFO 1 is `numPoints` 14 with 15 entries. The reader walks while the leaves
    // exist rather than trusting the count, so this asserts what a *shape* looks like once read.
    LfoCurve closed;
    closed.closed = true;
    closed.add (1.0, 0.0, 0.5);
    closed.add (0.23268698060941828, 0.6253446691176471, 0.5);

    // A closed path of two points traverses both ways: down and back, which is what `isOpen == false`
    // means for the corpus's own 2-point paths.
    CHECK (closed.samplePathY (0.0) == Catch::Approx (0.0));
    CHECK (closed.samplePathY (0.25) == Catch::Approx (0.3125).margin (0.01));
    CHECK (closed.samplePathY (0.5) == Catch::Approx (0.625).margin (0.01));
    CHECK (closed.samplePathY (0.75) == Catch::Approx (0.3125).margin (0.01));

    auto open = closed;
    open.closed = false;

    // An open path traverses once and stops: at the end of the cycle it is at its last point, not back at
    // the first.
    CHECK (open.samplePathY (1.0) == Catch::Approx (0.625).margin (0.01));
}

TEST_CASE ("a drawn shape differs from the built-in one, and the built-ins are the corpus's own", "[modsources][lfo]")
{
    LfoSettings drawn;
    drawn.curve = rampCurve();
    REQUIRE (drawn.hasDrawnCurve());

    // The built-in shapes are the `curveDisplayName` vocabulary's, at the corpus's own phase: a sine is at
    // its centre when its cycle starts, which is what makes the 0..1 source range mean `2s - 1` for a
    // bipolar row.
    LfoSettings sine;
    sine.shape = LfoShape::Sine;

    CHECK (lfoValue (sine, 0.0, 0) == Catch::Approx (0.5));
    CHECK (lfoValue (sine, 0.25, 0) == Catch::Approx (1.0));
    CHECK (lfoValue (sine, 0.75, 0) == Catch::Approx (0.0));

    LfoSettings triangle;
    triangle.shape = LfoShape::Triangle;

    CHECK (lfoValue (triangle, 0.0, 0) == Catch::Approx (0.5));
    CHECK (lfoValue (triangle, 0.25, 0) == Catch::Approx (1.0));
    CHECK (lfoValue (triangle, 0.75, 0) == Catch::Approx (0.0));

    LfoSettings sawUp;
    sawUp.shape = LfoShape::SawUp;

    CHECK (lfoValue (sawUp, 0.0, 0) == Catch::Approx (0.0));
    CHECK (lfoValue (sawUp, 0.5, 0) == Catch::Approx (0.5));

    LfoSettings sawDown;
    sawDown.shape = LfoShape::SawDown;

    CHECK (lfoValue (sawDown, 0.0, 0) == Catch::Approx (1.0));
    CHECK (lfoValue (sawDown, 0.5, 0) == Catch::Approx (0.5));

    LfoSettings square;
    square.shape = LfoShape::Square;

    CHECK (lfoValue (square, 0.25, 0) == Catch::Approx (1.0));
    CHECK (lfoValue (square, 0.75, 0) == Catch::Approx (0.0));

    // **A drawn shape differs from every built-in one**, which is the acceptance's synthetic-shape clause.
    for (const auto shape : { LfoShape::Sine, LfoShape::Triangle, LfoShape::SawUp, LfoShape::Square })
    {
        LfoSettings builtIn;
        builtIn.shape = shape;

        double worst = 0.0;

        for (int i = 0; i < 100; ++i)
        {
            const auto phase = static_cast<double> (i) / 100.0;
            worst = std::max (worst, std::abs (lfoValue (drawn, phase, 0) - lfoValue (builtIn, phase, 0)));
        }

        CHECK (worst > 0.1);
    }

    // A drawn curve takes precedence over the named shape beside it: the preset that names a library shape
    // also carries its points, and the points are the shape.
    LfoSettings named;
    named.shape = LfoShape::Square;
    named.curve = rampCurve();

    LfoSettings squareOnly;
    squareOnly.shape = LfoShape::Square;

    CHECK (lfoValue (named, 0.5, 0) == Catch::Approx (1.0));
    CHECK (lfoValue (named, 0.5, 0) != Catch::Approx (lfoValue (squareOnly, 0.5, 0)));
}

TEST_CASE ("an LFO's rate, direction, phase and swing shape its cycle", "[modsources][lfo]")
{
    // `kParamRate` percent to hertz is a stated convention; what is asserted here is that it is monotone,
    // that zero is silence-slow, and that the reference's own 10x flag multiplies it.
    CHECK (lfoRateHz (0.0, false) == Catch::Approx (0.0));
    CHECK (lfoRateHz (50.0, false) == Catch::Approx (10.0));
    CHECK (lfoRateHz (100.0, false) == Catch::Approx (20.0));
    CHECK (lfoRateHz (50.0, true) == Catch::Approx (100.0));
    CHECK (lfoRateHz (2.5, false) == Catch::Approx (0.5));   // the corpus's median rate

    LfoSettings up;
    up.shape = LfoShape::SawUp;

    LfoSettings down = up;
    down.direction = -1.0;

    CHECK (lfoValue (down, 0.0, 0) == Catch::Approx (1.0));
    CHECK (lfoValue (down, 0.25, 0) == Catch::Approx (0.75));

    LfoSettings shifted = up;
    shifted.phaseTurns = 0.25;

    CHECK (lfoValue (shifted, 0.0, 0) == Catch::Approx (0.25));

    // Swing lengthens the first half of the cycle, so a saw reaches its halfway value *later* than the
    // middle: at half swing that is three quarters of the way through the cycle.
    LfoSettings swung = up;
    swung.swing = 0.5;

    CHECK (lfoValue (swung, 0.75, 0) == Catch::Approx (0.5).margin (0.01));
    CHECK (lfoValue (swung, 0.5, 0) == Catch::Approx (1.0 / 3.0).margin (0.01));
    CHECK (lfoValue (swung, 0.0, 0) == Catch::Approx (0.0));

    // The second output follows a drawn path when one is there, and the curve when it is not — so
    // `LFO n Y` moves for a preset that never drew a path instead of sitting at zero.
    LfoSettings pathOnly = up;
    pathOnly.path.closed = false;
    pathOnly.path.add (0.0, 0.2, 0.5);
    pathOnly.path.add (1.0, 0.9, 0.5);

    CHECK (lfoSecondValue (pathOnly, 0.0, 0) == Catch::Approx (0.2));
    CHECK (lfoSecondValue (pathOnly, 0.999, 0) == Catch::Approx (0.9).margin (0.01));
    CHECK (lfoSecondValue (pathOnly, 0.5, 0) == Catch::Approx (0.55).margin (0.01));
    CHECK (lfoSecondValue (up, 0.25, 0) == Catch::Approx (lfoValue (up, 0.25, 0)));
}

TEST_CASE ("the sample-and-hold LFO type holds one value per cycle", "[modsources][lfo]")
{
    LfoSettings hold;
    hold.type = LfoType::RandomSampleHold;

    const auto first = lfoValue (hold, 0.1, 0);
    const auto second = lfoValue (hold, 0.4, 0);
    const auto third = lfoValue (hold, 0.1, 1);

    CHECK (first == Catch::Approx (second));
    CHECK (first >= 0.0);
    CHECK (first <= 1.0);
    CHECK (first != Catch::Approx (third));

    // Deterministic on purpose: two identical renders must not diverge.
    CHECK (third == Catch::Approx (lfoValue (hold, 0.9, 1)));
}

// ------------------------------------------------------------------- the envelopes

TEST_CASE ("a matrix envelope runs its own stages, independently of the amplitude envelope",
           "[modsources][envelope]")
{
    MatrixEnvelope envelope;
    envelope.prepare (kSampleRate);

    MatrixEnvelopeSettings settings;
    settings.attack = 0.01;
    settings.hold = 0.01;
    settings.decay = 0.01;
    settings.sustain = 0.4;
    settings.release = 0.01;
    settings.curve1 = 50.0;
    settings.curve2 = 50.0;
    settings.curve3 = 50.0;
    settings.start = 0.0;
    settings.end = 0.0;

    CHECK (envelope.idle());

    envelope.trigger (settings, false);

    // The stages the reference's `Env{N}/plainParams` describe, in order: attack to full, hold, decay to
    // sustain, sustain, release to the end level — and each is a *level*, measured.
    const auto block = [&] (const int frames)
    {
        double level = 0.0;

        for (int done = 0; done < frames; ++done)
            level = envelope.advance (settings, 1);

        return level;
    };

    CHECK (block (240) == Catch::Approx (0.5).margin (0.02));    // half way through a 10 ms attack
    CHECK (block (240) == Catch::Approx (1.0).margin (0.01));    // attack complete

    CHECK (block (480) == Catch::Approx (1.0).margin (0.01));    // hold: still full
    CHECK (block (240) == Catch::Approx (0.7).margin (0.02));    // half way down the decay

    envelope.advance (settings, 10000);
    CHECK (envelope.level() == Catch::Approx (0.4).margin (0.01));   // sustain
    CHECK_FALSE (envelope.idle());

    envelope.release (settings);
    CHECK (block (240) == Catch::Approx (0.2).margin (0.02));    // half way down the release
    CHECK (block (480) == Catch::Approx (0.0).margin (0.01));
    CHECK (envelope.idle());

    // The segment curves are the reference's own control: 50 is a straight line, and the two extremes bend
    // it the two ways.
    CHECK (shapedSegment (0.5, 50.0) == Catch::Approx (0.5));
    CHECK (shapedSegment (0.5, 100.0) == Catch::Approx (std::sqrt (0.5)));
    CHECK (shapedSegment (0.5, 0.0) == Catch::Approx (0.25));
    CHECK (shapedSegment (0.0, 0.0) == Catch::Approx (0.0));
    CHECK (shapedSegment (1.0, 100.0) == Catch::Approx (1.0));

    // `start` and `end` are levels rather than times: the attack begins where the preset says and the
    // release ends there too.
    MatrixEnvelopeSettings lifted = settings;
    lifted.start = 0.3;
    lifted.end = 0.1;

    MatrixEnvelope other;
    other.prepare (kSampleRate);
    other.trigger (lifted, false);

    CHECK (other.level() == Catch::Approx (0.3));
    other.advance (lifted, 10000);
    other.release (lifted);
    other.advance (lifted, 100000);

    CHECK (other.level() == Catch::Approx (0.1));

    // A legato-inverted envelope does not restart on a legato note change — which is what the flag is for.
    MatrixEnvelope legato;
    legato.prepare (kSampleRate);

    auto inverted = settings;
    inverted.legatoInverted = true;

    legato.trigger (inverted, false);
    legato.advance (inverted, 10000);
    const auto atSustain = legato.level();

    legato.trigger (inverted, /* legato */ true);
    CHECK (legato.level() == Catch::Approx (atSustain));

    legato.trigger (inverted, /* legato */ false);
    CHECK (legato.level() == Catch::Approx (0.0).margin (0.01));
}

// -------------------------------------------------------------- the engine's wiring

TEST_CASE ("an LFO setting reaches a destination through the engine, and the render moves", "[modsources][lfo]")
{
    const auto table = sineTable();

    const auto renderWithRate = [&table] (const double rateHz)
    {
        VoiceEngine engine;
        engine.prepare (kSampleRate, 128, 2);
        engine.setSettings (filteredSettings (&table));
        engine.setModulationSlots (std::vector<ModSlot> { cutoffRow (xerum::dsp::modSourceFromName ("LFO 1")) });

        ModSourceSettings sources;
        sources.lfos[0].rateHz = rateHz;
        sources.lfos[0].shape = LfoShape::SawUp;
        engine.setModulationSources (sources);

        REQUIRE (engine.noteOn (57, 1.0) == 0);
        return renderEngine (engine, 8192);
    };

    const auto slow = renderWithRate (1.0);
    const auto fast = renderWithRate (20.0);

    CHECK (difference (slow, fast) > 0.01);

    // With no source settings at all the LFO still runs — its default rate and shape — so the difference
    // above is the *rate* and not "an LFO exists".
    VoiceEngine quiet;
    quiet.prepare (kSampleRate, 128, 2);
    quiet.setSettings (filteredSettings (&table));
    quiet.setModulationSlots (std::vector<ModSlot> { cutoffRow (xerum::dsp::modSourceFromName ("LFO 1")) });
    REQUIRE (quiet.noteOn (57, 1.0) == 0);
    const auto defaulted = renderEngine (quiet, 8192);

    CHECK (difference (defaulted, fast) > 0.005);
    CHECK (difference (defaulted, slow) > 0.005);
}

TEST_CASE ("a macro reaches a destination through the engine's global inputs", "[modsources][macros]")
{
    const auto table = sineTable();

    const auto renderWithMacro = [&table] (const double value)
    {
        VoiceEngine engine;
        engine.prepare (kSampleRate, 128, 2);
        engine.setSettings (filteredSettings (&table));
        engine.setModulationSlots (std::vector<ModSlot> { cutoffRow (xerum::dsp::modSourceFromName ("Macro 1")) });

        ModulationInputs globals;
        globals.macro[0] = value;
        engine.setGlobalInputs (globals);

        REQUIRE (engine.noteOn (57, 1.0) == 0);
        return renderEngine (engine, 8192);
    };

    CHECK (difference (renderWithMacro (0.0), renderWithMacro (0.8)) > 0.01);
}

TEST_CASE ("the voice hands a source-driven warp its modulator, and reads the right signal", "[modsources][warp]")
{
    const auto table = sineTable();

    const auto renderWith = [&table] (const WarpMode mode, const WarpSource source, const bool secondOscillator)
    {
        VoiceEngine engine;
        engine.prepare (kSampleRate, 128, 2);

        auto settings = filteredSettings (&table);
        settings.oscillators[0].warpMode[0] = mode;
        settings.oscillators[0].warpAmount[0] = 0.9;
        settings.oscillators[0].warpSource[0] = source;

        if (secondOscillator)
        {
            settings.oscillators[1].enabled = true;
            settings.oscillators[1].table = &table;
            settings.oscillators[1].volume = 0.8;
            settings.oscillators[1].pitchSemitones = 7.0;
        }

        engine.setSettings (settings);
        REQUIRE (engine.noteOn (57, 1.0) == 0);
        return renderEngine (engine, 8192);
    };

    const auto unwarped = renderWith (WarpMode::None, WarpSource::None, true);
    const auto modulated = renderWith (WarpMode::FrequencyModulation, WarpSource::OtherOsc, true);
    const auto silentSource = renderWith (WarpMode::FrequencyModulation, WarpSource::OtherOsc, false);
    const auto unwarpedSilent = renderWith (WarpMode::None, WarpSource::None, false);

    // The warp is in the signal path: the modulator another oscillator supplies reaches it.
    CHECK (difference (unwarped, modulated) > 0.01);

    // ... and it is *that* oscillator: with the second slot empty the modulator is zero, and a
    // frequency-modulated phase with a zero modulator is the unwarped phase exactly.
    CHECK (silentSource == unwarpedSilent);

    // A source that is not a tabled oscillator — the sub — is read from where it is produced instead.
    WarpSource mapped = WarpSource::OtherOsc;
    CHECK (warpSourceSlot (mapped, 0, 3) == 1);
    CHECK (warpSourceSlot (mapped, 1, 3) == 2);
    CHECK (warpSourceSlot (mapped, 2, 3) == 0);      // wraps rather than reading out of range

    CHECK (warpSourceSlot (WarpSource::Sub, 0, 3) == -1);
    CHECK (warpSourceSlot (WarpSource::Filter1, 0, 3) == -1);
}

TEST_CASE ("a warp mode's name says which signal it reads", "[modsources][warp]")
{
    // The suffix is the whole of the difference between two modes of the same family, so it is decoded
    // rather than dropped — ticket 25 mapped every variant onto its family and left the source nowhere.
    CHECK (warpSourceFromName ("kPD_OSC") == WarpSource::OtherOsc);
    CHECK (warpSourceFromName ("kFM_OSC") == WarpSource::OtherOsc);
    CHECK (warpSourceFromName ("kAM_OSC") == WarpSource::OtherOsc);
    CHECK (warpSourceFromName ("kRM_OSC") == WarpSource::OtherOsc);

    CHECK (warpSourceFromName ("kPD_OSC2") == WarpSource::SecondOtherOsc);
    CHECK (warpSourceFromName ("kFM_OSC2") == WarpSource::SecondOtherOsc);
    CHECK (warpSourceFromName ("kFMX_OSC2") == WarpSource::SecondOtherOsc);

    CHECK (warpSourceFromName ("kFM_SUB") == WarpSource::Sub);
    CHECK (warpSourceFromName ("kPD_SUB") == WarpSource::Sub);
    CHECK (warpSourceFromName ("kRM_SUB") == WarpSource::Sub);

    CHECK (warpSourceFromName ("kFM_NOISE") == WarpSource::Noise);
    CHECK (warpSourceFromName ("kFMP_NOISE") == WarpSource::Noise);

    CHECK (warpSourceFromName ("kFM_FILT1") == WarpSource::Filter1);
    CHECK (warpSourceFromName ("kFM_FILT2") == WarpSource::Filter2);
    CHECK (warpSourceFromName ("kPD_FILT1") == WarpSource::Filter1);

    CHECK (warpSourceFromName ("kSelfPD") == WarpSource::Self);

    // A mode that takes no modulator is not given one.
    CHECK (warpSourceFromName ("kPWM") == WarpSource::None);
    CHECK (warpSourceFromName ("kDistTube") == WarpSource::None);
    CHECK (warpSourceFromName ("kSync") == WarpSource::None);
    CHECK (warpSourceFromName ("") == WarpSource::None);
}

TEST_CASE ("a mono LFO is the pool's, and a per-voice one is not", "[modsources][lfo]")
{
    const auto table = sineTable();

    // The generator's own rule, asserted where it is decided: `advance` — which a *voice* calls — leaves a
    // `Mono` LFO alone, and `advanceMonoLfos` — which the engine calls once per block — writes it.
    ModSourceSettings sources;
    sources.lfos[0].rateHz = 6.0;
    sources.lfos[0].shape = LfoShape::SawUp;
    sources.lfos[0].mono = true;

    sources.lfos[1].rateHz = 6.0;
    sources.lfos[1].shape = LfoShape::SawUp;

    ModulationGenerator voiceGenerator;
    voiceGenerator.prepare (kSampleRate);
    voiceGenerator.noteOn (sources, false);

    ModulationInputs inputs;
    voiceGenerator.advance (sources, inputs, 128);

    CHECK (inputs.lfo[0] == 0.0);                                  // the pool's, not this voice's
    CHECK (inputs.lfo[1] == Catch::Approx (6.0 * 128.0 / kSampleRate).margin (0.001));

    ModulationGenerator poolGenerator;
    poolGenerator.prepare (kSampleRate);
    poolGenerator.advanceMonoLfos (sources, inputs, 128);

    CHECK (inputs.lfo[0] == Catch::Approx (6.0 * 128.0 / kSampleRate).margin (0.001));
    CHECK (inputs.lfo[1] == Catch::Approx (6.0 * 128.0 / kSampleRate).margin (0.001));   // untouched

    // And through the engine, a mono LFO reaches every voice that is playing: two notes on one shared
    // generator still modulate the one filter, and the rate is heard.
    const auto renderWithRate = [&table] (const double rateHz, const bool mono)
    {
        VoiceEngine engine;
        engine.prepare (kSampleRate, 128, 4);
        engine.setSettings (filteredSettings (&table));
        engine.setModulationSlots (std::vector<ModSlot> { cutoffRow (xerum::dsp::modSourceFromName ("LFO 1")) });

        ModSourceSettings settings;
        settings.lfos[0].rateHz = rateHz;
        settings.lfos[0].shape = LfoShape::SawUp;
        settings.lfos[0].mono = mono;
        engine.setModulationSources (settings);

        REQUIRE (engine.noteOn (57, 1.0) == 0);
        REQUIRE (engine.noteOn (64, 1.0) == 1);

        return renderEngine (engine, 8192);
    };

    const auto slow = renderWithRate (1.0, true);
    const auto fast = renderWithRate (20.0, true);

    CHECK (std::any_of (slow.begin(), slow.end(), [] (float v) { return v != 0.0f; }));
    CHECK (difference (slow, fast) > 0.005);
}
