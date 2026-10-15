// The plugin plays, and it plays *measurably*: peak levels, spectral-free level relationships, and the
// envelope's own behaviour. Never "it ran", never "the output was non-empty".
//
// Run alone with:  ctest --test-dir build-30 -R xerum-plugin-tests  (or the binary with "[audio]")

#include "TestSupport.h"

#include <catch2/catch_approx.hpp>

#include <cstdio>

using namespace xerum::plugin;
using namespace xerum::plugin::tests;

TEST_CASE ("the init patch plays a note, and switching the oscillator off silences it", "[plugin][audio]")
{
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    // Out of the box, with nothing set: the init patch has oscillator A on (the reference's own init
    // state has it on — see the bridge table) and xerum's own table, so a host that loads the plugin and
    // plays a note hears something. That is the difference between a playable instrument and a plugin
    // that only works once a file is loaded.
    const auto outOfTheBox = render (processor, 16, 512, noteThenRelease (60, 8));
    CHECK (peakOf (outOfTheBox) > 0.1);

    // Switching the oscillator off is silence, and it is attributable to that control alone.
    processor.setParameterValue ("Oscillator0/plainParams/kParamEnable", 0.0);
    const auto off = render (processor, 16, 512, noteThenRelease (60, 8));
    CHECK (peakOf (off) == 0.0);

    processor.setParameterValue ("Oscillator0/plainParams/kParamEnable", 1.0);
    const auto on = render (processor, 16, 512, noteThenRelease (60, 8));
    CHECK (peakOf (on) > 0.1);

    // And it is the oscillator's own output, not a stuck level: the voice decays once the key is up.
    const auto late = std::vector<float> (on.end() - 2048, on.end());
    CHECK (rmsOf (late) < rmsOf (on));
}

TEST_CASE ("the level controls are monotone: the render responds to them, and attribution is exact",
           "[plugin][audio]")
{
    const auto playAt = [] (const double volume, const double master)
    {
        XerumAudioProcessor processor;
        processor.prepareToPlay (48000.0, 512);
        processor.setParameterValue ("Oscillator0/plainParams/kParamVolume", volume);
        processor.setParameterValue ("Global0/plainParams/kParamMasterVolume", master);

        // The master gain is ramped across the block it changes in, so the peak is measured from the
        // second block on: this is about the gain's *value*, not about the ramp that gets there.
        const auto rendered = render (processor, 24, 512, noteThenRelease (60, 12));
        return peakOf (std::vector<float> (rendered.begin() + 1024, rendered.end()));
    };

    const auto full = playAt (1.0, 1.0);
    const auto half = playAt (0.5, 1.0);
    const auto masterHalf = playAt (1.0, 0.5);
    const auto zero = playAt (0.0, 1.0);

    CHECK (full > 0.1);
    CHECK (half < full);
    CHECK (half > full * 0.3);
    CHECK (masterHalf == Catch::Approx (full * 0.5).epsilon (0.05));
    CHECK (zero == 0.0);
}

TEST_CASE ("the filter is heard: a low cutoff attenuates where an open one does not", "[plugin][audio]")
{
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    processor.setParameterValue ("VoiceFilter0/plainParams/kParamEnable", 1.0);
    processor.setParameterValue ("VoiceFilter0/plainParams/kParamType",
                                 static_cast<double> (choiceIndexForText (
                                     processor.bridge().choices (ChoiceSet::FilterType), ChoiceSet::FilterType,
                                     "L24")));
    processor.setParameterValue ("VoiceFilter0/plainParams/kParamReso", 0.0);

    processor.setParameterValue ("VoiceFilter0/plainParams/kParamFreq", 1.0);
    const auto open = rmsOf (render (processor, 24, 512, noteThenRelease (60, 12)));

    processor.setParameterValue ("VoiceFilter0/plainParams/kParamFreq", 0.02);
    const auto closed = rmsOf (render (processor, 24, 512, noteThenRelease (60, 12)));

    CHECK (open > 0.05);
    CHECK (closed < open * 0.5);
}

TEST_CASE ("automation reaches the audio thread mid-render", "[plugin][audio]")
{
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    // The host writes a parameter *between blocks*, which is exactly what a DAW's automation does, and the
    // engine must pick it up on the next block without a re-prepare.
    const auto automated = render (processor, 40, 512,
                                   [&processor] (juce::MidiBuffer& events, const int block)
                                   {
                                       if (block == 0)
                                           events.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 0);

                                       if (block == 20)
                                           processor.setParameterValue ("Oscillator0/plainParams/kParamVolume", 0.25);
                                   });

    // **The boundary is the plugin's own latency** (ticket `58`). Block 20 is handed its parameters at host
    // time `20 x 512`, and the master ceiling delays what it passes by `getLatencySamples()`, so the change
    // reaches the *output* that many samples later. Slicing at the raw block edge would put 72 samples of
    // the old, louder level inside `late` and measure the declared latency instead of the automation.
    const auto boundary = 20 * 512 + processor.getLatencySamples();
    const auto early = std::vector<float> (automated.begin(), automated.begin() + boundary);
    const auto late = std::vector<float> (automated.begin() + boundary, automated.end());

    // The note is held throughout, so the drop is the automation and nothing else.
    CHECK (peakOf (early) > 0.1);
    CHECK (peakOf (late) == Catch::Approx (peakOf (early) * 0.25).epsilon (0.1));
}

