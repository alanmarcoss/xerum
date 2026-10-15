// xerum::plugin — the JUCE adapter, in code. See XerumAudioProcessor.h for the argument.

#include "XerumAudioProcessor.h"

#include "PluginIdentity.h"
#include "XerumAudioProcessorEditor.h"
#include "schema.h"
#include "spectral_morph.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace xerum::plugin
{

namespace
{

/// The compiled-in source tree, so the tests and the standalone find `content/` without an install step.
/// A shipped build has no such directory and falls through to the other roots.
juce::File sourceContentDir()
{
#ifdef XERUM_CONTENT_DIR
    return juce::File (XERUM_CONTENT_DIR);
#else
    return {};
#endif
}

/// The table a slot plays when the preset names none: xerum's own, and — per the content policy — one we
/// authored rather than captured. It doubles as the documented substitution when a named asset cannot be
/// resolved, which contract §5 allows as long as the report says so.
constexpr const char* kDefaultTable = "xerum/Sine.wav";

std::string slotPrefix (const int slot)
{
    return "Oscillator" + std::to_string (slot) + "/WTOsc" + std::to_string (slot);
}

/// How long the message thread waits for an in-flight block before it gives up and keeps the memory.
constexpr int kHandshakeTimeoutMs = 250;

/// The roots xerum offers its content library, in the import contract's order (`docs/preset-import-contract.md`
/// §5): a user-configured root, then the reference instrument's content tree, then xerum's own, and last the
/// source tree, which only exists in a development checkout.
///
/// These are **candidates**. Nothing here is examined — no `exists`, no directory listing — because asking
/// the filesystem is the thing that used to hang the host (ticket `56`), and the four paths above are
/// guesses until it answers. `TableLibrary::addRoot` filters them where it is safe to ask.
std::vector<std::filesystem::path> contentRootCandidates()
{
    std::vector<std::filesystem::path> roots;

    if (const auto userRoot = juce::SystemStats::getEnvironmentVariable ("XERUM_CONTENT_ROOT", {});
        userRoot.isNotEmpty())
        roots.emplace_back (userRoot.toStdString());

    roots.emplace_back ("/Library/Audio/Presets/Xfer Records/Serum 2 Presets");

    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    roots.emplace_back (home.getChildFile ("Library/Application Support/xerum/content").getFullPathName().toStdString());

    // A shipped build has no source tree, and `XERUM_CONTENT_DIR` is then not defined at all: an invalid
    // `File` is no root, and offering one would put an empty entry in the list the report shows.
    if (const auto source = sourceContentDir(); source != juce::File())
        roots.emplace_back (source.getFullPathName().toStdString());

    return roots;
}

constexpr int kAmpEnvelopeSlot = 0;

/// The document value one **modulation field's** live value stands for, or `nullopt` when nothing should
/// be written. `spec` must come from `bridge.modulationSpecs()`, and the result is the exact leaf the
/// document stores the field under — an int for a row's source (the reference's 1-based enum), a token
/// string for its destination class, a number for the rest.
///
/// `nullopt` is the destination class's **sentinel**: the preset named a class outside xerum's
/// vocabulary, the interface shows it as "not in xerum's set", and rewriting the document's own name
/// with a class we cannot spell would drop data. So the document's value is preserved, as ticket `32`
/// requires of every token the plugin does not understand.
std::optional<model::Value> modulationDocumentValue (const ParameterBridge& bridge, const ParamSpec& spec,
                                                     const double live)
{
    if (spec.choiceSet == ChoiceSet::ModSource)
    {
        model::Value value;
        value.type = model::Value::Type::Int;
        value.integer = static_cast<std::int64_t> (std::llround (live));
        return value;
    }

    if (spec.type == ParamType::Choice)
    {
        const auto& choices = bridge.choices (spec.choiceSet);
        const auto index = static_cast<int> (std::lround (live));

        if (index < 0 || index == choices.sentinel)
            return std::nullopt;

        const auto& token = choices.modelNames[static_cast<std::size_t> (index)];

        if (token.empty())
            return std::nullopt;

        model::Value value;
        value.type = model::Value::Type::String;
        value.text = token;
        return value;
    }

    const auto* leaf = model::Schema::instance().describe (spec.schemaPath);
    model::Value value;

    if (leaf != nullptr && leaf->kind == model::LeafKind::Int)
    {
        value.type = model::Value::Type::Int;
        value.integer = static_cast<std::int64_t> (std::llround (live));
    }
    else
    {
        value.type = model::Value::Type::Float;
        value.real = live;
    }

    return value;
}

} // namespace

XerumAudioProcessor::XerumAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    setParameterTree (bridge_.releaseTree());
    resolveLayout();

    // Content roots, in the import contract's order (§5 / `content/table_library.h`). The list is built
    // here and *read* here; the filesystem is not asked anything about it, because this is the host's
    // thread inside a constructor (ticket 56 — see the header note).
    library_ = std::make_shared<content::TableLibrary>();
    contentRoots_ = contentRootCandidates();

    // Both arrays cover the *whole* parameter table — the interface surface and the matrix surface — and
    // a spec's `paramIndex` is the index into them.
    values_.assign (static_cast<std::size_t> (bridge_.parameterCount()),
                    std::numeric_limits<double>::quiet_NaN());
    applied_.assign (static_cast<std::size_t> (bridge_.parameterCount()),
                     std::numeric_limits<double>::quiet_NaN());

    // Ticket 45: the matrix's own parameters. A change on the message thread republishes the engine's rows
    // at once; a change from any other thread sets the flag the timer drains.
    for (const auto& spec : bridge_.modulationSpecs())
        bridge_.parameters()[static_cast<std::size_t> (spec.paramIndex)]->addListener (this);

    // Only when a message loop exists and this is its thread — a bare `XerumAudioProcessor` in a unit
    // test has neither, and a host has both. The timer is what drains the coalesced matrix flag, and what
    // publishes a content resolution that outlived its bounded wait (ticket 56).
    ensureTimer();

    // The init patch: no document at all, so every parameter resolves through `resolveAbsent`, and the
    // default content is resolved so the plugin makes a sound before anything is loaded.
    applyDocument ({}, {}, 0);
}

XerumAudioProcessor::~XerumAudioProcessor()
{
    stopTimer();

    for (const auto& spec : bridge_.modulationSpecs())
        bridge_.parameters()[static_cast<std::size_t> (spec.paramIndex)]->removeListener (this);
}

void XerumAudioProcessor::parameterValueChanged (const int parameterIndex, float)
{
    // The interface surface is the head of the table and the audio thread's own poll owns it; only the
    // modulation surface is republished here. (`getParameterIndex()` is the flattened tree order, which
    // is the insertion order: `specs_` first, `modulationSpecs_` after.)
    if (parameterIndex < static_cast<int> (bridge_.specs().size()))
        return;

    // A preset or a state restore writes the whole matrix in one batch; `applyDocument` publishes once
    // when it is done, and republishing per write would rebuild the engine 512 times.
    if (applyingDocument_.load (std::memory_order_acquire))
        return;

    // **Coalesced, deliberately.** A host's UI can move every parameter in one message (pluginval's
    // thread-safety test does exactly that, 500 times over), and republishing per write would rebuild the
    // engine's 64 rows a quarter of a million times. The flag is the whole of the listener's job — the
    // message thread's timer, or `flushModulation()`, applies it once. Nothing here can allocate or lock,
    // because this can run on the audio thread.
    modulationDirty_.store (true, std::memory_order_release);
}

void XerumAudioProcessor::parameterGestureChanged (int, bool) {}

void XerumAudioProcessor::timerCallback()
{
    flushModulation();

    // A rack whose install timed out on the handshake while the message thread was inside `applyDocument`.
    flushFx();

    // Ticket 56: a content resolution whose bounded wait expired publishes here, when the filesystem
    // finally answered. Cheap when there is nothing pending — one atomic load.
    flushContent();
}

void XerumAudioProcessor::ensureTimer()
{
    if (! isTimerRunning() && juce::MessageManager::existsAndIsCurrentThread())
        startTimerHz (20);
}

void XerumAudioProcessor::flushModulation()
{
    if (! modulationDirty_.exchange (false, std::memory_order_acq_rel))
        return;

    // All three are idempotent reads of the document, and any can defer the others' retry by re-raising
    // the flag — so drain the set rather than assuming only the matrix moved. Message thread (the timer,
    // or a caller); never the audio thread.
    publishPoleZero();
    publishModulationSlots();
    publishModulationSources();
}

void XerumAudioProcessor::resolveLayout()
{
    const auto find = [this] (const std::string& id)
    {
        for (int i = 0; i < static_cast<int> (bridge_.specs().size()); ++i)
            if (bridge_.specs()[static_cast<std::size_t> (i)].id == id)
                return i;

        return -1;
    };

    layout_.polyCount = find ("global0.polycount");
    layout_.mono = find ("global0.monotoggle");
    layout_.legato = find ("global0.legato");
    layout_.portamentoTime = find ("global0.portamentotime");
    layout_.portamentoCurve = find ("global0.portamentocurve");
    layout_.portaAlways = find ("global0.portaalways");
    layout_.masterVolume = find ("global0.mastervolume");

    // Ticket `58`: xerum's own two paths. The reference's schema has no limiter leaf, so these never
    // resolve against a preset — they resolve against the bridge's own table, which is the point.
    layout_.limitOn = find ("master0.limiton");
    layout_.limitCeiling = find ("master0.limitceiling");

    for (int slot = 0; slot < dsp::kOscillatorSlots; ++slot)
    {
        const auto oscillator = "oscillator" + std::to_string (slot) + ".";
        auto& osc = layout_.osc[slot];

        osc.enable = find (oscillator + "enable");
        osc.volume = find (oscillator + "volume");
        osc.pitch = find (oscillator + "pitch");
        osc.octave = find (oscillator + "octave");
        osc.fine = find (oscillator + "fine");
        osc.unison = find (oscillator + "unison");
        osc.unisonRange = find (oscillator + "unisonrange");
        osc.unisonStereo = find (oscillator + "unisonstereo");
        osc.warp1Mode = find (oscillator + "wtosc" + std::to_string (slot) + ".warpmenu");
        osc.warp1Amount = find (oscillator + "wtosc" + std::to_string (slot) + ".warp");
        osc.warp2Mode = find (oscillator + "wtosc" + std::to_string (slot) + ".warpmenu2");
        osc.warp2Amount = find (oscillator + "wtosc" + std::to_string (slot) + ".warp2");
        osc.tablePos = find (oscillator + "wtosc" + std::to_string (slot) + ".tablepos");
    }

    for (int slot = 0; slot < dsp::kFilterSlots; ++slot)
    {
        const auto filter = "voicefilter" + std::to_string (slot) + ".";
        auto& entry = layout_.filter[slot];

        entry.enable = find (filter + "enable");
        entry.type = find (filter + "type");
        entry.freq = find (filter + "freq");
        entry.reso = find (filter + "reso");
        entry.drive = find (filter + "drive");
        entry.var = find (filter + "var");
        entry.levelOut = find (filter + "levelout");
    }

    // The two fixed slots (ticket 40). Their ids come from `parameterIdFromPath`'s rule on the model
    // paths, which is why they read as `oscillator3.noiseosc3.noisetype` rather than as the reference's
    // model names.
    layout_.noise.enable = find ("oscillator3.enable");
    layout_.noise.volume = find ("oscillator3.volume");
    layout_.noise.type = find ("oscillator3.noiseosc3.noisetype");
    layout_.noise.randomPhase = find ("oscillator3.noiseosc3.randomphase");

    layout_.sub.enable = find ("oscillator4.enable");
    layout_.sub.volume = find ("oscillator4.volume");
    layout_.sub.shape = find ("oscillator4.subosc4.shape");
    layout_.sub.octave = find ("oscillator4.octave");
    layout_.sub.pitch = find ("oscillator4.pitch");
    layout_.sub.fine = find ("oscillator4.fine");
    layout_.sub.coarse = find ("oscillator4.coarsepit");
    layout_.sub.initialPhase = find ("oscillator4.subosc4.initialphase");
    layout_.sub.contiguousPhase = find ("oscillator4.subosc4.contiguousphase");

    const auto amp = "env" + std::to_string (kAmpEnvelopeSlot) + ".";
    layout_.amp.attack = find (amp + "attack");
    layout_.amp.decay = find (amp + "decay");
    layout_.amp.sustain = find (amp + "sustain");
    layout_.amp.release = find (amp + "release");

    for (int macro = 0; macro < dsp::kMacroCount; ++macro)
        layout_.macro[static_cast<std::size_t> (macro)] = find ("macro" + std::to_string (macro) + ".value");
}

int XerumAudioProcessor::indexOfParameter (const std::string_view modelPath) const
{
    for (const auto& spec : bridge_.specs())
        if (spec.modelPath == modelPath)
            return spec.paramIndex;

    // The matrix's rows are addressed the same way, which is what makes `setParameterValue
    // ("ModSlot0/plainParams/kParamAmount", …)` an edit that the listener then publishes.
    for (const auto& spec : bridge_.modulationSpecs())
        if (spec.modelPath == modelPath)
            return spec.paramIndex;

    return -1;
}

