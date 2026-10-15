// The parameter bridge: what hosts see, and the two rules that make it xerum's own.
//
// Run alone with:  ctest --test-dir build-30 -R xerum-plugin-tests  (or the binary with "[plugin]")

#include "TestSupport.h"

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <set>

using namespace xerum::plugin;
using namespace xerum::plugin::tests;

namespace
{

const ParamSpec* findSpec (const ParameterBridge& bridge, const std::string& modelPath)
{
    for (const auto& spec : bridge.specs())
        if (spec.modelPath == modelPath)
            return &spec;

    return nullptr;
}

bool isHostSafeCharacter (const char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.';
}

} // namespace

TEST_CASE ("every parameter is derived from its module path, points at the schema, and is unique",
           "[plugin][bridge]")
{
    const ParameterBridge bridge;
    const auto& schema = xerum::model::Schema::instance();

    std::set<std::string> ids;
    std::set<std::string> modelPaths;
    int lastParamIndex = -1;

    // **Ticket `58`'s exception, and the table's only one.** The master ceiling is a deliberate divergence
    // from the reference, and the divergence is structural: the reference's schema has no limiter leaf, so
    // these two paths are xerum's own and the schema describes neither. They are **named** rather than the
    // rule being loosened, so it still binds every other parameter in the table — a hand-written path that
    // appeared without a line here would still fail, which is the property the rule exists for.
    const auto isXerumsOwn = [] (const std::string& modelPath)
    {
        return modelPath == "Master0/plainParams/kParamLimitOn"
               || modelPath == "Master0/plainParams/kParamLimitCeiling";
    };

    const auto check = [&] (const ParamSpec& spec)
    {
        // The id is *derived*, not hand-written: the rule in `parameterIdFromPath` is the definition, and
        // this asserts the table cannot drift from it.
        CHECK (spec.id == parameterIdFromPath (spec.modelPath));

        // And the path really is the model's: the schema normalises the concrete path to the schema path,
        // and the schema path is one the schema describes — or, for the two named above, one the schema
        // pointedly does not.
        CHECK (spec.schemaPath == schema.normalise (spec.modelPath));

        if (isXerumsOwn (spec.modelPath))
            CHECK (schema.describe (spec.schemaPath) == nullptr);
        else
            REQUIRE (schema.describe (spec.schemaPath) != nullptr);

        CHECK (ids.insert (spec.id).second);
        CHECK (modelPaths.insert (spec.modelPath).second);
        CHECK (spec.paramIndex >= 0);

        // The two surfaces are contiguous and in index order, so `specForParameter` is arithmetic.
        CHECK (spec.paramIndex == lastParamIndex + 1);
        lastParamIndex = spec.paramIndex;
    };

    for (const auto& spec : bridge.specs())
        check (spec);

    // **Ticket 45's second surface**: the matrix's rows. The same rules hold — a derived id, a schema
    // path, a unique key — because these are the parameters the bridge now *writes*, and a duplicate key
    // would silently bind a cell to the wrong row.
    for (const auto& spec : bridge.modulationSpecs())
        check (spec);

    CHECK (bridge.parameterCount() == bridge.parameters().size());
    CHECK (bridge.parameterCount() == static_cast<int> (bridge.specs().size())
                                          + static_cast<int> (bridge.modulationSpecs().size()));
    CHECK (bridge.parameters().size() == static_cast<std::size_t> (bridge.parameterCount()));

    // The matrix surface is exactly the acceptance's field list: the source pair, the amount, the
    // polarity, the bypass and the destination triple — eight per row, for the reference's 64 rows.
    CHECK (bridge.modulationSpecs().size() == 8u * 64u);
    CHECK (bridge.modulationSpec (0, ModRowField::Source) != nullptr);
    CHECK (bridge.modulationSpec (0, ModRowField::Source)->modelPath == "ModSlot0/source[0]");
    CHECK (bridge.modulationSpec (63, ModRowField::DestinationParameter)->modelPath
           == "ModSlot63/destModuleParamID");
    CHECK (bridge.modulationSpec (64, ModRowField::Source) == nullptr);

    // The interface surface is what the engine implements, and it is not the reference's 2623-parameter
    // VST3 space nor the model's 776 leaves. A fixed count is the point: it is a claim, not an accident.
    // The modulation surface is deliberately **not** in `specs()` — `InterfacePlan` is what turns specs
    // into device pages, and a 512-row page is not a matrix (ticket 45).
    //
    // 83 rather than 75 since ticket 48: the eight `Macro{N}/plainParams/kParamValue` leaves, because the
    // matrix's source table names `Macro 1..8` and a macro that no one can move would be a source with no
    // generator — the ticket's own defect, one step smaller. They are one group each, so the interface's
    // page-per-group rule is untouched.
    //
    // 85 rather than 83 since ticket 58: the master ceiling's two — the on/off and the ceiling. They are
    // the table's only parameters the schema does not describe, which is the divergence stated in
    // `isXerumsOwn` above and in `docs/test-strategy.md`.
    //
    // 87 rather than 85 since ticket 65: `VoiceFilter0/1/plainParams/kParamLevelOut` — the reference's
    // "Filter 1 Level" / "Filter 2 Level", on its surface at its own default `0.5` — because the leaf was
    // read by nothing and a preset that stores one now has somewhere to put it.
    CHECK (bridge.specs().size() == 87);
    CHECK (schema.leafCount() > bridge.specs().size());
}

