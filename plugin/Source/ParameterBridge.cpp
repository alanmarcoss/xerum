// xerum::plugin — the parameter bridge, in code.
//
// See ParameterBridge.h for the argument. This file is the table, the id rule, and the two derived
// vocabularies (warp families, filter shapes).

#include "ParameterBridge.h"

#include "PluginIdentity.h"
#include "limiter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <map>

namespace xerum::plugin
{

namespace
{

std::string lower (std::string_view text)
{
    std::string out;
    out.reserve (text.size());

    for (const auto c : text)
        out.push_back (static_cast<char> (std::tolower (static_cast<unsigned char> (c))));

    return out;
}

// ---------------------------------------------------------------------------------------------------
// The reference's own default-patch levels (ticket `63`)
// ---------------------------------------------------------------------------------------------------

// Neither leaf is *stored*: the reference's init state omits both, and its own parameter surface reports
// the defaults its plugin applies — `Main Vol` " 50% [-9.0 dB]" and `OSC A / A Level` " 75% [-5.0 dB]",
// read back with `xerum-host --dump-params` against Serum 2 (tickets `38` and `61`). Where a *preset*
// omits a leaf, `resolveAbsent` resolves it here; that is why the values belong in xerum's **authored
// init** rather than in a hard-coded output trim. Carrying the leaves is what makes an un-presetted
// instance render where the reference's does *by construction* — and, because the leaves are carried and
// not the 10.8 dB *difference* between the two defaults, the two instruments agree for the same reason
// rather than by coincidence (ticket `63`'s whole argument).
//
// Both leaves are a **gain**, in the model's units, and the engine applies each as one
// (`OscillatorSettings::volume`, and the master gain in `XerumAudioProcessor::processBlock`). The
// reference's two knobs share one taper — a **square** — which ticket `32` measured directly on this leaf
// (writing a stored `0.25` reads back " 50% [-12.0 dB]"), and the reference's own display of a level is
// `20·log10` of the stored value:
//
//   * `kParamVolume`, `A Level` at 75%: 0.75² = **0.5625**, whose `20·log10` is -5.0 dB — the reference's
//     own display. Its init state holds all three wavetable slots at 75%, so all three carry it (a slot a
//     user enables without touching its level would otherwise be 5 dB hot).
//   * `kParamMasterVolume`, `Main Vol` at 50%: the same square taper under a **+3 dB top** — swept against
//     the reference, `Main Vol` spans -inf..+3 dB and its display reads "+3.0 dB" at the top and "-9.0 dB"
//     at 50% — so 0.5² · 10^(3/20) = **0.3531344**, which `20·log10`s to -9.04 dB and so displays as the
//     reference's own "-9.0 dB" (the 0.1 dB its display rounds to is the whole of the uncertainty).
//
// The two together are -14.04 dB, the reference's own 14.0 dB of default attenuation. What they do *not*
// close is the ~3.2 dB between that and the measured default-to-default audio difference (its 0.288216
// against ours at unity) — a content/unit-model residual ticket `63` reports rather than absorbs, and
// which belongs to ticket `62`.
constexpr double kReferenceDefaultOscillatorVolume = 0.5625;
constexpr double kReferenceDefaultMasterVolume = 0.3531343861556886;

const char* kindText (const dsp::FilterKind kind)
{
    switch (kind)
    {
        case dsp::FilterKind::LowPass:   return "low pass";
        case dsp::FilterKind::HighPass:  return "high pass";
        case dsp::FilterKind::BandPass:  return "band pass";
        case dsp::FilterKind::Notch:     return "notch";
        case dsp::FilterKind::Peak:      return "peak";
        case dsp::FilterKind::LowShelf:  return "low shelf";
        case dsp::FilterKind::HighShelf: return "high shelf";
        case dsp::FilterKind::AllPass:   return "all pass";
    }

    return "unknown";
}

const char* modelText (const dsp::FilterModel model)
{
    switch (model)
    {
        case dsp::FilterModel::Svf:           return "svf";
        case dsp::FilterModel::DualSvf:       return "dual svf";
        case dsp::FilterModel::MorphSvf:      return "morph svf";
        case dsp::FilterModel::Ladder:        return "ladder";
        case dsp::FilterModel::Comb:          return "comb";
        case dsp::FilterModel::Allpass:       return "all pass";
        case dsp::FilterModel::Diffuser:      return "diffuser";
        case dsp::FilterModel::PoleZero:      return "drawn response";
        case dsp::FilterModel::Unimplemented: return "not shipped";
    }

    return "not shipped";
}

/// A key that identifies a spec by value, used to collapse several model names onto one choice. Two
/// names the engine builds identically are one *behaviour*, and offering two identical controls would be
/// two labels for one thing.
std::string specKey (const dsp::FilterSpec& spec)
{
    return std::string (modelText (spec.model)) + "/" + kindText (spec.kind) + "/"
           + kindText (spec.secondary) + "/" + kindText (spec.tertiary) + "/"
           + std::to_string (spec.poles) + "/" + std::to_string (spec.stages) + "/"
           + std::to_string (spec.comb.negative) + std::to_string (spec.comb.lowPassPoles)
           + std::to_string (spec.comb.highPassPoles) + std::to_string (spec.comb.distorted) + "/"
           + std::to_string (spec.flavourDrive);
}

/// The order choices are listed in: by model, then response, then slope, then the qualifiers. A fixed
/// order rather than the model enum's alphabetical one, because a choice list's indices are what a host
/// stores and a deterministic order is the least surprising thing to give it.
bool specLess (const dsp::FilterSpec& a, const dsp::FilterSpec& b)
{
    const auto asInt = [] (const dsp::FilterModel m) { return static_cast<int> (m); };

    if (asInt (a.model) != asInt (b.model))              return asInt (a.model) < asInt (b.model);
    if (static_cast<int> (a.kind) != static_cast<int> (b.kind))
        return static_cast<int> (a.kind) < static_cast<int> (b.kind);
    if (static_cast<int> (a.secondary) != static_cast<int> (b.secondary))
        return static_cast<int> (a.secondary) < static_cast<int> (b.secondary);
    if (static_cast<int> (a.tertiary) != static_cast<int> (b.tertiary))
        return static_cast<int> (a.tertiary) < static_cast<int> (b.tertiary);
    if (a.poles != b.poles)                             return a.poles < b.poles;
    if (a.stages != b.stages)                           return a.stages < b.stages;
    if (a.comb.negative != b.comb.negative)             return a.comb.negative < b.comb.negative;
    if (a.comb.lowPassPoles != b.comb.lowPassPoles)     return a.comb.lowPassPoles < b.comb.lowPassPoles;
    if (a.comb.highPassPoles != b.comb.highPassPoles)   return a.comb.highPassPoles < b.comb.highPassPoles;
    if (a.comb.distorted != b.comb.distorted)           return a.comb.distorted < b.comb.distorted;
    return a.flavourDrive < b.flavourDrive;
}

/// xerum's own label for each implemented warp mode. Authored, and the canonical model token for each is
/// *derived* below by asking `warpModeFromName`, so the label, the mode and the token cannot drift.
struct WarpLabel
{
    const char* label;
    dsp::WarpMode mode;
};

constexpr WarpLabel kWarpLabels[] =
{
    { "off",                    dsp::WarpMode::None },
    { "sync",                   dsp::WarpMode::Sync },
    { "bend +",                 dsp::WarpMode::BendPos },
    { "bend -",                 dsp::WarpMode::BendNeg },
    { "bend +/-",               dsp::WarpMode::BendPosNeg },
    { "asymmetry +",            dsp::WarpMode::AsymPos },
    { "asymmetry -",            dsp::WarpMode::AsymNeg },
    { "asymmetry +/-",          dsp::WarpMode::AsymPosNeg },
    { "pulse width",            dsp::WarpMode::PWM },
    { "flip",                   dsp::WarpMode::Flip },
    { "quantize",               dsp::WarpMode::Quantize },
    { "phase distortion",       dsp::WarpMode::PhaseDistortion },
    { "frequency modulation",   dsp::WarpMode::FrequencyModulation },
    { "distortion",             dsp::WarpMode::Distortion },
    { "ring modulation",        dsp::WarpMode::RingModulation },
    { "amplitude modulation",   dsp::WarpMode::AmplitudeModulation },
    { "filter (low pass)",      dsp::WarpMode::FilterLowPass },
    { "filter (high pass)",     dsp::WarpMode::FilterHighPass },
};

/// xerum's own label for each SUB waveform. Authored, and the canonical model token for each is *derived*
/// from `subShapeName`, so the label, the shape and the token cannot drift — the same rule `kWarpLabels`
/// follows. `Sine`'s label is lower-case because it is a label, not the model's token.
struct SubShapeLabel
{
    const char* label;
    dsp::SubShape shape;
};

constexpr SubShapeLabel kSubShapeLabels[] =
{
    { "sine",         dsp::SubShape::Sine },
    { "rounded rect", dsp::SubShape::RoundRect },
    { "triangle",     dsp::SubShape::Triangle },
    { "saw",          dsp::SubShape::Saw },
    { "square",       dsp::SubShape::Square },
    { "pulse",        dsp::SubShape::Pulse },
};

/// The schema's enum for a leaf, copied out of the descriptor. `Schema::describe` hands back a pointer to
/// a `thread_local` descriptor, so the next call overwrites it — a lesson the model layer learned the
/// hard way and this one inherits.
std::vector<std::string_view> enumFor (std::string_view schemaPath)
{
    std::vector<std::string_view> out;

    if (const auto* leaf = model::Schema::instance().describe (schemaPath); leaf != nullptr)
        for (std::size_t i = 0; i < leaf->valueCount; ++i)
            out.push_back (leaf->values[i]);

    return out;
}

model::Value numberValue (const model::Value::Type type, const double number)
{
    model::Value value;

    if (type == model::Value::Type::Int)
    {
        value.type = model::Value::Type::Int;
        value.integer = static_cast<std::int64_t> (number);
    }
    else
    {
        value.type = model::Value::Type::Float;
        value.real = number;
    }

    return value;
}

/// An integer parameter that **says it is discrete**.
///
/// JUCE 9's `AudioParameterInt` overrides `getNumSteps()` (its range is an integer count) but not
/// `isDiscrete()`, and `AudioProcessorParameter::isDiscrete()` returns false. The VST3 wrapper asks
/// `isDiscrete()` before it writes `ParameterInfo::stepCount`, so an integer parameter reaches a host as
/// a **continuous** one: measured through the ticket 08 harness, xerum's `A Voices` arrived as
/// `steps=2147483647` (the SDK's "continuous" marker) rather than 16, and `A Table Position` read back as
/// 1 where its value is 0. Two lines of override fix both, and the same fact makes a host show a stepped
/// control instead of a slider.
class DiscreteIntParameter final : public juce::AudioParameterInt
{
public:
    using juce::AudioParameterInt::AudioParameterInt;