TEST_CASE ("portamento and mono are honoured from a preset document", "[plugin][audio]")
{
    xerum::model::PresetDocument document;
    document.parameters["Oscillator0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Global0/plainParams/kParamMonoToggle"] = number (1.0);
    document.parameters["Global0/plainParams/kParamLegato"] = number (1.0);
    document.parameters["Global0/plainParams/kParamPortamentoTime"] = number (0.05);
    document.parameters["Env0/plainParams/kParamAttack"] = number (0.001);
    document.parameters["Env0/plainParams/kParamRelease"] = number (0.005);

    XerumAudioProcessor legato;
    legato.prepareToPlay (48000.0, 512);
    legato.applyDocument (document, {}, 0);

    // A legato note change must not retrigger the envelope, so the level carries over instead of falling
    // to zero. Ticket 28 measures this in the engine; here it is measured through the plugin's own path.
    const auto rendered = render (legato, 8, 128,
                                  [] (juce::MidiBuffer& events, const int block)
                                  {
                                      if (block == 0)
                                          events.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 0);
                                      if (block == 4)
                                      {
                                          events.addEvent (juce::MidiMessage::noteOn (1, 67, static_cast<juce::uint8> (100)), 0);
                                      }
                                  });

    const auto before = rmsOf (std::vector<float> (rendered.begin() + 3 * 128, rendered.begin() + 4 * 128));
    const auto after = rmsOf (std::vector<float> (rendered.begin() + 5 * 128, rendered.begin() + 6 * 128));

    CHECK (before > 0.01);
    CHECK (after > before * 0.5);   // no retrigger-to-silence
}

TEST_CASE ("the unison spread is measured between the output channels, not read back",
           "[plugin][audio][unison-stereo]")
{
    // Ticket 41's second acceptance, at the boundary a host sees: the same note is rendered at spread 0
    // and at full spread, and the **difference between the two output channels** is what is compared. A
    // parameter read-back would pass with the value going nowhere.
    // A fresh processor per spread, so nothing carries over between the three renders — a shared one
    // would compare a note against the *tail* of the note before it.
    const auto renderSpread = [] (const double spread)
    {
        XerumAudioProcessor processor;
        processor.prepareToPlay (48000.0, 512);

        processor.setParameterValue ("Oscillator0/plainParams/kParamEnable", 1.0);
        processor.setParameterValue ("Oscillator0/plainParams/kParamUnison", 4.0);
        processor.setParameterValue ("Oscillator0/plainParams/kParamUnisonRange", 12.0);
        processor.setParameterValue ("Oscillator0/plainParams/kParamUnisonStereo", spread);

        return renderStereo (processor, 24, 512, noteThenRelease (60, 16));
    };

    const auto centre = renderSpread (0.0);

    CHECK (peakOf (centre.first) > 0.1);                        // it really rendered
    CHECK (channelDifference (centre.first, centre.second) == 0.0);

    const auto wide = renderSpread (100.0);

    CHECK (peakOf (wide.first) > 0.1);

    const double difference = channelDifference (wide.first, wide.second);
    CHECK (difference > 0.05);
    CAPTURE (difference);

    // And the image mirrors when the sign flips, which is what the corpus's own negative values ask for.
    const auto mirror = renderSpread (-100.0);

    CHECK (channelDifference (mirror.first, wide.second) < 1e-5);
}

TEST_CASE ("a bus layout other than stereo or mono is refused", "[plugin][audio]")
{
    XerumAudioProcessor processor;

    juce::AudioProcessor::BusesLayout stereo;

    stereo.inputBuses.add (juce::AudioChannelSet::stereo());
    stereo.outputBuses.add (juce::AudioChannelSet::stereo());

    CHECK_FALSE (processor.isBusesLayoutSupported (stereo));   // an instrument has no input bus

    juce::AudioProcessor::BusesLayout outputs;

    outputs.outputBuses.add (juce::AudioChannelSet::stereo());
    CHECK (processor.isBusesLayoutSupported (outputs));

    outputs.outputBuses.clear();
    outputs.outputBuses.add (juce::AudioChannelSet::mono());
    CHECK (processor.isBusesLayoutSupported (outputs));

    outputs.outputBuses.clear();
    outputs.outputBuses.add (juce::AudioChannelSet::create5point1());
    CHECK_FALSE (processor.isBusesLayoutSupported (outputs));
}

//==============================================================================
// Ticket `58` — the master ceiling.

namespace
{

/// `voices` notes struck together at the first block and held until `releaseBlock`. The dev's report is
/// "the audio turns red when more than one key is held", so the stimulus is chordal, not a single note.
std::function<void (juce::MidiBuffer&, int)> chord (const int voices, const int releaseBlock)
{
    return [voices, releaseBlock] (juce::MidiBuffer& events, const int block)
    {
        if (block == 0)
            for (int voice = 0; voice < voices; ++voice)
                events.addEvent (juce::MidiMessage::noteOn (1, 36 + 3 * voice, static_cast<juce::uint8> (127)), 0);

        if (block == releaseBlock)
            for (int voice = 0; voice < voices; ++voice)
                events.addEvent (juce::MidiMessage::noteOff (1, 36 + 3 * voice), 0);
    };
}

/// The default ceiling as a linear amplitude, computed from the documented figure rather than read out of
/// the limiter the test is checking.
double ceilingLinear()
{
    return std::pow (10.0, xerum::dsp::kMasterCeilingDb / 20.0);
}

/// Render a processor under a stimulus and return the peak across **both** channels. The ceiling is a
/// stereo-linked master stage, so a one-channel peak would not establish anything about the other.
double peakStereo (const int voices)
{
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    // The ceiling is off: these cases measure the engine's own headroom, not the limiter's.
    processor.setParameterValue ("Master0/plainParams/kParamLimitOn", 0.0);

    const auto rendered = renderStereo (processor, 24, 512, chord (voices, 20));
    return std::max (peakOf (rendered.first), peakOf (rendered.second));
}

} // namespace

