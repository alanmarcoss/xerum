// xerum::plugin — the JUCE adapter.
//
// Ticket 30. One target, three formats (AUv2, VST3, Standalone) per ticket 11. What this class owns:
//
//   * the **parameter bridge** (hosts see xerum's ids and labels — `ParameterBridge.h`);
//   * **preset loading** through ticket 23's document, with the import report reaching the interface;
//   * **state**: `getStateInformation` writes a `PresetDocument` in xerum's own block format, never a
//     reference container (ticket 32 — see `PresetState.h`);
//   * the **real-time discipline**: the audio thread never allocates, never locks, and never throws into
//     the host.
//
// ## Why the parameters, not the document, are the live store
//
// A `PresetDocument` is the *import* form: a sparse map of model paths, held as `std::map<std::string,
// Value>`. It cannot be the live store, because the audio thread must read every value without touching
// a map — so the live store is the JUCE parameter objects (atomics; ticket 38 already showed the host's
// own dump is keyed on their ids), and a preset load is a batch of parameter writes. The document is kept
// alongside so that a save does not destroy a parameter the plugin does not model, which the import
// contract forbids.
//
// ## Applying a document while audio is running
//
// Replacing the wavetables would free memory the audio thread may be reading inside a block, so
// `applyDocument` does two things: it calls `suspendProcessing`, and it waits for the in-flight block to
// finish using a flag the audio thread sets and clears around `processBlock`. Waiting on a flag is not a
// lock on the audio thread — the audio thread only ever does two atomic stores — and it is correct even
// for a host that ignores suspension.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ParameterBridge.h"
#include "PresetState.h"
#include "table_library.h"
#include "voice.h"

#include <atomic>
#include <optional>
#include <vector>