TEST_CASE ("the two fixed slots are on the surface, with their own vocabularies", "[plugin][bridge][noise][sub]")
{
    // Ticket 40: NOISE (slot 3) and SUB (slot 4) are not container slots, so nothing in the oscillator
    // loop above builds them — if these ids stopped resolving, the slots would silently render silence
    // with the engine fully implemented. That is the failure this asserts against.
    const ParameterBridge bridge;

    for (const auto* path : { "Oscillator3/plainParams/kParamEnable",
                              "Oscillator3/plainParams/kParamVolume",
                              "Oscillator3/NoiseOsc3/plainParams/kParamNoiseType",
                              "Oscillator3/NoiseOsc3/plainParams/kParamRandomPhase",
                              "Oscillator4/plainParams/kParamEnable",
                              "Oscillator4/plainParams/kParamVolume",
                              "Oscillator4/SubOsc4/plainParams/kParamShape",
                              "Oscillator4/plainParams/kParamOctave",
                              "Oscillator4/plainParams/kParamPitch",
                              "Oscillator4/plainParams/kParamFine",
                              "Oscillator4/plainParams/kParamCoarsePit",
                              "Oscillator4/SubOsc4/plainParams/kParamInitialPhase",
                              "Oscillator4/SubOsc4/plainParams/kParamContiguousPhase" })
        CHECK (findSpec (bridge, path) != nullptr);

    // The colour vocabulary is the schema's four plus the sentinel, and the sentinel is the reference's
    // *sample* mode — which is why the choice's default is the sentinel and not a colour.
    const auto& noise = bridge.choices (ChoiceSet::NoiseType);
    CHECK (noise.size() == 5);
    CHECK (noise.sentinel == 4);
    CHECK (noise.modelNames[0] == "White");
    CHECK (noise.modelNames[3] == "Geiger");
    CHECK (noise.noiseTypes[1] == xerum::dsp::NoiseType::Pink);

    const auto* colour = findSpec (bridge, "Oscillator3/NoiseOsc3/plainParams/kParamNoiseType");
    REQUIRE (colour != nullptr);
    CHECK (colour->init == noise.sentinel);

    // The waveform vocabulary is six, including the `Sine` default the schema never stores: five stored
    // names in the schema, and the reference's own init state's sixth.
    const auto& sub = bridge.choices (ChoiceSet::SubShape);
    CHECK (sub.size() == 7);
    CHECK (sub.sentinel == 6);
    CHECK (sub.modelNames[0] == "Sine");
    CHECK (sub.subShapes[1] == xerum::dsp::SubShape::RoundRect);
    CHECK (sub.subShapes[5] == xerum::dsp::SubShape::Pulse);

    const auto* shape = findSpec (bridge, "Oscillator4/SubOsc4/plainParams/kParamShape");
    REQUIRE (shape != nullptr);
    CHECK (shape->init == 0);   // Sine, the reference's own default

    // A document's token maps onto the choice, and an out-of-vocabulary one onto the sentinel rather
    // than onto a substitute.
    CHECK (choiceIndexForText (noise, ChoiceSet::NoiseType, "Brown") == 2);
    CHECK (choiceIndexForText (noise, ChoiceSet::NoiseType, "Snow") == noise.sentinel);
    CHECK (choiceIndexForText (sub, ChoiceSet::SubShape, "kTriangle") == 2);
    CHECK (choiceIndexForText (sub, ChoiceSet::SubShape, "kSine") == sub.sentinel);
}