TEST_CASE ("the ceiling holds a sixteen-voice chord under full scale, and the chord exceeds it without",
           "[plugin][audio][limiter]")
{
    const auto play = [] (const bool ceilingOn)
    {
        XerumAudioProcessor processor;
        processor.prepareToPlay (48000.0, 512);
        processor.setParameterValue ("Master0/plainParams/kParamLimitOn", ceilingOn ? 1.0 : 0.0);

        const auto rendered = renderStereo (processor, 32, 512, chord (16, 28));
        return std::make_pair (peakOf (rendered.first), peakOf (rendered.second));
    };

    const auto open = play (false);
    const auto limited = play (true);

    const auto ceiling = ceilingLinear();

    // The measured numbers, in the house style the bank test uses, so a run carries the finding rather
    // than only a pass.
    std::fprintf (stderr,
                  "[limiter] polyphony stress, 16 voices: peak with the ceiling off %.6f / %.6f, "
                  "with it on %.6f / %.6f\n",
                  open.first, open.second, limited.first, limited.second);

    // **Red before.** The same sixteen-voice chord with the ceiling off exceeds full scale — the dev's own
    // report, measured rather than taken on trust. This is the counterfactual that makes the assertion
    // below bite: it is not describing an already-safe signal.
    CHECK (open.first > 1.0);
    CHECK (open.second > 1.0);

    // **Green after.** The ceiling is an absolute maximum at the sample level, on both channels, not a
    // claim about a meter. The epsilon is float rounding on `ceiling / magnitude * magnitude`.
    CHECK (limited.first <= ceiling + 1.0e-6);
    CHECK (limited.second <= ceiling + 1.0e-6);

    // And it did not simply crush the render: the peak still reaches the ceiling.
    CHECK (limited.first > ceiling * 0.95);
}

TEST_CASE ("a render that never reaches the ceiling is bit-identical with it on and off",
           "[plugin][audio][limiter]")
{
    const auto make = [] (const bool ceilingOn)
    {
        auto processor = std::make_unique<XerumAudioProcessor>();
        processor->prepareToPlay (48000.0, 512);
        processor->setParameterValue ("Master0/plainParams/kParamLimitOn", ceilingOn ? 1.0 : 0.0);

        // Quiet enough that the limiter has nothing to do, which is the case this asserts about.
        processor->setParameterValue ("Global0/plainParams/kParamMasterVolume", 0.4);
        return processor;
    };

    auto limited = make (true);
    auto open = make (false);

    const auto latency = limited->getLatencySamples();

    // **The latency is declared, and it does not move with the switch.** A lookahead limiter delays the
    // signal by its lookahead; the delay line runs whether the ceiling is enforced or not, so a host that
    // has compensated for it stays compensated when the parameter moves under automation.
    CHECK (latency == 72);                                            // 1.5 ms at 48 kHz
    CHECK (open->getLatencySamples() == latency);

    const auto limitedRender = render (*limited, 24, 512, noteThenRelease (60, 16));
    const auto openRender = render (*open, 24, 512, noteThenRelease (60, 16));

    const auto ceiling = ceilingLinear();

    REQUIRE (peakOf (openRender) > 0.05);        // it really rendered
    REQUIRE (peakOf (openRender) < ceiling);     // and never came near the ceiling

    REQUIRE (limitedRender.size() == openRender.size());

    // **Sample for sample, at the same index.** No shift: the delay line runs whether the ceiling is
    // enforced or not, so both renders carry the same declared latency and the invariant is literally
    // "these two arrays are equal", not "these two arrays are equal once one is moved". **Exact equality**,
    // not a tolerance — the whole point is that a preset which already sat below the ceiling is not
    // rewritten at all.
    for (std::size_t i = 0; i < openRender.size(); ++i)
    {
        if (limitedRender[i] != openRender[i])
            FAIL ("sample " << i << " was " << limitedRender[i] << " with the ceiling, " << openRender[i] << " without");
    }
}

TEST_CASE ("the ceiling is a parameter, saved and restored with the patch", "[plugin][audio][limiter]")
{
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    // **Default on**, because the dev asked for the red to stop, at the reference's own ceiling.
    CHECK (processor.parameterValue ("Master0/plainParams/kParamLimitOn") == 1.0);
    CHECK (processor.parameterValue ("Master0/plainParams/kParamLimitCeiling")
           == Catch::Approx (xerum::dsp::kMasterCeilingDb));

    // It is a control, not a constant: a user who wants the reference's own output turns it off, and one
    // who wants a different ceiling moves it.
    processor.setParameterValue ("Master0/plainParams/kParamLimitOn", 0.0);
    processor.setParameterValue ("Master0/plainParams/kParamLimitCeiling", -6.0);

    juce::MemoryBlock state;
    processor.getStateInformation (state);
    REQUIRE (state.getSize() > 0);

    XerumAudioProcessor restored;
    restored.prepareToPlay (48000.0, 512);
    restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

    CHECK (restored.parameterValue ("Master0/plainParams/kParamLimitOn") == 0.0);
    CHECK (restored.parameterValue ("Master0/plainParams/kParamLimitCeiling") == Catch::Approx (-6.0));
}