//==============================================================================
void XerumAudioProcessor::prepareToPlay (const double sampleRate, const int maximumExpectedSamplesPerBlock)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = juce::jmax (1, maximumExpectedSamplesPerBlock);

    left_.assign (static_cast<std::size_t> (maxBlock_), 0.0f);
    right_.assign (static_cast<std::size_t> (maxBlock_), 0.0f);

    const int polyphony = layout_.polyCount >= 0
                              ? static_cast<int> (parameterValue (bridge_.specs()[static_cast<std::size_t> (layout_.polyCount)]
                                                                      .modelPath))
                              : 16;

    // The pool is sized here and never on the audio thread: `VoiceEngine::prepare` rebuilds the voice
    // pool and the modulation matrix, and the matrix allocates (ticket 28's contract).
    engine_.prepare (sampleRate_, maxBlock_, polyphony);

    // **The master ceiling's latency, declared to the host** (ticket `58`). A lookahead limiter delays the
    // signal by its lookahead, and an unreported delay breaks every host's delay compensation — which is a
    // worse bug than the clipping this stage exists to fix. The number is the lookahead in samples at this
    // rate, and it does **not** move when the on/off parameter does: the delay line runs either way, so a
    // host that has compensated for it stays compensated.
    limiter_.prepare (sampleRate_);
    setLatencySamples (limiter_.latencySamples());

    // **The rack's scratch** (ticket 60). `prepare` sizes every effect's dry path and a splitter's bands
    // and rebuilds the chain from the slots a preset already installed — the rack is order-independent, and
    // this is the order that needs no deferral. It also re-dereferences the impulses those slots borrow, so
    // the plan `publishFxRack` installed has to outlive this call, which it does.
    fxBank_.prepare (sampleRate_, maxBlock_);

    // The bus racks' inputs are the mixer's, and the mixer is the voice layer's sends (ticket `64`): they
    // are cleared and filled per block in `processBlock`, and hold silence only until the first one.
    for (auto* bus : { &bus1Left_, &bus1Right_, &bus2Left_, &bus2Right_ })
        bus->assign (static_cast<std::size_t> (maxBlock_), 0.0f);

    prepared_ = 1;

    masterGain_ = 1.0;
    settingsDirty_.store (true, std::memory_order_release);
    refreshSettingsFromParameters();
}

void XerumAudioProcessor::releaseResources()
{
    prepared_ = 0;
}

bool XerumAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (! layouts.getChannelSet (true, 0).isDisabled())
        return false;   // an instrument: no input bus

    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::stereo() || output == juce::AudioChannelSet::mono();
}

//==============================================================================
void XerumAudioProcessor::refreshSettingsFromParameters() noexcept
{
    const auto& specs = bridge_.specs();
    bool changed = settingsDirty_.exchange (false, std::memory_order_acq_rel);

    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        const auto& spec = specs[i];
        const auto value = ParameterBridge::currentModelValue (*bridge_.parameters()[i], spec,
                                                              bridge_.choices (spec.choiceSet));

        if (value != values_[i])
        {
            values_[i] = value;
            changed = true;
        }
    }

    if (! changed)
        return;

    dsp::VoiceSettings next;
    const auto amount = [this] (const int index)
    {
        jassert (index >= 0);
        return index >= 0 ? values_[static_cast<std::size_t> (index)] : 0.0;
    };

    next.mode = dsp::voiceModeFor (amount (layout_.mono) != 0.0, amount (layout_.legato) != 0.0);
    next.portamentoTime = amount (layout_.portamentoTime);
    next.portamentoCurve = amount (layout_.portamentoCurve);
    next.portamentoAlways = amount (layout_.portaAlways) != 0.0;

    next.envelope.attack = amount (layout_.amp.attack);
    next.envelope.decay = amount (layout_.amp.decay);
    next.envelope.sustain = amount (layout_.amp.sustain);
    next.envelope.release = amount (layout_.amp.release);

    for (int slot = 0; slot < dsp::kOscillatorSlots; ++slot)
    {
        const auto& layout = layout_.osc[slot];
        auto& osc = next.oscillators[static_cast<std::size_t> (slot)];

        osc.enabled = amount (layout.enable) != 0.0;
        osc.volume = amount (layout.volume);
        osc.pitchSemitones = amount (layout.pitch) + 12.0 * amount (layout.octave)
                             + amount (layout.fine) / 100.0 + bendSemitones_;
        osc.unison = static_cast<int> (amount (layout.unison));
        osc.unisonRangeSemitones = amount (layout.unisonRange);
        osc.unisonStereo = amount (layout.unisonStereo);
        osc.framePosition = amount (layout.tablePos);
        osc.table = tables_[static_cast<std::size_t> (slot)].has_value()
                        ? &*tables_[static_cast<std::size_t> (slot)]
                        : nullptr;

        const auto mode1 = static_cast<int> (amount (layout.warp1Mode));
        const auto mode2 = static_cast<int> (amount (layout.warp2Mode));

        const auto& warpChoices = bridge_.choices (ChoiceSet::WarpMode);

        osc.warpMode[0] = warpChoices.warpModes[mode1];
        osc.warpMode[1] = warpChoices.warpModes[mode2];
        osc.warpAmount[0] = amount (layout.warp1Amount);
        osc.warpAmount[1] = amount (layout.warp2Amount);

        // **The warp's source** (ticket 48). The mode's *name* is where the source lives — `kFM_OSC` and
        // `kFM_SUB` are the same family and different sounds — so it is decoded from the preset's own name
        // when the document has one, and from the choice's canonical token when it does not (an interface
        // edit selects the family, and the canonical token is that family's plain variant). Both halves are
        // read off the audio thread: the document's name in `publishWarpSources`, the canonical token here.
        osc.warpSource[0] = warpSource_[static_cast<std::size_t> (slot)][0] != dsp::WarpSource::None
                                ? warpSource_[static_cast<std::size_t> (slot)][0]
                                : dsp::warpSourceFromName (warpChoices.modelNames[static_cast<std::size_t> (mode1)]);
        osc.warpSource[1] = warpSource_[static_cast<std::size_t> (slot)][1] != dsp::WarpSource::None
                                ? warpSource_[static_cast<std::size_t> (slot)][1]
                                : dsp::warpSourceFromName (warpChoices.modelNames[static_cast<std::size_t> (mode2)]);
    }

    // --- The two fixed single-engine slots (ticket 40) -------------------------------------------------
    //
    // NOISE's colour is a *menu*, and its sentinel is the reference's sample mode: an enabled slot with
    // no colour is `nullopt`, which the engine renders as silence and `completeReport` names. That is the
    // one place a substituted colour would be tempting and the one place the contract forbids it.
    {
        auto& noise = next.noise;

        noise.enabled = amount (layout_.noise.enable) != 0.0;
        noise.volume = amount (layout_.noise.volume);
        noise.randomPhase = amount (layout_.noise.randomPhase) != 0.0;

        const auto& choices = bridge_.choices (ChoiceSet::NoiseType);
        const int index = layout_.noise.type >= 0 ? static_cast<int> (amount (layout_.noise.type))
                                                  : choices.sentinel;

        noise.type = index >= 0 && index < choices.size() && index != choices.sentinel
                         ? std::optional<dsp::NoiseType> (choices.noiseTypes[index])
                         : std::nullopt;
    }

    {
        auto& sub = next.sub;

        sub.enabled = amount (layout_.sub.enable) != 0.0;
        sub.volume = amount (layout_.sub.volume);

        // One number, because that is what a frequency needs: the slot's own four pitch controls summed
        // in semitones, exactly as the model stores them.
        sub.pitchSemitones = 12.0 * amount (layout_.sub.octave) + amount (layout_.sub.pitch)
                             + amount (layout_.sub.fine) / 100.0 + amount (layout_.sub.coarse);
        sub.initialPhaseTurns = amount (layout_.sub.initialPhase) / 360.0;
        sub.contiguousPhase = amount (layout_.sub.contiguousPhase) != 0.0;

        const auto& choices = bridge_.choices (ChoiceSet::SubShape);
        const int index = layout_.sub.shape >= 0 ? static_cast<int> (amount (layout_.sub.shape)) : 0;

        // `Sine` (index 0) is the reference's own default, and its name is the one the schema never
        // stores, so an out-of-range index falls back to it rather than to a waveform nobody asked for.
        sub.shape = index >= 0 && index < choices.size() && index != choices.sentinel
                        ? choices.subShapes[index]
                        : dsp::SubShape::Sine;
    }

    for (int slot = 0; slot < dsp::kFilterSlots; ++slot)
    {
        const auto& layout = layout_.filter[slot];
        auto& filter = next.filters[static_cast<std::size_t> (slot)];

        filter.enabled = amount (layout.enable) != 0.0;
        filter.spec = bridge_.choices (ChoiceSet::FilterType)
                          .filterSpecs[static_cast<int> (amount (layout.type))];
        filter.cutoff = amount (layout.freq);
        filter.resonance = amount (layout.reso);
        filter.drive = amount (layout.drive);
        filter.morph = amount (layout.var) / 100.0;

        // **`Filter {N} Level`** (ticket `65`). The parameter carries the leaf's stored value and the voice
        // applies the reference's squared taper (`dsp::levelTaperGain`) to it — *after* the matrix has
        // moved it, because the leaf is a modulation destination and the presets that store a non-default
        // one all modulate it. See `kLevelModulationSpan`.
        filter.levelOut = amount (layout.levelOut);

        // The **drawn response**, copied from the off-thread table. A plain member copy: the table is
        // written only while the audio thread is held out (`publishPoleZero`), so this read is safe, and
        // it allocates nothing.
        filter.poleZero = poleZero_[static_cast<std::size_t> (slot)];
    }

    // **The master ceiling** reads the same parameter snapshot, so a host automating it reaches the audio
    // thread without a lock. `setEnabled` re-arms the detector (a fill, never an allocation) and does not
    // touch the reported latency, which stays where `prepareToPlay` put it.
    if (layout_.limitOn >= 0)
        limiter_.setEnabled (amount (layout_.limitOn) != 0.0);

    if (layout_.limitCeiling >= 0)
        limiter_.setCeilingDb (amount (layout_.limitCeiling));

    // **The mixer's sends** (ticket `64`), copied from the table the message thread wrote while the audio
    // thread was held out — the same rule the drawn responses above follow. Nothing else in a reference
    // preset's routing matrix is a parameter, so there is no `values_` entry to read for it.
    next.sends = routing_;

    engine_.setSettings (next);
    settings_ = next;
}

void XerumAudioProcessor::refreshModulationParameters() noexcept
{
    // A change here rebuilds the matrix, which allocates, so the audio thread only **flags** it for the
    // message thread. The comparison runs every block like the settings poll, so a host automating a
    // matrix cell is noticed even though the listener is not called for `setValue`.
    for (const auto& spec : bridge_.modulationSpecs())
    {
        const auto index = static_cast<std::size_t> (spec.paramIndex);
        const auto value = ParameterBridge::currentModelValue (*bridge_.parameters()[index], spec,
                                                              bridge_.choices (spec.choiceSet));

        if (value != values_[index])
        {
            values_[index] = value;
            modulationDirty_.store (true, std::memory_order_release);
        }
    }
}

void XerumAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    /// The whole of this block happens with `inBlock_` held, which is what lets the message thread free a
    /// wavetable the moment this returns. Two relaxed atomic operations — not a lock.
    struct InFlight
    {
        explicit InFlight (std::atomic<int>& flag) : flag_ (flag)
        {
            flag_.fetch_add (1, std::memory_order_acq_rel);
        }

        ~InFlight() { flag_.fetch_sub (1, std::memory_order_acq_rel); }

        std::atomic<int>& flag_;
    } inFlight { inBlock_ };

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), getTotalNumOutputChannels());

    for (int channel = 0; channel < numChannels; ++channel)
        buffer.clear (channel, 0, numSamples);

    if (prepared_ == 0 || numSamples <= 0)
        return;

    // Frozen means `applyDocument` is mid-swap: the settings in hand point at a table that is still
    // alive (the message thread is waiting for this block to end), so render with them and let the next
    // block pick up the new parameters.
    if (! frozen_.load (std::memory_order_acquire))
    {
        refreshSettingsFromParameters();
        refreshModulationParameters();
    }

    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();

        if (message.isNoteOn())
            engine_.noteOn (message.getNoteNumber(), static_cast<double> (message.getFloatVelocity()));
        else if (message.isNoteOff())
            engine_.noteOff (message.getNoteNumber());
        else if (message.isSustainPedalOn())
            engine_.setSustainPedal (true);
        else if (message.isSustainPedalOff())
            engine_.setSustainPedal (false);
        else if (message.isAllNotesOff() || message.isAllSoundOff())
            engine_.reset();
        else if (message.isPitchWheel())
            bendSemitones_ = 2.0 * (static_cast<double> (message.getPitchWheelValue()) - 8192.0) / 8192.0;

        // **The global modulation inputs, from MIDI** (ticket 48). Nothing constructed a
        // `ModulationInputs` before, so `Mod Wheel`, `Aftertouch` and the rest of the matrix's
        // player-driven sources were as zero as the LFOs — the same defect, one family over. What is
        // filled here is what MIDI names unambiguously; `plugin/README.md` lists what is not.
        else if (message.isController())
        {
            const auto value = static_cast<double> (message.getControllerValue()) / 127.0;

            if (message.getControllerNumber() == 1)
                modWheel_ = value;
        }
        else if (message.isChannelPressure())
            aftertouch_ = static_cast<double> (message.getChannelPressureValue()) / 127.0;
        else if (message.isAftertouch())
            polyAftertouch_ = static_cast<double> (message.getAfterTouchValue()) / 127.0;
    }

    midi.clear();

    // The pool's block inputs: the player's controls and the eight macros. A `ModulationInputs` is a
    // plain struct of doubles, so this is a store per block and never an allocation — and it is the only
    // place the matrix's player-driven sources are filled from the host's MIDI.
    {
        dsp::ModulationInputs globals;
        globals.modWheel = modWheel_;
        globals.aftertouch = aftertouch_;
        globals.polyAftertouch = polyAftertouch_;
        globals.pitchBend = bendSemitones_ / 2.0 + 0.5;   // `Pitch Bend` is a source on the 0..1 range
        globals.activeVoices = 0.0;                        // the pool fills it per voice
        globals.noteNumber = 0.0;

        for (int macro = 0; macro < dsp::kMacroCount; ++macro)
        {
            const auto index = layout_.macro[static_cast<std::size_t> (macro)];

            if (index >= 0)
                globals.macro[macro] = std::clamp (values_[static_cast<std::size_t> (index)] / 100.0, 0.0, 1.0);
        }

        engine_.setGlobalInputs (globals);
    }

    const auto gainTarget = layout_.masterVolume >= 0
                                ? std::clamp (values_[static_cast<std::size_t> (layout_.masterVolume)], 0.0, 1.0)
                                : 1.0;
    const auto gainStart = masterGain_;
    const auto gainStep = numSamples > 0 ? (gainTarget - gainStart) / static_cast<double> (numSamples) : 0.0;

    // The engine renders **stereo** and overwrites, so it is rendered in chunks no larger than `prepare`
    // sized. Ticket 41 made the bus stereo so that the unison spread reaches the output; the master gain
    // is still applied here, per sample, on both channels.
    //
    // **The mixer's sends** (ticket `64`) come out of the same call: the pool's voices *add* their BUS 1 and
    // BUS 2 sends into the buffers below, at the levels the preset's routing matrix gives them, so what the
    // bus racks process is the preset's own signal rather than the silence these used to hold. They are
    // cleared first, because the engine adds to them rather than overwriting.
    std::fill (bus1Left_.begin(), bus1Left_.end(), 0.0f);
    std::fill (bus1Right_.begin(), bus1Right_.end(), 0.0f);
    std::fill (bus2Left_.begin(), bus2Left_.end(), 0.0f);
    std::fill (bus2Right_.begin(), bus2Right_.end(), 0.0f);

    const int chunkLimit = juce::jmax (1, static_cast<int> (left_.size()));

    for (int done = 0; done < numSamples; done += chunkLimit)
    {
        const int chunk = juce::jmin (chunkLimit, numSamples - done);

        dsp::BusSends sends;
        sends.left[0] = bus1Left_.data() + done;
        sends.right[0] = bus1Right_.data() + done;
        sends.left[1] = bus2Left_.data() + done;
        sends.right[1] = bus2Right_.data() + done;

        engine_.process (left_.data() + done, right_.data() + done, sends, chunk);
    }

    // The master gain is applied to the engine's own stereo render **before** the ceiling, because the
    // ceiling is a master stage: it has to see what the master gain produced, not the host's channel
    // mapping of it. The ramp is unchanged — one step per sample across the block.
    //
    // **The sends take it too**, because they are the same mixer stage: the reference's own `Main Vol`
    // scales the bus as well as the main path (measured through its host at the init patch). When nothing
    // is sent the four buffers stay the silence they were filled with, and the multiplies are skipped.
    {
        auto gain = gainStart;
        const bool sending = engine_.settings().sends.any();

        for (int i = 0; i < numSamples; ++i)
        {
            const auto coefficient = static_cast<float> (gain);

            left_[static_cast<std::size_t> (i)] *= coefficient;
            right_[static_cast<std::size_t> (i)] *= coefficient;

            if (sending)
            {
                bus1Left_[static_cast<std::size_t> (i)] *= coefficient;
                bus1Right_[static_cast<std::size_t> (i)] *= coefficient;
                bus2Left_[static_cast<std::size_t> (i)] *= coefficient;
                bus2Right_[static_cast<std::size_t> (i)] *= coefficient;
            }

            gain += gainStep;
        }
    }

    masterGain_ = gainTarget;

    // **The rack** (ticket `60`), in the preset's own order, between the master gain and the ceiling: MAIN
    // first, then the two busses where their `kParamFXBus{n}Dest` says. It runs on what the voice pool
    // produced, so an effect sees the same signal a host would hear without it. Bus 1 and bus 2 are the
    // mixer's sends (ticket `64`), filled above at the routing matrix's levels; a preset that sends nothing
    // leaves them silent, and a bus rack then processes what it is handed — which is nothing.
    fxBank_.process (left_.data(), right_.data(), bus1Left_.data(), bus1Right_.data(), bus2Left_.data(),
                     bus2Right_.data(), numSamples);

    // **The ceiling, last** (ticket `58`). Off, `process` is a delay line and nothing else; on and not
    // engaging, it multiplies by exactly `1.0f`. Either way a preset that already sat below the ceiling
    // comes out unchanged. The delay line runs whatever the on/off parameter says, so the latency
    // declared in `prepareToPlay` is the same number with the stage on or off.
    limiter_.process (left_.data(), right_.data(), numSamples);

    // Channel 0 is the left, channel 1 the right. A mono output bus — which `isBusesLayoutSupported`
    // allows — takes their mean, and so does any channel past the second, because that is the only
    // honest answer for a layout xerum does not render. With no spread the two channels are identical, so
    // a mono host hears exactly the render it heard before the bus was stereo.
    for (int channel = 0; channel < numChannels; ++channel)
    {
        auto* out = buffer.getWritePointer (channel);
        const bool leftChannel = channel == 0 && numChannels > 1;
        const bool rightChannel = channel == 1;

        if (leftChannel || rightChannel)
        {
            const auto* source = rightChannel ? right_.data() : left_.data();

            for (int i = 0; i < numSamples; ++i)
                out[i] = source[static_cast<std::size_t> (i)];
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
                out[i] = 0.5f * (left_[static_cast<std::size_t> (i)] + right_[static_cast<std::size_t> (i)]);
        }
    }
}

//==============================================================================
void XerumAudioProcessor::setParameterTo (const int paramIndex, const double modelValue)
{
    const auto* found = bridge_.specForParameter (paramIndex);

    if (found == nullptr)
        return;

    auto* parameter = bridge_.parameters()[static_cast<std::size_t> (paramIndex)];
    const auto& spec = *found;

    double value = modelValue;

    if (spec.type == ParamType::Choice)
    {
        const auto& choices = bridge_.choices (spec.choiceSet);
        value = std::clamp (std::lround (value), 0L, static_cast<long> (choices.size() - 1));
    }
    else
    {
        value = std::clamp (value, spec.min, spec.max);
    }

    parameter->setValueNotifyingHost (parameter->convertTo0to1 (static_cast<float> (value)));
}

void XerumAudioProcessor::publishTables (std::array<std::optional<dsp::Wavetable>, dsp::kOscillatorSlots> next)
{
    // Held out of the audio thread while the old tables are destroyed. See the header note.
    frozen_.store (true, std::memory_order_release);

    bool quiet = false;

    for (int waited = 0; waited < kHandshakeTimeoutMs && ! quiet; ++waited)
    {
        quiet = inBlock_.load (std::memory_order_acquire) == 0;

        if (! quiet)
            juce::Thread::yield();
    }

    for (int slot = 0; slot < dsp::kOscillatorSlots; ++slot)
        std::swap (tables_[static_cast<std::size_t> (slot)], next[static_cast<std::size_t> (slot)]);

    // The audio thread must not reuse a stale table pointer: it rebuilds before it reads either.
    settingsDirty_.store (true, std::memory_order_release);

    if (quiet)
    {
        next = {};   // the retired tables are released here, on the message thread
    }
    else
    {
        for (auto& table : next)
            if (table.has_value())
                retired_.push_back (std::move (*table));
    }

    frozen_.store (false, std::memory_order_release);
}

bool XerumAudioProcessor::slotSelectsWavetable (const model::PresetDocument& document, const int slot) const
{
    // Ticket 36: absence of `kParamType` means the wavetable engine, which is the default; every value the
    // reference writes for it names one of the four engines v1 does not synthesise.
    const auto key = "Oscillator" + std::to_string (slot) + "/plainParams/kParamType";
    const auto found = document.parameters.find (key);

    if (found == document.parameters.end())
        return true;

    std::string text;

    if (! modelText (found->second, text) || text.empty())
        return true;

    return text == "kOsc_WT" || text == "kOsc_Wavetable";
}

std::optional<dsp::Wavetable> XerumAudioProcessor::embeddedTable (const model::PresetDocument& document,
                                                                  const std::string& prefix) const
{
    // The document keys array elements by their real index — `embeddedWTData[0]`, `[1]`, … — so
    // `std::map`'s ordering is **lexicographic**, which puts `[10]` before `[2]`. Trusting iteration
    // order would interleave the cycles; the index is parsed instead.
    const auto marker = prefix + "/embeddedWTData[";
    std::vector<float> samples;

    for (auto it = document.parameters.lower_bound (marker);
         it != document.parameters.end() && it->first.starts_with (marker); ++it)
    {
        const auto close = it->first.find (']', marker.size());

        if (close == std::string::npos)
            return std::nullopt;

        std::size_t index = 0;
        bool digits = false;

        for (auto at = marker.size(); at < close; ++at)
        {
            const char c = it->first[at];

            if (c < '0' || c > '9')
                return std::nullopt;

            index = index * 10 + static_cast<std::size_t> (c - '0');
            digits = true;
        }

        if (! digits)
            return std::nullopt;

        double value = 0.0;

        if (! modelNumber (it->second, value))
            return std::nullopt;

        if (index + 1 > samples.size())
            samples.resize (index + 1, 0.0f);

        samples[index] = static_cast<float> (value);
    }

    // A missing index would leave a silent hole inside a cycle, which is a corruption rather than a
    // table, so the count has to match the highest index seen. The format is `frameCount` cycles of
    // `kDefaultFrameSize` samples: measured over the whole factory bank, all 34 embedded slots are a
    // whole number of 2048-sample cycles (1, 2, 4 or 9 of them).
    if (samples.empty() || samples.size() % dsp::kDefaultFrameSize != 0)
        return std::nullopt;

    dsp::Wavetable table;
    table.frameSize = dsp::kDefaultFrameSize;
    table.frameCount = samples.size() / dsp::kDefaultFrameSize;
    table.samples = std::move (samples);
    table.interpolation = dsp::FrameInterpolation::Linear;
    return table;
}

int XerumAudioProcessor::interpolateAfterLoadOf (const model::PresetDocument& document,
                                                 const std::string& prefix) const
{
    const auto found = document.parameters.find (prefix + "/interpolateAfterLoad");

    if (found == document.parameters.end())
        return 0;

    double value = 0.0;

    if (! modelNumber (found->second, value))
        return 0;

    return static_cast<int> (value);
}

void XerumAudioProcessor::resolveContent (const model::PresetDocument& document)
{
    // Whatever was in flight belongs to the *previous* document, so it goes now. Its result, if it ever
    // arrives, is not this document's answer and `flushContent` will not publish it. The impulse wait goes
    // with it: a rack that is not published because of *this* document's impulses must not stay unpublished
    // because of the last one's.
    pendingPlan_.reset();
    pendingResolved_.reset();
    pendingFinished_.reset();
    impulsesPending_ = false;

    auto plan = std::make_shared<ContentPlan> (planContent (document));

    // Anything the worker has to *read*: a table reference, or an impulse a slot names by file. A preset
    // that carries everything inline needs no thread and no wait — and this is the case where ticket `56`
    // changes nothing at all.
    const bool needsFilesystem =
        ! plan->references.empty()
        || std::any_of (plan->impulses.begin(), plan->impulses.end(),
                        [] (const ContentPlan::ImpulseWant& want)
                        { return ! want.embedded && ! want.reference.empty(); });

    if (! needsFilesystem)
    {
        ResolvedContent nothing;
        publishResolved (*plan, nothing);
        return;
    }

    auto resolved = std::make_shared<ResolvedContent>();
    const auto finished = std::make_shared<std::atomic<bool>> (false);
    const auto library = library_;
    const auto roots = contentRoots_;

    contentWork_.submit ([library, roots, plan, resolved]
    {
        // **Every question the filesystem is asked is asked here.** Which roots are readable, the walk
        // that tells an exact hit from a lucky one, the read itself: ticket 56's whole substance is that
        // this does not run on the thread that asked for it, and that that thread does not wait for it
        // longer than its budget.
        for (const auto& root : roots)
            library->addRoot (root);

        resolved->lookups.reserve (plan->references.size());

        for (const auto& reference : plan->references)
            resolved->lookups.push_back (library->loadTable (reference));

        // **The impulses** (ticket `64`): the same walk, then a decode that is a codec job rather than a
        // filesystem one — and it happens here rather than on the caller's thread because a ten-second
        // impulse is a real cost and the caller's budget is 250 ms.
        resolved->impulseLookups.assign (plan->impulses.size(), {});

        for (std::size_t i = 0; i < plan->impulses.size(); ++i)
        {
            const auto& want = plan->impulses[i];

            if (want.embedded || want.reference.empty())
                continue;

            auto lookup = library->loadAsset (want.reference, content::kImpulseDirectory);

            if (lookup.found())
            {
                IrAsset asset;
                asset.modulePath = want.modulePath;
                asset.reference = want.reference;
                std::string error;

                if (decodeImpulse (lookup.bytes, asset, error))
                    resolved->impulses.push_back (std::move (asset));
                else
                {
                    // A path that is there and is not audio is a refusal like any other: recorded with its
                    // path and its reason, never silence.
                    lookup.resolution = content::Resolution::Unreadable;
                    lookup.error = error;
                }
            }

            resolved->impulseLookups[i] = std::move (lookup);
        }

        resolved->problems = library->problems();
    }, finished);

    if (contentWork_.waitForNewest (kContentResolveBudgetMs) && finished->load (std::memory_order_acquire))
    {
        publishResolved (*plan, *resolved);
        return;
    }

    // The root has not answered inside the budget. Nothing already published is disturbed, the report
    // says exactly what is missing and why, and the message thread's timer publishes this resolution —
    // with its own report entries — when the filesystem finally answers.
    //
    // `waitForNewest` can also be true here with the flag still shut: another thread's resolution
    // superseded this one, and *that* work is the one that will be published. Either way this call leaves
    // the document's tables as they are.
    addReportEntry (report_, model::Severity::High, "content", "contentRoots",
                    "the content root did not answer within " + std::to_string (kContentResolveBudgetMs)
                    + " ms, so the oscillator tables are still being resolved; they will load when the "
                    "filesystem answers");

    pendingPlan_ = std::move (plan);
    pendingResolved_ = std::move (resolved);
    pendingFinished_ = finished;

    // **A preset whose impulses are on their way is not published yet** (ticket `64`). The rack is what
    // borrows them, and a rack published now would hand its convolver nothing and say "no impulse response
    // was supplied" about a preset whose impulse is a filesystem answer away. The timer's `flushContent`
    // publishes the content and then the rack, once, with the report it deserves. Only when a slot names an
    // impulse by file: an inline one is already in hand, and a preset that names none is not waiting.
    impulsesPending_ = std::any_of (pendingPlan_->impulses.begin(), pendingPlan_->impulses.end(),
                                    [] (const ContentPlan::ImpulseWant& want)
                                    { return ! want.embedded && ! want.reference.empty(); });
}

