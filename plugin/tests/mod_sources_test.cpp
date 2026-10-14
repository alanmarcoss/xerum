// Ticket 48: the modulation sources that had no generator, asserted **from a preset** through the whole
// path — document -> parameter bridge -> engine settings -> generators -> matrix -> render.
//
// The failure this suite exists to catch is the one ticket 25 fell into: a warp family implemented and
// tested *against a modulator the test supplied itself*, so its unit tests passed while the product did
// nothing, because nothing ever handed it one. Every assertion here is therefore a **render difference
// driven by a preset document**, never a parameter value and never a generator read back directly:
//
//   * an LFO moves its destination, and its rate and its shape change the render;
//   * a matrix envelope moves a filter while the amplitude envelope shapes the level, and neither reaches
//     the other's destination;
//   * a macro moves something;
//   * a preset whose warp is driven by *another oscillator* renders differently from the same preset with
//     the warp removed — and identically to it when that oscillator is silent, which is what pins the
//     modulator to the oscillator the mode's name encodes rather than to some signal that happens to be
//     there;
//   * the audio-rate sources (`OSC A/B/C`, `SUB OSC`, `NOISE OSC`, `Filter 1`) are non-zero, asserted by a
//     row that reads one and changes the render where a row with a zero source would not.
//
// Run alone:  build-48/plugin/tests/xerum-plugin-tests "[modsources]"

#include "TestSupport.h"

#include "write.h"

#include <catch2/catch_approx.hpp>

#include <string>
#include <vector>

using namespace xerum::plugin;
using namespace xerum::plugin::tests;