TEST_CASE ("the default patch carries the reference's own level, so a voice no longer clips but a chord does",
           "[plugin][audio][limiter]")
{
    // Ticket 58's finding was that **one voice already reached full scale**, which made any polyphony a
    // guaranteed overload. Ticket 63 changed the premise rather than the engine: the default patch now
    // carries the reference's own two level leaves (`Main Vol` at -9.04 dB, `A Level` at -5.0 dB), so one
    // voice sits ~14 dB lower than it did and the *second* key no longer clips. A chord still does, because
    // the voice bus still sums with no headroom. That is the report, measured rather than reasoned.
    const auto one = peakStereo (1);
    const auto two = peakStereo (2);
    const auto sixteen = peakStereo (16);

    CHECK (one > 0.05);            // one voice renders
    CHECK (one < 0.5);             // and with the reference's own attenuation it is well below full scale
    CHECK (two < 1.0);             // **and the second key no longer goes red** — ticket 63's change

    std::fprintf (stderr, "[limiter] gain staging, ceiling off: 1 voice %.6f, 2 voices %.6f, 16 voices %.6f\n",
                  one, two, sixteen);

    // Near-linear summation, sub-linear only because the voices are different notes with different phases:
    // the voice bus has no per-voice headroom and no mix-bus trim, so N voices are about N times one voice.
    CHECK (two == Catch::Approx (2.0 * one).epsilon (0.2));
    CHECK (sixteen > 4.0 * one);

    // A sixteen-note chord still overloads — which is what the ceiling is for, and why ticket 63's change
    // returns the limiter to a safety net rather than a stage that works on every note played.
    CHECK (sixteen > 1.0);

    // The ceiling is what stops that reaching the host's meters, which is the whole of ticket 58's ask.
    CHECK (sixteen <= 32.0);
}

//==============================================================================
// Ticket `60` — the FX rack in the signal path.
//
// `dsp::FxBank` was built and tested by ticket 29 and instantiated nowhere, so a preset whose sound
// depends on its rack rendered without it. What follows measures the wiring the way the ticket asks: by
// **difference**. No case below claims an effect *sounds* right — nobody here has ears — only that the
// preset's slots reach the output, in the preset's order, before the ceiling, and that whatever the rack
// could not build says so.

namespace
{

/// A minimal audible document: oscillator A on, both voice filters off, the ceiling off, no FX. Every case
/// adds the rack it wants to measure to *this*, so the difference between two renders is the rack and
/// nothing else.
xerum::model::PresetDocument fxDocument()
{
    xerum::model::PresetDocument document;
    document.name = "fx fixture";
    document.schemaVersion = "4.0";
    document.parameters["Oscillator0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Oscillator0/plainParams/kParamVolume"] = number (0.5);
    // The master leaf is pinned rather than left to the default, because ticket 63 moved that default to
    // the reference's own `Main Vol` (-9.04 dB) and every case below compares two renders of *this*
    // document at its own level. Pinning 1.0 keeps the stimulus these cases were calibrated on and makes
    // the dependence explicit; a document that omits the leaf on purpose is `[gain-staging]`'s job.
    document.parameters["Global0/plainParams/kParamMasterVolume"] = number (1.0);
    document.parameters["VoiceFilter0/plainParams/kParamEnable"] = number (0.0);
    document.parameters["VoiceFilter1/plainParams/kParamEnable"] = number (0.0);
    document.parameters["Master0/plainParams/kParamLimitOn"] = number (0.0);
    return document;
}

/// One rack slot, written the way a preset writes one: the class's numeric `type` at the slot, and its
/// parameters under `<class>/plainParams`. That is the document shape `planFxRack` reads, so a case built
/// this way is exercising the decode and the rack, not a shortcut past either.
void addFxSlot (xerum::model::PresetDocument& document, const int index, const xerum::dsp::FxClass cls,
                const std::vector<std::pair<std::string, xerum::model::Value>>& values, const int rack = 0)
{
    const auto slot = "FXRack" + std::to_string (rack) + "/FX[" + std::to_string (index) + "]";

    document.parameters[slot + "/type"] = number (static_cast<double> (xerum::dsp::fxTypeId (cls)));

    for (const auto& [name, value] : values)
        document.parameters[slot + "/" + std::string (xerum::dsp::fxClassName (cls)) + "/plainParams/" + name] =
            value;
}

/// Render a document through a fresh processor — the whole path a preset takes, minus the file.
std::vector<float> renderDocument (const xerum::model::PresetDocument& document, const int blocks,
                                   const int blockSize,
                                   const std::function<void (juce::MidiBuffer&, int)>& midi)
{
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, blockSize);
    processor.applyDocument (document, {}, 0);
    return render (processor, blocks, blockSize, midi);
}

/// What a slot did to the render, in the form the ticket asks for: how much of the dry render's energy the
/// difference accounts for, how many samples are not bit-identical, and the peak of each side.
struct RenderDelta
{
    double differenceDb { -std::numeric_limits<double>::infinity() };
    std::size_t differing { 0 };
    double peakWithout { 0.0 };
    double peakWith { 0.0 };
};

RenderDelta deltaOf (const std::vector<float>& without, const std::vector<float>& with)
{
    RenderDelta out;
    out.peakWithout = peakOf (without);
    out.peakWith = peakOf (with);

    double residual = 0.0;
    double reference = 0.0;

    for (std::size_t i = 0; i < without.size() && i < with.size(); ++i)
    {
        const auto difference = static_cast<double> (with[i]) - static_cast<double> (without[i]);
        residual += difference * difference;
        reference += static_cast<double> (without[i]) * static_cast<double> (without[i]);

        if (with[i] != without[i])
            ++out.differing;
    }

    if (residual > 0.0 && reference > 0.0)
        out.differenceDb = 10.0 * std::log10 (residual / reference);

    return out;
}

/// The report entries the FX layer contributed — every one of them names a rack.
std::vector<xerum::model::ReportEntry> fxEntries (const XerumAudioProcessor& processor)
{
    std::vector<xerum::model::ReportEntry> out;

    for (const auto& entry : processor.report().entries)
        if (entry.modulePath.starts_with ("FXRack"))
            out.push_back (entry);

    return out;
}

} // namespace