    bool isDiscrete() const override { return true; }
};

} // namespace

std::string parameterIdFromPath (std::string_view modelPath)
{
    std::string out;
    std::string segment;

    const auto flush = [&]
    {
        if (segment.empty())
            return;

        auto text = lower (segment);
        segment.clear();

        // `plainParams` is the model's container for a module's own parameters; it is structure, not
        // identity, and it would appear in every id if it were kept.
        if (text == "plainparams")
            return;

        // The reference's prefix on a parameter's own name. Dropped here so the id is xerum's.
        if (text.rfind ("kparam", 0) == 0)
            text = text.substr (6);

        if (! out.empty())
            out.push_back ('.');

        out += text;
    };

    for (const auto c : modelPath)
    {
        if (c == '/')
            flush();
        else
            segment.push_back (c);
    }

    flush();
    return out;
}

std::string modulationFieldPath (const int row, const ModRowField field)
{
    const auto prefix = "ModSlot" + std::to_string (row) + "/";

    switch (field)
    {
        case ModRowField::Source:              return prefix + "source[0]";
        case ModRowField::AuxSource:           return prefix + "source[1]";
        case ModRowField::Amount:              return prefix + "plainParams/kParamAmount";
        case ModRowField::Bipolar:             return prefix + "plainParams/kParamBipolar";
        case ModRowField::Bypass:              return prefix + "plainParams/kParamBypass";
        case ModRowField::DestinationClass:    return prefix + "destModuleTypeString";
        case ModRowField::DestinationInstance: return prefix + "destModuleID";
        case ModRowField::DestinationParameter: return prefix + "destModuleParamID";
    }

    return prefix;
}

dsp::PoleZeroDescription poleZeroFromDocument (const model::PresetDocument& document,
                                               const PoleZeroObjectPath objectPath)
{
    using Section = dsp::PoleZeroDescription;

    dsp::PoleZeroDescription description;

    for (std::size_t object = 0; object < Section::kMaxObjects; ++object)
    {
        const auto prefix = objectPath.path + "/PZs[" + std::to_string (object) + "]";

        // `fp[0]` is the object's presence: the reference writes all eight sections of an object or none
        // of it, so a partial object is not a case this has to read.
        if (document.parameters.find (prefix + "/fp[0]") == document.parameters.end())
            break;

        double poleFrequency[Section::kSectionsPerObject] {};
        double zeroFrequency[Section::kSectionsPerObject] {};
        double poleQ[Section::kSectionsPerObject] {};
        double zeroQ[Section::kSectionsPerObject] {};
        double gain[Section::kSectionsPerObject] {};
        int type[Section::kSectionsPerObject] {};

        const std::pair<std::string_view, double*> numericFields[] = {
            { "fp", poleFrequency }, { "fz", zeroFrequency }, { "qp", poleQ }, { "qz", zeroQ },
            { "gain", gain },
        };

        for (const auto& [field, out] : numericFields)
            for (std::size_t i = 0; i < Section::kSectionsPerObject; ++i)
                if (const auto found = document.parameters.find (prefix + "/" + std::string (field) + "["
                                                                 + std::to_string (i) + "]");
                    found != document.parameters.end())
                    found->second.numberAsDouble (out[i]);

        for (std::size_t i = 0; i < Section::kSectionsPerObject; ++i)
            if (const auto found = document.parameters.find (prefix + "/types[" + std::to_string (i) + "]");
                found != document.parameters.end() && found->second.type == model::Value::Type::Int)
                type[i] = static_cast<int> (found->second.integer);

        dsp::appendPoleZeroObject (description, poleFrequency, zeroFrequency, poleQ, zeroQ, gain, type);
    }

    return description;
}

std::string describeFilterSpec (const dsp::FilterSpec& spec)
{
    char buffer[256];

    switch (spec.model)
    {
        case dsp::FilterModel::Svf:
            std::snprintf (buffer, sizeof (buffer), "%s %d dB/oct", kindText (spec.kind),
                           dsp::slopeForPoles (spec.poles));
            return buffer;

        case dsp::FilterModel::DualSvf:
            std::snprintf (buffer, sizeof (buffer), "dual: %s -> %s, %d dB/oct", kindText (spec.kind),
                           kindText (spec.secondary), dsp::slopeForPoles (spec.poles));
            return buffer;

        case dsp::FilterModel::MorphSvf:
            std::snprintf (buffer, sizeof (buffer), "morph: %s -> %s -> %s, %d dB/oct", kindText (spec.kind),
                           kindText (spec.secondary), kindText (spec.tertiary),
                           dsp::slopeForPoles (spec.poles));
            return buffer;

        case dsp::FilterModel::Ladder:
            std::snprintf (buffer, sizeof (buffer), "ladder %d dB/oct, drive %.2f",
                           dsp::slopeForPoles (spec.poles), spec.flavourDrive);
            return buffer;

        case dsp::FilterModel::Comb:
            std::snprintf (buffer, sizeof (buffer), "comb, %s feedback, loop filter %d lp %d hp, %s",
                           spec.comb.negative ? "negative" : "positive", spec.comb.lowPassPoles,
                           spec.comb.highPassPoles, spec.comb.distorted ? "driven" : "clean");
            return buffer;

        case dsp::FilterModel::Allpass:
            std::snprintf (buffer, sizeof (buffer), "all pass, %d stages", spec.stages);
            return buffer;

        case dsp::FilterModel::Diffuser:
            std::snprintf (buffer, sizeof (buffer), "diffuser, %d stages", spec.stages);
            return buffer;

        case dsp::FilterModel::PoleZero:
            return "drawn response (pole/zero sections)";

        case dsp::FilterModel::Unimplemented:
            return "not shipped";
    }

    return "not shipped";
}

bool modelNumber (const model::Value& value, double& out)
{
    return value.numberAsDouble (out);
}

bool modelText (const model::Value& value, std::string& out)
{
    if (! value.isText())
        return false;

    out = value.text;
    return true;
}

int choiceIndexForText (const ChoiceList& choices, const ChoiceSet set, std::string_view text)
{
    if (set == ChoiceSet::WarpMode)
    {
        const auto mode = dsp::warpModeFromName (text);

        for (int i = 0; i < choices.size(); ++i)
            if (i != choices.sentinel && choices.warpModes[i] == mode)
                return i;

        return choices.sentinel;
    }

    if (set == ChoiceSet::FilterType)
    {
        const auto spec = dsp::filterSpecFromName (text);

        if (spec.model == dsp::FilterModel::Unimplemented)
            return choices.sentinel;

        for (int i = 0; i < choices.size(); ++i)
            if (i != choices.sentinel && specKey (choices.filterSpecs[i]) == specKey (spec))
                return i;

        return choices.sentinel;
    }

    if (set == ChoiceSet::NoiseType)
    {
        const auto type = dsp::noiseTypeFromName (text);

        if (! type.has_value())
            return choices.sentinel;   // the sample path, and anything else outside the four colours

        for (int i = 0; i < choices.size(); ++i)
            if (i != choices.sentinel && choices.noiseTypes[i] == *type)
                return i;

        return choices.sentinel;
    }

    if (set == ChoiceSet::SubShape)
    {
        const auto shape = dsp::subShapeFromName (text);

        if (! shape.has_value())
            return choices.sentinel;

        for (int i = 0; i < choices.size(); ++i)
            if (i != choices.sentinel && choices.subShapes[i] == *shape)
                return i;

        return choices.sentinel;
    }

    if (set == ChoiceSet::DestinationClass)
    {
        // The class is stored by name, and the model's own enum is the vocabulary — so the token *is* the
        // match, and a name the schema does not carry (the model knows one more, `FXSplitMS`) selects the
        // sentinel and is preserved rather than rewritten.
        for (int i = 0; i < choices.size(); ++i)
            if (i != choices.sentinel && choices.modelNames[static_cast<std::size_t> (i)] == text)
                return i;

        return choices.sentinel;
    }

    return 0;
}

ParameterBridge::ParameterBridge()
{
    // Choices first: the filter type's authored default is a *spec*, and its index in the choice list is
    // only known once that list exists.
    buildChoices();
    buildSpecs();
    buildModulationSpecs();
    buildParameters();
}

bool ParameterBridge::choiceStoredAsNumber (const ChoiceSet set)
{
    // A row's `source[0]`/`source[1]` is the reference's **1-based enum stored as an int**, so the
    // document's value is the source number itself rather than a token string. Every other choice is
    // stored as the name of the thing it selects.
    return set == ChoiceSet::ModSource;
}

const ChoiceList& ParameterBridge::choices (const ChoiceSet set) const
{
    switch (set)
    {
        case ChoiceSet::WarpMode:         return warp_;
        case ChoiceSet::FilterType:       return filter_;
        case ChoiceSet::NoiseType:        return noise_;
        case ChoiceSet::SubShape:         return sub_;
        case ChoiceSet::ModSource:        return modSource_;
        case ChoiceSet::DestinationClass: return destinationClass_;
        case ChoiceSet::None:             break;
    }

    return warp_;
}

const ParamSpec* ParameterBridge::modulationSpec (const int row, const ModRowField field) const
{
    if (row < 0 || row >= kMatrixRows)
        return nullptr;

    const auto wanted = modulationFieldPath (row, field);

    for (const auto& spec : modulationSpecs_)
        if (spec.modelPath == wanted)
            return &spec;

    return nullptr;
}

double ParameterBridge::resolveAbsent (const ParamSpec& spec)
{
    // A choice's default is an index into its own list, never a model-unit number, so the schema is not
    // asked about it.
    if (spec.type == ParamType::Choice)
        return spec.init;

    if (const auto established = model::Schema::instance().defaultFor (spec.schemaPath))
        return *established;

    return spec.init;
}

double ParameterBridge::currentModelValue (const juce::RangedAudioParameter& parameter, const ParamSpec& spec,
                                           const ChoiceList& choices)
{
    juce::ignoreUnused (choices);

    switch (spec.type)
    {
        case ParamType::Bool:
            return static_cast<const juce::AudioParameterBool&> (parameter).get() ? 1.0 : 0.0;

        case ParamType::Int:
            return static_cast<double> (static_cast<const juce::AudioParameterInt&> (parameter).get());

        case ParamType::Choice:
            return static_cast<double> (static_cast<const juce::AudioParameterChoice&> (parameter).getIndex());

        case ParamType::Float:
            break;
    }

    return static_cast<double> (static_cast<const juce::AudioParameterFloat&> (parameter).get());
}

ParamSpec& ParameterBridge::appendSpec (std::vector<ParamSpec>& list, std::string modelPath,
                                        std::string label, std::string group, const ParamType type,
                                        const ChoiceSet set, const double min, const double max,
                                        const double init)
{
    ParamSpec spec;
    spec.modelPath = std::move (modelPath);
    spec.schemaPath = model::Schema::instance().normalise (spec.modelPath);
    spec.id = parameterIdFromPath (spec.modelPath);
    spec.label = std::move (label);
    spec.group = std::move (group);
    spec.type = type;
    spec.choiceSet = set;
    spec.min = min;
    spec.max = max;
    spec.init = init;

    // `specs_` comes first in the parameter table, so the modulation surface's indices continue from it.
    spec.paramIndex = &list == &specs_ ? static_cast<int> (specs_.size())
                                       : static_cast<int> (specs_.size() + modulationSpecs_.size());

    list.push_back (std::move (spec));
    return list.back();
}

void ParameterBridge::buildSpecs()
{
    const auto add = [this] (std::string modelPath, std::string label, std::string group, const ParamType type,
                             const ChoiceSet set, const double min, const double max, const double init)
    {
        appendSpec (specs_, std::move (modelPath), std::move (label), std::move (group), type, set, min, max,
                    init);
    };

    const auto addGlobal = [&add] (const char* leaf, std::string label, const ParamType type, const double min,
                                   const double max, const double init)
    {
        add (std::string ("Global0/plainParams/") + leaf, std::move (label), "Global", type, ChoiceSet::None, min,
             max, init);
    };

    // --- Global ---------------------------------------------------------------------------------------
    //
    // `kParamPolyCount`'s 1..32 is the reference's own range and the engine's ceiling (ticket 28);
    // `kParamPortamentoTime`'s 0..4 s is xerum's authored range (the corpus observes up to 2.61 s, and an
    // observed maximum is not a bound — ticket 21).
    addGlobal ("kParamPolyCount", "Global Polyphony", ParamType::Int, 1, 32, 16);
    addGlobal ("kParamMonoToggle", "Global Mono", ParamType::Bool, 0, 1, 0);
    addGlobal ("kParamLegato", "Global Legato", ParamType::Bool, 0, 1, 0);
    addGlobal ("kParamPortamentoTime", "Global Glide Time (s)", ParamType::Float, 0, 4, 0);
    addGlobal ("kParamPortamentoCurve", "Global Glide Curve (%)", ParamType::Float, 0, 100, 50);
    addGlobal ("kParamPortaAlways", "Global Glide Always", ParamType::Bool, 0, 1, 0);
    addGlobal ("kParamMasterVolume", "Global Master Level", ParamType::Float, 0, 1,
               kReferenceDefaultMasterVolume);

    // --- The master ceiling (ticket `58`) -------------------------------------------------------------
    //
    // **The one deliberate divergence from the reference's schema.** The reference has no limiter leaf —
    // nothing in its decoded model is a ceiling — so these two paths are xerum's own and no reference
    // preset carries them. That is exactly why they belong on this surface as parameters rather than as a
    // hard-coded stage: a user who wants the reference's own output sets `kParamLimitOn` to 0 and gets a
    // wire, and one who leaves it alone gets the ceiling. `resolveAbsent` lands both on `init` when a
    // preset does not carry them, so the default is stated here and nowhere else in the plugin.
    //
    // The default is **on**, because the dev asked for the clipping to stop; the ceiling's default, its
    // range and its unit are Ableton's own factory preset (`limiter.h` records the artefact).
    add ("Master0/plainParams/kParamLimitOn", "Master Limiter", "Master", ParamType::Bool, ChoiceSet::None, 0, 1,
         1);
    add ("Master0/plainParams/kParamLimitCeiling", "Master Ceiling (dB)", "Master", ParamType::Float,
         ChoiceSet::None, dsp::kMasterCeilingMinDb, dsp::kMasterCeilingMaxDb, dsp::kMasterCeilingDb);

    // --- The three wavetable oscillator slots ---------------------------------------------------------
    for (int slot = 0; slot < dsp::kOscillatorSlots; ++slot)
    {
        const auto index = std::to_string (slot);
        const std::string group = std::string ("Osc ") + static_cast<char> ('A' + slot);
        const std::string base = "Oscillator" + index + "/plainParams/";
        const std::string warp = "Oscillator" + index + "/WTOsc" + index + "/plainParams/";
        const std::string tag = std::string (1, static_cast<char> ('A' + slot)) + " ";

        // `kParamEnable`'s default is **on for slot A and off for B and C**, and *off* for a filter. Both
        // halves are the reference's own init state, read from its parameter surface through the ticket 08
        // harness: `OSC A | A Enable | 1.000000000 | On`, `OSC B | B Enable | 0.000000000 | Off`,
        // `Filter 1 | Filter 1 On | 0.000000000 | Off`. It is also what makes the plugin a playable
        // instrument the moment a host loads it: a synth that renders silence until a preset arrives is
        // broken in a way no validator checks.
        add (base + "kParamEnable", tag + "Enable", group, ParamType::Bool, ChoiceSet::None, 0, 1, slot == 0 ? 1 : 0);
        add (base + "kParamVolume", tag + "Level", group, ParamType::Float, ChoiceSet::None, 0, 1,
             kReferenceDefaultOscillatorVolume);
        add (base + "kParamPitch", tag + "Pitch (semitones)", group, ParamType::Float, ChoiceSet::None, -12, 12, 0);
        add (base + "kParamOctave", tag + "Octave", group, ParamType::Float, ChoiceSet::None, -4, 4, 0);
        add (base + "kParamFine", tag + "Fine (cents)", group, ParamType::Float, ChoiceSet::None, -100, 100, 0);
        add (base + "kParamUnison", tag + "Voices", group, ParamType::Int, ChoiceSet::None, 1, dsp::kMaxUnison, 1);
        add (base + "kParamUnisonRange", tag + "Voices Detune (semitones)", group, ParamType::Float,
             ChoiceSet::None, 0, 48, 0);
        // The stereo spread the unison sub-voices are placed across the image with, in percent. Authoring
        // it here (rather than taking the schema's observed -100..79.84 as legality) is the bridge's rule:
        // the sign mirrors the spread, so **-100..100** is the engine's own range and an observed value is
        // never clamped. 0 is xerum's init and means "no width", which is what the bus rendered before
        // ticket 41 made it stereo.
        add (base + "kParamUnisonStereo", tag + "Stereo", group, ParamType::Float,
             ChoiceSet::None, -100, 100, 0);
        add (warp + "kParamWarpMenu", tag + "Warp 1 Mode", group, ParamType::Choice, ChoiceSet::WarpMode, 0, 0, 0);
        add (warp + "kParamWarp", tag + "Warp 1 Amount", group, ParamType::Float, ChoiceSet::None, 0, 1, 0);
        add (warp + "kParamWarpMenu2", tag + "Warp 2 Mode", group, ParamType::Choice, ChoiceSet::WarpMode, 0, 0, 0);
        add (warp + "kParamWarp2", tag + "Warp 2 Amount", group, ParamType::Float, ChoiceSet::None, 0, 1, 0);
        // `kParamTablePos` is a frame position and the model stores it as a **float** leaf, which is also
        // what the engine takes (`OscillatorSettings::framePosition`, a `double` read by
        // `Wavetable::interpolated` as a fractional frame). The range starts at **1** because the model's
        // own observed values do — 1.51 to 256 in the schema table, and 256 is the ceiling because no
        // table in the corpus has more than 256 frames (ticket 02, `src/dsp/wavetable.h`).
        //
        // Starting at 0 is what a reader would expect and is what this did first; through the ticket 08
        // harness a 0..255 parameter read **back** as 1 at its default (a host-side normalised value of
        // 1/255), while every 1-based integer and float parameter read back exactly. The plugin's own
        // value was 0, so this is a JUCE VST3 wrapper/host quirk around a zero minimum that we observed
        // rather than explained — recorded in plugin/README.md — and the schema's own range makes
        // avoiding it cost nothing.
        add (warp + "kParamTablePos", tag + "Table Position (frames)", group, ParamType::Float, ChoiceSet::None,
             1, 256, 1);
    }

    // --- The two fixed single-engine slots: NOISE and SUB ---------------------------------------------
    //
    // Ticket 40. They are not container slots — there is no `kParamType` and no warp menu — so they get
    // their own groups and their own parameters rather than the oscillator loop above. Defaults are the
    // reference's own init state, measured from its parameter surface (`xerum-host --dump-params`):
    // `Noise Enable 0.000000000 Off`, `Noise Level 0.25`, `Sub Enable 0.000000000 Off`, `Sub Level 0.75`,
    // `Sub Octave 0.5 → 0`, `Sub Shape 0.000000000 Sine`, `Sub Phase 0`, `Sub Cont. Phase Off`.
    {
        const std::string noiseBase = "Oscillator3/plainParams/";
        const std::string noiseParams = "Oscillator3/NoiseOsc3/plainParams/";

        add (noiseBase + "kParamEnable", "Enable", "Noise", ParamType::Bool, ChoiceSet::None, 0, 1, 0);
        add (noiseBase + "kParamVolume", "Level", "Noise", ParamType::Float, ChoiceSet::None, 0, 1, 0.25);
        add (noiseParams + "kParamNoiseType", "Colour", "Noise", ParamType::Choice, ChoiceSet::NoiseType,
             0, 0, noise_.sentinel);
        add (noiseParams + "kParamRandomPhase", "Random Phase", "Noise", ParamType::Float, ChoiceSet::None,
             0, 100, 0);

        const std::string subBase = "Oscillator4/plainParams/";
        const std::string subParams = "Oscillator4/SubOsc4/plainParams/";

        add (subBase + "kParamEnable", "Enable", "Sub Osc", ParamType::Bool, ChoiceSet::None, 0, 1, 0);
        add (subBase + "kParamVolume", "Level", "Sub Osc", ParamType::Float, ChoiceSet::None, 0, 1, 0.75);
        add (subParams + "kParamShape", "Waveform", "Sub Osc", ParamType::Choice, ChoiceSet::SubShape, 0, 0, 0);
        add (subBase + "kParamOctave", "Octave", "Sub Osc", ParamType::Float, ChoiceSet::None, -4, 4, 0);
        add (subBase + "kParamPitch", "Pitch (semitones)", "Sub Osc", ParamType::Float, ChoiceSet::None,
             -12, 12, 0);
        add (subBase + "kParamFine", "Fine (cents)", "Sub Osc", ParamType::Float, ChoiceSet::None,
             -100, 100, 0);
        // The manual calls CRS "continuous (no snap) semitones"; the schema's own observed range is
        // +-64, which is what a range can be taken from (ticket 21: an observed range is not legality).
        add (subBase + "kParamCoarsePit", "Coarse Pitch (semitones)", "Sub Osc", ParamType::Float,
             ChoiceSet::None, -64, 64, 0);
        // Degrees, 0..360. The schema's observed values stop at 180 and the reference's own display is in
        // degrees ("  0°"), which is why the unit is degrees rather than turns.
        add (subParams + "kParamInitialPhase", "Phase (degrees)", "Sub Osc", ParamType::Float, ChoiceSet::None,
             0, 360, 0);
        add (subParams + "kParamContiguousPhase", "Contiguous Phase", "Sub Osc", ParamType::Bool,
             ChoiceSet::None, 0, 1, 0);
    }

    // --- The two voice filters ------------------------------------------------------------------------
    //
    // The filter type's default is xerum's authored choice: a 12 dB/oct low pass. The reference's own init
    // type displays "MG Low 12", but that is a *display* string and the model stores the default as
    // absence — `MgL12` is not in the schema's enum, because the corpus never needed to store a value the
    // reference already had — so mapping that text onto a choice would be a guess. `defaultStringFor`
    // returns it (ticket 38) and the plugin deliberately does not use it for a choice.
    int defaultFilterType = 0;

    {
        const auto wanted = specKey (dsp::filterSpecFromName ("L12"));

        for (int i = 0; i < filter_.size(); ++i)
            if (i != filter_.sentinel && specKey (filter_.filterSpecs[i]) == wanted)
                defaultFilterType = i;
    }

    for (int slot = 0; slot < dsp::kFilterSlots; ++slot)
    {
        const auto index = std::to_string (slot);
        const std::string group = "Filter " + std::to_string (slot + 1);
        const std::string base = "VoiceFilter" + index + "/plainParams/";
        const std::string tag = std::to_string (slot + 1) + " ";

        add (base + "kParamEnable", tag + "Enable", group, ParamType::Bool, ChoiceSet::None, 0, 1, 0);
        add (base + "kParamType", tag + "Type", group, ParamType::Choice, ChoiceSet::FilterType, 0, 0,
             defaultFilterType);
        add (base + "kParamFreq", tag + "Cutoff", group, ParamType::Float, ChoiceSet::None, 0, 1, 1);
        add (base + "kParamReso", tag + "Resonance (%)", group, ParamType::Float, ChoiceSet::None, 0, 100, 0);
        add (base + "kParamDrive", tag + "Drive (%)", group, ParamType::Float, ChoiceSet::None, 0, 100, 0);
        add (base + "kParamVar", tag + "Variation (%)", group, ParamType::Float, ChoiceSet::None, 0, 100, 0);
    }

    // --- The amplitude envelope -----------------------------------------------------------------------
    //
    // **Which envelope this is, and why it is `Env0`.** `tools/reference-init-defaults.py`'s crosswalk
    // maps the reference plugin's "Env 1" group onto `Env{N}` instance 0, and the reference's own
    // envelope 1 is its fixed amplitude envelope — the one that is not routable. That last half is a
    // claim about the reference we have not measured, so it is marked in `plugin/README.md` as an
    // inference, and the binding is a stated convention either way.
    {
        const std::string base = "Env0/plainParams/";
        add (base + "kParamAttack", "Attack (s)", "Amp Envelope", ParamType::Float, ChoiceSet::None, 0, 32, 0.005);
        add (base + "kParamDecay", "Decay (s)", "Amp Envelope", ParamType::Float, ChoiceSet::None, 0, 32, 0.0);
        add (base + "kParamSustain", "Sustain", "Amp Envelope", ParamType::Float, ChoiceSet::None, 0, 1, 1.0);
        add (base + "kParamRelease", "Release (s)", "Amp Envelope", ParamType::Float, ChoiceSet::None, 0, 32, 0.05);
    }

    // --- The eight macros (ticket 48) ------------------------------------------------------------------
    //
    // `Macro{N}/plainParams/kParamValue` is the reference's own leaf and its own range (0..100), and it is
    // the whole of a macro that xerum can carry: the `name` beside it is the preset's label for the knob,
    // not a value, and xerum's parameter table has no string type, so a macro arrives with the number the
    // preset assigned it.
    //
    // They are parameters rather than document-only values because a macro is a **control**: the matrix's
    // source table names `Macro 1..8`, so a macro that no one can move is the same defect as an LFO with
    // no generator, one step smaller.
    //
    // **One group per macro, not one `Macros` group.** A page is a group and a page lives in one model
    // module (`InterfacePlan.h`: the strip's report dots and the page's subtitle are keyed on it), and
    // `Macro0..7` are eight modules — so eight pages of one knob is the shape the design's own rule
    // produces. A single `Macros` page would span eight modules and make its own subtitle a lie.
    for (int macro = 0; macro < dsp::kMacroCount; ++macro)
    {
        const auto index = std::to_string (macro);
        add ("Macro" + index + "/plainParams/kParamValue", "Amount",
             "Macro " + std::to_string (macro + 1), ParamType::Float, ChoiceSet::None, 0, 100, 0);
    }
}

void ParameterBridge::buildChoices()
{
    // --- Warp ----------------------------------------------------------------------------------------
    //
    // Built from the schema's own enum, so every mode the engine implements is selectable and no mode it
    // does not implement is offered as though it were. The canonical token for a mode is the first enum
    // value that maps to it — deterministic, and never hand-written.
    {
        const auto names = enumFor ("Oscillator{N}/WTOsc{N}/plainParams/kParamWarpMenu");

        for (const auto& entry : kWarpLabels)
        {
            std::string canonical;

            for (const auto& name : names)
                if (dsp::warpModeFromName (name) == entry.mode)
                {
                    canonical = name;
                    break;
                }

            // **A source-driven family canonicalises to its `_OSC` variant** (ticket 48). Every PD, FM, AM
            // and RM name carries the signal it reads as a suffix, and the enum's alphabetical order puts
            // `kFMP_NOISE` before `kFM_OSC` and `kPD_FILT1` before `kPD_OSC` — so the first match would
            // make the choice for "frequency modulation" read a signal the user never chose. The token a
            // mode writes back to a document, and the source `warpSourceFromName` recovers from it, are
            // therefore the plainest variant the family has. A *preset* is not affected: its own name is
            // read for the variant it stored, and this only decides what the interface selects.
            if (dsp::warpSourceFromName (canonical) != dsp::WarpSource::None)
                for (const auto& name : names)
                    if (dsp::warpModeFromName (name) == entry.mode
                        && dsp::warpSourceFromName (name) == dsp::WarpSource::OtherOsc)
                    {
                        canonical = name;
                        break;
                    }

            warp_.labels.add (entry.label);
            warp_.modelNames.push_back (canonical);
            warp_.warpModes[warp_.labels.size() - 1] = entry.mode;
        }

        warp_.sentinel = warp_.labels.size();
        warp_.labels.add ("not in xerum's set");
        warp_.modelNames.emplace_back();
    }

    // --- Filter --------------------------------------------------------------------------------------
    {
        const auto names = enumFor ("VoiceFilter{N}/plainParams/kParamType");

        struct Found
        {
            dsp::FilterSpec spec;
            std::string canonical;
        };

        std::map<std::string, Found> byKey;

        for (const auto& name : names)
        {
            const auto spec = dsp::filterSpecFromName (name);

            if (spec.model == dsp::FilterModel::Unimplemented)
                continue;

            byKey.try_emplace (specKey (spec), Found { spec, std::string (name) });
        }

        std::vector<Found> found;
        found.reserve (byKey.size());

        for (auto& [key, value] : byKey)
            found.push_back (value);

        std::sort (found.begin(), found.end(),
                   [] (const Found& a, const Found& b) { return specLess (a.spec, b.spec); });

        for (const auto& entry : found)
        {
            filter_.labels.add (describeFilterSpec (entry.spec));
            filter_.modelNames.push_back (entry.canonical);

            const int index = filter_.labels.size() - 1;
            filter_.filterSpecs[index] = entry.spec;
        }

        filter_.sentinel = filter_.labels.size();
        filter_.labels.add ("not in xerum's set");
        filter_.modelNames.emplace_back();
    }

    // --- NOISE's colour menu -------------------------------------------------------------------------
    //
    // Four colours, from the schema's own enum (`kValues510`), in the manual's table order. The
    // **sentinel is the reference's sample mode** — `kParamNoiseType`'s absence — and it is the default,
    // because a preset that enables NOISE without a colour asked for a sample file, not for a colour:
    // handing it White would be a substitution the import contract forbids.
    {
        for (const auto type : { dsp::NoiseType::White, dsp::NoiseType::Pink, dsp::NoiseType::Brown,
                                 dsp::NoiseType::Geiger })
        {
            noise_.labels.add (juce::String (std::string (dsp::noiseTypeName (type))));
            noise_.modelNames.emplace_back (dsp::noiseTypeName (type));
            noise_.noiseTypes[noise_.labels.size() - 1] = type;
        }

        noise_.sentinel = noise_.labels.size();
        noise_.labels.add ("sample (not synthesised)");
        noise_.modelNames.emplace_back();
    }

    // --- SUB's waveform menu -------------------------------------------------------------------------
    //
    // Six, from the reference's own init state rather than from the schema's five: `Sub Shape` is a
    // six-step control whose default is `Sine`, and the schema's vocabulary holds only the five the
    // corpus stores (ticket 40).
    {
        for (const auto& entry : kSubShapeLabels)
        {
            sub_.labels.add (entry.label);
            sub_.modelNames.emplace_back (dsp::subShapeName (entry.shape));
            sub_.subShapes[sub_.labels.size() - 1] = entry.shape;
        }

        sub_.sentinel = sub_.labels.size();
        sub_.labels.add ("not in xerum's set");
        sub_.modelNames.emplace_back();
    }

    // --- A matrix row's source -----------------------------------------------------------------------
    //
    // The reference's own 1-based 60-name table, which ticket `27` recovered and `modSourceName` owns:
    // **the label IS the model value's name**, so the two cannot drift. Index 0 is the reference's "no
    // source", which the table has no name for. There is no sentinel — every value 0..60 is
    // representable, and one v1 does not evaluate is still selectable and reported where it is used.
    {
        modSource_.labels.add ("none");

        for (int value = 1; value <= dsp::kReferenceSourceCount; ++value)
            modSource_.labels.add (juce::String (std::string (dsp::modSourceName (value))));

        modSource_.sentinel = modSource_.labels.size();   // unreachable: the enum is closed
    }

    // --- A matrix row's destination class ------------------------------------------------------------
    //
    // The model's own closed enum, so a class the reference uses is selectable and a class the model
    // does not carry is preserved by the sentinel rather than rewritten.
    {
        for (const auto& name : enumFor ("ModSlot{N}/destModuleTypeString"))
        {
            destinationClass_.labels.add (juce::String (std::string (name)));
            destinationClass_.modelNames.emplace_back (name);
        }

        destinationClass_.sentinel = destinationClass_.labels.size();
        destinationClass_.labels.add ("not in xerum's set");
        destinationClass_.modelNames.emplace_back();
    }
}

void ParameterBridge::buildModulationSpecs()
{
    // One spec per field of each of the 64 rows. The **model paths are the document's own keys** — the
    // very paths `modulationRowsFromDocument` reads and `ModSlot` is decoded from — so a parameter edit,
    // a document row and the engine's row all address the same key. That is ticket 45's requirement:
    // these are the *parameters the bridge writes*, and without them a preset would overwrite every edit
    // through `applyDocument` (ticket 40's finding, applied to the matrix).
    const auto add = [this] (const int row, const ModRowField field, std::string label, const ParamType type,
                             const ChoiceSet set, const double min, const double max, const double init)
    {
        appendSpec (modulationSpecs_, modulationFieldPath (row, field), std::move (label), "Matrix", type, set,
                    min, max, init);
    };

    for (int row = 0; row < kMatrixRows; ++row)
    {
        const auto index = std::to_string (row);

        // The source is the reference's 0..60 enum; the schema observes 0..59 and the reference's own
        // table runs to 60 (`ModSlot`, which no preset routes), so xerum's range is the engine's own.
        add (row, ModRowField::Source, "Row " + index + " Source", ParamType::Choice, ChoiceSet::ModSource,
             0, 0, 0);
        add (row, ModRowField::AuxSource, "Row " + index + " Aux Source", ParamType::Choice,
             ChoiceSet::ModSource, 0, 0, 0);

        // `kParamAmount`'s range is the schema's own (-100..100, percent, signed — the sign is the
        // polarity), and 0 is the reference's init.
        add (row, ModRowField::Amount, "Row " + index + " Amount", ParamType::Float, ChoiceSet::None,
             -100, 100, 0);
        add (row, ModRowField::Bipolar, "Row " + index + " Polarity", ParamType::Bool, ChoiceSet::None,
             0, 1, 0);
        add (row, ModRowField::Bypass, "Row " + index + " Bypass", ParamType::Bool, ChoiceSet::None, 0, 1, 0);

        add (row, ModRowField::DestinationClass, "Row " + index + " Destination", ParamType::Choice,
             ChoiceSet::DestinationClass, 0, 0, destinationClass_.sentinel);

        // The instance and the parameter id are the engine's own addressing (`destModuleID`,
        // `destModuleParamID`); the ranges are the schema's observed maxima. The *name* is not editable
        // and is preserved from the document: the id is what the engine addresses, and the name is the
        // only stable identity across schema versions (ticket 03), so rewriting one without the other
        // would break both.
        add (row, ModRowField::DestinationInstance, "Row " + index + " Instance", ParamType::Int,
             ChoiceSet::None, 0, 207, 0);
        add (row, ModRowField::DestinationParameter, "Row " + index + " Destination Param", ParamType::Int,
             ChoiceSet::None, 0, 59, 0);
    }
}

void ParameterBridge::buildParameters()
{
    tree_ = std::make_unique<juce::AudioProcessorParameterGroup> ("xerum", "xerum", "|");

    // Groups in first-appearance order, which is the table's order, so the tree a host sees is ordered
    // the way the table reads.
    std::map<std::string, juce::AudioProcessorParameterGroup*> groups;

    const auto create = [this, &groups] (const ParamSpec& spec)
    {
        const juce::ParameterID parameterID { spec.id, 1 };
        const juce::String parameterName { spec.label };

        std::unique_ptr<juce::RangedAudioParameter> parameter;

        switch (spec.type)
        {
            case ParamType::Bool:
                parameter = std::make_unique<juce::AudioParameterBool> (parameterID, parameterName,
                                                                        spec.init != 0.0);
                break;

            case ParamType::Float:
                parameter = std::make_unique<juce::AudioParameterFloat> (
                    parameterID, parameterName,
                    juce::NormalisableRange<float> (static_cast<float> (spec.min), static_cast<float> (spec.max)),
                    static_cast<float> (spec.init));
                break;

            case ParamType::Int:
                parameter = std::make_unique<DiscreteIntParameter> (
                    parameterID, parameterName, static_cast<int> (spec.min), static_cast<int> (spec.max),
                    static_cast<int> (spec.init));
                break;

            case ParamType::Choice:
                parameter = std::make_unique<juce::AudioParameterChoice> (
                    parameterID, parameterName, choices (spec.choiceSet).labels,
                    static_cast<int> (spec.init));
                break;
        }

        parameters_.push_back (parameter.get());

        auto*& group = groups[spec.group];

        if (group == nullptr)
        {
            const auto id = "group." + lower (spec.group);
            group = new juce::AudioProcessorParameterGroup (id, juce::String (spec.group), "|");
            tree_->addChild (std::unique_ptr<juce::AudioProcessorParameterGroup> (group));
        }

        // The group owns the parameter; the tree owns the group and is handed to the processor.
        group->addChild (std::move (parameter));
    };

    for (const auto& spec : specs_)
        create (spec);

    for (const auto& spec : modulationSpecs_)
        create (spec);
}

} // namespace xerum::plugin