XerumAudioProcessor::ContentPlan XerumAudioProcessor::planContent (const model::PresetDocument& document)
{
    ContentPlan plan;

    for (int slot = 0; slot < dsp::kOscillatorSlots; ++slot)
    {
        auto& wanted = plan.slots[static_cast<std::size_t> (slot)];
        wanted.prefix = slotPrefix (slot);

        if (! slotSelectsWavetable (document, slot))
        {
            addReportEntry (report_, model::Severity::Info, "Oscillator" + std::to_string (slot), "kParamType",
                            "the slot selects an engine this build does not synthesise, so it renders silence "
                            "rather than a substituted wavetable");
            continue;
        }

        wanted.tablePath = wanted.prefix + "/relativePathToWT";

        if (const auto found = document.parameters.find (wanted.tablePath); found != document.parameters.end())
            modelText (found->second, wanted.reference);

        const bool carriesEmbedded = document.parameters.find (wanted.prefix + "/embeddedWTData[0]")
                                     != document.parameters.end();

        // **The embedded table comes first**, because contract §5 says an embedded asset takes precedence
        // over a path reference — a preset carrying its own data is self-contained, and one slot in the
        // factory bank (`BA - Gnarly Reese`, oscillator C) carries both. This is also the only place
        // `interpolateAfterLoad` applies at all: ticket 37 measured the reference ignoring it for a table
        // referenced by path, so the morph is decided here, once, at bind — 2 MB per table paid once
        // rather than per sample — and a table that came from a file is never morphed.
        //
        // Deciding it *here* rather than where the file is read is the ticket 56 split: morphing an inlined
        // table is arithmetic on memory the document already holds, and asks the filesystem nothing.
        if (carriesEmbedded)
        {
            const auto embedded = embeddedTable (document, wanted.prefix);

            if (! embedded.has_value())
            {
                // The preset inlined a table, but not as a whole number of 2048-sample cycles, so there is
                // no cycle to read. Reported rather than substituted: a file in its place would be exactly
                // the silent drop the contract forbids.
                addReportEntry (report_, model::Severity::High, wanted.prefix, "embeddedWTData",
                                "the preset carries its wavetable inline, but not as a whole number of "
                                "2048-sample cycles, so this slot renders silence");
                continue;
            }

            wanted.kind = ContentPlan::Slot::Kind::Embedded;
            wanted.embedded = dsp::tableForInterpolateAfterLoad (*embedded, /*embedded*/ true,
                                                                interpolateAfterLoadOf (document, wanted.prefix));
            continue;
        }

        wanted.kind = ContentPlan::Slot::Kind::Reference;

        if (! wanted.reference.empty())
        {
            wanted.lookup = static_cast<int> (plan.references.size());
            plan.references.push_back (wanted.reference);
        }
    }

    // The substitution is resolved in the same batch as the references themselves: a slot whose own
    // reference misses falls back to xerum's own table, and asking for it here costs no second walk. It is
    // last in the list, so a slot's own lookup is always found by its own index.
    for (const auto& wanted : plan.slots)
        if (wanted.kind == ContentPlan::Slot::Kind::Reference)
        {
            plan.fallback = static_cast<int> (plan.references.size());
            plan.references.push_back (kDefaultTable);
            break;
        }

    // **The racks' impulses** (ticket `64`) join the same walk, and for a hard reason rather than a tidy
    // one: `contentWork_` supersedes its queue rather than growing it, so a second submission would drop
    // the table walk that has not run yet. One plan, one walk, one bounded wait. A slot that carries its
    // impulse inline asks for nothing — the rack's own decode builds it — so only the file references are
    // listed.
    plan.impulses = impulseWants (document);

    return plan;
}

void XerumAudioProcessor::publishResolved (ContentPlan& plan, ResolvedContent& resolved)
{
    // A root that is there and refuses to be read is the largest thing that can fail to load, so it is
    // said out loud — every time, whether or not this resolution ends up missing anything (ticket 56).
    for (const auto& problem : resolved.problems)
        addReportEntry (report_, model::Severity::High, "content", problem.path.string(), problem.reason);

    const auto lookupAt = [&resolved] (const int index) -> content::TableLookup*
    {
        return index >= 0 && index < static_cast<int> (resolved.lookups.size())
                   ? &resolved.lookups[static_cast<std::size_t> (index)]
                   : nullptr;
    };

    std::array<std::optional<dsp::Wavetable>, dsp::kOscillatorSlots> next;

    for (int slot = 0; slot < dsp::kOscillatorSlots; ++slot)
    {
        auto& wanted = plan.slots[static_cast<std::size_t> (slot)];

        if (wanted.kind == ContentPlan::Slot::Kind::Embedded)
        {
            next[static_cast<std::size_t> (slot)] = std::move (wanted.embedded);
            continue;
        }

        if (wanted.kind != ContentPlan::Slot::Kind::Reference)
            continue;

        // A slot's own lookup is its own entry in the batch, so it can be moved out: two slots naming the
        // same reference asked for it twice and hold a lookup each.
        if (auto* lookup = lookupAt (wanted.lookup); lookup != nullptr)
        {
            if (lookup->found())
            {
                next[static_cast<std::size_t> (slot)] = std::move (lookup->table);

                if (lookup->resolution == content::Resolution::CaseInsensitive)
                    addReportEntry (report_, model::Severity::Info, wanted.tablePath, wanted.reference,
                                    "resolved case-insensitively to " + lookup->path.string());
            }
            else
            {
                addReportEntry (report_, model::Severity::High, wanted.tablePath, wanted.reference,
                                lookup->error.empty()
                                    ? std::string ("the asset could not be found under any content root")
                                    : lookup->error);
            }
        }

        if (next[static_cast<std::size_t> (slot)].has_value())
            continue;

        // The fallback is **one** lookup shared by every slot that needs it, so it is copied rather than
        // moved: a slot that took it must not leave the next one with nothing.
        if (const auto* fallback = lookupAt (plan.fallback); fallback != nullptr && fallback->found())
        {
            next[static_cast<std::size_t> (slot)] = fallback->table;

            if (! wanted.reference.empty())
                addReportEntry (report_, model::Severity::Info, wanted.tablePath, wanted.reference,
                                std::string ("substituted xerum's own ") + kDefaultTable);
        }
        else
        {
            // A root that is there and will not be read has a *reason* — a permission, a path that is not a
            // regular file — and the reason is more use than the sentence below. A root that simply is not
            // installed has none, and gets the sentence the contract has always used.
            const bool unreadable = fallback != nullptr
                                    && fallback->resolution == content::Resolution::Unreadable;

            addReportEntry (report_, model::Severity::High, wanted.prefix,
                            wanted.reference.empty() ? kDefaultTable : wanted.reference,
                            unreadable
                                ? fallback->error + ", so this slot renders silence"
                                : std::string ("no content root could supply a wavetable, so this slot renders "
                                               "silence (set XERUM_CONTENT_ROOT to a directory containing Tables/)"));
        }
    }

    publishTables (std::move (next));

    // **The impulses** (ticket `64`). A miss is an ordinary miss and it is named at the slot it belongs to:
    // the convolution then passes the dry signal, which the effect itself says too. A path that is *there*
    // and could not be read — including one that is not audio this build can read — is a refusal, and the
    // walk's own words for it are carried out with its path. Neither becomes silence: the dry signal
    // passing through is said, in this report and by the effect.
    for (std::size_t i = 0; i < plan.impulses.size(); ++i)
    {
        const auto& want = plan.impulses[i];

        if (want.embedded || want.reference.empty())
            continue;

        const auto* lookup = i < resolved.impulseLookups.size()
                                 ? &resolved.impulseLookups[i]
                                 : nullptr;

        if (lookup != nullptr && lookup->found())
        {
            if (lookup->resolution == content::Resolution::CaseInsensitive)
                addReportEntry (report_, model::Severity::Info, want.modulePath, want.reference,
                                "resolved case-insensitively to " + lookup->path.string());

            continue;
        }

        addReportEntry (report_, model::Severity::High, want.modulePath, want.reference,
                        lookup != nullptr && ! lookup->error.empty()
                            ? lookup->error + ", so this convolution passes the dry signal"
                            : std::string ("no content root could supply an impulse response, so this "
                                           "convolution passes the dry signal"));
    }

    // The decoded impulses, shared with the plan that borrows them. Replacing this pointer is safe: the
    // installed rack holds the *plan*, and the plan holds the previous set.
    irAssets_ = std::make_shared<IrAssets> (std::move (resolved.impulses));
}

void XerumAudioProcessor::flushContent()
{
    const juce::ScopedLock lock (stateLock_);

    if (pendingPlan_ == nullptr || pendingResolved_ == nullptr
        || pendingFinished_ == nullptr
        || ! pendingFinished_->load (std::memory_order_acquire))
        return;

    publishResolved (*pendingPlan_, *pendingResolved_);

    pendingPlan_.reset();
    pendingResolved_.reset();
    pendingFinished_.reset();

    // **The rack this document's impulses were holding up** (ticket `64`). Its impulses are here now, so
    // the rack is published once, with them, and the report says what actually happened rather than what
    // was true before the filesystem answered.
    if (impulsesPending_)
    {
        impulsesPending_ = false;
        publishFxRack (document_);
    }

    // The tables the interface shows have changed, and so has the report: this is the same notification a
    // preset load makes, for the same reason.
    sendChangeMessage();
}