TEST_CASE ("a preset's FX reach the output: the render with the slots differs from the render without them",
           "[plugin][audio][fx]")
{
    const auto dry = renderDocument (fxDocument(), 16, 512, noteThenRelease (60, 8));

    // `kHardClip` at 60 % drive is a shape no wavetable produces, so the two renders cannot agree. The
    // text parameter is carried the way a preset carries it (`kParamMode` is a string leaf).
    auto document = fxDocument();

    addFxSlot (document, 0, xerum::dsp::FxClass::Distortion,
               { { "kParamMode", text ("kHardClip") },
                 { "kParamDrive", number (60.0) },
                 { "kParamWet", number (100.0) } });

    const auto wet = renderDocument (document, 16, 512, noteThenRelease (60, 8));

    REQUIRE (wet.size() == dry.size());

    const auto delta = deltaOf (dry, wet);

    std::fprintf (stderr,
                  "[fx] one FXDistortion slot (hard clip, drive 60, wet 100): difference %.2f dB, "
                  "%zu of %zu samples differ, peak %.6f -> %.6f\n",
                  delta.differenceDb, delta.differing, dry.size(), delta.peakWithout, delta.peakWith);

    CHECK (delta.peakWithout > 0.01);   // the patch renders without the slot
    CHECK (delta.differing > 1000);     // **and the slot changes it, sample by sample**

    // The counterfactual the whole ticket is stated in: the same document with the slot removed renders
    // differently, so "the rack is wired" is a measurement rather than a claim about the call existing.
    CHECK (delta.differenceDb > -20.0);

    // And the decode did not have to guess at anything: the slot's parameters are all names the effect
    // knows, so the rack had nothing to report.
    XerumAudioProcessor reported;
    reported.prepareToPlay (48000.0, 512);
    reported.applyDocument (document, {}, 0);
    CHECK (fxEntries (reported).empty());
}

TEST_CASE ("the rack runs in the preset's order, so swapping two slots swaps the render",
           "[plugin][audio][fx]")
{
    // Two slots whose order a listener would hear: a hard clip into a delay is not a delay into a hard
    // clip — the first echoes a clipped signal, the second clips an echo. If the rack ran in any fixed
    // order of its own, these two documents would render identically.
    const auto delayValues = std::vector<std::pair<std::string, xerum::model::Value>> {
        { "kParamTimeL", number (0.05) }, { "kParamLink", number (1.0) },
        { "kParamFeedback", number (40.0) }, { "kParamWet", number (60.0) }
    };
    const auto clipValues = std::vector<std::pair<std::string, xerum::model::Value>> {
        { "kParamMode", text ("kHardClip") }, { "kParamDrive", number (40.0) },
        { "kParamWet", number (100.0) }
    };

    auto clipThenDelay = fxDocument();
    addFxSlot (clipThenDelay, 0, xerum::dsp::FxClass::Distortion, clipValues);
    addFxSlot (clipThenDelay, 1, xerum::dsp::FxClass::Delay, delayValues);

    auto delayThenClip = fxDocument();
    addFxSlot (delayThenClip, 0, xerum::dsp::FxClass::Delay, delayValues);
    addFxSlot (delayThenClip, 1, xerum::dsp::FxClass::Distortion, clipValues);

    const auto a = renderDocument (clipThenDelay, 24, 512, noteThenRelease (60, 12));
    const auto b = renderDocument (delayThenClip, 24, 512, noteThenRelease (60, 12));

    REQUIRE (a.size() == b.size());

    const auto delta = deltaOf (a, b);

    std::fprintf (stderr, "[fx] distortion/delay vs delay/distortion: difference %.2f dB, %zu of %zu samples "
                          "differ\n",
                  delta.differenceDb, delta.differing, a.size());

    CHECK (delta.differing > 100);
    CHECK (delta.differenceDb > -30.0);

    // And each order really is the preset's: the two slots are not silently collapsed into one.
    CHECK (a != b);
}

TEST_CASE ("the rack feeds the ceiling, so the ceiling is still the last thing in the chain",
           "[plugin][audio][fx][limiter]")
{
    // The ceiling's place is ticket 58's, and its latency declaration assumes the rack is *before* it. What
    // proves the order is a render the rack has pushed over the ceiling: if the rack ran after the limiter,
    // the boosted signal would arrive at the host unbounded.
    const auto boosted = [&] (const double ceilingOn)
    {
        auto document = fxDocument();
        document.parameters["Master0/plainParams/kParamLimitOn"] = number (ceilingOn);

        // A peak band, +24 dB, on the note's own fundamental: the band boost is the rack's own
        // `kParamGain1` in its schema units, so this is a preset's value and not a test-only hook.
        addFxSlot (document, 0, xerum::dsp::FxClass::Equalizer,
                   { { "kParamFreq1", number (261.0) }, { "kParamGain1", number (24.0) },
                     { "kParamType1", number (2.0) }, { "kParamReso1", number (20.0) },
                     { "kParamWet", number (100.0) } });

        return renderDocument (document, 24, 512, noteThenRelease (60, 12));
    };

    const auto open = boosted (0.0);
    const auto limited = boosted (1.0);

    REQUIRE (open.size() == limited.size());

    const auto openPeak = peakOf (open);
    const auto limitedPeak = peakOf (limited);

    std::fprintf (stderr, "[fx] EQ +24 dB through the rack: peak %.6f with the ceiling off, %.6f with it on\n",
                  openPeak, limitedPeak);

    // **Red before:** the rack's output exceeds the ceiling, which is what makes the next assertion bite.
    CHECK (openPeak > ceilingLinear() * 1.2);

    // **Green after:** the ceiling holds it, so the rack is upstream of the ceiling and not downstream.
    CHECK (limitedPeak <= ceilingLinear() + 1.0e-6);
    CHECK (limitedPeak > ceilingLinear() * 0.9);
}

