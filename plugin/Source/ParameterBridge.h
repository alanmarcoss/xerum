// xerum::plugin — the parameter bridge.
//
// Ticket 30, the load-bearing half. Hosts see **xerum's own** parameter identity:
//
//   * an id derived from the module path (`Oscillator0/plainParams/kParamPitch` -> `oscillator0.pitch`),
//     so the id is a function of the model's addressing rather than an index, and a preset's document
//     key and the plugin's parameter id cannot drift apart;
//   * a label xerum authors (`Osc A Pitch`), never the reference's model names (`kParamPitch`) and never
//     the reference *plugin's* VST3 strings (`A Level`). Ticket `08` measured that those two namespaces
//     do not even correspond to each other (2623 VST3 names against 776 preset leaf paths), so borrowing
//     either buys nothing and imports a naming dependency (ticket 13, D6).
//
// The surface is **exactly what the engine implements** — 59 parameters, not 776. Exposing the rest
// would put controls in a host that write a value nothing reads, which is the placeholder-fallback smell
// the project forbids. A preset's other parameters are not lost: they stay in the document and in the
// import report.
//
// ## Ranges and defaults come from xerum, and say so
//
// The schema's `min`/`max` are **corpus-observed ranges, not legality** (ticket 21), and 7 of the 14
// established defaults lie outside them (ticket 38). So a parameter's range is authored here, in the
// engine's own units, and `plugin/tests/bridge_test.cpp` checks the two places they must agree.
//
// A parameter's default is `Schema::defaultFor` when that is established and xerum's own init value
// otherwise — see `resolveAbsent`. **nullopt is not zero**: an unestablished default falls through to
// xerum's init, never to 0.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "filter.h"
#include "voice.h"
#include "warp.h"
#include "document.h"
#include "schema.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xerum::plugin
{

enum class ParamType { Bool, Float, Int, Choice };

/// Which table the choice strings come from. The model paths that are discrete-valued and need a
/// vocabulary: the warp menu, the filter type, and ticket 40's two fixed slots — NOISE's colour menu and
/// SUB's waveform menu.
enum class ChoiceSet { None, WarpMode, FilterType, NoiseType, SubShape };

/// A built choice list: xerum's labels, and the model token each choice stands for.
struct ChoiceList
{
    juce::StringArray labels;

    /// Parallel to `labels`: the preset-model token the choice writes into a document. **Empty** means
    /// the choice has no model form — either "off" (the key is erased) or the sentinel, which means
    /// "leave whatever the preset said alone" and is never written.
    std::vector<std::string> modelNames;

    dsp::WarpMode warpModes[64] {};
    dsp::FilterSpec filterSpecs[160] {};
    dsp::NoiseType noiseTypes[8] {};
    dsp::SubShape subShapes[8] {};

    int sentinel { -1 };   ///< the "not in xerum's set" choice, always last

    int size() const { return labels.size(); }
};

/// One host-facing parameter, and the model path it stands for.
struct ParamSpec
{
    std::string id;           ///< xerum's stable id, derived from `modelPath`
    std::string modelPath;    ///< the concrete path a document is keyed by
    std::string schemaPath;   ///< `modelPath` normalised, what the schema is asked about
    std::string label;        ///< xerum's own display label
    std::string group;        ///< xerum's own group ("Osc A", "Filter 1", …)

    ParamType type { ParamType::Float };
    ChoiceSet choiceSet { ChoiceSet::None };

    double min { 0.0 };
    double max { 0.0 };
    double init { 0.0 };      ///< xerum's own init value, in model units (a `Choice`: the default index)

    int paramIndex { -1 };    ///< into `ParameterBridge::parameters()`
};

/// The parameter surface: the table, the JUCE parameters and the group tree.
class ParameterBridge
{
public:
    ParameterBridge();

    const std::vector<ParamSpec>& specs() const { return specs_; }
    const std::vector<juce::RangedAudioParameter*>& parameters() const { return parameters_; }
    const ChoiceList& choices (ChoiceSet set) const;

    /// Hand the group tree to the processor. `setParameterTree` takes it by value, so it is moved once.
    juce::AudioProcessorParameterGroup releaseTree() { return std::move (*tree_); }

    /// The value an **absent** document leaf resolves to, in model units.
    ///
    /// Order: `Schema::defaultFor` when established, else `spec.init` (xerum's own init). `nullopt` is
    /// not zero — a leaf with no established default resolves to the value xerum chose for it, which is
    /// a different thing from a value nobody chose.
    static double resolveAbsent (const ParamSpec& spec);

    /// A parameter's current value, in model units, read from its JUCE object.
    static double currentModelValue (const juce::RangedAudioParameter& parameter, const ParamSpec& spec,
                                     const ChoiceList& choices);

private:
    void buildSpecs();
    void buildChoices();
    void buildParameters();

    std::vector<ParamSpec> specs_;
    std::vector<juce::RangedAudioParameter*> parameters_;
    std::unique_ptr<juce::AudioProcessorParameterGroup> tree_;
    ChoiceList warp_;
    ChoiceList filter_;
    ChoiceList noise_;
    ChoiceList sub_;
};

/// The rule that turns a module path into xerum's parameter id, stated once:
/// lower-case, drop `plainParams`, drop the `kParam` prefix, join with `.`.
///
///   `Oscillator0/WTOsc0/plainParams/kParamWarpMenu` -> `oscillator0.wtosc0.warpmenu`
std::string parameterIdFromPath (std::string_view modelPath);

/// xerum's own description of a filter spec — the *only* place a filter choice's label is produced, so a
/// label cannot disagree with the spec the engine will build.
std::string describeFilterSpec (const dsp::FilterSpec& spec);

/// Read a number out of a document value. Returns false for anything that is not numeric.
bool modelNumber (const model::Value& value, double& out);

/// Read a string out of a document value. Returns false for anything that is not text.
bool modelText (const model::Value& value, std::string& out);

/// The choice index a document's text value selects. Out-of-vocabulary values select the sentinel, which
/// is what keeps an unimplemented warp name visible in the interface rather than silently replaced.
int choiceIndexForText (const ChoiceList& choices, ChoiceSet set, std::string_view text);

} // namespace xerum::plugin