void XerumAudioProcessor::completeReport (const model::PresetDocument& document)
{
    // The model's `loadPreset` reports what the *importer* cannot honour: unknown modules, unknown
    // parameters, unimplemented engines, warps, filters and effects. What is left here is what only the
    // plugin knows, because it is about which values the *bridge* carries to the engine — and the
    // contract's rule is that nothing is dropped silently, so each gets said out loud.
    const auto text = [&document] (const std::string& path) -> std::string
    {
        std::string value;

        if (const auto found = document.parameters.find (path); found != document.parameters.end())
            modelText (found->second, value);

        return value;
    };

    const auto numberAt = [&document] (const std::string& path) -> double
    {
        double value = 0.0;

        if (const auto found = document.parameters.find (path); found != document.parameters.end())
            modelNumber (found->second, value);

        return value;
    };

    /// A stored CBOR `true`. `modelNumber` handles numbers only, and this leaf is a real bool.
    const auto truthy = [&document] (const std::string& path)
    {
        const auto found = document.parameters.find (path);

        if (found == document.parameters.end())
            return false;

        if (found->second.type == model::Value::Type::Bool)
            return found->second.boolean;

        double number = 0.0;
        return modelNumber (found->second, number) && number != 0.0;
    };

    // **The warp variants are not reported any more, and that is deliberate.** This block used to say, at
    // `info`, that a name such as `kPD_OSC` reached its family but "the source or variant this name
    // encodes is not routed yet". Ticket 48 routed it — `warpSourceFromName` recovers the suffix and
    // `Voice::render` calls `setWarpModulator` with the signal it names — so the entry was true before and
    // false after, which is worse than no entry (the same retraction ticket 45 made for the drawn
    // response). `plugin/README.md` records it, and `preset_test.cpp` now asserts the entry is gone.

    // --- The modulation generators (ticket 48) ---------------------------------------------------------
    //
    // The ten LFOs and the four matrix envelopes generate now, so there is nothing to say about them as a
    // class. What is left is the parts of an LFO the schema carries and this build does not act on: the
    // two chaotic generator types, and the fields whose unit the corpus does not settle. Each is named,
    // because "an LFO parameter was ignored" is not something a user can act on.
    for (int lfo = 0; lfo < dsp::kLfoCount; ++lfo)
    {
        const auto prefix = "LFO" + std::to_string (lfo);
        const auto params = prefix + "/plainParams/";
        const auto type = text (params + "kParamType");

        if (type == "Lorenz" || type == "Rossler")
            addReportEntry (report_, model::Severity::Medium, prefix, type,
                            "the LFO's chaotic generator type is not implemented: the preset stores the "
                            "type's name and none of its coefficients, so the LFO plays its curve and the "
                            "attractor's character is missing");

        const auto unapplied = [&] (const std::string& leaf, const char* reason)
        {
            if (numberAt (params + leaf) != 0.0)
                addReportEntry (report_, model::Severity::Medium, prefix,
                                leaf.substr (leaf.rfind ('/') + 1), reason);
        };

        unapplied ("kParamBeatSync",
                   "the LFO runs free here: its rate is `kParamRate` mapped to a free rate in hertz, and a "
                   "preset that asks to sync it to the host's tempo keeps the free rate instead");
        unapplied ("kParamAnchored",
                   "not applied: what the reference anchors an LFO's phase to is not established");
        unapplied ("kParamDefaultMode",
                   "not applied: the reference's own default mode for an LFO is not established");
        unapplied ("kParamPhaseSnap",
                   "not applied: the reference's phase snapping is a draw-time aid, and its unit is not "
                   "established");

        if (numberAt (prefix + "/curveData/loopbackPointNum") != 0.0)
            addReportEntry (report_, model::Severity::Info, prefix, "loopbackPointNum",
                            "the drawn shape loops over its whole cycle: the loopback point the preset "
                            "stores is not a point index (481 occurs with `numPoints` 3), so its unit is "
                            "not established and xerum plays every point");
    }

    // **The drawn response is not reported any more, and that is deliberate.** It used to say, at `high`,
    // that `PZ_SVF`'s response was shown and not applied. Ticket `45` made it reach
    // `dsp::VoiceFilter::setPoleZero`, so that entry was true before and false after — which is worse
    // than no entry, and exactly why ticket `41` retracted `interpolateAfterLoad`. `plugin/README.md`
    // records the retraction; `preset_test.cpp` asserts the entry is gone (it used to assert it present).

    // --- The two fixed single-engine slots (ticket 40) -------------------------------------------------
    //
    // NOISE is a *sample player* in the reference — the manual (p. 129) says so in as many words — and
    // xerum synthesises its four generated colours and nothing else. A preset that names a file, or that
    // feeds the oscillator from the host's audio input, therefore renders silence here, and that is a
    // `high`: the slot's sound is missing, not merely shaped differently. The playback controls the
    // reference offers on a *sample* — a start phase, one-shot, a rate — have nothing to act on in a
    // generated colour, and each is named rather than silently ignored.
    {
        const std::string prefix = "Oscillator3/NoiseOsc3";
        const std::string params = prefix + "/plainParams/";

        const auto sample = text (prefix + "/relativePathToNoiseSample");

        if (! sample.empty())
            addReportEntry (report_, model::Severity::High, prefix, sample,
                            "the noise oscillator is a sample player and xerum synthesises only its four "
                            "generated colours, so this slot renders silence");

        if (truthy (prefix + "/sampleFromAudioInput"))
            addReportEntry (report_, model::Severity::High, prefix, "sampleFromAudioInput",
                            "the noise oscillator is fed by the host's audio input, and xerum is an "
                            "instrument with no input bus, so this slot renders silence");

        const auto unapplied = [&] (const std::string& leaf, const char* reason)
        {
            if (numberAt (leaf) != 0.0)
                addReportEntry (report_, model::Severity::Medium, prefix,
                                leaf.substr (leaf.rfind ('/') + 1), reason);
        };

        unapplied (params + "kParamOneShot",
                   "a generated colour runs continuously and has no end to reach, so one-shot is not applied");
        unapplied (params + "kParamInitialPhase",
                   "a generated colour has no sample to start part-way into, so the start phase is not applied");
        unapplied (params + "kParamFine",
                   "a generated colour has no playback rate, so there is nothing for this to detune");
        unapplied (params + "kParamColor",
                   "the reference's own parameter surface has no counterpart for this field (measured, with "
                   "and without a colour mode loaded), so what it controls is not established and xerum does "
                   "not act on it");
        unapplied ("Oscillator3/plainParams/kParamPitch",
                   "a generated colour has no playback rate, so the slot's pitch is not applied");
        unapplied ("Oscillator3/plainParams/kParamOctave",
                   "a generated colour has no playback rate, so the slot's octave is not applied");

        // SUB is implemented in full for the four things the model stores on it — waveform, phase,
        // contiguous phase and pitch — so the only things left to say are the two the reference has and
        // xerum's engine does not.
        if (numberAt ("Oscillator4/SubOsc4/plainParams/kParamWrapPhase") != 0.0)
            addReportEntry (report_, model::Severity::Medium, "Oscillator4/SubOsc4", "kParamWrapPhase",
                            "not applied: what this private field controls is not established");

        if (document.parameters.count ("Oscillator4/plainParams/kParamPitchTrack") != 0
            && numberAt ("Oscillator4/plainParams/kParamPitchTrack") == 0.0)
            addReportEntry (report_, model::Severity::Medium, "Oscillator4", "kParamPitchTrack",
                            "the preset switches the sub's key tracking off, and xerum's sub oscillator "
                            "always follows the note");
    }
}

std::vector<dsp::ModSlot> XerumAudioProcessor::modulationRowsFromParameters() const
{
    // The **eight editable fields** come from the parameters — the live store, which `applyDocument`
    // fills from the document and a host can automate. The two the interface does not edit come from the
    // document: the row's **curves** (`kParamCurveIn`/`Out`, which the engine evaluates) and its
    // **destination name** (the redundant-with-the-id field ticket 03 showed is the only stable identity
    // across schema versions). Reading them here rather than defaulting them keeps a preset's own curve
    // and name intact through an edit.
    std::vector<dsp::ModSlot> slots (static_cast<std::size_t> (kMatrixRows));

    const auto numberAt = [this] (const std::string& path, const double fallback)
    {
        const auto found = document_.parameters.find (path);
        double value = fallback;
        return found != document_.parameters.end() && found->second.numberAsDouble (value) ? value : fallback;
    };

    const auto textAt = [this] (const std::string& path)
    {
        const auto found = document_.parameters.find (path);
        return found != document_.parameters.end() && found->second.isText() ? found->second.text
                                                                            : std::string();
    };

    const auto liveOf = [this] (const int row, const ModRowField field)
    {
        const auto* spec = bridge_.modulationSpec (row, field);
        return ParameterBridge::currentModelValue (*bridge_.parameters()[static_cast<std::size_t> (spec->paramIndex)],
                                                   *spec, bridge_.choices (spec->choiceSet));
    };

    const auto intOf = [&liveOf] (const int row, const ModRowField field)
    { return static_cast<int> (std::lround (liveOf (row, field))); };

    for (int row = 0; row < kMatrixRows; ++row)
    {
        const auto prefix = "ModSlot" + std::to_string (row) + "/";
        auto& slot = slots[static_cast<std::size_t> (row)];

        slot.source = intOf (row, ModRowField::Source);
        slot.auxSource = intOf (row, ModRowField::AuxSource);
        slot.amount = liveOf (row, ModRowField::Amount);
        slot.bipolar = liveOf (row, ModRowField::Bipolar) != 0.0;
        slot.bypassed = liveOf (row, ModRowField::Bypass) != 0.0;
        slot.destinationInstance = intOf (row, ModRowField::DestinationInstance);
        slot.destinationParameter = intOf (row, ModRowField::DestinationParameter);

        const auto& classes = bridge_.choices (ChoiceSet::DestinationClass);
        const auto classIndex = intOf (row, ModRowField::DestinationClass);
        slot.destinationClass = classIndex >= 0 && classIndex < classes.size() && classIndex != classes.sentinel
                                    ? classes.modelNames[static_cast<std::size_t> (classIndex)]
                                    : textAt (prefix + "destModuleTypeString");

        slot.destinationName = textAt (prefix + "destModuleParamName");
        slot.curveIn = numberAt (prefix + "plainParams/kParamCurveIn", slot.curveIn);
        slot.curveOut = numberAt (prefix + "plainParams/kParamCurveOut", slot.curveOut);

        // A row with no source contributes nothing and is not a row the engine evaluates; that is also
        // what keeps the 40-odd empty slots of a preset from becoming destinations that all sum to zero.
        slot.used = slot.source != 0;
    }

    return slots;
}

void XerumAudioProcessor::writeChangedModulationRowsToDocument()
{
    const auto fieldList = { ModRowField::Source,      ModRowField::AuxSource,   ModRowField::Amount,
                             ModRowField::Bipolar,     ModRowField::Bypass,      ModRowField::DestinationClass,
                             ModRowField::DestinationInstance, ModRowField::DestinationParameter };

    for (int row = 0; row < kMatrixRows; ++row)
    {
        auto changed = false;

        for (const auto field : fieldList)
        {
            const auto* spec = bridge_.modulationSpec (row, field);
            const auto index = static_cast<std::size_t> (spec->paramIndex);
            const auto live = ParameterBridge::currentModelValue (*bridge_.parameters()[index], *spec,
                                                                  bridge_.choices (spec->choiceSet));

            if (live != applied_[index])
            {
                changed = true;
                break;
            }
        }

        if (! changed)
            continue;

        // The **whole row** is written, not just the field that moved: a row the preset did not carry
        // must land in the document as a complete row, and one that did is unchanged for the fields that
        // did not move.
        for (const auto field : fieldList)
        {
            const auto* spec = bridge_.modulationSpec (row, field);
            const auto index = static_cast<std::size_t> (spec->paramIndex);
            const auto live = ParameterBridge::currentModelValue (*bridge_.parameters()[index], *spec,
                                                                  bridge_.choices (spec->choiceSet));
            applied_[index] = live;

            if (const auto value = modulationDocumentValue (bridge_, *spec, live))
                document_.parameters[spec->modelPath] = *value;
        }
    }
}

bool XerumAudioProcessor::freezeAndWait() noexcept
{
    frozen_.store (true, std::memory_order_release);

    for (int waited = 0; waited < kHandshakeTimeoutMs; ++waited)
    {
        if (inBlock_.load (std::memory_order_acquire) == 0)
            return true;

        juce::Thread::sleep (1);
    }

    frozen_.store (false, std::memory_order_release);
    return false;
}

void XerumAudioProcessor::publishModulationSlots()
{
    const juce::ScopedLock lock (stateLock_);

    // The document first, so the row this edit produced is the row the engine is then given — an edit is
    // a **preset edit**, not a parallel state.
    writeChangedModulationRowsToDocument();

    auto slots = modulationRowsFromParameters();

    if (! freezeAndWait())
    {
        // A block never ended within the bound — a host that will not leave `processBlock`, or one that
        // runs it on this thread. The flag is what makes this a retry rather than a silent drop; the timer
        // drains it when the block is finally out of the way.
        modulationDirty_.store (true, std::memory_order_release);
        return;
    }

    engine_.setModulationSlots (std::move (slots));

    frozen_.store (false, std::memory_order_release);

    sendChangeMessage();
}

void XerumAudioProcessor::publishModulationSources()
{
    const juce::ScopedLock lock (stateLock_);

    // Read while the state lock is held, and publish under the audio thread's handshake: the settings
    // carry the drawn point arrays, so a half-swapped block would be a render with one LFO's shape and
    // another LFO's phase.
    const auto next = modulationSourcesFromDocument (document_);

    std::array<std::array<dsp::WarpSource, 2>, dsp::kOscillatorSlots> warpSources {};

    for (int slot = 0; slot < dsp::kOscillatorSlots; ++slot)
    {
        const auto prefix = slotPrefix (slot) + "/plainParams/";

        for (int warp = 0; warp < 2; ++warp)
        {
            const auto leaf = std::string (warp == 0 ? "kParamWarpMenu" : "kParamWarpMenu2");
            std::string name;

            if (const auto found = document_.parameters.find (prefix + leaf);
                found != document_.parameters.end())
                modelText (found->second, name);

            warpSources[static_cast<std::size_t> (slot)][static_cast<std::size_t> (warp)]
                = dsp::warpSourceFromName (name);
        }
    }

    if (! freezeAndWait())
    {
        modulationDirty_.store (true, std::memory_order_release);
        return;
    }

    engine_.setModulationSources (next);
    warpSource_ = warpSources;

    frozen_.store (false, std::memory_order_release);
}