TEST_CASE ("what the rack cannot build is reported, per slot, and not quietly defaulted",
           "[plugin][audio][fx]")
{
    // Three things a slot can carry that nothing here acts on, each of which must be *named*: a parameter
    // name the effect does not know, an `FXFilter` whose type the preset never stored, and a drawn
    // `flex[0]` curve whose domain is not established.
    auto document = fxDocument();

    addFxSlot (document, 0, xerum::dsp::FxClass::Distortion,
               { { "kParamMode", text ("kHardClip") }, { "kParamNotAThing", number (1.0) } });
    addFxSlot (document, 1, xerum::dsp::FxClass::Filter, { { "kParamFreq", number (0.5) } });
    document.parameters["FXRack0/FX[1]/FXFilter/plainParams/kParamReso"] = number (20.0);

    // A drawn curve, the way the corpus stores one: `flex[0]` beside the class's parameters.
    document.parameters["FXRack0/FX[0]/flex[0]/numPoints"] = number (1.0);
    document.parameters["FXRack0/FX[0]/flex[0]/xVals[0]"] = number (0.0);
    document.parameters["FXRack0/FX[0]/flex[0]/xVals[1]"] = number (1.0);
    document.parameters["FXRack0/FX[0]/flex[0]/yVals[0]"] = number (1.0);
    document.parameters["FXRack0/FX[0]/flex[0]/yVals[1]"] = number (0.0);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (document, {}, 0);

    const auto entries = fxEntries (processor);

    const auto named = [&entries] (const std::string& item)
    {
        return std::any_of (entries.begin(), entries.end(),
                            [&item] (const xerum::model::ReportEntry& entry)
                            { return entry.item == item || entry.item.find (item) != std::string::npos; });
    };

    const auto text = processor.reportText();

    std::fprintf (stderr, "[fx] report for a slot the rack cannot build in full:\n");

    for (const auto& entry : entries)
        std::fprintf (stderr, "[fx]   %-9s %-14s %-34s %s\n",
                      std::string (xerum::model::toString (entry.severity)).c_str(), entry.modulePath.c_str(),
                      entry.item.c_str(), entry.reason.c_str());

    // The parameter name no effect knows: named with its class, so a user can find it.
    CHECK (named ("FXDistortion/kParamNotAThing"));
    CHECK (text.contains ("kParamNotAThing"));

    // The filter whose type the preset never stored. The reference's *default* type applies in the
    // reference; xerum does not know it, so the band passes unfiltered and the report says which.
    CHECK (named ("FXFilter"));
    CHECK (text.contains ("no filter type"));

    // The drawn curve: reported as not applied rather than read as the transfer curve.
    CHECK (named ("flex[0]"));
    CHECK (text.contains ("drawn curve"));
}

TEST_CASE ("a deferred splitter is named by the import report rather than skipped in silence",
           "[plugin][audio][fx][corpus]")
{
    // `FXSplit3` and `FXSplitMS` are deferred by name (ticket 12), and the model's own walk already reports
    // them. This is the end-to-end check that survives the wiring: the plugin's FX layer must not swallow
    // the entry, and the rack must not run the slots a deferred splitter claims — their routing is not
    // reproduced, so running them serially would invent a topology the preset does not have.
    const juce::File preset (juce::String (corpusRoot()) + "/Presets/Factory/Keyboard/KY - Additive.SerumPreset");

    if (! preset.existsAsFile())
        SKIP ("the reference's corpus is not installed at " << corpusRoot().toStdString());

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (preset).wasOk());

    const auto entries = fxEntries (processor);

    const auto named = [&entries] (const std::string& item)
    {
        return std::any_of (entries.begin(), entries.end(),
                            [&item] (const xerum::model::ReportEntry& entry) { return entry.item == item; });
    };

    // The preset carries `FXRack0/FX[1]/type = 15` — `FXSplitMS` — and the report says so, once.
    CHECK (named ("FXSplitMS"));
    CHECK (processor.reportText().contains ("not shipped in v1"));
}

TEST_CASE ("a preset whose rack holds a resonant delay does not run away", "[plugin][audio][fx][corpus]")
{
    // **The regression the wiring found.** `ARP - Aardvark`'s `FXDelay` stores BW 3.28 with 47 % feedback,
    // and `FXDelay` reads BW as the loop filter's Q — so the loop's gain was `feedback x Q` = 1.5 and the
    // delay grew without bound instead of decaying. Measured here through the plugin, with the ceiling
    // **off** (which is how the fidelity layer renders, its golden being the reference's own unlimited
    // output): 2.3e9 by the end of a 2 s render. The effect's own case is `[fx-loop]` in
    // `tests/dsp/fx_rack_test.cpp`; this is the same property at the boundary a user hears.
    const juce::File preset (juce::String (corpusRoot()) + "/Presets/Factory/Arp/ARP - Aardvark.SerumPreset");

    if (! preset.existsAsFile())
        SKIP ("the reference's corpus is not installed at " << corpusRoot().toStdString());

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (preset).wasOk());

    // The ceiling would hide a runaway behind its own clamp, so it is off: what is measured is the rack.
    processor.setParameterValue ("Master0/plainParams/kParamLimitOn", 0.0);

    const auto rendered = render (processor, 190, 512, noteThenRelease (60, 47));
    const auto peak = peakOf (rendered);

    std::fprintf (stderr, "[fx] ARP - Aardvark with its rack, ceiling off: peak %.6f over 2 s\n", peak);

    CHECK (std::isfinite (peak));
    CHECK (peak < 2.0);
}