TEST_CASE ("hosts see xerum's ids and xerum's labels, never the model's parameter names",
           "[plugin][bridge]")
{
    const ParameterBridge bridge;

    for (const auto& spec : bridge.specs())
    {
        CHECK (spec.label.find ("kParam") == std::string::npos);
        CHECK (spec.id.find ("kparam") == std::string::npos);
        CHECK (spec.label.find ('/') == std::string::npos);

        // A host-safe id shape: lower case, digits and dots. No slashes, no spaces, no `kParam`.
        CHECK (! spec.id.empty());
        CHECK (std::all_of (spec.id.begin(), spec.id.end(), isHostSafeCharacter));

        CHECK (! spec.label.empty());
        CHECK (! spec.group.empty());
    }

    // Spot-check the derivation against ticket 08's two namespaces, which is why this rule exists:
    // the reference's model name and the reference plugin's display name are both different, and neither
    // is what a host sees here.
    const auto* pitch = findSpec (bridge, "Oscillator0/plainParams/kParamPitch");
    REQUIRE (pitch != nullptr);
    CHECK (pitch->id == "oscillator0.pitch");
    CHECK (pitch->label != "kParamPitch");
    CHECK (pitch->label != "A Pitch Bend");
}

TEST_CASE ("ranges are xerum's own, and match the engine's ceilings", "[plugin][bridge]")
{
    const ParameterBridge bridge;

    for (const auto& spec : bridge.specs())
    {
        if (spec.type == ParamType::Choice)
            continue;

        CHECK (spec.min < spec.max);
        CHECK (spec.init >= spec.min);
        CHECK (spec.init <= spec.max);
    }

    // The ceilings are the engine's compile-time constants, not numbers chosen here.
    const auto* unison = findSpec (bridge, "Oscillator0/plainParams/kParamUnison");
    REQUIRE (unison != nullptr);
    CHECK (unison->max == xerum::dsp::kMaxUnison);
    CHECK (unison->min == 1);      // "not stored" is 1, and 1 means off (ticket 28)

    const auto* poly = findSpec (bridge, "Global0/plainParams/kParamPolyCount");
    REQUIRE (poly != nullptr);
    CHECK (poly->max == xerum::dsp::kMaxVoices);

    // And the units are the engine's own, which is what makes a preset's value usable unchanged.
    const auto* freq = findSpec (bridge, "VoiceFilter0/plainParams/kParamFreq");
    REQUIRE (freq != nullptr);
    CHECK (freq->min == 0.0);
    CHECK (freq->max == 1.0);      // normalised, as `VoiceFilter::setCutoff` takes it

    const auto* reso = findSpec (bridge, "VoiceFilter0/plainParams/kParamReso");
    REQUIRE (reso != nullptr);
    CHECK (reso->max == 100.0);    // percent, as the engine takes it
}