dsp::ModSourceSettings
XerumAudioProcessor::modulationSourcesFromDocument (const model::PresetDocument& document) const
{
    dsp::ModSourceSettings settings;

    const auto numberAt = [&document] (const std::string& path) -> std::optional<double>
    {
        const auto found = document.parameters.find (path);

        if (found == document.parameters.end())
            return std::nullopt;

        double value = 0.0;

        if (! modelNumber (found->second, value))
            return std::nullopt;

        return value;
    };

    const auto textAt = [&document] (const std::string& path) -> std::string
    {
        std::string value;

        if (const auto found = document.parameters.find (path); found != document.parameters.end())
            modelText (found->second, value);

        return value;
    };

    /// A stored CBOR `true`, or a non-zero number. `pathData/isOpen` is a real bool leaf.
    const auto truthyAt = [&document] (const std::string& path)
    {
        const auto found = document.parameters.find (path);

        if (found == document.parameters.end())
            return false;

        if (found->second.type == model::Value::Type::Bool)
            return found->second.boolean;

        double value = 0.0;
        return modelNumber (found->second, value) && value != 0.0;
    };

    /// The point arrays. A preset stores `numPoints + 1` entries — the last closes the cycle at `x == 1`
    /// — so the read walks while the leaves exist rather than trusting the count, and a shape longer than
    /// the ceiling is read to the ceiling rather than truncated in the middle of a segment.
    const auto readCurve = [&numberAt] (const std::string& prefix, const bool closed)
    {
        dsp::LfoCurve curve;
        curve.closed = closed;

        for (int i = 0; i < dsp::kMaxLfoPoints; ++i)
        {
            const auto index = std::to_string (i);
            const auto x = numberAt (prefix + "xVals[" + index + "]");

            if (! x.has_value())
                break;

            const auto y = numberAt (prefix + "yVals[" + index + "]").value_or (0.0);
            const auto bend = numberAt (prefix + "curveVals[" + index + "]").value_or (0.5);

            curve.add (*x, y, bend);
        }

        return curve;
    };

    const auto shapeFromName = [] (std::string_view name)
    {
        // The corpus's own `curveDisplayName` vocabulary (`kValues336`). Names like `7 SKIES Sig` and
        // `HALF RANDOM` are *library* shapes, and the preset that names one also carries its points, so
        // the built-in fallback only has to cover the five the reference draws itself.
        const auto equals = [name] (std::string_view candidate)
        { return name.size() == candidate.size()
                 && std::equal (name.begin(), name.end(), candidate.begin(),
                                [] (char a, char b) { return std::tolower (static_cast<unsigned char> (a)) == b; }); };

        if (equals ("sine"))
            return dsp::LfoShape::Sine;

        if (equals ("triangle") || equals ("triangle 0"))
            return dsp::LfoShape::Triangle;

        if (equals ("saw up") || equals ("saw up curved"))
            return dsp::LfoShape::SawUp;

        if (equals ("saw down") || equals ("saw down curved"))
            return dsp::LfoShape::SawDown;

        if (equals ("square"))
            return dsp::LfoShape::Square;

        return dsp::LfoShape::Sine;
    };

    for (int lfo = 0; lfo < dsp::kLfoCount; ++lfo)
    {
        const auto base = "LFO" + std::to_string (lfo);
        const auto params = base + "/plainParams/";
        auto& lfoSettings = settings.lfos[static_cast<std::size_t> (lfo)];

        const auto type = textAt (params + "kParamType");

        if (type == "Path")
            lfoSettings.type = dsp::LfoType::Path;
        else if (type == "RandomSH")
            lfoSettings.type = dsp::LfoType::RandomSampleHold;
        else if (type == "Lorenz")
            lfoSettings.type = dsp::LfoType::Lorenz;
        else if (type == "Rossler")
            lfoSettings.type = dsp::LfoType::Rossler;

        // A preset that stores no rate gets the engine's default (0.5 Hz). `kParamRate` is present in 552
        // of 626 presets, so this is the exception rather than the rule, and 0.5 Hz is the rate the model's
        // own default sits at.
        const auto rate = numberAt (params + "kParamRate");
        const bool rate10x = numberAt (params + "kParamRate10x").value_or (0.0) != 0.0;

        lfoSettings.rateHz = rate.has_value() ? dsp::lfoRateHz (*rate, rate10x) : 0.5;

        // Dotted and triplet modify the *length* of a step, so they scale the rate the other way: a dotted
        // step is 1.5 times as long, a triplet step two thirds.
        if (numberAt (params + "kParamDotted").value_or (0.0) != 0.0)
            lfoSettings.rateHz *= 2.0 / 3.0;

        if (numberAt (params + "kParamTriplets").value_or (0.0) != 0.0)
            lfoSettings.rateHz *= 3.0 / 2.0;

        lfoSettings.phaseTurns = numberAt (params + "kParamPhase").value_or (0.0) / 360.0;
        lfoSettings.delaySeconds = numberAt (params + "kParamDelay").value_or (0.0);
        lfoSettings.riseSeconds = numberAt (params + "kParamRise").value_or (0.0);
        lfoSettings.smooth = numberAt (params + "kParamSmooth").value_or (0.0) / 100.0;
        lfoSettings.swing = numberAt (params + "kParamSwing").value_or (0.0) / 100.0;
        lfoSettings.direction = numberAt (params + "kParamDirection").value_or (1.0) == 2.0 ? -1.0 : 1.0;
        lfoSettings.mono = numberAt (params + "kParamMono").value_or (0.0) != 0.0;

        // The two modes the schema stores: `Envelope` is one cycle per note, `Free` is free-running.
        const auto mode = textAt (params + "kParamMode");
        lfoSettings.oneShot = mode == "Envelope";
        lfoSettings.freeRunning = mode == "Free";

        lfoSettings.shape = shapeFromName (textAt (base + "/curveDisplayName"));
        lfoSettings.curve = readCurve (base + "/curveData/", true);
        lfoSettings.path = readCurve (base + "/pathData/", ! truthyAt (base + "/pathData/isOpen"));
    }

    for (int envelope = 0; envelope < dsp::kMatrixEnvelopeCount; ++envelope)
    {
        const auto params = "Env" + std::to_string (envelope) + "/plainParams/";
        auto& env = settings.envelopes[static_cast<std::size_t> (envelope)];

        env.attack = numberAt (params + "kParamAttack").value_or (0.0);
        env.hold = numberAt (params + "kParamHold").value_or (0.0);
        env.decay = numberAt (params + "kParamDecay").value_or (0.0);
        env.sustain = numberAt (params + "kParamSustain").value_or (1.0);
        env.release = numberAt (params + "kParamRelease").value_or (0.0);
        env.curve1 = numberAt (params + "kParamCurve1").value_or (50.0);
        env.curve2 = numberAt (params + "kParamCurve2").value_or (50.0);
        env.curve3 = numberAt (params + "kParamCurve3").value_or (50.0);
        env.start = numberAt (params + "kParamStart").value_or (0.0);
        env.end = numberAt (params + "kParamEnd").value_or (0.0);
        env.legatoInverted = numberAt (params + "kParamLegatoInverted").value_or (0.0) != 0.0;
    }

    return settings;
}

void XerumAudioProcessor::publishPoleZero()
{
    const juce::ScopedLock lock (stateLock_);

    std::array<dsp::PoleZeroDescription, dsp::kFilterSlots> next;

    for (int slot = 0; slot < dsp::kFilterSlots; ++slot)
        next[static_cast<std::size_t> (slot)] =
            poleZeroFromDocument (document_, PoleZeroObjectPath { "VoiceFilter" + std::to_string (slot) });

    if (! freezeAndWait())
    {
        // Deferred, not dropped: `settingsDirty_` is already raised by the edit path, and the timer
        // republishes once the block is out of the way. `modulationDirty_` carries the retry too, so a
        // deferred pole/zero write is not lost when only it moved.
        modulationDirty_.store (true, std::memory_order_release);
        return;
    }

    poleZero_ = next;

    frozen_.store (false, std::memory_order_release);

    // The audio thread copies `poleZero_` into the settings when it next rebuilds, and the drawn response
    // only reaches the filter through `VoiceFilter::setPoleZero` — which `Voice::setSettings` calls when
    // the model is `PoleZero`.
    settingsDirty_.store (true, std::memory_order_release);
}

void XerumAudioProcessor::setPoleZeroSection (const int filterSlot, const int section,
                                              const dsp::PoleZeroSection& sectionValue)
{
    if (filterSlot < 0 || filterSlot >= dsp::kFilterSlots || section < 0
        || section >= static_cast<int> (dsp::PoleZeroDescription::kMaxSections))
        return;

    {
        const juce::ScopedLock lock (stateLock_);

        // Eight sections per object, eight objects max — the reference's own shape, so a section index
        // maps onto `PZs[object]/field[index]` arithmetically.
        const auto perObject = static_cast<int> (dsp::PoleZeroDescription::kSectionsPerObject);
        const auto object = section / perObject;
        const auto index = section % perObject;
        const auto prefix = "VoiceFilter" + std::to_string (filterSlot) + "/PZs[" + std::to_string (object) + "]";
        const auto leaf = [&prefix, index] (const char* field)
        { return prefix + "/" + field + "[" + std::to_string (index) + "]"; };

        const auto setNumber = [this] (const std::string& path, const double number)
        {
            model::Value value;
            value.type = model::Value::Type::Float;
            value.real = number;
            document_.parameters[path] = std::move (value);
        };

        setNumber (leaf ("fp"), sectionValue.poleFrequency);
        setNumber (leaf ("fz"), sectionValue.zeroFrequency);
        setNumber (leaf ("qp"), sectionValue.poleQ);
        setNumber (leaf ("qz"), sectionValue.zeroQ);
        setNumber (leaf ("gain"), sectionValue.gain);

        if (document_.parameters.find (leaf ("types")) == document_.parameters.end())
        {
            model::Value value;
            value.type = model::Value::Type::Int;
            value.integer = sectionValue.type;
            document_.parameters[leaf ("types")] = std::move (value);
        }
    }

    publishPoleZero();
    sendChangeMessage();
}

//==============================================================================
// **The FX rack** (ticket `60`). See the header for why a slot is read from the document rather than from
// the bridge, and why the plan exists at all.

namespace
{

/// One slot key in a document, with the array index **parsed** rather than taken from iteration order.
///
/// The document keys an array by its real index and `std::map` orders those keys lexicographically, so
/// `FX[10]` sorts before `FX[2]`. Ticket 41's embedded-table reader was caught by exactly that; the slots
/// here are emitted in numeric order for the same reason.
struct FxSlotKey
{
    bool ok { false };
    int index { 0 };
    std::string rest;   ///< what follows the index: `/type`, `/FXComp/plainParams/kParamDrive`, `/flex[0]/…`
};

FxSlotKey parseFxSlotKey (const std::string_view key, const std::string_view prefix)
{
    FxSlotKey out;

    if (! key.starts_with (prefix))
        return out;

    const auto close = key.find (']', prefix.size());

    if (close == std::string_view::npos || close == prefix.size())
        return out;

    int index = 0;

    for (const auto digit : key.substr (prefix.size(), close - prefix.size()))
    {
        if (digit < '0' || digit > '9')
            return out;   // a malformed index is not a slot, and guessing one would invent a position

        index = index * 10 + (digit - '0');
    }

    out.ok = true;
    out.index = index;
    out.rest = std::string (key.substr (close + 1));
    return out;
}

constexpr std::size_t kPlainParamsLength = 13;   // strlen ("/plainParams/")

/// The reference's own names for the routing matrix's seven rows, in `RoutingSlot{N}` order — the names its
/// host surface uses (`A>BUS1` … `Filter 2>BUS2`), so a report entry reads like the control it is about.
constexpr const char* kRoutingRowNames[dsp::kRoutingRowCount] = { "A", "B", "C", "Noise", "Sub Osc",
                                                                 "Filter 1", "Filter 2" };

} // namespace

std::shared_ptr<XerumAudioProcessor::FxPlan>
XerumAudioProcessor::planFxRack (const model::PresetDocument& document) const
{
    auto plan = std::make_shared<FxPlan>();

    for (int rack = 0; rack < dsp::FxBank::kRackCount; ++rack)
    {
        const auto prefix = "FXRack" + std::to_string (rack) + "/FX[";

        std::map<int, dsp::FxSlot> slots;
        std::map<int, std::string> responseObjects;   ///< slot -> the object path its `PZs[]` hang under
        std::map<int, dsp::FxClass> classKeys;        ///< the class a slot's children name, from its keys
        std::map<int, bool> drewCurve;                ///< the slot carries a `flex[]` curve

        for (auto it = document.parameters.lower_bound (prefix);
             it != document.parameters.end() && it->first.starts_with (prefix); ++it)
        {
            const auto key = parseFxSlotKey (it->first, prefix);

            if (! key.ok)
                continue;

            auto& slot = slots[key.index];
            const auto& value = it->second;

            // The child that names the class — `FXComp`, `FXDistortion`, … — whichever leaf it turns up on.
            // It is where the class's parameters hang, and what a slot that stored no `type` says it is.
            if (const auto segment = key.rest.find ('/', 1); key.rest.size() > 1)
            {
                const auto named = dsp::fxClassFromName (
                    key.rest.substr (1, segment == std::string::npos ? std::string::npos : segment - 1));

                if (named != dsp::FxClass::Unknown)
                    classKeys[key.index] = named;
            }

            if (key.rest == "/type")
            {
                double number = 0.0;

                if (modelNumber (value, number))
                    slot.cls = dsp::fxClassFromTypeId (static_cast<int> (std::lround (number)));

                continue;
            }

            if (key.rest.starts_with ("/flex["))
            {
                drewCurve[key.index] = true;
                continue;
            }

            if (const auto pzs = key.rest.find ("/PZs["); pzs != std::string::npos)
            {
                responseObjects[key.index] = "FXRack" + std::to_string (rack) + "/FX["
                                             + std::to_string (key.index) + "]" + key.rest.substr (0, pzs);
                continue;
            }

            // The parameter leaves. Everything else a slot's class child carries is not a parameter: the
            // saved modulator state (`lfophasor`), the asset hints the content layer resolves
            // (`relativePathToIR` and its cached size) and the rack's own metadata (`displayName`), which
            // the model keeps in the document and this layer has nothing to do with.
            const auto plain = key.rest.find ("/plainParams/");

            if (plain == std::string::npos)
                continue;

            const auto name = key.rest.substr (plain + kPlainParamsLength);
            double number = 0.0;
            std::string text;

            if (modelNumber (value, number))
                slot.numbers.emplace_back (name, number);
            else if (modelText (value, text))
                slot.texts.emplace_back (name, text);
            else if (value.type == model::Value::Type::Bool)
                slot.numbers.emplace_back (name, value.boolean ? 1.0 : 0.0);
        }

        auto& rackSlots = plan->racks[static_cast<std::size_t> (rack)];
        int next = 0;

        for (auto& [index, slot] : slots)
        {
            // The flat array is the topology: an index the document does not carry is an empty slot, not a
            // shift. (Measured over the factory bank — 4168 slot entries in 626 presets — every entry
            // carries a `type` and no rack has a gap, so this is a guard rather than a case in the corpus.)
            for (; next < index; ++next)
                rackSlots.emplace_back();

            if (slot.cls == dsp::FxClass::Unknown)
                if (const auto named = classKeys.find (index); named != classKeys.end())
                    slot.cls = named->second;

            if (const auto response = responseObjects.find (index); response != responseObjects.end())
            {
                auto description = std::make_unique<dsp::PoleZeroDescription> (
                    poleZeroFromDocument (document, PoleZeroObjectPath { response->second }));

                if (! description->empty())
                {
                    slot.poleZero = description.get();
                    plan->responses.push_back (std::move (description));
                }
            }

            // **A drawn curve is not applied, and that is a report rather than a guess.** `flex[0]` is a
            // real curve in 88 factory presets, and its domain is not established: the corpus stores
            // `xVals` over 0..1 with `yVals` falling, while the transfer curve a distortion applies is the
            // input sample over -1..1 — and `curveShape` returns the first ordinate for anything at or
            // below the first abscissa, so reading one as the other would put a constant on every negative
            // sample of a `kXShaper` slot. The built-in curve plays instead, and this says so.
            if (drewCurve.count (index) != 0)
            {
                model::ReportEntry note;
                note.severity = model::Severity::Medium;
                note.modulePath = "FXRack" + std::to_string (rack) + "/FX[" + std::to_string (index) + "]";
                note.item = "flex[0]";
                note.reason =
                    "the slot carries a drawn curve and xerum does not apply it: the curve's domain is not "
                    "established (the corpus stores `xVals` over 0..1, the transfer curve is the input sample "
                    "over -1..1), so the effect's built-in curve plays instead";
                plan->notes.push_back (std::move (note));
            }

            // **The impulse** (ticket `64`). A slot that convolves borrows one, and where it sits has to be
            // recorded because the rack's slots are built here in array order, with an empty slot for a gap
            // — the position is not the FX index. An inline impulse is built from the document right here,
            // because it asks the filesystem nothing; a file reference is the content layer's, and attaches
            // in `publishFxRack`.
            if (slot.cls == dsp::FxClass::Convolve)
            {
                const auto modulePath = "FXRack" + std::to_string (rack) + "/FX[" + std::to_string (index) + "]";
                FxPlan::ImpulsePosition position;
                position.modulePath = modulePath;
                position.rack = rack;
                position.position = rackSlots.size();

                if (const auto reference = document.parameters.find (modulePath + "/FXConv/relativePathToIR");
                    reference != document.parameters.end())
                    modelText (reference->second, position.reference);

                plan->impulsePositions.push_back (std::move (position));

                if (document.parameters.count (modulePath + "/FXConv/embeddedIR[0][0]") != 0)
                {
                    auto embedded = embeddedImpulse (document, modulePath);

                    if (embedded.has_value())
                    {
                        // The pointer goes into the plan's own storage, not the local: a slot dereferences
                        // it for as long as the plan lives, and a `prepareToPlay` after a load rebuilds the
                        // chain from those slots.
                        plan->inlineImpulses.push_back (std::make_unique<IrAsset> (std::move (*embedded)));
                        slot.impulse = &plan->inlineImpulses.back()->impulse;
                    }
                    else
                    {
                        model::ReportEntry note;
                        note.severity = model::Severity::Medium;
                        note.modulePath = modulePath;
                        note.item = "embeddedIR";
                        note.reason =
                            "the preset carries an impulse response inline, but not as the `numChannels` x "
                            "`numFrames` array it declares, so the convolution passes the dry signal";
                        plan->notes.push_back (std::move (note));
                    }
                }
            }

            rackSlots.push_back (std::move (slot));
            ++next;
        }
    }

    return plan;
}

