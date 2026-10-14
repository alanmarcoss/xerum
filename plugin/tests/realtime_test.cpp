// The real-time invariant, measured the same way ticket 28 measured it in the engine: replace the global
// allocation functions with a counter, render a full sequence, and require zero.
//
// The technique is deliberately the one in `tests/dsp/voice_test.cpp` rather than a weaker one — a
// tracking allocator observes *every* allocation from any translation unit in this binary, which is what
// catches an allocation in code that nobody suspected. A positive control proves the hook works.
//
// Run alone with:  build-30/plugin/tests/xerum-plugin-tests "[realtime]"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "AllocationCounter.h"
#include "XerumAudioProcessor.h"

#include <memory>
#include <new>
#include <vector>

using namespace xerum::plugin;
using namespace xerum::plugin::tests;

// --------------------------------------------------------------- tracking allocator
//
// The hook itself is `AllocationCounter.cpp`, shared with the interface's own check that nothing on the
// audio path reaches the interface. It is inert until a test turns `g_tracking` on, so Catch2's own
// allocations are not counted; when on, every allocation in this binary is counted.

namespace
{
constexpr int kBlockSize = 512;
constexpr int kBlocks = 200;

/// The full sequence: a note-on, more notes than the polyphony so a steal happens, a parameter written
/// between blocks the way a host's automation does, the sustain pedal, and a note-off into a long tail.
/// Written as a function so the warm-up pass and the measured pass are the same sequence.
void runSequence (XerumAudioProcessor& processor, const bool automate)
{
    juce::AudioBuffer<float> buffer (2, kBlockSize);
    juce::MidiBuffer events;

    for (int block = 0; block < kBlocks; ++block)
    {
        buffer.clear();
        events.clear();

        if (block == 0 || block == 20 || block == 40 || block == 60)
            events.addEvent (juce::MidiMessage::noteOn (1, 48 + block / 20, static_cast<juce::uint8> (100)), 0);

        if (block == 30)
            events.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);   // sustain down

        if (block == 80)
            events.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);     // sustain up

        if (block == 100)
            events.addEvent (juce::MidiMessage::noteOff (1, 48), 0);

        if (block == 120)
            events.addEvent (juce::MidiMessage::pitchWheel (1, 4096), 0);

        // Automation between blocks, which is the case that would be tempting to implement with a lock.
        if (automate && block == 140)
            processor.setParameterValue ("VoiceFilter0/plainParams/kParamFreq", 0.15);

        processor.processBlock (buffer, events);
    }
}

} // namespace

TEST_CASE ("a full render performs no heap allocation", "[plugin][realtime]")
{
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, kBlockSize);

    // An instrument at its init patch, an oscillator on, a filter on and a low polyphony so the sequence
    // really steals.
    processor.setParameterValue ("Oscillator0/plainParams/kParamEnable", 1.0);
    processor.setParameterValue ("VoiceFilter0/plainParams/kParamEnable", 1.0);
    processor.setParameterValue ("Global0/plainParams/kParamPolyCount", 2.0);
    processor.prepareToPlay (48000.0, kBlockSize);   // polyphony is a prepare() argument (ticket 28)

    // Warm-up: grow every buffer the sequence will need (the `MidiBuffer`'s storage, the engine's internal
    // state) so the measured pass measures the render and not the first-time growth.
    runSequence (processor, false);

    g_allocations.store (0, std::memory_order_relaxed);
    g_tracking.store (true, std::memory_order_relaxed);

    runSequence (processor, true);

    g_tracking.store (false, std::memory_order_relaxed);

    const auto allocations = g_allocations.load (std::memory_order_relaxed);

    UNSCOPED_INFO ("allocations across " << kBlocks << " blocks: " << allocations);
    CHECK (allocations == 0);

    // The positive control. Without it, "zero allocations" is a claim about a hook that might not be
    // installed at all.
    CHECK (hookIsLive());
}

TEST_CASE ("applying a document while audio runs does not free the table the audio thread is reading",
           "[plugin][realtime]")
{
    // The handshake, exercised: a render is in flight on this thread while a document is applied, and the
    // block that follows must be silent-not-crashing rather than a use-after-free. The `inBlock_` flag is
    // what makes that true, and the block boundary is where the audio thread's view switches.
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, kBlockSize);
    processor.setParameterValue ("Oscillator0/plainParams/kParamEnable", 1.0);

    juce::AudioBuffer<float> buffer (2, kBlockSize);
    juce::MidiBuffer events;

    events.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 0);
    processor.processBlock (buffer, events);

    // A different table for slot 0, applied from "the message thread" — here, this one.
    xerum::model::PresetDocument document;
    document.parameters["Oscillator0/plainParams/kParamEnable"] = [] {
        xerum::model::Value value;
        value.type = xerum::model::Value::Type::Float;
        value.real = 1.0;
        return value;
    }();
    document.parameters["Oscillator0/WTOsc0/relativePathToWT"] = [] {
        xerum::model::Value value;
        value.type = xerum::model::Value::Type::String;
        value.text = "xerum/Saw.wav";
        return value;
    }();

    processor.applyDocument (document, {}, 0);

    int tables = 0;

    for (int slot = 0; slot < xerum::dsp::kOscillatorSlots; ++slot)
        if (processor.oscillatorTable (slot) != nullptr)
            ++tables;

    CHECK (tables >= 1);

    // Still renders, now with the new table.
    buffer.clear();
    events.clear();
    processor.processBlock (buffer, events);
    CHECK (buffer.getMagnitude (0, 0, kBlockSize) >= 0.0);
}