TEST_CASE ("choice lists are unique, complete, and stick to the engine's own vocabulary",
           "[plugin][bridge]")
{
    const ParameterBridge bridge;

    for (const auto set : { ChoiceSet::WarpMode, ChoiceSet::FilterType })
    {
        const auto& choices = bridge.choices (set);

        REQUIRE (choices.size() > 2);
        CHECK (choices.sentinel == choices.size() - 1);

        std::set<std::string> labels;

        for (int i = 0; i < choices.size(); ++i)
        {
            const auto label = choices.labels[i].toStdString();

            // Unique labels matter for more than tidiness: a host that stores a parameter as *text* would
            // restore the first match, so two choices spelled the same is a state bug.
            CHECK (labels.insert (label).second);

            // The label is xerum's, not the reference's token.
            CHECK (label.find ("kParam") == std::string::npos);
            CHECK (label.find ("kOsc") == std::string::npos);

            if (i == choices.sentinel)
                continue;

            if (set == ChoiceSet::WarpMode)
            {
                // The label may not lie: the canonical token really maps to the mode the choice builds.
                CHECK (xerum::dsp::warpModeFromName (choices.modelNames[static_cast<std::size_t> (i)])
                       == choices.warpModes[i]);
            }
            else
            {
                const auto spec = xerum::dsp::filterSpecFromName (choices.modelNames[static_cast<std::size_t> (i)]);
                CHECK (spec.model != xerum::dsp::FilterModel::Unimplemented);
                CHECK (xerum::dsp::filterSpecFromName (choices.modelNames[static_cast<std::size_t> (i)]).model
                       == choices.filterSpecs[i].model);
            }
        }
    }

    // A doc value the engine implements must land on a choice, not on the sentinel.
    CHECK (choiceIndexForText (bridge.choices (ChoiceSet::WarpMode), ChoiceSet::WarpMode, "kSync") !=
           bridge.choices (ChoiceSet::WarpMode).sentinel);
    CHECK (choiceIndexForText (bridge.choices (ChoiceSet::WarpMode), ChoiceSet::WarpMode, "kPD_OSC") !=
           bridge.choices (ChoiceSet::WarpMode).sentinel);

    // And one it does not must land on the sentinel rather than on a neighbour.
    CHECK (choiceIndexForText (bridge.choices (ChoiceSet::WarpMode), ChoiceSet::WarpMode, "kRemap_4") ==
           bridge.choices (ChoiceSet::WarpMode).sentinel);
    CHECK (choiceIndexForText (bridge.choices (ChoiceSet::FilterType), ChoiceSet::FilterType, "L12") !=
           bridge.choices (ChoiceSet::FilterType).sentinel);
    CHECK (choiceIndexForText (bridge.choices (ChoiceSet::FilterType), ChoiceSet::FilterType, "Reverb1") ==
           bridge.choices (ChoiceSet::FilterType).sentinel);
}

TEST_CASE ("the choice vocabularies cover every implemented value the schema can store",
           "[plugin][bridge]")
{
    const ParameterBridge bridge;
    const auto& schema = xerum::model::Schema::instance();

    const auto describe = [&schema] (const std::string& path)
    {
        const auto* leaf = schema.describe (path);
        REQUIRE (leaf != nullptr);

        std::vector<std::string> values;

        for (std::size_t i = 0; i < leaf->valueCount; ++i)
            values.emplace_back (leaf->values[i]);

        return values;
    };

    const auto warpNames = describe ("Oscillator{N}/WTOsc{N}/plainParams/kParamWarpMenu");

    REQUIRE (warpNames.size() == 52);

    for (const auto& name : warpNames)
    {
        if (xerum::dsp::warpModeFromName (name) == xerum::dsp::WarpMode::Unknown)
            continue;   // reported at high by `loadPreset`; not a choice, which is the point

        CHECK (choiceIndexForText (bridge.choices (ChoiceSet::WarpMode), ChoiceSet::WarpMode, name)
               != bridge.choices (ChoiceSet::WarpMode).sentinel);
    }

    const auto filterNames = describe ("VoiceFilter{N}/plainParams/kParamType");

    REQUIRE (filterNames.size() == 83);

    int implemented = 0;

    for (const auto& name : filterNames)
    {
        if (xerum::dsp::filterSpecFromName (name).model == xerum::dsp::FilterModel::Unimplemented)
            continue;

        ++implemented;
        CHECK (choiceIndexForText (bridge.choices (ChoiceSet::FilterType), ChoiceSet::FilterType, name)
               != bridge.choices (ChoiceSet::FilterType).sentinel);
    }

    // Ticket 26's measured coverage: 66 of the 83 names resolve to a built model.
    CHECK (implemented == 66);
}