TEST_CASE ("a preset whose rack carries a drawn filter response loads and renders", "[plugin][audio][fx][corpus]")
{
    // **The drawn response, on real material.** `FXFilter` is the voice filter exposed as a master effect,
    // so a preset can inline a pole/zero response for it — six factory slots do, and `BA - Kitchen Sink`'s
    // is `FXRack0/FX[1]/FXFilter` with four objects (32 sections, the engine's cap). Ticket 60 parameterised
    // `poleZeroFromDocument` so this slot could share the voice filter's reader; the reader is the one place
    // a *stale* call could walk a null path (see `PoleZeroObjectPath`), so the product path is measured
    // here rather than only in a unit test with a hand-built document: it loads, it does not report a
    // problem for that slot, and it renders.
    const juce::File preset (juce::String (corpusRoot()) + "/Presets/Factory/Bass/Reese/BA - Kitchen Sink.SerumPreset");

    if (! preset.existsAsFile())
        SKIP ("the reference's corpus is not installed at " << corpusRoot().toStdString());

    const auto loaded = xerum::plugin::loadPresetFile (preset);
    REQUIRE (loaded.ok);

    // The document really carries the response under the object path the decode builds from it.
    CHECK (loaded.document.parameters.count ("FXRack0/FX[1]/FXFilter/plainParams/kParamType") == 1);
    CHECK (loaded.document.parameters.count ("FXRack0/FX[1]/FXFilter/PZs[0]/fp[0]") == 1);
    CHECK (loaded.document.parameters.count ("FXRack0/FX[1]/FXFilter/PZs[3]/types[7]") == 1);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (preset).wasOk());

    const auto rendered = render (processor, 32, 512, noteThenRelease (60, 16));
    const auto peak = peakOf (rendered);

    std::fprintf (stderr, "[fx] BA - Kitchen Sink (drawn FXFilter response): peak %.6f\n", peak);

    CHECK (std::isfinite (peak));
    CHECK (peak > 0.001);   // it renders, and nothing in the decode walked off the response

    // **The reader itself, on this preset's own object path** — the same path `planFxRack` builds from the
    // document key, and the surface the crash was on. The preset carries four objects, which is the engine's
    // cap of 32 sections; a decode that had read the wrong object path would come back empty.
    const auto description = xerum::plugin::poleZeroFromDocument (
        loaded.document, xerum::plugin::PoleZeroObjectPath { "FXRack0/FX[1]/FXFilter" });

    CHECK (description.count == 4 * xerum::dsp::PoleZeroDescription::kSectionsPerObject);
    CHECK (description.count == xerum::dsp::PoleZeroDescription::kMaxSections);
}

