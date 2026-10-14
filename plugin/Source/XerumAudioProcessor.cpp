// xerum::plugin — the JUCE adapter, in code. See XerumAudioProcessor.h for the argument.

#include "XerumAudioProcessor.h"

#include "PluginIdentity.h"
#include "XerumAudioProcessorEditor.h"
#include "schema.h"

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

constexpr int kAmpEnvelopeSlot = 0;

} // namespace

XerumAudioProcessor::XerumAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    setParameterTree (bridge_.releaseTree());
    resolveLayout();

    // Content roots, in the import contract's order (§5 / `content/table_library.h`): a user-configured
    // root, then the reference instrument's content tree, then xerum's own, and last the source tree,
    // which only exists in a development checkout.
    if (const auto userRoot = juce::SystemStats::getEnvironmentVariable ("XERUM_CONTENT_ROOT", {});
        userRoot.isNotEmpty())
        library_.addRoot (userRoot.toStdString());

    library_.addRoot ("/Library/Audio/Presets/Xfer Records/Serum 2 Presets");

    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    library_.addRoot (home.getChildFile ("Library/Application Support/xerum/content").getFullPathName().toStdString());
    library_.addRoot (sourceContentDir().getFullPathName().toStdString());

    values_.assign (bridge_.specs().size(), std::numeric_limits<double>::quiet_NaN());
    applied_.assign (bridge_.specs().size(), std::numeric_limits<double>::quiet_NaN());

    // The init patch: no document at all, so every parameter resolves through `resolveAbsent`, and the
    // default content is resolved so the plugin makes a sound before anything is loaded.
    applyDocument ({}, {}, 0);
}

XerumAudioProcessor::~XerumAudioProcessor() = default;

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
}

int XerumAudioProcessor::indexOfParameter (const std::string_view modelPath) const
{
    for (const auto& spec : bridge_.specs())
        if (spec.modelPath == modelPath)
            return spec.paramIndex;

    return -1;
}

//==============================================================================
void XerumAudioProcessor::prepareToPlay (const double sampleRate, const int maximumExpectedSamplesPerBlock)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = juce::jmax (1, maximumExpectedSamplesPerBlock);

    mono_.assign (static_cast<std::size_t> (maxBlock_), 0.0f);

    const int polyphony = layout_.polyCount >= 0
                              ? static_cast<int> (parameterValue (bridge_.specs()[static_cast<std::size_t> (layout_.polyCount)]
                                                                      .modelPath))
                              : 16;

    // The pool is sized here and never on the audio thread: `VoiceEngine::prepare` rebuilds the voice
    // pool and the modulation matrix, and the matrix allocates (ticket 28's contract).
    engine_.prepare (sampleRate_, maxBlock_, polyphony);
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
        osc.framePosition = amount (layout.tablePos);
        osc.table = tables_[static_cast<std::size_t> (slot)].has_value()
                        ? &*tables_[static_cast<std::size_t> (slot)]
                        : nullptr;

        const auto mode1 = static_cast<int> (amount (layout.warp1Mode));
        const auto mode2 = static_cast<int> (amount (layout.warp2Mode));

        osc.warpMode[0] = bridge_.choices (ChoiceSet::WarpMode).warpModes[mode1];
        osc.warpMode[1] = bridge_.choices (ChoiceSet::WarpMode).warpModes[mode2];
        osc.warpAmount[0] = amount (layout.warp1Amount);
        osc.warpAmount[1] = amount (layout.warp2Amount);
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
    }

    engine_.setSettings (next);
    settings_ = next;
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
        refreshSettingsFromParameters();

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
    }

    midi.clear();

    const auto gainTarget = layout_.masterVolume >= 0
                                ? std::clamp (values_[static_cast<std::size_t> (layout_.masterVolume)], 0.0, 1.0)
                                : 1.0;
    const auto gainStart = masterGain_;
    const auto gainStep = numSamples > 0 ? (gainTarget - gainStart) / static_cast<double> (numSamples) : 0.0;

    // The engine writes mono and overwrites, so it is rendered in chunks no larger than `prepare` sized.
    const int chunkLimit = juce::jmax (1, static_cast<int> (mono_.size()));

    for (int done = 0; done < numSamples; done += chunkLimit)
    {
        const int chunk = juce::jmin (chunkLimit, numSamples - done);
        engine_.process (mono_.data() + done, chunk);
    }

    for (int channel = 0; channel < numChannels; ++channel)
    {
        auto* out = buffer.getWritePointer (channel);
        auto gain = gainStart;

        for (int i = 0; i < numSamples; ++i)
        {
            out[i] = mono_[static_cast<std::size_t> (i)] * static_cast<float> (gain);
            gain += gainStep;
        }
    }

    masterGain_ = gainTarget;
}

