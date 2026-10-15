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
//
// ## Content resolution is the one thing that is not done where it is asked for (ticket 56)
//
// The root cause of ticket `56` is a filesystem call on the construction path, and the sample is a host's
// main thread inside `open$NOCANCEL` for two hours while `tccd` waited for an answer to a Desktop-folder
// consent prompt. The rule that comes out of it:
//
//   * **no content-root filesystem call runs on the calling thread**, construction included. The walk
//     runs on a `content::OffThreadWork` worker; the caller waits at most `kContentResolveBudgetMs`;
//   * the ordinary case is unchanged, because a walk of a handful of local files finishes inside that
//     wait — a preset load publishes its tables before it returns, exactly as it did;
//   * when the wait expires, the previous tables stand, the report says the root did not answer, and the
//     tables are published — with their own report entries — when they arrive, from the message thread.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ParameterBridge.h"
#include "PresetState.h"
#include "fx_rack.h"
#include "limiter.h"
#include "off_thread_work.h"
#include "table_library.h"
#include "voice.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace xerum::plugin
{

class XerumAudioProcessor final : public juce::AudioProcessor,
                                  public juce::ChangeBroadcaster,
                                  private juce::AudioProcessorParameter::Listener,
                                  private juce::Timer
{
public:
    XerumAudioProcessor();
    ~XerumAudioProcessor() override;

    //==============================================================================
    // Ticket 45: the modulation matrix's edits reach the engine here, not from the audio thread.
    //
    //   * a modulation parameter that changes on the **message thread** (our own editor, a host's UI)
    //     republishes immediately;
    //   * one that changes on any other thread — a host automating during `processBlock` — raises
    //     `modulationDirty_`, which the timer above drains on the message thread. The audio thread
    //     therefore never rebuilds the matrix, which is what keeps the render allocation-free.
    void parameterValueChanged (int parameterIndex, float newValue) override;
    void parameterGestureChanged (int parameterIndex, bool gestureIsStarting) override;
    void timerCallback() override;

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

    /// Apply any pending matrix (and drawn-response) rebuild **now**. Message thread.
    ///
    /// A modulation parameter's change only raises a flag — a host can move every parameter in one message,
    /// and rebuilding the engine per write would be a storm. The message-thread timer drains the flag
    /// within 50 ms; this is the explicit drain, for a caller that wants it done before returning (the
    /// tests, and any host that would rather not wait).
    void flushModulation();

    /// Publish a content resolution that landed after its bounded wait had expired, and say what it
    /// found. Message thread; does nothing when nothing is pending or when the filesystem has still not
    /// answered. The message-thread timer calls it, and a caller that wants it done before returning may
    /// call it directly.
    void flushContent();

    /// Edit one section of a voice filter's drawn response. Message thread.
    ///
    /// The edit is written to the document's `VoiceFilter{slot}/PZs[]` leaves — so it is a preset edit —
    /// and the engine's response is republished with it. The plane's callback is the only caller.
    void setPoleZeroSection (int filterSlot, int section, const dsp::PoleZeroSection& sectionValue);

    /// The content roots content is resolved from, in the order they are tried.
    ///
    /// These are the roots xerum *offers*, not the ones it has managed to read: a root that is absent, or
    /// that the OS will not let anybody read, is on this list too. Whether one can be read is reported
    /// through the import report rather than answered by leaving it out.
    std::vector<juce::String> contentRoots() const;

    /// How long content resolution may hold the thread that asked for it. A walk of a handful of local
    /// files finishes in microseconds, so this bound only ever matters for a root that will not answer — a
    /// consent prompt nobody has answered, a volume that has gone to sleep, a drive that is not spinning.
    /// Then it is the whole of what a host's construction, or a preset load, waits (ticket `56`).
    static constexpr int kContentResolveBudgetMs = 250;

private:
    /// The table's layout, resolved once by id so nothing else has to count positions.
    struct Layout
    {
        int polyCount { -1 }, mono { -1 }, legato { -1 }, portamentoTime { -1 }, portamentoCurve { -1 };
        int portaAlways { -1 }, masterVolume { -1 };

        /// Ticket `58`'s master ceiling. Its two paths are xerum's own — the reference has no such leaf —
        /// so these are the only layout entries whose document paths a reference preset never carries.
        int limitOn { -1 }, limitCeiling { -1 };

        struct Osc
        {
            int enable { -1 }, volume { -1 }, pitch { -1 }, octave { -1 }, fine { -1 }, unison { -1 };
            int unisonRange { -1 }, unisonStereo { -1 }, warp1Mode { -1 }, warp1Amount { -1 };
            int warp2Mode { -1 }, warp2Amount { -1 }, tablePos { -1 };
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

        /// The eight macros (ticket 48). They are parameters because a macro is a *control* the matrix's
        /// source table names, so it must be movable — the live value reaches
        /// `ModulationInputs::macro[m] / 100`.
        std::array<int, dsp::kMacroCount> macro {};
    };

    void resolveLayout();
    int indexOfParameter (std::string_view modelPath) const;

    /// Read every parameter, and rebuild the engine's settings if any of them moved.
    void refreshSettingsFromParameters() noexcept;

    /// Poll the matrix's parameters, flagging a rebuild when any moved. Allocation-free, so the audio
    /// thread may call it; the rebuild itself is the message thread's.
    void refreshModulationParameters() noexcept;

    /// Write one parameter, taking model units, the way a document does. Message thread.
    void setParameterTo (int paramIndex, double modelValue);

    /// Swap in a resolved set of oscillator tables, with the audio thread held out.
    void publishTables (std::array<std::optional<dsp::Wavetable>, dsp::kOscillatorSlots> next);

    /// Does the slot select the wavetable engine? Absent `kParamType` means yes (ticket 36's finding);
    /// anything else means the preset asked for an engine v1 does not synthesise, and the slot is left
    /// silent rather than substituted.
    bool slotSelectsWavetable (const model::PresetDocument& document, int slot) const;

    /// The table a slot carries inline (`WTOsc{slot}/embeddedWTData[]`), or `nullopt` when the slot has
    /// none or the array is not a whole number of cycles. Contract §5: an embedded asset takes precedence
    /// over a path reference, because a preset carrying its own data is self-contained.
    std::optional<dsp::Wavetable> embeddedTable (const model::PresetDocument& document,
                                                 const std::string& prefix) const;

    /// The slot's `WTOsc{slot}/interpolateAfterLoad`, as an int; 0 when the preset does not set it.
    int interpolateAfterLoadOf (const model::PresetDocument& document, const std::string& prefix) const;

    /// Resolve the oscillator tables and complete the report with what only the plugin knows.
    ///
    /// Two halves, and the split is ticket `56`: `planContent` decides everything the *document* says —
    /// which slots want a table, which of those carry one inline, which name a file — and adds the report
    /// entries that are facts about the document alone. Only the part that reads the filesystem goes to
    /// the worker, and the caller waits a bounded time for it.
    void resolveContent (const model::PresetDocument& document);

    /// What resolving the content of one document wants: the reads it needs, and what each slot is
    /// waiting for. Pure — it never touches the filesystem, which is the whole point.
    struct ContentPlan
    {
        struct Slot
        {
            enum class Kind { Silent, Embedded, Reference };

            Kind kind { Kind::Silent };
            std::string prefix;        ///< `Oscillator{n}/WTOsc{n}`, for the report
            std::string tablePath;     ///< the document leaf a reference came from
            std::string reference;     ///< as the preset spelled it; empty when it named none
            std::optional<dsp::Wavetable> embedded;
            int lookup { -1 };         ///< this slot's index in `references`, -1 when it names no file
        };

        /// One impulse response a rack slot names (ticket `64`). Its bytes are resolved by the same worker
        /// pass as the tables — one submission, because `contentWork_` supersedes rather than queues, so a
        /// second submission would drop the first — and decoded into an `IrAsset` when the pass lands.
        struct ImpulseWant
        {
            std::string modulePath;    ///< `FXRack{n}/FX[{i}]`, for the report and for the rack's own lookup
            std::string reference;     ///< as the preset spelled it; empty when the slot names no file
            bool embedded { false };   ///< the slot carries its impulse inline (`embeddedIR`)
        };

        std::array<Slot, dsp::kOscillatorSlots> slots;
        std::vector<std::string> references;   ///< what the worker must resolve, in this order
        int fallback { -1 };                   ///< where `kDefaultTable` sits in `references`, -1 if unused

        std::vector<ImpulseWant> impulses;             ///< the racks' convolution slots, in rack order
    };

    /// **An impulse response, decoded**, and the storage its `dsp::ImpulseResponse` borrows.
    ///
    /// Ticket `64`. A rack slot holds a *pointer* to one of these, so it must outlive every `setSlots` that
    /// refers to it — which is why a plan holds them and the processor holds the plan.
    struct IrAsset
    {
        std::vector<float> storage;          ///< channel-major: `channelCount * frames` samples
        std::vector<const float*> channels;  ///< one pointer per channel, into `storage`
        dsp::ImpulseResponse impulse {};     ///< what the effect reads
        std::string modulePath;              ///< `FXRack{n}/FX[{i}]`
        std::string reference;               ///< as the preset spelled it; empty when it was inline
    };

    using IrAssets = std::vector<IrAsset>;

    /// What the worker found: one lookup per requested reference, in that order, and the content paths
    /// that refused. Nothing in here says whether it is *finished* — that is the flag `submit` hands back,
    /// which stays shut for a submission the worker superseded, so a caller holding this block knows not to
    /// publish an empty resolution.
    struct ResolvedContent
    {
        std::vector<content::TableLookup> lookups;
        std::vector<content::AssetLookup> impulseLookups;
        std::vector<content::ContentProblem> problems;

        /// The impulses that decoded, one per file reference that resolved. Decoding is a codec job, not a
        /// filesystem one, but it happens on the same worker for the same reason: a long IR is a real cost
        /// and the caller's budget is 250 ms.
        IrAssets impulses;
    };

    /// Every convolution slot a document carries, in rack order: where it sits, what it names, and whether
    /// the preset carries the impulse inline. Pure — the decode it feeds touches nothing.
    std::vector<ContentPlan::ImpulseWant> impulseWants (const model::PresetDocument& document) const;

    /// **The impulse a slot carries inline** (`FXRack{n}/FX[{i}]/FXConv/embeddedIR[c][i]`), as an asset, or
    /// nothing when the preset's own `numChannels`/`numFrames` do not add up to one. Contract §5's
    /// precedence rule applied to impulses: a preset that carries its own impulse is self-contained, so it
    /// is used whatever else the slot names. Pure.
    std::optional<IrAsset> embeddedImpulse (const model::PresetDocument& document,
                                            const std::string& modulePath) const;

    /// Decode audio bytes into channel-major float storage — FLAC or WAV, which is what the reference's
    /// `Impulses/` corpus and a user's own IR are. The codec is JUCE's; this layer only says what the bytes
    /// are for. Returns false and fills `error` when no registered format can read them.
    static bool decodeImpulse (const std::vector<std::byte>& bytes, IrAsset& asset, std::string& error);

    ContentPlan planContent (const model::PresetDocument& document);

    /// Apply a resolution: report the roots that refused, report every slot that could not be supplied,
    /// and publish the tables. Both arguments are **consumed** — the tables are moved out of them, so
    /// this runs once per resolution and once only.
    void publishResolved (ContentPlan& plan, ResolvedContent& resolved);

    /// The engine-level gaps the bridge knowingly leaves: a warp variant's source routing. Named rather
    /// than dropped, per the contract. (The drawn pole/zero response left this list in ticket `45`, which
    /// made it reach `dsp::VoiceFilter::setPoleZero`.)
    void completeReport (const model::PresetDocument& document);

    /// Build the engine's matrix rows from the live parameters, with a row's curves and destination
    /// *name* taken from the document (the two fields the interface does not edit). Message thread.
    std::vector<dsp::ModSlot> modulationRowsFromParameters() const;

    /// Hand the engine a rebuilt matrix, holding the audio thread out of the way first — the same
    /// handshake `publishTables` uses, because `setModulationSlots` allocates. Message thread.
    void publishModulationSlots();

    /// Write back into the document every row whose parameters differ from what the last `applyDocument`
    /// installed, so a matrix edit is a **preset edit** and not a parallel state. Message thread.
    void writeChangedModulationRowsToDocument();

    /// Read every voice filter's drawn response out of the document and publish it with the settings,
    /// holding the audio thread out. Message thread.
    void publishPoleZero();

    /// Read the ten LFOs and the four matrix envelopes out of the document (ticket 48) and hand them to the
    /// engine, holding the audio thread out — the settings carry the drawn point arrays, so this is a swap
    /// of engine state the audio thread reads, not a message. Message thread.
    void publishModulationSources();

    /// The generators' settings a document describes: every `LFO{N}` and `Env{N}` the reference stores.
    dsp::ModSourceSettings modulationSourcesFromDocument (const model::PresetDocument& document) const;

    /// Each oscillator's two warp **_sources_**, read from the document's own mode names (ticket 48).
    ///
    /// A warp mode's *family* is a parameter — the interface's choice list — and the signal the mode reads
    /// is only in the name the preset stored: `kFM_OSC` and `kFM_SUB` are one choice and two sources. Both
    /// arrays are written while the audio thread is held out and only read from it.
    std::array<std::array<dsp::WarpSource, 2>, dsp::kOscillatorSlots> warpSource_ {};

    /// Start the drain timer if it is not running and this is the message thread. Called from the
    /// constructor, from `prepareToPlay` and from `createEditor`, so a host that constructs the processor
    /// off the message thread still gets one the moment it can.
    void ensureTimer();

    /// Raise `frozen_` and wait, **bounded**, for the in-flight block to end. Returns false when the wait
    /// times out, and un-raises it; a caller that gets false must defer rather than touch engine state the
    /// audio thread may be reading. The bound matters: a host that calls `processBlock` on this very
    /// thread, or a host that never leaves its block, must not hang the message thread.
    bool freezeAndWait() noexcept;

    //==============================================================================
    // **The FX rack** (ticket `60`).
    //
    // A preset's three `FXRack{n}/FX[]` arrays are the reference's MAIN / BUS 1 / BUS 2 racks, and their
    // slots are *not* parameters: nothing in the reference's preset puts an effect on the host-visible
    // surface, so there is nothing for the bridge to carry and nothing to poll per block. They are read
    // from the **document** — the same schema paths everything else arrives by — when a preset is applied,
    // and handed to `dsp::FxBank` with the audio thread held out, because building an effect allocates.
    //
    // **The plan exists because a `FxSlot` borrows.** Its drawn curve and its drawn response are pointers,
    // and the rack dereferences them when it *rebuilds* — which `prepareToPlay` may ask for a second time.
    // So the storage the slots point into is owned here, and replaced only while the audio thread is out.

    /// The decoded racks, the assets they borrow from, and what the decode itself has to report.
    struct FxPlan
    {
        std::array<std::vector<dsp::FxSlot>, dsp::FxBank::kRackCount> racks {};

        /// One drawn response per slot that carried one. `unique_ptr` because a slot holds a *pointer* to
        /// it and the vector grows; owned here because `FxSlot` only borrows.
        std::vector<std::unique_ptr<dsp::PoleZeroDescription>> responses {};

        /// **The impulses** (ticket `64`), for the same reason and by the same rule: a slot holds a
        /// pointer, so the plan owns the storage. `inlineImpulses` are the ones a slot carries in the
        /// document (`embeddedIR`), which the decode builds with no filesystem at all; `resolvedImpulses`
        /// are the content layer's, shared with the processor so that a `prepareToPlay` after a load that
        /// re-builds the chain out of these slots still dereferences live memory. `unique_ptr` for the same
        /// reason the drawn responses use it: a slot points into the element, and a vector that grows moves
        /// its elements.
        std::vector<std::unique_ptr<IrAsset>> inlineImpulses {};
        std::shared_ptr<const IrAssets> resolvedImpulses {};

        /// Where a slot's `FxSlot` sits in `racks[rack]`, for the slots that borrow an impulse: the rack's
        /// slots are built in array order with an empty one for a gap, so a slot's position is not its FX
        /// index, and the attach pass has to be told where each one landed. The reference travels with it,
        /// because the content layer's assets are keyed by both: a document that names a *different*
        /// impulse for the same slot must not be handed the last document's.
        struct ImpulsePosition
        {
            std::string modulePath;
            std::string reference;
            int rack { 0 };
            std::size_t position { 0 };
        };

        std::vector<ImpulsePosition> impulsePositions {};

        /// What the decode could not hand the rack: a slot's drawn curve, whose domain is not established.
        /// The rack's own `unresolved()` and `problems()` are added to these when it is built.
        std::vector<model::ReportEntry> notes {};
    };

    /// Read the three racks out of a document. Pure — it touches no engine state and no audio thread.
    std::shared_ptr<FxPlan> planFxRack (const model::PresetDocument& document) const;

    /// **The routing matrix's bus sends** (ticket `64`), read out of a document and reported: a level a
    /// slot could not have honoured says so. Pure, like the rack's own decode, and published with it —
    /// both are the document's routing and both are read by the audio thread only through `settings_`.
    dsp::BusSendSettings planRouting (const model::PresetDocument& document);

    /// Install a document's racks, with the audio thread held out, and report what neither the decode nor
    /// the rack could honour. Message thread, under the state lock (which `applyDocument` holds).
    void publishFxRack (const model::PresetDocument& document);

    /// Publish the rack a timed-out handshake deferred. Message thread; the timer calls it, and it re-plans
    /// from the document rather than installing a plan the document may have outlived.
    void flushFx();

    ParameterBridge bridge_;
    Layout layout_;

    dsp::VoiceEngine engine_;

    /// **The master ceiling** (ticket `58`) — the last stage in the chain, after the rack the flat `FX`
    /// arrays describe. Sized in `prepareToPlay`; `process` allocates nothing.
    dsp::Limiter limiter_;

    /// **The rack itself** (ticket `60`): MAIN, BUS 1 and BUS 2, in that order, between the master gain
    /// and the ceiling. A preset with no `FXRack{n}` entries leaves all three empty, and an empty rack's
    /// `process` is a loop over no nodes.
    dsp::FxBank fxBank_ {};

    /// The decoded racks the bank was built from, and the assets its slots borrow. A `prepareToPlay` after
    /// a load re-prepares the bank, which rebuilds the chain from the slots it holds — and dereferences
    /// this plan's curves and responses while it does. So the plan outlives every `setSlots` that refers to
    /// it, and a new one is installed only with the audio thread held out.
    std::shared_ptr<FxPlan> fxPlan_ {};

    /// **The voice layer's bus sends** (ticket `64`), decoded from the current document. Read by the audio
    /// thread only as part of `settings_` — `refreshSettingsFromParameters` copies it in — and written here
    /// only while the audio thread is held out, which is the same rule the drawn responses follow.
    dsp::BusSendSettings routing_ {};

    /// The impulse responses the content layer has resolved for the current document, keyed by slot. Shared
    /// with the plan that borrows them, so replacing this leaves the installed rack's pointers valid.
    std::shared_ptr<const IrAssets> irAssets_ {};

    /// A preset whose impulse responses are still being resolved. The rack is **not** published while this
    /// is set: publishing it would say "no impulse response was supplied" about a preset whose impulses are
    /// on their way, and the report is not allowed to say something it will have to take back. The timer's
    /// `flushContent` clears it and publishes the rack.
    bool impulsesPending_ { false };

    /// A publish whose handshake timed out: the audio thread never left its block, so the rack was not
    /// replaced. The timer retries, re-planning from the document — never installing a plan the document
    /// may have outlived.
    std::atomic<bool> fxDeferred_ { false };

    /// **BUS 1 and BUS 2's inputs.** The mixer that feeds them is the voice layer's sends (ticket `64`):
    /// the engine adds each voice's send into these at the routing matrix's levels, the master gain is
    /// applied to them with the main path's, and the rack processes what is left. Cleared per block, because
    /// the engine adds rather than overwrites. Sized in `prepareToPlay`.
    std::vector<float> bus1Left_ {}, bus1Right_ {}, bus2Left_ {}, bus2Right_ {};

    std::array<std::optional<dsp::Wavetable>, dsp::kOscillatorSlots> tables_ {};

    /// Shared with the worker, which may outlive a destructor that cannot join it: the resolution reads
    /// this library on its own thread, so it cannot be a member the processor could destroy underneath it.
    /// Touched by that thread alone — `contentRoots()` answers from `contentRoots_` instead.
    std::shared_ptr<content::TableLibrary> library_;

    /// The content-root walk, off the calling thread. Ticket `56`.
    content::OffThreadWork contentWork_;

    /// The roots xerum offers its library, in the import contract's order. **Candidates**: whether one can
    /// be read is the filesystem's answer, asked for on the worker, so this list holds no filesystem
    /// question's outcome and never changes.
    std::vector<std::filesystem::path> contentRoots_;

    /// The newest resolution handed to `contentWork_`, and its plan, result and completion flag, kept
    /// only while it is still in flight past its bounded wait. Guarded by `stateLock_`, which both
    /// `applyDocument` and the timer's `flushContent` hold.
    std::shared_ptr<ContentPlan> pendingPlan_;
    std::shared_ptr<ResolvedContent> pendingResolved_;
    std::shared_ptr<std::atomic<bool>> pendingFinished_;

    /// The drawn responses, read off the document and read by the audio thread when it rebuilds the
    /// settings. Written only while the audio thread is held out (`publishPoleZero`), which is why the
    /// audio thread may read it without a lock.
    std::array<dsp::PoleZeroDescription, dsp::kFilterSlots> poleZero_ {};

    /// A modulation parameter changed on a thread that must not rebuild the matrix (the audio thread).
    /// The message-thread timer drains it.
    std::atomic<bool> modulationDirty_ { false };

    /// Set while `applyDocument` writes the parameter batch. Every one of the 512 matrix writes would
    /// otherwise fire the listener and republish the whole matrix; this collapses them into the single
    /// publish `applyDocument` makes when the batch is done. Message thread writes it; the listener may
    /// read it on any thread.
    std::atomic<bool> applyingDocument_ { false };

    /// The engine renders stereo (ticket 41), so the plugin holds both channels. Sized in `prepareToPlay`.
    std::vector<float> left_ {};
    std::vector<float> right_ {};
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

    /// **The global modulation inputs' MIDI half** (ticket 48). Held across blocks because a controller
    /// event is a change, not a state: the matrix reads the value that is standing, the same way the mod
    /// wheel on a hardware synth stays where it was left. Written on the audio thread in `processBlock`
    /// and read on it in the same call.
    double modWheel_ { 0.0 };
    double aftertouch_ { 0.0 };
    double polyAftertouch_ { 0.0 };

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
