// Tests for the parameter schema.
//
// These assert that the generated table is the shape the model claims it is. They deliberately do
// NOT assert that any parameter has a usable default: none does yet, and the point of the
// `Unestablished` provenance is that this is visible rather than papered over with zero.

#include <catch2/catch_test_macros.hpp>

#include "generated/schema_data.h"

#include "schema.h"

using namespace xerum::model;

TEST_CASE ("the schema covers the whole decoded model", "[schema]")
{
    const auto& schema = Schema::instance();

    // The corpus-wide counts from ticket 01. If these move, the schema was regenerated from a
    // different corpus and every downstream assumption needs re-checking.
    // 776, not 778: canonicalising bracket indices to `[]` merged `lfo[0][]`/`lfo[1][]` and
    // `embeddedIR[0][]`/`embeddedIR[1][]` into two rows each. They were always the same parameter
    // addressed two ways — `normalise` collapses every bracket index — and keeping both spellings meant
    // neither could be looked up. See the generator's merge step.
    REQUIRE (schema.leafCount() == 776);
    // 48, not 46: the generator now unions the taxonomy with every indexed segment the leaf paths use,
    // because the taxonomy alone omitted `NoiseOsc{N}` — which normalisation needs in order to tell an
    // instance index from a literal digit in a parameter name.
    REQUIRE (schema.modulePatternCount() == 48);

    REQUIRE (Schema::versions() == std::vector<std::string> { "3.0", "4.0", "5.0", "6.0" });
}

TEST_CASE ("a known parameter describes itself", "[schema]")
{
    const auto& schema = Schema::instance();

    const auto* attack = schema.describe ("Env{N}/plainParams/kParamAttack");

    REQUIRE (attack != nullptr);
    CHECK (attack->kind == LeafKind::Float);
    CHECK (attack->hasRange);
    CHECK (attack->min == 0.0);
    CHECK (attack->max > 9.0);

    // A bipolar parameter whose range crosses zero, which a naive `min >= 0` assumption would wreck.
    const auto* amount = schema.describe ("ModSlot{N}/plainParams/kParamAmount");

    REQUIRE (amount != nullptr);
    CHECK (amount->min == -100.0);
    CHECK (amount->max == 100.0);
}

TEST_CASE ("string parameters carry their vocabulary", "[schema]")
{
    const auto& schema = Schema::instance();

    const auto* types = schema.describe ("VoiceFilter{N}/plainParams/kParamType");

    REQUIRE (types != nullptr);
    CHECK (types->kind == LeafKind::String);
    REQUIRE (types->values != nullptr);
    CHECK (types->valueCount == 83);

    bool hasLadder = false;
    for (std::size_t i = 0; i < types->valueCount; ++i)
        if (types->values[i] == "LadderEMS")
            hasLadder = true;

    CHECK (hasLadder);
}

TEST_CASE ("an unknown path is unknown, not an error", "[schema]")
{
    const auto& schema = Schema::instance();

    // An unrecognised parameter is an ordinary outcome — the import report exists to describe it.
    // Anything that threw here would turn a strange preset into a crash.
    CHECK (schema.describe ("NoSuchModule/whatever") == nullptr);
    CHECK (schema.describe ("") == nullptr);
    CHECK (schema.describe ("Oscillator{N}/plainParams/kParamNotAThing") == nullptr);
}

TEST_CASE ("concrete paths normalise to schema paths", "[schema]")
{
    CHECK (Schema::instance().normalise ("/Oscillator0/WTOsc1/plainParams/kParamWarpMenu")
           == "Oscillator{N}/WTOsc{N}/plainParams/kParamWarpMenu");

    CHECK (Schema::instance().normalise ("Oscillator0/WTOsc0/plainParams/kParamWarpMenu")
           == "Oscillator{N}/WTOsc{N}/plainParams/kParamWarpMenu");

    CHECK (Schema::instance().normalise ("FXRack0/FX[2]/FXReverb/plainParams/kParamWet")
           == "FXRack{N}/FX[]/FXReverb/plainParams/kParamWet");

    // A bare number is data, not an index: only a digit run following a name becomes {N}.
    CHECK (Schema::instance().normalise ("Global0/plainParams/123") == "Global{N}/plainParams/123");
}

TEST_CASE ("the module taxonomy recognises indexed families", "[schema]")
{
    const auto& schema = Schema::instance();

    CHECK (schema.recognisesModule ("ModSlot37"));
    CHECK (schema.recognisesModule ("Env1"));
    CHECK (schema.recognisesModule ("LFOPointModBus15"));
    CHECK (schema.recognisesModule ("Filter"));       // no index at all
    CHECK (schema.recognisesModule ("WTOsc2"));

    CHECK_FALSE (schema.recognisesModule ("NotAModule"));
    CHECK_FALSE (schema.recognisesModule (""));
    CHECK_FALSE (schema.recognisesModule ("Env"));    // the pattern demands an index
}