//==============================================================================
void XerumAudioProcessor::setParameterTo (const int paramIndex, const double modelValue)
{
    if (paramIndex < 0)
        return;

    auto* parameter = bridge_.parameters()[static_cast<std::size_t> (paramIndex)];
    const auto& spec = bridge_.specs()[static_cast<std::size_t> (paramIndex)];

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

void XerumAudioProcessor::resolveContent (const model::PresetDocument& document)
{
    std::array<std::optional<dsp::Wavetable>, dsp::kOscillatorSlots> next;

    for (int slot = 0; slot < dsp::kOscillatorSlots; ++slot)
    {
        const auto prefix = slotPrefix (slot);

        if (! slotSelectsWavetable (document, slot))
        {
            addReportEntry (report_, model::Severity::Info, "Oscillator" + std::to_string (slot), "kParamType",
                            "the slot selects an engine this build does not synthesise, so it renders silence "
                            "rather than a substituted wavetable");
            continue;
        }

        const auto tablePath = prefix + "/relativePathToWT";
        std::string reference;

        if (const auto found = document.parameters.find (tablePath);
            found != document.parameters.end() && modelText (found->second, reference) && ! reference.empty())
        {
            const auto lookup = library_.loadTable (reference);

            if (lookup.found())
            {
                next[static_cast<std::size_t> (slot)] = lookup.table;

                if (lookup.resolution == content::Resolution::CaseInsensitive)
                    addReportEntry (report_, model::Severity::Info, tablePath, reference,
                                    "resolved case-insensitively to " + lookup.path.string());
            }
            else
            {
                addReportEntry (report_, model::Severity::High, tablePath, reference,
                                lookup.error.empty()
                                    ? std::string ("the asset could not be found under any content root")
                                    : lookup.error);
            }
        }
        else if (document.parameters.find (prefix + "/embeddedWTData[0]") != document.parameters.end())
        {
            // The preset carries the table inline. xerum reads wavetable *files*; an embedded table is a
            // different asset form, and substituting a file for it would be the silent drop the contract
            // forbids.
            addReportEntry (report_, model::Severity::High, prefix, "embeddedWTData",
                            "the preset carries its wavetable inline; xerum reads wavetable files, so this "
                            "slot renders silence");
            continue;
        }

        if (! next[static_cast<std::size_t> (slot)].has_value())
        {
            const auto fallback = library_.loadTable (kDefaultTable);

            if (fallback.found())
            {
                next[static_cast<std::size_t> (slot)] = fallback.table;

                if (! reference.empty())
                    addReportEntry (report_, model::Severity::Info, tablePath, reference,
                                    std::string ("substituted xerum's own ") + kDefaultTable);
            }
            else
            {
                addReportEntry (report_, model::Severity::High, prefix, reference.empty() ? kDefaultTable : reference,
                                "no content root could supply a wavetable, so this slot renders silence "
                                "(set XERUM_CONTENT_ROOT to a directory containing Tables/)");
            }
        }
    }

    publishTables (std::move (next));
}

void XerumAudioProcessor::completeReport (const model::PresetDocument& document)
{
    // The model's `loadPreset` reports what the *importer* cannot honour: unknown modules, unknown
    // parameters, unimplemented engines, warps, filters and effects. These three are things only the
    // plugin knows, because they are about which values the *bridge* carries to the engine — and the
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

    for (int slot = 0; slot < dsp::kOscillatorSlots; ++slot)
    {
        const auto prefix = slotPrefix (slot);

        for (const auto* leaf : { "kParamWarpMenu", "kParamWarpMenu2" })
        {
            const auto path = prefix + "/plainParams/" + leaf;
            const auto name = text (path);

            if (name.empty())
                continue;

            const auto mode = dsp::warpModeFromName (name);

            if (mode == dsp::WarpMode::Unknown)
                continue;   // the model's report already names this mode, at high

            const auto& choices = bridge_.choices (ChoiceSet::WarpMode);

            for (int i = 0; i < choices.size(); ++i)
                if (i != choices.sentinel && choices.warpModes[i] == mode && choices.modelNames[i] != name)
                {
                    addReportEntry (report_, model::Severity::Info, path, name,
                                    "xerum applies the " + std::string (dsp::warpModeFamilyName (mode))
                                        + " family; the source or variant this name encodes is not routed yet");
                    break;
                }
        }

        // Ticket 28 records the stereo unison spread and the voice bus is mono, so this one is a value
        // the engine holds and does not act on. It is a *number* in the model, which is why this reads it
        // rather than its text.
        if (numberAt ("Oscillator" + std::to_string (slot) + "/plainParams/kParamUnisonStereo") != 0.0)
            addReportEntry (report_, model::Severity::Info, "Oscillator" + std::to_string (slot),
                            "kParamUnisonStereo", "the unison spread is not applied: the voice bus is mono in v1");
    }

    for (int slot = 0; slot < dsp::kFilterSlots; ++slot)
    {
        const auto prefix = "VoiceFilter" + std::to_string (slot);
        const auto type = text (prefix + "/plainParams/kParamType");

        if (type.empty())
            continue;

        // `PZ_SVF` is an implemented filter *model* whose response is drawn in the preset. The engine's
        // `FilterSettings` carries a `FilterSpec` and no pole/zero table, so the type reaches the engine
        // and the response does not — a substitution worth naming, because the filter will sound wrong
        // rather than be missing.
        if (dsp::filterSpecFromName (type).model == dsp::FilterModel::PoleZero)
        {
            bool drawn = false;

            for (const auto& [path, value] : document.parameters)
                if (path.starts_with (prefix + "/PZs["))
                {
                    drawn = true;
                    break;
                }

            if (drawn)
                addReportEntry (report_, model::Severity::High, prefix, type,
                                "the filter's response is drawn in the preset; xerum's engine takes a filter "
                                "model and no pole/zero sections, so the type is applied without its response");
        }
    }

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

void XerumAudioProcessor::applyDocument (const model::PresetDocument& document, const model::ImportReport& report,
                                         const int reportEntriesFromModel)
{
    const juce::ScopedLock lock (stateLock_);

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

        setParameterTo (static_cast<int> (i), target);
        applied_[i] = ParameterBridge::currentModelValue (*bridge_.parameters()[i], spec,
                                                          bridge_.choices (spec.choiceSet));
    }

    resolveContent (document);
    completeReport (document);

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

    if (index < 0)
        return 0.0;

    const auto& spec = bridge_.specs()[static_cast<std::size_t> (index)];
    return ParameterBridge::currentModelValue (*bridge_.parameters()[static_cast<std::size_t> (index)], spec,
                                             bridge_.choices (spec.choiceSet));
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

    for (const auto& root : library_.roots())
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
    return new XerumAudioProcessorEditor (*this);
}

} // namespace xerum::plugin

// The plugin's entry point. JUCE's wrappers call this from the shared code.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new xerum::plugin::XerumAudioProcessor();
}