std::vector<XerumAudioProcessor::ContentPlan::ImpulseWant>
XerumAudioProcessor::impulseWants (const model::PresetDocument& document) const
{
    std::vector<ContentPlan::ImpulseWant> wants;

    for (int rack = 0; rack < dsp::FxBank::kRackCount; ++rack)
    {
        const auto prefix = "FXRack" + std::to_string (rack) + "/FX[";

        // Keyed by the slot, because a slot's two markers arrive in whatever order the map holds them.
        std::vector<ContentPlan::ImpulseWant> found;
        std::map<int, std::size_t> at;

        for (auto it = document.parameters.lower_bound (prefix);
             it != document.parameters.end() && it->first.starts_with (prefix); ++it)
        {
            const auto key = parseFxSlotKey (it->first, prefix);

            if (! key.ok)
                continue;

            // **The impulse is a class child, not a `plainParams` leaf.** `FXRack{n}/FX[i]/FXConv/` holds
            // `relativePathToIR` and `embeddedIR[][]`; ticket `60`'s decode skips everything outside
            // `…/plainParams/` on purpose (asset hints are not parameters), so this is where they are read.
            const auto conv = key.rest.find ("/FXConv/");

            if (conv == std::string::npos)
                continue;

            auto where = at.find (key.index);

            if (where == at.end())
            {
                ContentPlan::ImpulseWant want;
                want.modulePath = "FXRack" + std::to_string (rack) + "/FX[" + std::to_string (key.index) + "]";
                found.push_back (std::move (want));
                where = at.emplace (key.index, found.size() - 1).first;
            }

            auto& want = found[where->second];
            const auto leaf = key.rest.substr (conv + 8);

            if (leaf == "relativePathToIR")
                modelText (it->second, want.reference);
            else if (leaf.starts_with ("embeddedIR["))
                want.embedded = true;
        }

        for (auto& want : found)
            wants.push_back (std::move (want));
    }

    return wants;
}

std::optional<XerumAudioProcessor::IrAsset>
XerumAudioProcessor::embeddedImpulse (const model::PresetDocument& document, const std::string& modulePath) const
{
    const auto prefix = modulePath + "/FXConv";

    const auto countLeaf = [&document, &prefix] (const char* leafName, int& out)
    {
        const auto found = document.parameters.find (prefix + "/" + leafName);
        double number = 0.0;

        if (found == document.parameters.end() || ! modelNumber (found->second, number))
            return false;

        out = static_cast<int> (std::lround (number));
        return out > 0;
    };

    int channels = 0;
    int frames = 0;

    if (! countLeaf ("numChannels", channels) || ! countLeaf ("numFrames", frames))
        return std::nullopt;

    IrAsset asset;
    asset.modulePath = modulePath;
    asset.storage.assign (static_cast<std::size_t> (channels) * static_cast<std::size_t> (frames), 0.0f);
    asset.channels.assign (static_cast<std::size_t> (channels), nullptr);

    for (int channel = 0; channel < channels; ++channel)
    {
        auto* destination = asset.storage.data() + static_cast<std::size_t> (channel) * static_cast<std::size_t> (frames);

        for (int frame = 0; frame < frames; ++frame)
        {
            const auto found = document.parameters.find (prefix + "/embeddedIR[" + std::to_string (channel) + "]["
                                                         + std::to_string (frame) + "]");
            double number = 0.0;

            // A partial array is not an impulse: one missing sample is reported by the caller rather than
            // convoluted with a hole in it.
            if (found == document.parameters.end() || ! modelNumber (found->second, number))
                return std::nullopt;

            destination[frame] = static_cast<float> (number);
        }

        asset.channels[static_cast<std::size_t> (channel)] = destination;
    }

    double rate = 0.0;
    const auto rateLeaf = document.parameters.find (prefix + "/sampleRate");

    if (rateLeaf != document.parameters.end())
        modelNumber (rateLeaf->second, rate);

    asset.impulse.channels = asset.channels.data();
    asset.impulse.channelCount = channels;
    asset.impulse.frames = frames;
    asset.impulse.sampleRate = rate;

    return asset;
}

bool XerumAudioProcessor::decodeImpulse (const std::vector<std::byte>& bytes, IrAsset& asset, std::string& error)
{
    // The reference's `Impulses/` corpus is FLAC and a user's own impulse is usually WAV, so both are
    // registered. Nothing here is xerum's own format, and nothing here is on the audio thread.
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (
        formats.createReaderFor (std::make_unique<juce::MemoryInputStream> (bytes.data(), bytes.size(), false)));

    if (reader == nullptr)
    {
        error = "the impulse response is not an audio file this build can read";
        return false;
    }

    const auto frames = static_cast<int> (juce::jlimit<juce::int64> (0, juce::int64 (1) << 24,
                                                                    reader->lengthInSamples));

    if (frames <= 0)
    {
        error = "the impulse response file reads as empty";
        return false;
    }

    const auto channels = juce::jlimit (1, 2, static_cast<int> (reader->numChannels));
    juce::AudioBuffer<float> buffer (static_cast<int> (reader->numChannels), frames);

    if (! reader->read (&buffer, 0, frames, 0, true, true))
    {
        error = "the impulse response could not be read";
        return false;
    }

    asset.storage.assign (static_cast<std::size_t> (channels) * static_cast<std::size_t> (frames), 0.0f);
    asset.channels.assign (static_cast<std::size_t> (channels), nullptr);

    for (int channel = 0; channel < channels; ++channel)
    {
        auto* destination = asset.storage.data() + static_cast<std::size_t> (channel) * static_cast<std::size_t> (frames);

        std::copy (buffer.getReadPointer (channel), buffer.getReadPointer (channel) + frames, destination);
        asset.channels[static_cast<std::size_t> (channel)] = destination;
    }

    asset.impulse.channels = asset.channels.data();
    asset.impulse.channelCount = channels;
    asset.impulse.frames = frames;
    asset.impulse.sampleRate = reader->sampleRate;

    return true;
}

dsp::BusSendSettings XerumAudioProcessor::planRouting (const model::PresetDocument& document)
{
    dsp::BusSendSettings settings;

    const auto leafValue = [&document] (const std::string& path, double& out)
    {
        const auto found = document.parameters.find (path);

        return found != document.parameters.end() && modelNumber (found->second, out);
    };

    // **The reference's own law for a send**, measured through its host with the init patch loaded (see
    // `dsp::BusSendSettings`): the preset stores the level as a percent and the send's amplitude is that
    // level cubed, so 50 % is -18 dB and full level is unity.
    const auto sendGain = [] (const double percent)
    {
        const auto level = std::clamp (percent / 100.0, 0.0, 1.0);
        return level * level * level;
    };

    std::string unappliedDestinations;

    for (int row = 0; row < dsp::kRoutingRowCount; ++row)
    {
        const auto prefix = "RoutingSlot" + std::to_string (row) + "/plainParams/";
        double level = 0.0;

        if (leafValue (prefix + "kParamFXBus1Level", level))
            settings.gain[static_cast<std::size_t> (row)][0] = sendGain (level);

        if (leafValue (prefix + "kParamFXBus2Level", level))
            settings.gain[static_cast<std::size_t> (row)][1] = sendGain (level);

        // **The destination is not applied, and says so.** The reference gives each routing row a
        // destination (`kRoutingDestDirect`/`Filter`/`Master`/`None`); xerum's voice layer has one fixed
        // path — every source runs into the voice filters and then the mixer — which is what `Filter`
        // describes. A preset that stored anything else is asking for something this build does not do, and
        // rendering it through the fixed path anyway is exactly the silent default the contract forbids. Its
        // *sends* are applied either way, because those are what ticket `64` wired.
        const auto destination = document.parameters.find (prefix + "kParamRoutingDest");
        std::string text;

        if (destination != document.parameters.end() && modelText (destination->second, text)
            && text != "kRoutingDestFilter")
        {
            if (! unappliedDestinations.empty())
                unappliedDestinations += ", ";

            unappliedDestinations += std::string (kRoutingRowNames[row]) + " -> " + text;
        }
    }

    if (! unappliedDestinations.empty())
        addReportEntry (report_, model::Severity::Medium, "RoutingSlot", "kParamRoutingDest",
                        "the preset routes " + unappliedDestinations
                            + "; xerum's voice path is fixed (every source runs through the voice filters "
                              "into the mixer, which is what `kRoutingDestFilter` describes) and the "
                              "destination is not applied, so those rows sound through that path — their "
                              "BUS 1 / BUS 2 sends are applied");

    return settings;
}

void XerumAudioProcessor::publishFxRack (const model::PresetDocument& document)
{
    auto plan = planFxRack (document);

    // **The impulses the content layer resolved** (ticket `64`) attach to the slots that named them, before
    // the rack is built: a slot holds a pointer into this storage, and `setSlots` dereferences it. The
    // inline ones `planFxRack` already built are in the plan; these are the files, matched by the slot's
    // own path.
    plan->resolvedImpulses = irAssets_;

    if (plan->resolvedImpulses != nullptr)
        for (const auto& position : plan->impulsePositions)
        {
            // Both halves matter: the slot's path says *where* the impulse goes, the reference says which
            // one — and the content layer's assets are this document's, matched against this document's
            // own leaves.
            const auto& assets = *plan->resolvedImpulses;
            const auto asset = std::find_if (assets.begin(), assets.end(),
                                             [&position] (const IrAsset& candidate)
                                             {
                                                 return candidate.modulePath == position.modulePath
                                                        && ! position.reference.empty()
                                                        && candidate.reference == position.reference;
                                             });

            if (asset != assets.end())
                plan->racks[static_cast<std::size_t> (position.rack)][position.position].impulse = &asset->impulse;
        }

    // **A rack whose impulses are still being resolved is not published yet.** Building it now would hand
    // its convolver nothing and report "no impulse response was supplied" about a preset whose impulse is a
    // filesystem answer away, and the report is not allowed to say something it will have to take back.
    // The timer's `flushContent` publishes the content and then this rack, once, with the impulses;
    // `fxDeferred_` is what makes that a retry rather than a dropped preset.
    if (impulsesPending_)
    {
        fxDeferred_.store (true, std::memory_order_release);
        return;
    }

    if (! freezeAndWait())
    {
        // The audio thread never left its block — a host that runs `processBlock` on this thread, or one
        // that never returns. Nothing is installed, so nothing the audio thread may be reading is
        // destroyed underneath it; the timer re-plans from the document and tries again. Setting the flag
        // is what makes that a retry rather than a silent drop of the preset's whole rack.
        fxDeferred_.store (true, std::memory_order_release);
        return;
    }

    for (int rack = 0; rack < dsp::FxBank::kRackCount; ++rack)
        fxBank_.rack (rack).setSlots (plan->racks[static_cast<std::size_t> (rack)]);

    // The busses' routing, from the same document. Absence means the reference's own default, which is
    // what the rack already holds (destination 1 — summed into MAIN's input — at unity volume).
    const auto busNumber = [&document] (const std::string& leaf)
    {
        double value = 0.0;
        const auto found = document.parameters.find ("Global0/plainParams/" + leaf);
        return found != document.parameters.end() && modelNumber (found->second, value) ? value : 1.0;
    };

    // **The bus volume is a leaf with a unit model, not a linear gain** (ticket `65`). The reference's own
    // display for `Bus 1 Vol` reads `-28.0 / -12.1 / -0.0 / 7.0 / 12.0 dB` at stored
    // `0.1 / 0.25 / 0.5 / 0.75 / 1.0`, which is `4·n²` to 0.1 dB — so its `0.5` default is 0 dB, and a
    // preset that omits the leaf really is unity. The *send* level beside it is deliberately a different
    // curve: ticket `64` measured that one as `n³`.
    const auto busVolume = [&] (const std::string& leaf)
    {
        double value = 0.0;
        const auto found = document.parameters.find ("Global0/plainParams/" + leaf);
        return found != document.parameters.end() && modelNumber (found->second, value)
                   ? dsp::levelTaperGain (value)
                   : 1.0;
    };

    fxBank_.setBusRouting (busNumber ("kParamFXBus1Dest"), busVolume ("kParamFXBus1Vol"),
                           busNumber ("kParamFXBus2Dest"), busVolume ("kParamFXBus2Vol"));

    // **The mixer's sends** (ticket `64`), from the same document and inside the same frozen window: the
    // audio thread reads them through `settings_`, and an array of doubles written underneath a running
    // block is exactly the tear the handshake exists to prevent.
    routing_ = planRouting (document);

    // The plan outlives this call because the slots borrow from it: `prepareToPlay` re-prepares the bank,
    // which rebuilds the chain and dereferences those curves and responses again.
    fxPlan_ = std::move (plan);
    fxDeferred_.store (false, std::memory_order_release);

    frozen_.store (false, std::memory_order_release);

    // The audio thread copies the new sends into its own settings on the next block. It has to be told:
    // a routing change is not a parameter change, and nothing else would raise the flag.
    settingsDirty_.store (true, std::memory_order_release);

    // What neither half could honour. The **decode's** own notes are per slot; the **rack's**
    // `unresolved()` is a class name — which the model's walk has already reported (`checkEffects`) — or
    // `Class/kParamName` for a parameter the effect does not know; its `problems()` are the effects' own
    // (a convolution with no impulse response, a filter family v1 does not build).
    for (const auto& note : fxPlan_->notes)
        report_.entries.push_back (note);

    for (int rack = 0; rack < dsp::FxBank::kRackCount; ++rack)
    {
        const auto module = "FXRack" + std::to_string (rack);

        for (const auto& unresolved : fxBank_.rack (rack).unresolved())
        {
            if (dsp::fxClassFromName (unresolved) != dsp::FxClass::Unknown)
                continue;   // a deferred class, named by the model's own walk — not twice

            addReportEntry (report_, model::Severity::Medium, module, unresolved,
                            "the effect does not know this parameter, so it is not applied");
        }

        for (const auto& problem : fxBank_.rack (rack).problems())
        {
            // An effect's own sentence is `Class: what is wrong`, and the entry already carries the class
            // as its item — so the reason is what follows the colon rather than the sentence again.
            const auto colon = problem.find (':');
            const auto separator = colon != std::string::npos && problem.substr (colon).starts_with (": ") ? 2u : 1u;

            addReportEntry (report_, model::Severity::High, module,
                            colon == std::string::npos ? problem : problem.substr (0, colon),
                            colon == std::string::npos ? problem : problem.substr (colon + separator));
        }
    }
}