TEST_CASE ("each shipped class engages: the per-class difference the rack makes to one render",
           "[plugin][audio][fx]")
{
    // The dev's version of "what was missing": for each class, one slot with a representative setting, and
    // the measured difference against the same document without the slot. It is a table of *engagements*,
    // not of quality — the settings are stated here rather than derived, and whether the effect sounds like
    // the reference's is not something a number answers.
    struct Row
    {
        const char* label;
        xerum::dsp::FxClass cls;
        std::vector<std::pair<std::string, xerum::model::Value>> values;
        std::size_t expectedReports;   ///< what the rack has to say about this one
    };

    const std::vector<Row> rows {
        { "FXDistortion", xerum::dsp::FxClass::Distortion,
          { { "kParamMode", text ("kHardClip") }, { "kParamDrive", number (60.0) }, { "kParamWet", number (100.0) } }, 0 },
        { "FXFlanger", xerum::dsp::FxClass::Flanger,
          { { "kParamRate", number (0.5) }, { "kParamDepth", number (100.0) }, { "kParamFeedback", number (60.0) },
            { "kParamWet", number (100.0) } }, 0 },
        { "FXPhaser", xerum::dsp::FxClass::Phaser,
          { { "kParamNumPoles", number (6.0) }, { "kParamFreq", number (500.0) }, { "kParamRate", number (0.5) },
            { "kParamDepth", number (100.0) }, { "kParamFeedback", number (40.0) },
            { "kParamWet", number (100.0) } }, 0 },
        { "FXChorus", xerum::dsp::FxClass::Chorus,
          { { "kParamRate", number (0.8) }, { "kParamDepth", number (100.0) }, { "kParamDelay", number (5.0) },
            { "kParamWet", number (100.0) } }, 0 },
        { "FXDelay", xerum::dsp::FxClass::Delay,
          { { "kParamTimeL", number (0.05) }, { "kParamLink", number (1.0) }, { "kParamFeedback", number (50.0) },
            { "kParamWet", number (100.0) } }, 0 },
        { "FXComp", xerum::dsp::FxClass::Compressor,
          { { "kParamThresh", number (0.2) }, { "kParamRatio", number (4.0) }, { "kParamAttack", number (10.0) },
            { "kParamRelease", number (100.0) }, { "kParamMakeup", number (6.0) },
            { "kParamWet", number (100.0) } }, 0 },
        { "FXReverb", xerum::dsp::FxClass::Reverb,
          { { "kParamSize", number (60.0) }, { "kParamDelay", number (2.0) }, { "kParamWet", number (100.0) } }, 0 },
        { "FXEQ", xerum::dsp::FxClass::Equalizer,
          { { "kParamFreq1", number (261.0) }, { "kParamGain1", number (18.0) }, { "kParamType1", number (2.0) },
            { "kParamReso1", number (20.0) }, { "kParamWet", number (100.0) } }, 0 },
        { "FXFilter", xerum::dsp::FxClass::Filter,
          { { "kParamType", text ("L12") }, { "kParamFreq", number (0.3) }, { "kParamReso", number (40.0) },
            { "kParamWet", number (100.0) } }, 0 },
        { "FXHyperD", xerum::dsp::FxClass::HyperDimension,
          { { "kParamUnison", number (4.0) }, { "kParamRate", number (50.0) }, { "kParamDetune", number (50.0) },
            { "kParamWet", number (100.0) } }, 0 },
        { "FXBode", xerum::dsp::FxClass::Bode,
          { { "kParamShift", number (50.0) }, { "kParamRange", number (2000.0) },
            { "kParamWet", number (100.0) } }, 0 },
        // **No impulse response exists to resolve**: the content layer does not read the reference's `IR`
        // tree yet (tickets 16/34), so this one passes the dry signal and *says so* — a row in this table
        // with no difference is the honest answer, not a silent one.
        { "FXConv", xerum::dsp::FxClass::Convolve,
          { { "kParamWet", number (100.0) }, { "kParamDecay", number (2.0) } }, 1 },
        { "FXUtils", xerum::dsp::FxClass::Utility,
          { { "kParamLPF", number (700.0) }, { "kParamWet", number (100.0) } }, 0 },
        { "FXSplit", xerum::dsp::FxClass::Split,
          { { "kParamFreq", number (2000.0) }, { "kParamModuleCount1", number (1.0) },
            { "kParamModuleCount2", number (1.0) } }, 0 },
    };

    const auto dry = renderDocument (fxDocument(), 24, 512, noteThenRelease (60, 12));

    std::fprintf (stderr,
                  "\n[fx] per-class engagement against the same document without the slot (24 blocks x 512, "
                  "note 60). `residual-vs-dry` is how much of the dry render the difference accounts for: a "
                  "large negative number means the render survived, near 0 dB means it was replaced.\n");

    for (const auto& row : rows)
    {
        auto document = fxDocument();
        addFxSlot (document, 0, row.cls, row.values);

        // The splitter needs its bands: two of them, the first a hard clip so the low band is shaped and
        // the second left empty, which is the smallest arrangement that engages both halves of the
        // crossover. `kParamModuleCount1/2` are what claim them, which is the topology the schema encodes.
        if (row.cls == xerum::dsp::FxClass::Split)
        {
            addFxSlot (document, 1, xerum::dsp::FxClass::Distortion,
                       { { "kParamMode", text ("kHardClip") }, { "kParamDrive", number (60.0) },
                         { "kParamWet", number (100.0) } });
        }

        const auto wet = renderDocument (document, 24, 512, noteThenRelease (60, 12));
        const auto delta = deltaOf (dry, wet);

        // What the rack had to say about this one, so "engages nothing" and "was not reported" cannot be
        // confused for each other.
        XerumAudioProcessor processor;
        processor.prepareToPlay (48000.0, 512);
        processor.applyDocument (document, {}, 0);
        const auto notes = fxEntries (processor);

        std::fprintf (stderr,
                      "[fx]   %-12s residual-vs-dry %7.2f dB   differing %6zu   peak %.6f -> %.6f   reports %zu\n",
                      row.label, delta.differenceDb, delta.differing, delta.peakWithout, delta.peakWith,
                      notes.size());

        for (const auto& note : notes)
            std::fprintf (stderr, "[fx]        %s: %s\n", note.item.c_str(), note.reason.c_str());


        CHECK (notes.size() == row.expectedReports);

        // **Every shipped class except the convolution changes the render.** The convolution has no
        // impulse response to convolve with, which is a reported gap rather than a silent one.
        if (row.cls != xerum::dsp::FxClass::Convolve)
            CHECK (delta.differing > 0);
        else
            CHECK (delta.differing == 0);
    }
}

TEST_CASE ("a rack on BUS 1 changes nothing, because nothing feeds it — the mixer's sends are missing",
           "[plugin][audio][fx]")
{
    // **A recorded gap, not a defect of the rack.** The reference's voices send to MAIN, BUS 1 and BUS 2;
    // the sends live in the voice layer (`RoutingSlot`'s `kParamFXBus{n}Level`), which is not implemented,
    // so the bus racks are wired and handed silence. This case is the gap's record: the day the mixer
    // lands, `FXRack1` will change this render and this assertion is the one to invert.
    const auto clip = std::vector<std::pair<std::string, xerum::model::Value>> {
        { "kParamMode", text ("kHardClip") }, { "kParamDrive", number (60.0) }, { "kParamWet", number (100.0) }
    };

    auto onMain = fxDocument();
    addFxSlot (onMain, 0, xerum::dsp::FxClass::Distortion, clip);

    // The same slot on BUS 1 instead, at the default destination (summed into MAIN's input) and at unity
    // volume — so if the bus carried anything, this render would differ from the dry one.
    auto onBus = fxDocument();
    addFxSlot (onBus, 0, xerum::dsp::FxClass::Distortion, clip, 1);

    const auto dry = renderDocument (fxDocument(), 16, 512, noteThenRelease (60, 8));
    const auto mainRender = renderDocument (onMain, 16, 512, noteThenRelease (60, 8));
    const auto busRender = renderDocument (onBus, 16, 512, noteThenRelease (60, 8));

    CHECK (deltaOf (dry, mainRender).differing > 0);   // MAIN reaches the output
    CHECK (deltaOf (dry, busRender).differing == 0);   // BUS 1 does not, and that is the gap
}