namespace
{

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;

juce::File writeFixture (const xerum::model::PresetDocument& document)
{
    const auto written = xerum::model::writeContainer (document, {});
    REQUIRE (written.ok);

    auto file = juce::File::createTempFile (".SerumPreset");
    REQUIRE (file.replaceWithData (written.bytes.data(), written.bytes.size()));

    return file;
}

/// The smallest preset this suite can make a sound with: one wavetable oscillator, four voices, and a
/// short amplitude envelope whose sustain is flat — so any movement in a render is the thing under test
/// rather than the level envelope's own shape.
xerum::model::PresetDocument baseDocument()
{
    xerum::model::PresetDocument document;
    document.name = "ticket 48 fixture";
    document.schemaVersion = "4.0";

    document.parameters["Global0/plainParams/kParamPolyCount"] = number (4.0);
    document.parameters["Oscillator0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Oscillator0/plainParams/kParamVolume"] = number (0.8);
    document.parameters["Env0/plainParams/kParamAttack"] = number (0.002);

    return document;
}

/// A voice filter a modulation can be heard on: a low cutoff, so a row that moves it changes the render.
void addFilter (xerum::model::PresetDocument& document, const double cutoff = 0.32)
{
    document.parameters["VoiceFilter0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["VoiceFilter0/plainParams/kParamFreq"] = number (cutoff);
    document.parameters["VoiceFilter0/plainParams/kParamReso"] = number (40.0);
}

/// One matrix row, addressed exactly as the reference addresses it. The destination's *name* comes along
/// because it is the only stable identity across the schema versions we accept (ticket 03).
void addRow (xerum::model::PresetDocument& document, const int row, const int source,
             const std::string& destinationClass, const int instance, const int parameter,
             const std::string& parameterName, const double amount = 100.0)
{
    const auto prefix = "ModSlot" + std::to_string (row) + "/";

    document.parameters[prefix + "source[0]"] = number (static_cast<double> (source));
    document.parameters[prefix + "plainParams/kParamAmount"] = number (amount);
    document.parameters[prefix + "destModuleTypeString"] = text (destinationClass);
    document.parameters[prefix + "destModuleID"] = number (static_cast<double> (instance));
    document.parameters[prefix + "destModuleParamID"] = number (static_cast<double> (parameter));
    document.parameters[prefix + "destModuleParamName"] = text (parameterName);
}

/// Load a document as a real `.SerumPreset`, press a note, and render it. The file round trip is
/// deliberate: it is the path a user's preset takes, so a field the bridge does not carry shows up here
/// as a render that does not move.
std::vector<float> renderPreset (const xerum::model::PresetDocument& document, const int blocks = 96,
                                 const int note = 60)
{
    const auto file = writeFixture (document);

    XerumAudioProcessor processor;
    processor.prepareToPlay (kSampleRate, kBlock);

    const auto loaded = processor.loadPresetFromFile (file);
    REQUIRE (loaded.wasOk());

    return render (processor, blocks, kBlock, noteThenRelease (note, blocks - 8));
}

double differenceOf (const std::vector<float>& left, const std::vector<float>& right)
{
    return channelDifference (left, right);
}

/// The peak of one half of a render, for the level-shaped assertions.
double peakOfHalf (const std::vector<float>& samples, const int half)
{
    const auto middle = samples.size() / 2;

    return half == 0 ? peakOf (std::vector<float> (samples.begin(), samples.begin() + middle))
                     : peakOf (std::vector<float> (samples.begin() + middle, samples.end()));
}

} // namespace

//==============================================================================
// The ten LFOs

TEST_CASE ("a preset's LFO 1 moves its destination, and its rate changes the render", "[lfo][modsources]")
{
    // The only modulation in this preset is one row: LFO 1 -> the voice filter's cutoff. Nothing else in
    // it moves, so every difference below is the LFO.
    const auto documentFor = [] (const double ratePercent)
    {
        auto document = baseDocument();
        addFilter (document);
        addRow (document, 0, xerum::dsp::modSourceFromName ("LFO 1"), "VoiceFilter", 0, 3, "kParamFreq");
        document.parameters["LFO0/plainParams/kParamRate"] = number (ratePercent);
        return document;
    };

    REQUIRE (xerum::dsp::modSourceFromName ("LFO 1") == 6);

    const auto slow = renderPreset (documentFor (6.0));
    const auto fast = renderPreset (documentFor (70.0));

    auto unmodulated = documentFor (6.0);
    unmodulated.parameters.erase ("ModSlot0/source[0]");
    const auto withoutRow = renderPreset (unmodulated);

    // The row's destination really is the filter, and the LFO really reaches it: with a rate the filter is
    // heard moving, with no row it is not.
    CHECK (peakOf (slow) > 0.001);
    CHECK (differenceOf (slow, withoutRow) > 0.01);

    // ... and the *rate* is the difference the acceptance names.
    CHECK (differenceOf (slow, fast) > 0.01);

    // A second LFO at the same rate, moved to a different output, is independent of the first: the second
    // output (`LFO 1 Y`) is the **path** the preset drew, which is what the separate source name is for.
    auto withSecondOutput = documentFor (6.0);
    withSecondOutput.parameters["ModSlot0/source[0]"] = number (39.0);   // `LFO 1 Y`

    const double pathX[] = { 0.0, 1.0 };
    const double pathY[] = { 0.9, 0.1 };

    for (int i = 0; i < 2; ++i)
    {
        const auto index = std::to_string (i);
        withSecondOutput.parameters["LFO0/pathData/xVals[" + index + "]"] = number (pathX[i]);
        withSecondOutput.parameters["LFO0/pathData/yVals[" + index + "]"] = number (pathY[i]);
        withSecondOutput.parameters["LFO0/pathData/curveVals[" + index + "]"] = number (0.5);
    }

    withSecondOutput.parameters["LFO0/pathData/numPoints"] = number (2.0);

    const auto secondOutput = renderPreset (withSecondOutput);

    CHECK (differenceOf (slow, secondOutput) > 0.005);
}

TEST_CASE ("an LFO's drawn shape plays as drawn, and differs from the built-in one", "[lfo][modsources]")
{
    // The schema's own point arrays: `curveData` carries `xVals`/`yVals`/`curveVals` and a `numPoints`,
    // and the reference stores `numPoints + 1` entries — the last closes the cycle at `x == 1`. This shape
    // is a rising ramp with two hard steps, which no built-in shape is.
    const auto drawn = []
    {
        auto document = baseDocument();
        addFilter (document);
        addRow (document, 0, xerum::dsp::modSourceFromName ("LFO 1"), "VoiceFilter", 0, 3, "kParamFreq");
        document.parameters["LFO0/plainParams/kParamRate"] = number (40.0);
        document.parameters["LFO0/curveDisplayName"] = text ("Custom");

        const double x[] = { 0.0, 0.25, 0.25, 0.5, 0.5, 0.75, 1.0, 1.0 };
        const double y[] = { 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0 };

        for (int i = 0; i < 8; ++i)
        {
            const auto index = std::to_string (i);
            document.parameters["LFO0/curveData/xVals[" + index + "]"] = number (x[i]);
            document.parameters["LFO0/curveData/yVals[" + index + "]"] = number (y[i]);
            document.parameters["LFO0/curveData/curveVals[" + index + "]"] = number (0.5);
        }

        document.parameters["LFO0/curveData/numPoints"] = number (7.0);
        return document;
    };

    // The same preset with the drawn points removed: the LFO's built-in shape (a sine) plays instead.
    auto builtIn = drawn();
    for (int i = 0; i < 8; ++i)
    {
        const auto index = std::to_string (i);
        builtIn.parameters.erase ("LFO0/curveData/xVals[" + index + "]");
        builtIn.parameters.erase ("LFO0/curveData/yVals[" + index + "]");
        builtIn.parameters.erase ("LFO0/curveData/curveVals[" + index + "]");
    }

    builtIn.parameters.erase ("LFO0/curveData/numPoints");

    const auto drawnRender = renderPreset (drawn());
    const auto builtInRender = renderPreset (builtIn);

    CHECK (peakOf (drawnRender) > 0.001);
    CHECK (differenceOf (drawnRender, builtInRender) > 0.01);

    // ... and the drawn shape is what moves it: two *different* drawn shapes give two different renders,
    // so the difference above is the shape and not merely "a curve was present".
    auto other = drawn();
    other.parameters["LFO0/curveData/yVals[1]"] = number (1.0);

    CHECK (differenceOf (drawnRender, renderPreset (other)) > 0.005);
}

//==============================================================================
// The four matrix envelopes

TEST_CASE ("the matrix envelopes are separate generators from the amplitude envelope", "[envelope][modsources]")
{
    // The two halves of the claim, and the second half is the one that catches a conflation:
    //
    //   * a matrix envelope's settings do not reach the level, so changing them with nothing reading the
    //     envelope leaves the render **byte-identical**;
    //   * the amplitude envelope's settings do reach it.
    //
    // The preset's own module naming is the confusing part and worth stating: the matrix source `Env 3`
    // is `in.envelope[2]`, which is the document's `Env2`, because source values are 1-based
    // (`Env 1` = 2) and instances are 0-based. `Env0` is the amplitude envelope, which is what the
    // acceptance means by "the two are independent".
    const auto documentFor = [] (const double matrixAttack, const bool withRow, const bool withFilter)
    {
        auto document = baseDocument();

        if (withFilter)
            addFilter (document);

        document.parameters["Env2/plainParams/kParamAttack"] = number (matrixAttack);
        document.parameters["Env2/plainParams/kParamDecay"] = number (0.0);
        document.parameters["Env2/plainParams/kParamSustain"] = number (0.0);

        if (withRow)
            addRow (document, 0, xerum::dsp::modSourceFromName ("Env 3"), "VoiceFilter", 0, 3, "kParamFreq");

        return document;
    };

    REQUIRE (xerum::dsp::modSourceFromName ("Env 3") == 4);

    // (i) No filter to modulate: the matrix envelope is generated but nothing in the voice reads it, so a
    // fast one and a slow one render identically. If the matrix envelopes were the amplitude envelope —
    // the placeholder this ticket removed — these two would differ.
    CHECK (renderPreset (documentFor (0.001, true, false)) == renderPreset (documentFor (1.5, true, false)));

    // (ii) With the destination present, the same change moves the filter.
    const auto fastFilter = renderPreset (documentFor (0.001, true, true));
    const auto slowFilter = renderPreset (documentFor (1.5, true, true));

    CHECK (differenceOf (fastFilter, slowFilter) > 0.01);

    // (iii) And the amplitude envelope still shapes the level, independently: its attack moves the render
    // and the *level trajectory* with it.
    auto slowAmp = documentFor (0.001, true, true);
    slowAmp.parameters["Env0/plainParams/kParamAttack"] = number (0.4);
    const auto slowAmpRender = renderPreset (slowAmp);

    CHECK (differenceOf (fastFilter, slowAmpRender) > 0.01);
    CHECK (peakOfHalf (slowAmpRender, 0) < peakOfHalf (fastFilter, 0) * 0.9);
}

TEST_CASE ("a matrix envelope moves a filter through its stages while the amplitude envelope holds the level",
           "[envelope][modsources]")
{
    // The acceptance's own arrangement: one matrix envelope on the filter, the amplitude envelope flat
    // (sustain 1, instant attack), so every difference in the render is the filter moving through the
    // envelope's stages rather than the level moving under it. Independence itself is asserted above —
    // with the destination absent, a matrix envelope's settings leave the render byte-identical.
    const auto documentFor = [] (const double decay, const double sustain)
    {
        auto document = baseDocument();
        addFilter (document, 0.25);

        document.parameters["Env0/plainParams/kParamAttack"] = number (0.001);
        document.parameters["Env0/plainParams/kParamSustain"] = number (1.0);

        document.parameters["Env1/plainParams/kParamAttack"] = number (0.005);
        document.parameters["Env1/plainParams/kParamDecay"] = number (decay);
        document.parameters["Env1/plainParams/kParamSustain"] = number (sustain);

        addRow (document, 0, xerum::dsp::modSourceFromName ("Env 2"), "VoiceFilter", 0, 3, "kParamFreq");
        return document;
    };

    const auto holdOpen = renderPreset (documentFor (0.0, 1.0));
    const auto fallToHalf = renderPreset (documentFor (0.2, 0.5));
    const auto fallClosed = renderPreset (documentFor (0.2, 0.0));

    // **The stages**, not merely "an envelope ran": a held sustain, a half sustain and a closed one are
    // three different renders, because the envelope's decay and sustain reach the destination.
    CHECK (differenceOf (holdOpen, fallClosed) > 0.01);
    CHECK (differenceOf (holdOpen, fallToHalf) > 0.005);
    CHECK (differenceOf (fallToHalf, fallClosed) > 0.005);

    // The envelope's own decay curve is the CRV control rather than a second shape: a straight decay and a
    // bent one — the corpus's own 50/66.6 pair — are different renders.
    auto bowed = documentFor (0.2, 0.0);
    bowed.parameters["Env1/plainParams/kParamCurve2"] = number (95.0);

    CHECK (differenceOf (fallClosed, renderPreset (bowed)) > 0.002);
}

//==============================================================================
// The eight macros

TEST_CASE ("a macro moves its destination, and the interface can move it", "[macros][modsources]")
{
    auto document = baseDocument();
    addFilter (document);
    addRow (document, 0, xerum::dsp::modSourceFromName ("Macro 1"), "VoiceFilter", 0, 3, "kParamFreq");
    document.parameters["Macro0/plainParams/kParamValue"] = number (0.0);

    REQUIRE (xerum::dsp::modSourceFromName ("Macro 1") == 25);

    const auto atZero = renderPreset (document);

    // The macro is a **parameter** — that is what makes it a control rather than a constant — so the same
    // preset with the macro turned up renders differently.
    const auto file = writeFixture (document);
    XerumAudioProcessor processor;
    processor.prepareToPlay (kSampleRate, kBlock);
    REQUIRE (processor.loadPresetFromFile (file).wasOk());

    CHECK (processor.parameterValue ("Macro0/plainParams/kParamValue") == Catch::Approx (0.0));

    processor.setParameterValue ("Macro0/plainParams/kParamValue", 80.0);

    const auto turnedUp = render (processor, 96, kBlock, noteThenRelease (60, 88));

    CHECK (peakOf (turnedUp) > 0.001);
    CHECK (differenceOf (atZero, turnedUp) > 0.01);

    // The macro's own value is in the document's own units (percent), and the matrix source is 0..1, so
    // the row contributes 0.8 of the destination's range rather than 80.
    CHECK (processor.parameterValue ("Macro0/plainParams/kParamValue") == Catch::Approx (80.0));
}

//==============================================================================
// The audio-rate sources

TEST_CASE ("the audio-rate sources are no longer zero", "[modsources]")
{
    // Every one of these sources was a constant zero before ticket 48, and a row that reads a zero source
    // with a unipolar amount contributes *nothing* — so "the render changes when the row is added" is
    // exactly the assertion that the source is now generated. `Filter 1` is the loop through the voice's
    // own signal, which is the sharpest of the five: it can only be non-zero if the sample the voice just
    // produced reaches the matrix.
    struct Case
    {
        const char* name;
        int source;
        bool needsSub;
        bool needsNoise;
    };

    const Case cases[] = {
        { "OSC A", 49, false, false },     { "OSC B", 50, false, false },
        { "SUB OSC", 52, true, false },    { "NOISE OSC", 20, false, true },
        { "Filter 1", 53, false, false },
    };

    for (const auto& entry : cases)
    {
        INFO (entry.name);

        auto document = baseDocument();
        addFilter (document);

        // The sources that need a generator of their own get one, or the render would differ because the
        // *slot* was added rather than because the source was read.
        if (entry.source == 50)
        {
            document.parameters["Oscillator1/plainParams/kParamEnable"] = number (1.0);
            document.parameters["Oscillator1/plainParams/kParamVolume"] = number (0.6);
        }

        if (entry.needsSub)
        {
            document.parameters["Oscillator4/plainParams/kParamEnable"] = number (1.0);
            document.parameters["Oscillator4/plainParams/kParamVolume"] = number (0.6);
            document.parameters["Oscillator4/SubOsc4/plainParams/kParamShape"] = text ("Sine");
        }

        if (entry.needsNoise)
        {
            document.parameters["Oscillator3/plainParams/kParamEnable"] = number (1.0);
            document.parameters["Oscillator3/plainParams/kParamVolume"] = number (0.4);
            document.parameters["Oscillator3/NoiseOsc3/plainParams/kParamNoiseType"] = text ("Geiger");
        }

        const auto withoutRow = renderPreset (document);

        // The destination is the oscillator's own level, so a non-zero source is heard as level.
        addRow (document, 0, entry.source, "Oscillator", 0, 1, "kParamVolume", 60.0);
        const auto withRow = renderPreset (document);

        REQUIRE (peakOf (withoutRow) > 0.001);
        CHECK (differenceOf (withoutRow, withRow) > 0.005);
    }
}

//==============================================================================
// The warp's source, end to end

TEST_CASE ("a preset's source-driven warp reads the oscillator its mode names", "[warp][modsources]")
{
    // **The trap ticket 25 fell into.** Its tests handed the warp a modulator themselves, so they passed
    // while `setWarpModulator` was never called anywhere in the product and `kFM_OSC` was transparent.
    // These are presets, and the warp's source is another oscillator in the same preset:
    //
    //   * `kFM_OSC` on OSC A differs from `kNoWarp` — the family reaches the render at all;
    //   * with OSC B **silent**, `kFM_OSC` is *byte-identical* to `kNoWarp` — which pins the modulator to
    //     OSC B and to nothing else, because a frequency-modulated phase with a zero modulator is the
    //     unwarped phase;
    //   * with OSC B sounding, they differ — so the wiring is live, not merely non-crashing.
    // An absent `kParamWarpMenu` is the reference's "no warp" — the field is simply not stored — and the
    // bridge's own default for the choice is its index 0, which is `off`.
    const auto documentFor = [] (const std::string& warpMode, const double secondLevel)
    {
        auto document = baseDocument();

        if (! warpMode.empty())
        {
            document.parameters["Oscillator0/WTOsc0/plainParams/kParamWarpMenu"] = text (warpMode);
            document.parameters["Oscillator0/WTOsc0/plainParams/kParamWarp"] = number (0.9);
        }

        document.parameters["Oscillator1/plainParams/kParamEnable"] = number (1.0);
        document.parameters["Oscillator1/plainParams/kParamVolume"] = number (secondLevel);
        document.parameters["Oscillator1/plainParams/kParamPitch"] = number (7.0);

        return document;
    };

    const auto unwarped = renderPreset (documentFor ("", 0.8));
    const auto modulated = renderPreset (documentFor ("kFM_OSC", 0.8));
    const auto modulatedWithSilentSource = renderPreset (documentFor ("kFM_OSC", 0.0));
    const auto unwarpedWithSilentSource = renderPreset (documentFor ("", 0.0));

    REQUIRE (xerum::dsp::warpModeFromName ("kFM_OSC") == xerum::dsp::WarpMode::FrequencyModulation);

    CHECK (peakOf (modulated) > 0.001);

    // The wiring, end to end: the family reaches the signal path from a preset.
    CHECK (differenceOf (unwarped, modulated) > 0.01);

    // And it reads *that* oscillator: silence it and the warp is exactly the unmodulated phase.
    CHECK (modulatedWithSilentSource == unwarpedWithSilentSource);

    // The same with the source's own pitch moved instead of its level — an audible change it is not, but
    // the render's own difference is what the acceptance asks for.
    auto retuned = documentFor ("kFM_OSC", 0.8);
    retuned.parameters["Oscillator1/plainParams/kParamPitch"] = number (-19.0);

    CHECK (differenceOf (modulated, renderPreset (retuned)) > 0.005);
}

TEST_CASE ("a warp whose source is the sub oscillator also reads it", "[warp][modsources]")
{
    // The other suffix the names encode, pinned the same way as the oscillator one: the same family, the
    // sub as its source, and a silent sub leaves the warp exactly as unwarped.
    const auto documentFor = [] (const bool warp, const double subLevel)
    {
        auto document = baseDocument();

        if (warp)
        {
            document.parameters["Oscillator0/WTOsc0/plainParams/kParamWarpMenu"] = text ("kFM_SUB");
            document.parameters["Oscillator0/WTOsc0/plainParams/kParamWarp"] = number (0.9);
        }

        document.parameters["Oscillator4/plainParams/kParamEnable"] = number (1.0);
        document.parameters["Oscillator4/plainParams/kParamVolume"] = number (subLevel);
        document.parameters["Oscillator4/SubOsc4/plainParams/kParamShape"] = text ("Sine");

        return document;
    };

    const auto warped = renderPreset (documentFor (true, 0.7));
    const auto unwarped = renderPreset (documentFor (false, 0.7));
    const auto warpedSilent = renderPreset (documentFor (true, 0.0));
    const auto unwarpedSilent = renderPreset (documentFor (false, 0.0));

    CHECK (peakOf (warped) > 0.001);

    // The sub reaches the warp: with it sounding, the family is heard.
    CHECK (differenceOf (warped, unwarped) > 0.005);

    // And it is *the sub*: silent it, and the warp is a no-op again.
    CHECK (warpedSilent == unwarpedSilent);

    // The sub is audible on its own, so the difference above is the warped oscillator and not the sub's
    // own level.
    CHECK (differenceOf (unwarped, unwarpedSilent) > 0.005);
}