void XerumAudioProcessor::flushFx()
{
    if (! fxDeferred_.exchange (false, std::memory_order_acq_rel))
        return;

    const juce::ScopedLock lock (stateLock_);
    publishFxRack (document_);
}

void XerumAudioProcessor::applyDocument (const model::PresetDocument& document, const model::ImportReport& report,
                                         const int reportEntriesFromModel)
{
    const juce::ScopedLock lock (stateLock_);

    // Every matrix write below fires the listener; collapsing them keeps this a batch. Cleared before the
    // single publish at the end.
    applyingDocument_.store (true, std::memory_order_release);

    document_ = document;
    report_ = report;
    modelReportEntries_ = juce::jlimit (0, static_cast<int> (report_.entries.size()), reportEntriesFromModel);

    // The plugin's own entries are derived from the document, so they are regenerated rather than carried:
    // otherwise a state restore would carry them twice.
    report_.entries.resize (static_cast<std::size_t> (modelReportEntries_));

    const auto& specs = bridge_.specs();

    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        const auto& spec = specs[i];
        double target = ParameterBridge::resolveAbsent (spec);

        if (const auto found = document.parameters.find (spec.modelPath); found != document.parameters.end())
        {
            if (spec.type == ParamType::Choice)
            {
                std::string text;

                if (modelText (found->second, text))
                    target = choiceIndexForText (bridge_.choices (spec.choiceSet), spec.choiceSet, text);
            }
            else
            {
                double number = 0.0;

                if (modelNumber (found->second, number))
                    target = number;
            }
        }

        setParameterTo (spec.paramIndex, target);
        applied_[static_cast<std::size_t> (spec.paramIndex)] =
            ParameterBridge::currentModelValue (*bridge_.parameters()[static_cast<std::size_t> (spec.paramIndex)],
                                                spec, bridge_.choices (spec.choiceSet));
    }

    // The **modulation surface** too, or a preset's rows would be overwritten by whatever the last edit
    // left. This is ticket 40's finding applied to the matrix: a parameter `applyDocument` does not write
    // is dead the moment a preset arrives.
    for (const auto& spec : bridge_.modulationSpecs())
    {
        double target = ParameterBridge::resolveAbsent (spec);
        const auto found = document.parameters.find (spec.modelPath);

        if (found != document.parameters.end())
        {
            if (spec.choiceSet == ChoiceSet::ModSource)
            {
                // A row's source is the reference's 1-based enum stored as a number, so the document's
                // value *is* the choice index.
                double number = 0.0;

                if (modelNumber (found->second, number))
                    target = number;
            }
            else if (spec.type == ParamType::Choice)
            {
                std::string text;

                if (modelText (found->second, text))
                    target = choiceIndexForText (bridge_.choices (spec.choiceSet), spec.choiceSet, text);
            }
            else
            {
                double number = 0.0;

                if (modelNumber (found->second, number))
                    target = number;
            }
        }

        setParameterTo (spec.paramIndex, target);
        applied_[static_cast<std::size_t> (spec.paramIndex)] =
            ParameterBridge::currentModelValue (*bridge_.parameters()[static_cast<std::size_t> (spec.paramIndex)],
                                                spec, bridge_.choices (spec.choiceSet));
    }

    resolveContent (document);

    // **The rack** (ticket `60`) before `completeReport`, because its own entries belong in the report
    // this load produces — the report is regenerated per document, and an entry added after it would be
    // the one thing the interface never shows.
    publishFxRack (document);

    completeReport (document);

    applyingDocument_.store (false, std::memory_order_release);

    // The two paths ticket `45` built. Both are off the audio thread and both hand the engine their state
    // while holding it out — the same handshake the tables use, because both allocate.
    publishPoleZero();
    publishModulationSlots();
    publishModulationSources();

    // The active voice count is a `prepare()` argument (ticket 28), and re-preparing the pool allocates
    // (the modulation matrix), so it is applied here — off the audio thread, with the tables' handshake
    // already holding the audio thread out — and not from `processBlock`.
    if (prepared_ != 0 && layout_.polyCount >= 0)
    {
        const auto requested = static_cast<int> (applied_[static_cast<std::size_t> (layout_.polyCount)]);

        if (requested != engine_.polyphony())
        {
            frozen_.store (true, std::memory_order_release);

            while (inBlock_.load (std::memory_order_acquire) != 0)
                juce::Thread::yield();

            engine_.prepare (sampleRate_, maxBlock_, requested);

            frozen_.store (false, std::memory_order_release);
        }
    }

    settingsDirty_.store (true, std::memory_order_release);

    // The interface's report is refreshed from here, whether the document came from a preset file or a
    // state restore. Never called from the audio thread.
    sendChangeMessage();
}

//==============================================================================
juce::Result XerumAudioProcessor::loadPresetFromFile (const juce::File& file)
{
    const juce::ScopedLock lock (stateLock_);

    const auto loaded = loadPresetFile (file);

    if (! loaded.ok)
    {
        lastError_ = juce::String (loaded.error);
        return juce::Result::fail (lastError_);
    }

    lastError_.clear();
    applyDocument (loaded.document, loaded.report, loaded.reportEntriesFromModel);

    return juce::Result::ok();
}

model::PresetDocument XerumAudioProcessor::document() const
{
    const juce::ScopedLock lock (stateLock_);
    return document_;
}

model::ImportReport XerumAudioProcessor::report() const
{
    const juce::ScopedLock lock (stateLock_);
    return report_;
}

juce::String XerumAudioProcessor::reportText() const
{
    const juce::ScopedLock lock (stateLock_);

    juce::String text;

    if (lastError_.isNotEmpty())
        text << "could not load: " << lastError_ << "\n\n";

    return text + juce::String (plugin::reportText (report_));
}

juce::String XerumAudioProcessor::lastError() const
{
    const juce::ScopedLock lock (stateLock_);
    return lastError_;
}

double XerumAudioProcessor::parameterValue (const std::string_view modelPath) const
{
    const auto index = indexOfParameter (modelPath);
    const auto* spec = bridge_.specForParameter (index);

    if (spec == nullptr)
        return 0.0;

    return ParameterBridge::currentModelValue (*bridge_.parameters()[static_cast<std::size_t> (index)], *spec,
                                             bridge_.choices (spec->choiceSet));
}

void XerumAudioProcessor::setParameterValue (const std::string_view modelPath, const double modelValue)
{
    setParameterTo (indexOfParameter (modelPath), modelValue);
}

const dsp::Wavetable* XerumAudioProcessor::oscillatorTable (const int slot) const
{
    if (slot < 0 || slot >= dsp::kOscillatorSlots)
        return nullptr;

    const auto& table = tables_[static_cast<std::size_t> (slot)];
    return table.has_value() ? &*table : nullptr;
}

std::vector<juce::String> XerumAudioProcessor::contentRoots() const
{
    std::vector<juce::String> roots;
    roots.reserve (contentRoots_.size());

    for (const auto& root : contentRoots_)
        roots.emplace_back (root.string());

    return roots;
}

//==============================================================================
void XerumAudioProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    const juce::ScopedLock lock (stateLock_);

    auto merged = document_;
    const auto& specs = bridge_.specs();

    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        const auto& spec = specs[i];
        const auto live = ParameterBridge::currentModelValue (*bridge_.parameters()[i], spec,
                                                             bridge_.choices (spec.choiceSet));

        // Untouched parameters keep the preset's own token, byte for byte. That is what makes a
        // state round-trip exact: a preset free to collapse onto a broader xerum choice (two reference
        // filter names the engine builds identically) still saves the name it came with.
        if (live == applied_[i])
            continue;

        if (spec.type == ParamType::Choice)
        {
            const auto& choices = bridge_.choices (spec.choiceSet);
            const auto index = static_cast<int> (std::lround (live));

            if (index == choices.sentinel)
                continue;   // no model form: what the preset said stays untouched

            const auto& token = choices.modelNames[static_cast<std::size_t> (index)];

            if (token.empty())
                merged.parameters.erase (spec.modelPath);
            else
            {
                model::Value value;
                value.type = model::Value::Type::String;
                value.text = token;
                merged.parameters[spec.modelPath] = std::move (value);
            }

            continue;
        }

        const auto* leaf = model::Schema::instance().describe (spec.schemaPath);
        model::Value value;

        if (leaf != nullptr && leaf->kind == model::LeafKind::Int)
        {
            value.type = model::Value::Type::Int;
            value.integer = static_cast<std::int64_t> (std::llround (live));
        }
        else
        {
            value.type = model::Value::Type::Float;
            value.real = live;
        }

        merged.parameters[spec.modelPath] = std::move (value);
    }

    // The **matrix**, the same rule: a row the user moved is written into the document the state carries,
    // and one they did not is left as the preset wrote it. Without this an edit would live only in the
    // parameters and vanish on save.
    for (const auto& spec : bridge_.modulationSpecs())
    {
        const auto index = static_cast<std::size_t> (spec.paramIndex);
        const auto live = ParameterBridge::currentModelValue (*bridge_.parameters()[index], spec,
                                                             bridge_.choices (spec.choiceSet));

        if (live == applied_[index])
            continue;

        if (const auto value = modulationDocumentValue (bridge_, spec, live))
            merged.parameters[spec.modelPath] = *value;
    }

    const auto bytes = encodeStateBlock (merged, report_, modelReportEntries_);
    destination.replaceAll (bytes.data(), bytes.size());
}

void XerumAudioProcessor::setStateInformation (const void* data, const int sizeInBytes)
{
    const auto loaded = decodeStateBlock (data, sizeInBytes);

    if (! loaded.ok)
    {
        // Keep what is loaded and say so. A host that hands us a block we cannot read — another plugin's,
        // or a future format — must not lose the user's patch over it.
        const juce::ScopedLock lock (stateLock_);
        lastError_ = juce::String (loaded.error);
        report_ = {};
        modelReportEntries_ = 0;
        addReportEntry (report_, model::Severity::High, "state", "getStateInformation",
                        loaded.error + " — the previous patch was kept");
        return;
    }

    {
        const juce::ScopedLock lock (stateLock_);
        lastError_.clear();
    }

    applyDocument (loaded.document, loaded.report, loaded.reportEntriesFromModel);
}

//==============================================================================
const juce::String XerumAudioProcessor::getName() const
{
    return identity::kName;
}

const juce::String XerumAudioProcessor::getProgramName (const int index)
{
    if (index != 0)
        return {};

    const juce::ScopedLock lock (stateLock_);
    return document_.name.empty() ? juce::String ("Init") : juce::String (document_.name);
}

void XerumAudioProcessor::changeProgramName (const int, const juce::String&) {}

juce::AudioProcessorEditor* XerumAudioProcessor::createEditor()
{
    // The editor is created on the message thread, which is where the drain timer lives.
    ensureTimer();

    return new XerumAudioProcessorEditor (*this);
}

} // namespace xerum::plugin

// The plugin's entry point. JUCE's wrappers call this from the shared code.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new xerum::plugin::XerumAudioProcessor();
}