TEST_CASE ("every parameter's value at construction is the value a document would resolve",
           "[plugin][bridge]")
{
    // The init patch, read back through the host-facing parameters. This is the check that would catch a
    // parameter whose constructor default and whose resolution rule disagree — the two must be the same
    // value, because a host that resets a parameter to its default must land where a preset that omits it
    // lands.
    XerumAudioProcessor processor;

    for (const auto& spec : processor.bridge().specs())
        CHECK (processor.parameterValue (spec.modelPath)
               == Catch::Approx (ParameterBridge::resolveAbsent (spec)));

    // Spot-checks with names, so a failure above says which control it was.
    CHECK (processor.parameterValue ("Oscillator0/WTOsc0/plainParams/kParamTablePos") == 1.0);
    CHECK (processor.parameterValue ("Oscillator0/plainParams/kParamUnison") == 1.0);
    CHECK (processor.parameterValue ("Global0/plainParams/kParamPolyCount") == 16.0);
}

TEST_CASE ("an absent parameter resolves through defaultFor, and nullopt falls through to xerum's init",
           "[plugin][defaults]")
{
    const auto& schema = xerum::model::Schema::instance();
    const ParameterBridge bridge;

    // The rule, asserted for every parameter: an established default wins, otherwise xerum's own init.
    for (const auto& spec : bridge.specs())
    {
        const auto resolved = ParameterBridge::resolveAbsent (spec);

        if (const auto established = schema.defaultFor (spec.schemaPath))
            CHECK (resolved == Catch::Approx (*established));
        else
            CHECK (resolved == Catch::Approx (spec.init));
    }

    // **nullopt is not zero.** `kParamVolume` has no established default on any machine — its entry in
    // ticket 38's table is normalised-only, which is not a preset-unit value — so it resolves to xerum's
    // init, which is the reference's own default patch's level and the parameter's host-facing default
    // too. Ticket `63`: the reference's init state omits this leaf and its own surface reports `A Level`
    // at 75%, i.e. a stored 0.75² = 0.5625.
    const auto* volume = findSpec (bridge, "Oscillator0/plainParams/kParamVolume");
    REQUIRE (volume != nullptr);
    CHECK_FALSE (schema.defaultFor (volume->schemaPath).has_value());
    CHECK (ParameterBridge::resolveAbsent (*volume) == 0.5625);
    CHECK (ParameterBridge::resolveAbsent (*volume) != 0.0);

    // The positive control that makes the assertion above mean something: a parameter whose init *is*
    // zero resolves to zero, so the check above is not satisfied by every parameter answering 1.0.
    const auto* glide = findSpec (bridge, "Global0/plainParams/kParamPortamentoTime");
    REQUIRE (glide != nullptr);
    CHECK (ParameterBridge::resolveAbsent (*glide) == 0.0);

    // Where the machine does establish a default, the plugin applies it rather than its own init — and
    // this is asserted conditionally because ticket 38's table is generated locally and is absent on CI.
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    if (const auto established = schema.defaultFor ("VoiceFilter{N}/plainParams/kParamFreq"))
    {
        xerum::model::PresetDocument document;   // mentions nothing, so every parameter resolves
        processor.applyDocument (document, {}, 0);

        CHECK (processor.parameterValue ("VoiceFilter0/plainParams/kParamFreq") == Catch::Approx (*established));
    }

    // A document's own value always wins over both.
    xerum::model::PresetDocument explicitValue;
    explicitValue.parameters["VoiceFilter0/plainParams/kParamFreq"] = number (0.25);
    processor.applyDocument (explicitValue, {}, 0);

    CHECK (processor.parameterValue ("VoiceFilter0/plainParams/kParamFreq") == Catch::Approx (0.25));
}
