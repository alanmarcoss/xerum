// The plugin plays, and it plays *measurably*: peak levels, spectral-free level relationships, and the
// envelope's own behaviour. Never "it ran", never "the output was non-empty".
//
// Run alone with:  ctest --test-dir build-30 -R xerum-plugin-tests  (or the binary with "[audio]")

#include "TestSupport.h"

#include <catch2/catch_approx.hpp>

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>

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

TEST_CASE ("a preset's Filter Level leaf scales what leaves the voice filter, and its default is a no-op",
           "[plugin][audio][level]")
{
    // **Ticket `65`.** `VoiceFilter{N}/plainParams/kParamLevelOut` — the reference's "Filter 1 Level" — was
    // read by no code at all. The law is measured on the reference's own parameter of that name: its
    // display reads `-∞ / -40.0 / -28.0 / -12.1 / -0.0 / 7.0 / 12.0 dB` at stored
    // `0 / 0.05 / 0.1 / 0.25 / 0.5 / 0.75 / 1.0` — the same `4·s²` taper the FX leaves obey, unity at the
    // `0.5` the reference displays as its default — and rendered with `Filter 1 On` the law is exact:
    // stored `0.0` is silence and the peaks at `0.25 / 0.5 / 0.75 / 1.0` come back in the ratios
    // `0.25 / 1 / 2.25 / 4`. Read on presets that actually store one, the reference's own display agrees
    // again: `BA - 303 Die Treppe` stores `0.885965` / `0.635965` and displays `9.9 dB` / `4.1 dB` against
    // the law's `9.94` / `4.18`, and `ARP - Bell Arperium` stores `0.466533` and displays `-1.2 dB`
    // against `-1.20`.
    //
    // The document is `fxDocument`'s shape — ceiling off, `Main Vol` pinned — so the ratio is the law's and
    // not the limiter's, and the filter is left at its default `L12` wide open, so this measures the level
    // and not the cutoff. The leaf is written the way a preset writes it, through `applyDocument`.
    const auto levelOf = [&] (const bool setLeaf, const double stored)
    {
        auto document = fxDocument();
        document.parameters["VoiceFilter0/plainParams/kParamEnable"] = number (1.0);

        if (setLeaf)
            document.parameters["VoiceFilter0/plainParams/kParamLevelOut"] = number (stored);

        return rmsOf (renderDocument (document, 24, 512, noteThenRelease (60, 12)));
    };

    const auto unity = levelOf (true, 0.5);
    REQUIRE (unity > 0.01);

    for (const auto stored : { 0.25, 0.75, 1.0 })
    {
        INFO ("Filter 1 Level stored " << stored);
        CHECK (levelOf (true, stored) / unity == Catch::Approx (4.0 * stored * stored).epsilon (0.01));
    }

    // The bottom of the knob is silence, which is the reference's `[-∞ dB]`.
    CHECK (levelOf (true, 0.0) == 0.0);

    // **The leaf's own default is the unity point**, so a preset that omits it renders exactly what the
    // filter rendered before this law was applied — not merely "a sensible value close to unity".
    CHECK (levelOf (false, 0.0) == unity);

    // And a filter that is off ignores the leaf entirely, which is what the reference does: sweeping
    // `Filter 1 Level` with `Filter 1 On` off left its own render byte-identical.
    const auto disabledWith = [&] (const double stored)
    {
        auto document = fxDocument();
        document.parameters["VoiceFilter0/plainParams/kParamEnable"] = number (0.0);
        document.parameters["VoiceFilter0/plainParams/kParamLevelOut"] = number (stored);
        return renderDocument (document, 24, 512, noteThenRelease (60, 12));
    };

    CHECK (deltaOf (disabledWith (0.25), disabledWith (0.5)).differing == 0);
}

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
          // Ticket 62 re-measured the threshold knob (`1.1·(1−s)³`, not `-120·s`); 0.6 is the stored
          // value whose threshold is ≈ −23 dB, which is what this row has always exercised.
          { { "kParamThresh", number (0.6) }, { "kParamRatio", number (4.0) }, { "kParamAttack", number (10.0) },
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

TEST_CASE ("a rack on BUS 1 changes the render, because the voice layer's send feeds it",
           "[plugin][audio][fx]")
{
    // **The gap ticket `60` recorded, closed by ticket `64`.** The reference's voices send to MAIN, BUS 1
    // and BUS 2, and the send lives in the voice layer — `RoutingSlot{N}`'s `kParamFXBus{n}Level`, which
    // nothing read. So `FXRack1` was wired and handed silence, and this case's assertion was the reverse of
    // this one: that a rack on BUS 1 changed *nothing*. The pair below is the ticket's own proof shape —
    // the same rack, twice, with the send as the only difference between the renders.
    const auto clip = std::vector<std::pair<std::string, xerum::model::Value>> {
        { "kParamMode", text ("kHardClip") }, { "kParamDrive", number (60.0) }, { "kParamWet", number (100.0) }
    };

    auto onMain = fxDocument();
    addFxSlot (onMain, 0, xerum::dsp::FxClass::Distortion, clip);

    // The same slot on BUS 1 instead, at the default destination (summed into MAIN's input) and at unity
    // volume — so what reaches it is what the voice layer sends.
    auto onBus = fxDocument();
    addFxSlot (onBus, 0, xerum::dsp::FxClass::Distortion, clip, 1);

    // The routing matrix's own leaf, written the way a preset writes it: `RoutingSlot0` is `OSC A`, and the
    // level is the percent the reference stores. 100 % is unity through the send's cubic law.
    auto onBusSent = onBus;
    onBusSent.parameters["RoutingSlot0/plainParams/kParamFXBus1Level"] = number (100.0);

    const auto dry = renderDocument (fxDocument(), 16, 512, noteThenRelease (60, 8));
    const auto mainRender = renderDocument (onMain, 16, 512, noteThenRelease (60, 8));
    const auto busRender = renderDocument (onBus, 16, 512, noteThenRelease (60, 8));
    const auto sentRender = renderDocument (onBusSent, 16, 512, noteThenRelease (60, 8));

    CHECK (deltaOf (dry, mainRender).differing > 0);   // MAIN reaches the output

    // No send in the preset: the bus carries nothing, and a rack on it changes nothing. That is the
    // *absence* of a send, which is what most presets have — not a gap, any more.
    CHECK (deltaOf (dry, busRender).differing == 0);

    // **With the send, the same rack has something to process and reaches the output.** The difference is
    // the send's, nothing else changed.
    const auto sent = deltaOf (busRender, sentRender);
    CHECK (sent.differing > 0);
    CHECK (sent.differenceDb > -60.0);
}

//==============================================================================
// Ticket `64` — the two structural gaps ticket `60` named rather than hid.
//
// 1. **The mixer's sends.** `RoutingSlot{N}/plainParams/kParamFXBus{n}Level` is a voice's level into
//    BUS 1 or BUS 2, and nothing read it: the busses were wired end to end and fed silence. Both halves
//    are asserted here — the level's own law (measured on the reference first, through its host) and the
//    difference it makes to a render, on synthetic material and on a factory preset.
// 2. **The impulse responses.** A preset's `FXConv` names one (`relativePathToIR`) or carries one
//    (`embeddedIR`), and neither reached the convolver: the effect reported "no impulse response was
//    supplied" for every convolution in the bank. What is asserted is that it now renders *with* the
//    impulse, that a missing one is an ordinary miss named at the slot, and that a path the OS refuses is
//    a refusal carrying its path — ticket `56`'s distinction, applied to the IR path.

namespace
{

/// The reference's own send law, as `dsp::BusSendSettings` documents it: the preset stores a percent and
/// the amplitude is that percent cubed. Written here so the test states the law rather than reading it out
/// of the implementation.
double sendGainOf (const double percent)
{
    const auto unit = percent / 100.0;
    return unit * unit * unit;
}

/// Is this slot's own problem in the report? A convolution that could not be given an impulse says so
/// through the effect's `problem()`, which lands as an `FXRack{n}` entry whose item is the class.
bool hasFxConvProblem (const XerumAudioProcessor& processor)
{
    for (const auto& entry : processor.report().entries)
        if (entry.modulePath.starts_with ("FXRack") && entry.item == "FXConv" && entry.severity == xerum::model::Severity::High)
            return true;

    return false;
}

/// Every entry whose item is this slot's impulse reference, and every `content` entry — the two halves of
/// the absence/refusal distinction.
std::vector<xerum::model::ReportEntry> entriesFor (const XerumAudioProcessor& processor, const std::string& item)
{
    std::vector<xerum::model::ReportEntry> out;

    for (const auto& entry : processor.report().entries)
        if (entry.item == item)
            out.push_back (entry);

    return out;
}

/// A decaying impulse in a 16-bit WAV, which is what a user's own impulse response looks like and what the
/// decoder must read. Written by hand so the test has no fixture and no reference install behind it.
void writeImpulseWav (const juce::File& file, const int frames, const double rate)
{
    const auto samples = [frames]
    {
        std::vector<float> out (static_cast<std::size_t> (frames));

        for (int i = 0; i < frames; ++i)
            out[static_cast<std::size_t> (i)]
                = static_cast<float> (std::exp (-3.0 * static_cast<double> (i) / 512.0)
                                      * std::sin (2.0 * 3.14159265358979323846 * 0.01 * static_cast<double> (i)));

        return out;
    }();

    const auto dataBytes = static_cast<juce::uint32> (samples.size() * 2);
    juce::MemoryOutputStream out;

    out.write ("RIFF", 4);
    out.writeInt (static_cast<int> (36 + dataBytes));
    out.write ("WAVE", 4);
    out.write ("fmt ", 4);
    out.writeInt (16);
    out.writeShort (1);                                        // PCM
    out.writeShort (1);                                        // mono
    out.writeInt (static_cast<int> (rate));
    out.writeInt (static_cast<int> (rate) * 2);                // byte rate
    out.writeShort (2);                                        // block align
    out.writeShort (16);                                       // bits
    out.write ("data", 4);
    out.writeInt (static_cast<int> (dataBytes));

    for (const auto sample : samples)
        out.writeShort (static_cast<short> (juce::jlimit (-1.0f, 1.0f, sample) * 32767.0f));

    file.replaceWithData (out.getData(), out.getDataSize());
}

/// A content root of the test's own making, held as `XERUM_CONTENT_ROOT` for as long as it lives — the
/// same way `content_gate_test` and `third_party_test` pin one, and for the same reason: which root
/// supplies an impulse must be the test's decision, not the machine's.
struct IrRoot
{
    juce::File root;

    IrRoot()
    {
        root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getNonexistentChildFile ("xerum-ir-root", {}, false);
        REQUIRE (root.createDirectory().wasOk());

        setenv ("XERUM_CONTENT_ROOT", root.getFullPathName().toRawUTF8(), 1);
    }

    ~IrRoot()
    {
        unsetenv ("XERUM_CONTENT_ROOT");
        root.deleteRecursively();
    }

    /// `Impulses/<name>` under the root, which is the layout the reference's own tree uses.
    juce::File impulseAt (const juce::String& name) const
    {
        auto file = root.getChildFile ("Impulses").getChildFile (name);
        file.getParentDirectory().createDirectory();
        return file;
    }
};

/// A document with one oscillator and one `FXConv` slot on MAIN — everything the impulse cases need and
/// nothing that would make the difference ambiguous.
xerum::model::PresetDocument convDocument()
{
    auto document = fxDocument();
    addFxSlot (document, 0, xerum::dsp::FxClass::Convolve,
               { { "kParamWet", number (100.0) }, { "kParamLevelOut", number (50.0) } });
    return document;
}

} // namespace

TEST_CASE ("a send's level is the preset's percent cubed, which is the reference's own law",
           "[plugin][audio][fx]")
{
    // Measured on the reference through its host before it was implemented here: `A>BUS1` at the
    // normalised levels 0.05, 0.1, 0.25, 0.5, 0.75 and 1.0 produced bus contributions in the ratios
    // 1/8000, 1/1000, 1/64, 1/8, 0.4219 and 1.0 of the full-level one — `n^3`, to the measurement.
    const auto busRmsAt = [] (const double percent)
    {
        auto document = fxDocument();
        document.parameters["RoutingSlot0/plainParams/kParamFXBus1Level"] = number (percent);
        // An empty BUS 1 at the default destination (summed into MAIN's input) at unity volume: what the
        // send adds is the whole of the difference.
        return rmsOf (renderDocument (document, 16, 512, noteThenRelease (60, 8)));
    };

    const auto none = busRmsAt (0.0);
    const auto full = busRmsAt (100.0);
    const auto at50 = busRmsAt (50.0);

    REQUIRE (full > none);

    // The send's own contribution, at unity through the bus: `sqrt(full^2 - none^2)` is not valid here
    // because the two are the same signal, so the arithmetic is done on the linear levels — the added
    // level is `full - none` at a gain of 1, and `sendGain * (full - none)` at any other.
    const auto unity = full - none;
    CHECK (at50 - none == Catch::Approx (sendGainOf (50.0) * unity).epsilon (0.02));

    // And the law is monotone in the level, which is what makes a preset's own value mean something.
    const auto at25 = busRmsAt (25.0);
    CHECK (at25 - none < at50 - none);
    CHECK (at50 - none < unity);
}

TEST_CASE ("a bus's volume leaf is the reference's squared taper, not the stored value",
           "[plugin][audio][fx]")
{
    // **Ticket `65`.** `Global{N}/plainParams/kParamFXBus{n}Vol` was handed to `FxBank::setBusVolume`
    // **raw**, so a preset's bus came out at its stored value where the reference's own display for
    // `Bus 1 Vol` reads `0% [-∞ dB] / 10% [-28.0 dB] / 25% [-12.1 dB] / 50% [-0.0 dB] / 75% [7.0 dB] /
    // 100% [12.0 dB]` at stored `0 / 0.1 / 0.25 / 0.5 / 0.75 / 1.0`. `20·log10 (4·n²)` is
    // `12.04 + 40·log10 (n)`, which is `-28.0 / -12.0 / 0.0 / +7.0 / +12.0` at those points: the squared
    // taper, unity at the `0.5` the reference displays as its default. The *send* law beside it is
    // deliberately different — the case above pins `n³`, ticket `64`'s measurement.
    //
    // Measured by difference, and on the bus's own contribution only. BUS 1 sits at the default
    // destination (summed into MAIN's *input*) and MAIN's rack is empty here, so a render is
    // `dry + volume·bus` exactly; subtracting the dry render leaves `volume·bus`, whose rms is
    // proportional to the volume. That is what makes this a statement about the mapping and not about how
    // correlated signals sum.
    const auto clip = std::vector<std::pair<std::string, xerum::model::Value>> {
        { "kParamMode", text ("kSoftClip") }, { "kParamDrive", number (30.0) }, { "kParamWet", number (100.0) }
    };

    auto document = fxDocument();
    addFxSlot (document, 0, xerum::dsp::FxClass::Distortion, clip, 1);
    document.parameters["RoutingSlot0/plainParams/kParamFXBus1Level"] = number (100.0);   // full send

    const auto dry = renderDocument (fxDocument(), 16, 512, noteThenRelease (60, 8));

    const auto busLevel = [&] (const double storedVolume)
    {
        auto with = document;
        with.parameters["Global0/plainParams/kParamFXBus1Vol"] = number (storedVolume);
        const auto rendered = renderDocument (with, 16, 512, noteThenRelease (60, 8));

        REQUIRE (rendered.size() == dry.size());
        REQUIRE (rendered.size() > 0);

        double energy = 0.0;

        for (std::size_t i = 0; i < rendered.size(); ++i)
        {
            const auto contribution = static_cast<double> (rendered[i]) - static_cast<double> (dry[i]);
            energy += contribution * contribution;
        }

        return std::sqrt (energy / static_cast<double> (rendered.size()));
    };

    const auto unity = busLevel (0.5);   // the leaf's own default, and the taper's unity point
    REQUIRE (unity > 1.0e-4);

    CHECK (xerum::dsp::levelTaperGain (0.5) == 1.0);

    for (const auto stored : { 0.25, 0.75, 1.0 })
    {
        INFO ("Bus 1 Vol stored " << stored);
        CHECK (busLevel (stored) / unity == Catch::Approx (4.0 * stored * stored).epsilon (0.01));
    }

    // The bottom of the knob removes the bus entirely, which is the reference's `[-∞ dB]`.
    CHECK (busLevel (0.0) == 0.0);

    // Omitting the leaf is the reference's default, and the default is unity — not "smaller than 0.5".
    auto withoutLeaf = document;
    withoutLeaf.parameters.erase ("Global0/plainParams/kParamFXBus1Vol");

    auto atHalf = document;
    atHalf.parameters["Global0/plainParams/kParamFXBus1Vol"] = number (0.5);

    const auto absent = renderDocument (withoutLeaf, 16, 512, noteThenRelease (60, 8));
    const auto half = renderDocument (atHalf, 16, 512, noteThenRelease (60, 8));
    CHECK (deltaOf (absent, half).differing == 0);
}

TEST_CASE ("a factory preset's own routing sends change its render", "[plugin][audio][fx][corpus]")
{
    // **On real material.** `808 - Decomposed` sends `OSC A` into BUS 1 at full level and `OSC B` into
    // BUS 2 at 89.5 %; the render with them is compared against the same document with both zeroed. The
    // only difference between the two documents is the *sends*, so the difference between the two renders
    // is the sends' — the ticket's proof shape, applied to a preset nobody wrote for it.
    //
    // **Why this preset.** Most of the bank's routing sends belong to presets whose sources are engines
    // xerum does not synthesise yet — `ARP - Strum Multi`, `ARP - Kalimba Arps` and `ARP - Pizz Orch Arp`
    // each store sends and each render silence here, so a send in them could not be measured at all. This
    // one's oscillator A is a wavetable with a filter, which is what makes the difference audible; the
    // peak check below is what keeps that from being an assumption.
    const juce::File preset (juce::String (corpusRoot()) + "/Presets/Factory/Bass/808/808 - Decomposed.SerumPreset");

    if (! preset.existsAsFile())
        SKIP ("the reference's corpus is not installed at " << corpusRoot().toStdString());

    const auto loaded = xerum::plugin::loadPresetFile (preset);
    REQUIRE (loaded.ok);

    auto sent = loaded.document;
    auto silent = loaded.document;

    int silenced = 0;

    for (int row = 0; row < 7; ++row)
        for (int bus = 1; bus <= 2; ++bus)
        {
            const auto leaf = "RoutingSlot" + std::to_string (row) + "/plainParams/kParamFXBus"
                              + std::to_string (bus) + "Level";

            if (sent.parameters.count (leaf) != 0)
            {
                silent.parameters[leaf] = number (0.0);
                ++silenced;
            }
        }

    REQUIRE (silenced > 0);   // the preset's own sends, or this case measures nothing

    const auto withSends = renderDocument (sent, 24, 512, noteThenRelease (60, 12));
    const auto withoutSends = renderDocument (silent, 24, 512, noteThenRelease (60, 12));
    const auto delta = deltaOf (withoutSends, withSends);

    std::fprintf (stderr,
                  "[sends] 808 - Decomposed: %d send levels zeroed, residual-vs-silent %.2f dB, differing %zu "
                  "of %zu, peak %.6f\n", silenced, delta.differenceDb, delta.differing, withSends.size(),
                  delta.peakWith);

    // The preset really sounds, or a zero difference would mean nothing.
    REQUIRE (delta.peakWith > 0.05);

    CHECK (delta.differing > 0);
    CHECK (delta.differenceDb > -40.0);
}

TEST_CASE ("a routing destination xerum does not apply is reported, not defaulted", "[plugin][audio][fx]")
{
    // The reference's routing matrix gives each row a destination (`kRoutingDestDirect`/`Filter`/
    // `Master`/`None`) as well as its bus sends. xerum's voice path is fixed — every source runs through
    // the voice filters into the mixer, which is what `Filter` describes — so a preset that stored
    // anything else is asking for something this build does not do. Its **sends** are applied (that is
    // this ticket); the destination is reported rather than quietly rendered through the fixed path.
    const auto destinationEntries = [] (const xerum::model::PresetDocument& document)
    {
        XerumAudioProcessor processor;
        processor.prepareToPlay (48000.0, 512);
        processor.applyDocument (document, {}, 0);
        return entriesFor (processor, "kParamRoutingDest");
    };

    auto routedElsewhere = fxDocument();
    routedElsewhere.parameters["RoutingSlot0/plainParams/kParamRoutingDest"] = text ("kRoutingDestNone");
    routedElsewhere.parameters["RoutingSlot3/plainParams/kParamRoutingDest"] = text ("kRoutingDestDirect");

    const auto reported = destinationEntries (routedElsewhere);
    REQUIRE (reported.size() == 1);
    CHECK (reported.front().reason.find ("kRoutingDestNone") != std::string::npos);
    CHECK (reported.front().reason.find ("kRoutingDestDirect") != std::string::npos);
    CHECK (reported.front().reason.find ("BUS 1 / BUS 2 sends are applied") != std::string::npos);

    // `Filter` is the path xerum actually takes, so it is not something to report.
    auto routedToFilter = fxDocument();
    routedToFilter.parameters["RoutingSlot0/plainParams/kParamRoutingDest"] = text ("kRoutingDestFilter");
    CHECK (destinationEntries (routedToFilter).empty());
}

TEST_CASE ("a convolution renders with the impulse the preset carries inline", "[plugin][audio][fx]")
{
    // `embeddedIR[c][i]` is a preset's own impulse, and contract §5's precedence rule applies to it as it
    // does to an inline wavetable: a preset that carries its own data is self-contained. Nothing here
    // touches the filesystem, so this case runs anywhere.
    constexpr int kFrames = 512;

    auto carrying = convDocument();
    carrying.parameters["FXRack0/FX[0]/FXConv/numChannels"] = number (1.0);
    carrying.parameters["FXRack0/FX[0]/FXConv/numFrames"] = number (static_cast<double> (kFrames));
    carrying.parameters["FXRack0/FX[0]/FXConv/sampleRate"] = number (48000.0);

    for (int i = 0; i < kFrames; ++i)
        carrying.parameters["FXRack0/FX[0]/FXConv/embeddedIR[0][" + std::to_string (i) + "]"]
            = number (std::exp (-4.0 * static_cast<double> (i) / kFrames));

    // The counterfactual is the same slot with its impulse removed: nothing else changes, so the
    // difference between the two renders is the impulse.
    auto withoutImpulse = carrying;

    for (int i = 0; i < kFrames; ++i)
        withoutImpulse.parameters.erase ("FXRack0/FX[0]/FXConv/embeddedIR[0][" + std::to_string (i) + "]");

    const auto withImpulse = renderDocument (carrying, 24, 512, noteThenRelease (60, 12));
    const auto dry = renderDocument (withoutImpulse, 24, 512, noteThenRelease (60, 12));
    const auto delta = deltaOf (dry, withImpulse);

    std::fprintf (stderr, "[ir] inline impulse: differing %zu of %zu, residual %.2f dB\n", delta.differing,
                  dry.size(), delta.differenceDb);

    CHECK (delta.differing > 0);

    // **And it is not reported as missing**, which is what every convolution said before ticket `64`.
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (carrying, {}, 0);
    CHECK_FALSE (hasFxConvProblem (processor));
}

TEST_CASE ("a convolution renders with the impulse the content roots supply", "[plugin][audio][fx]")
{
    // The reference's own `Impulses/` corpus is FLAC and a user's is usually WAV, so the resolution goes
    // through the content roots exactly as a wavetable's does and the file is decoded by the one codec
    // layer the plugin already links. A root of the test's own making keeps the machine out of it.
    IrRoot root;
    writeImpulseWav (root.impulseAt ("Test/Hall.wav"), 2048, 48000.0);
    REQUIRE (root.impulseAt ("Test/Hall.wav").existsAsFile());

    auto carrying = convDocument();
    carrying.parameters["FXRack0/FX[0]/FXConv/relativePathToIR"] = text ("Test/Hall.wav");

    auto withoutReference = carrying;
    withoutReference.parameters.erase ("FXRack0/FX[0]/FXConv/relativePathToIR");

    const auto withImpulse = renderDocument (carrying, 24, 512, noteThenRelease (60, 12));
    const auto dry = renderDocument (withoutReference, 24, 512, noteThenRelease (60, 12));
    const auto delta = deltaOf (dry, withImpulse);

    std::fprintf (stderr, "[ir] resolved impulse: differing %zu of %zu, residual %.2f dB\n", delta.differing,
                  dry.size(), delta.differenceDb);

    CHECK (delta.differing > 0);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (carrying, {}, 0);
    CHECK_FALSE (hasFxConvProblem (processor));

    // **A miss is an ordinary miss named at the slot** — and the dry signal still passes, so neither case
    // becomes silence.
    auto missing = convDocument();
    missing.parameters["FXRack0/FX[0]/FXConv/relativePathToIR"] = text ("Test/NotThere.wav");

    XerumAudioProcessor missingProcessor;
    missingProcessor.prepareToPlay (48000.0, 512);
    missingProcessor.applyDocument (missing, {}, 0);

    const auto named = entriesFor (missingProcessor, "Test/NotThere.wav");
    REQUIRE (named.size() == 1);
    CHECK (named.front().modulePath == "FXRack0/FX[0]");
    CHECK (named.front().severity == xerum::model::Severity::High);
    CHECK (named.front().reason.find ("not found in") != std::string::npos);

    const auto dryRender = renderDocument (missing, 24, 512, noteThenRelease (60, 12));
    CHECK (peakOf (dryRender) > 0.01);   // the convolution passes the signal, it does not silence it
}

TEST_CASE ("a content root the OS refuses is a refusal with its path, for an impulse as for a table",
           "[plugin][audio][fx]")
{
    // Ticket `56`'s distinction, on the IR path: absence is an ordinary miss and says nothing about the
    // machine; a path that is *there* and will not be read is a refusal, and it carries its path. The
    // refusal here is a directory with its permissions taken away, which is the shape a consent prompt,
    // a locked volume or a protected folder eventually takes.
    IrRoot root;
    const auto impulses = root.root.getChildFile ("Impulses");
    impulses.createDirectory();
    writeImpulseWav (impulses.getChildFile ("Hidden.wav"), 512, 48000.0);
    REQUIRE (impulses.getChildFile ("Hidden.wav").existsAsFile());

    if (::chmod (impulses.getFullPathName().toRawUTF8(), 0000) != 0)
        SKIP ("could not take this directory's permissions away");

    auto document = convDocument();
    document.parameters["FXRack0/FX[0]/FXConv/relativePathToIR"] = text ("Hidden.wav");

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (document, {}, 0);

    // Put the permissions back before any assertion can throw, so a failure does not leave a directory the
    // test's own cleanup cannot remove.
    ::chmod (impulses.getFullPathName().toRawUTF8(), 0755);

    const auto refusals = entriesFor (processor, impulses.getChildFile ("Hidden.wav").getFullPathName().toStdString());

    bool sawRefusal = ! refusals.empty();

    for (const auto& entry : processor.report().entries)
        if (entry.modulePath == "content" && entry.reason.find ("could not be read") != std::string::npos)
            sawRefusal = true;

    CHECK (sawRefusal);

    // The refusal is not a silence: the convolution still passes the signal.
    const auto rendered = renderDocument (document, 16, 512, noteThenRelease (60, 8));
    CHECK (peakOf (rendered) > 0.01);
}

TEST_CASE ("a factory preset's convolution renders with its FLAC impulse", "[plugin][audio][fx][corpus]")
{
    // **The real thing**: `808 - Diesel` convolves with `Factory/Coloration/Woody.flac` out of the
    // reference's own `Impulses/` tree — 109 FLACs in 7 directories, referenced by relative path — which is
    // the corpus the resolution and the decoder exist for. The comparison is against the same document with
    // the reference removed, so the difference is the impulse.
    const juce::File preset (juce::String (corpusRoot()) + "/Presets/Factory/Bass/808/808 - Diesel.SerumPreset");

    if (! preset.existsAsFile())
        SKIP ("the reference's corpus is not installed at " << corpusRoot().toStdString());

    const auto loaded = xerum::plugin::loadPresetFile (preset);
    REQUIRE (loaded.ok);

    auto carrying = loaded.document;
    std::string reference;

    // Which slot names an impulse by file, and what it names. One pass: the erase below is the only other
    // change to the document, so the two renders differ in nothing else.
    std::vector<std::string> referencePaths;

    for (const auto& [path, value] : carrying.parameters)
        if (path.ends_with ("FXConv/relativePathToIR"))
        {
            referencePaths.push_back (path);

            if (value.isText())
                reference = value.text;
        }

    REQUIRE_FALSE (reference.empty());

    auto withoutReference = carrying;

    for (const auto& path : referencePaths)
        withoutReference.parameters.erase (path);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (carrying, {}, 0);

    // The impulse resolved: the reference's own report says which file, and the effect no longer reports a
    // missing one.
    const auto named = entriesFor (processor, reference);
    CHECK (named.empty());                 // found, so nothing to report about it
    CHECK_FALSE (hasFxConvProblem (processor));

    const auto withImpulse = renderDocument (carrying, 24, 512, noteThenRelease (60, 12));
    const auto dry = renderDocument (withoutReference, 24, 512, noteThenRelease (60, 12));
    const auto delta = deltaOf (dry, withImpulse);

    std::fprintf (stderr, "[ir] 808 - Diesel / %s: differing %zu of %zu, residual %.2f dB, peak %.6f -> %.6f\n",
                  reference.c_str(), delta.differing, dry.size(), delta.differenceDb, delta.peakWithout,
                  delta.peakWith);

    CHECK (delta.differing > 0);
    CHECK (peakOf (withImpulse) < 32.0);   // it convolves; it does not run away
}


