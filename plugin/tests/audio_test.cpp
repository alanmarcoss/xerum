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

TEST_CASE ("the gain-staging finding: the voices sum with no headroom, so the second key clips",
           "[plugin][audio][limiter]")
{
    // This is the report the ticket asks for, measured rather than asserted from reasoning, and it is
    // deliberately **not** a change to the engine: whether the voices are too hot is a separate decision.
    const auto one = peakStereo (1);
    const auto two = peakStereo (2);
    const auto sixteen = peakStereo (16);

    CHECK (one > 0.05);            // one voice renders
    CHECK (one < 1.0);             // and does not clip on its own
    CHECK (two > 1.0);             // **the second voice is the one that goes red**

    std::fprintf (stderr, "[limiter] gain staging, ceiling off: 1 voice %.6f, 2 voices %.6f, 16 voices %.6f\n",
                  one, two, sixteen);

    // Near-linear summation, sub-linear only because the voices are different notes with different phases:
    // the voice bus has no per-voice headroom and no mix-bus trim, so N voices are about N times one voice.
    CHECK (two == Catch::Approx (2.0 * one).epsilon (0.2));
    CHECK (sixteen > 4.0 * one);

    // The ceiling is what stops that reaching the host's meters, which is the whole of ticket 58's ask.
    CHECK (sixteen <= 32.0);
}