TEST_CASE ("version sets say what each schema version contains", "[schema]")
{
    const auto& schema = Schema::instance();

    // Version 3.0 predates the granular and spectral engines, which is why it is the small one.
    // Assert the specific cases that carry meaning rather than an aggregate count, which would
    // simply restate the shape of the table.
    const auto* channel = schema.describe ("ArpClip{N}/clip/notes[]/channel");

    REQUIRE (channel != nullptr);
    CHECK (schema.presentIn (*channel, "3.0"));
    CHECK (schema.presentIn (*channel, "5.0"));
    CHECK_FALSE (schema.presentIn (*channel, "6.0"));

    const auto* attack = schema.describe ("Env{N}/plainParams/kParamAttack");
    REQUIRE (attack != nullptr);
    for (const auto& v : Schema::versions())
        CHECK (schema.presentIn (*attack, v));

    CHECK_FALSE (schema.presentIn (*attack, "9.9"));   // an unknown version is not present
}

TEST_CASE ("no parameter has a default yet, and none pretends to", "[schema][defaults]")
{
    const auto& schema = Schema::instance();

    // This is the honest state of the defaults table, asserted so that it cannot quietly change.
    // Ticket 21's remaining work is to author defaults and validate them behaviourally; until that
    // happens, `defaultFor` must refuse rather than return a plausible-looking zero.
    for (const auto* path : { "Env{N}/plainParams/kParamAttack",
                              "ModSlot{N}/plainParams/kParamAmount",
                              "VoiceFilter{N}/plainParams/kParamType" })
    {
        const auto* leaf = schema.describe (path);
        REQUIRE (leaf != nullptr);
        CHECK (leaf->defaultProvenance == DefaultProvenance::Unestablished);
        CHECK_FALSE (schema.defaultFor (path).has_value());
    }

    // And an unknown path is also nullopt — the caller distinguishes the cases with `describe`.
    CHECK_FALSE (schema.defaultFor ("NoSuchModule/whatever").has_value());
}

TEST_CASE ("enum sets and their counts agree", "[schema]")
{
    const auto& schema = Schema::instance();

    for (const auto* path : { "VoiceFilter{N}/plainParams/kParamType",
                              "Oscillator{N}/WTOsc{N}/plainParams/kParamWarpMenu" })
    {
        const auto* leaf = schema.describe (path);
        REQUIRE (leaf != nullptr);
        CHECK (leaf->values != nullptr);
        CHECK (leaf->valueCount > 0);

        for (std::size_t i = 0; i < leaf->valueCount; ++i)
        {
            CHECK_FALSE (leaf->values[i].empty());
            // An enumeration is vocabulary. Anything with a newline in it is payload, and payload
            // in this table would mean reference content had leaked into a committed file.
            CHECK (leaf->values[i].find ('\n') == std::string_view::npos);
            CHECK (leaf->values[i].size() < 64);
        }
    }

    // A numeric leaf has no vocabulary.
    const auto* num = schema.describe ("Env{N}/plainParams/kParamAttack");
    REQUIRE (num != nullptr);
    CHECK (num->values == nullptr);
    CHECK (num->valueCount == 0);
}

TEST_CASE ("a name-keyed parent normalises to {name}, so multisample metadata resolves", "[schema]")
{
    // The multisample engine's `files` map is keyed by sample file name — not an index, not a parameter
    // name. Nothing about the segment itself says so: a file called `SuperJX 4 Osc Chorus Pad G6.flac`
    // looks like any other path segment. The schema's own `{name}` rows are the only evidence, which is
    // why normalisation has to consult them.
    //
    // Getting this wrong is not cosmetic: it made every multisample preset in the bank report one
    // "parameter is not in the schema" entry per sample-metadata field — 12,071 distinct paths and
    // 28,700 entries — while the schema covered every one of them.
    const auto& schema = Schema::instance();
    const auto concrete = "Oscillator0/MultiSampleOsc2/files/SuperJX 4 Osc Chorus Pad G6.flac/numFrames";

    CHECK (schema.normalise (concrete)
           == "Oscillator{N}/MultiSampleOsc{N}/files/{name}/numFrames");

    const auto* leaf = schema.describe (schema.normalise (concrete));
    REQUIRE (leaf != nullptr);
    CHECK (leaf->kind == LeafKind::Int);
}

TEST_CASE ("the generated table holds one canonical spelling per parameter", "[schema]")
{
    // The invariant that would have caught both real bugs in this table at once. `normalise` produces
    // exactly one spelling of a path; the table must hold that spelling. A row spelled `lfo[0][]` can
    // never be looked up, because no concrete path normalises to it — so the row is dead and the
    // presets that need it report a parameter the schema covers.
    //
    // Sorted-and-unique is half of the same invariant: `describe` binary-searches, so a duplicate row
    // makes the lookup's result depend on which of the two the search happens to land on.
    std::string_view previous;
    std::size_t rows = 0;

    for (const auto* leaf = generated::kLeaves; leaf != generated::kLeaves + generated::kLeafCount; ++leaf)
    {
        const std::string_view path = leaf->path;
        ++rows;

        // Every bracketed index is empty: `[]`, never `[0]`.
        for (auto open = path.find ('['); open != std::string_view::npos; open = path.find ('[', open + 1))
        {
            const auto close = path.find (']', open);
            REQUIRE (close != std::string_view::npos);
            CHECK (close == open + 1);
        }

        if (! previous.empty())
            CHECK (path > previous);

        previous = path;
    }

    CHECK (rows == Schema::instance().leafCount());
    CHECK (rows > 700);
}