namespace xerum::plugin
{

class XerumAudioProcessor final : public juce::AudioProcessor,
                                  public juce::ChangeBroadcaster
{
public:
    XerumAudioProcessor();
    ~XerumAudioProcessor() override;

    //==============================================================================
    // AudioProcessor
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override;
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    /// The longest tail xerum can produce: the amplitude envelope's authored maximum release (32 s).
    /// Not a guess — it is the parameter's own ceiling, and a host that honours it will render a long
    /// release to its end instead of cutting it off.
    double getTailLengthSeconds() const override { return 32.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destination) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // xerum's own surface, for the editor, the standalone and the tests.

    const ParameterBridge& bridge() const { return bridge_; }

    /// Load a `.SerumPreset`. Message thread. Returns the failure rather than throwing, and records it so
    /// the interface can say what happened.
    juce::Result loadPresetFromFile (const juce::File& file);

    /// Apply a document directly — the path a preset load and a state restore share.
    void applyDocument (const model::PresetDocument& document, const model::ImportReport& report,
                        int reportEntriesFromModel);

    /// A copy of the loaded document, under the state lock.
    model::PresetDocument document() const;

    /// The complete report the user sees: the model's entries, then the plugin's own.
    model::ImportReport report() const;

    /// The report as text, for the interface and for tests.
    juce::String reportText() const;

    /// Why the last load or restore failed, or empty.
    juce::String lastError() const;

    /// Set one parameter by its model path, the way a host would. Message thread.
    void setParameterValue (std::string_view modelPath, double modelValue);

    /// Read one parameter by its model path, in model units.
    double parameterValue (std::string_view modelPath) const;

    /// The table resolved for an oscillator slot, or nullptr. Message thread.
    const dsp::Wavetable* oscillatorTable (int slot) const;

    /// The content roots content is resolved from, in the order they were tried.
    std::vector<juce::String> contentRoots() const;

private:
    /// The table's layout, resolved once by id so nothing else has to count positions.
    struct Layout
    {
        int polyCount { -1 }, mono { -1 }, legato { -1 }, portamentoTime { -1 }, portamentoCurve { -1 };
        int portaAlways { -1 }, masterVolume { -1 };

        struct Osc
        {
            int enable { -1 }, volume { -1 }, pitch { -1 }, octave { -1 }, fine { -1 }, unison { -1 };
            int unisonRange { -1 }, warp1Mode { -1 }, warp1Amount { -1 }, warp2Mode { -1 }, warp2Amount { -1 };
            int tablePos { -1 };
        };

        Osc osc[dsp::kOscillatorSlots];

        /// The two fixed single-engine slots (ticket 40). They share no shape with the container slots:
        /// NOISE (slot 3) has a colour and slot-level power, SUB (slot 4) a waveform, phase and pitch.
        struct Noise
        {
            int enable { -1 }, volume { -1 }, type { -1 }, randomPhase { -1 };
        };

        struct Sub
        {
            int enable { -1 }, volume { -1 }, shape { -1 }, octave { -1 }, pitch { -1 }, fine { -1 };
            int coarse { -1 }, initialPhase { -1 }, contiguousPhase { -1 };
        };

        Noise noise;
        Sub sub;

        struct Filter
        {
            int enable { -1 }, type { -1 }, freq { -1 }, reso { -1 }, drive { -1 }, var { -1 };
        };

        Filter filter[dsp::kFilterSlots];

        struct Amp
        {
            int attack { -1 }, decay { -1 }, sustain { -1 }, release { -1 };
        };

        Amp amp;
    };

    void resolveLayout();
    int indexOfParameter (std::string_view modelPath) const;

    /// Read every parameter, and rebuild the engine's settings if any of them moved.
    void refreshSettingsFromParameters() noexcept;

    /// Write one parameter, taking model units, the way a document does. Message thread.
    void setParameterTo (int paramIndex, double modelValue);

    /// Swap in a resolved set of oscillator tables, with the audio thread held out.
    void publishTables (std::array<std::optional<dsp::Wavetable>, dsp::kOscillatorSlots> next);

    /// Does the slot select the wavetable engine? Absent `kParamType` means yes (ticket 36's finding);
    /// anything else means the preset asked for an engine v1 does not synthesise, and the slot is left
    /// silent rather than substituted.
    bool slotSelectsWavetable (const model::PresetDocument& document, int slot) const;

    /// Resolve the oscillator tables and complete the report with what only the plugin knows.
    void resolveContent (const model::PresetDocument& document);

    /// The engine-level gaps the bridge knowingly leaves: a warp variant's source routing, the stereo
    /// unison spread, and a drawn pole/zero filter response. Named rather than dropped, per the contract.
    void completeReport (const model::PresetDocument& document);

    ParameterBridge bridge_;
    Layout layout_;

    dsp::VoiceEngine engine_;
    std::array<std::optional<dsp::Wavetable>, dsp::kOscillatorSlots> tables_ {};
    content::TableLibrary library_;

    std::vector<float> mono_;
    double sampleRate_ { 48000.0 };
    int maxBlock_ { 0 };
    int prepared_ { 0 };

    /// The last parameter values the audio thread read. Its own copy, so a change is a comparison and
    /// not a lock; `refreshSettingsFromParameters` writes it and nothing else does.
    std::vector<double> values_;

    /// The values the last `applyDocument` installed. Message thread only, and what
    /// `getStateInformation` compares against to tell "the user moved this" from "the preset said this" —
    /// so an untouched preset token survives a save byte for byte.
    std::vector<double> applied_;

    dsp::VoiceSettings settings_ {};
    double masterGain_ { 1.0 };
    double bendSemitones_ { 0.0 };

    /// Set by anything that changes a parameter or a table off the audio thread; consumed by the first
    /// block that rebuilds. `std::atomic<double>` is deliberately not used: the parameters themselves are
    /// the atomics.
    std::atomic<bool> settingsDirty_ { true };

    /// The handshake that lets the message thread replace a wavetable without freeing memory the audio
    /// thread is reading: the audio thread increments `inBlock_` for the whole of `processBlock`, and the
    /// message thread sets `frozen_` and waits for `inBlock_` to reach zero before it swaps. No lock is
    /// ever taken on the audio thread — it does two atomic increments and a load.
    std::atomic<int> inBlock_ { 0 };
    std::atomic<bool> frozen_ { false };

    /// Wavetables the handshake timed out on. Only reached by a host that will not leave `processBlock`,
    /// and then they are kept rather than freed, because a leak beats a use-after-free in someone's DAW.
    std::vector<dsp::Wavetable> retired_;

    /// Guards the document, the report and the strings; **never taken on the audio thread**.
    mutable juce::CriticalSection stateLock_;
    model::PresetDocument document_;
    model::ImportReport report_;
    int modelReportEntries_ { 0 };
    juce::String lastError_;
    bool restoring_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XerumAudioProcessor)
};

} // namespace xerum::plugin
