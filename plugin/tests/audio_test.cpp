// The plugin plays, and it plays *measurably*: peak levels, spectral-free level relationships, and the
// envelope's own behaviour. Never "it ran", never "the output was non-empty".
//
// Run alone with:  ctest --test-dir build-30 -R xerum-plugin-tests  (or the binary with "[audio]")

#include "TestSupport.h"

#include <catch2/catch_approx.hpp>

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

    const auto early = std::vector<float> (automated.begin(), automated.begin() + 20 * 512);
    const auto late = std::vector<float> (automated.begin() + 20 * 512, automated.end());

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
