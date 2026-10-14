// Ticket 40's vocabulary cross-check: the model's own enums against the two engines.
//
// The dsp layer links no model and the model links no dsp, so a vocabulary that disagrees is invisible
// from either side alone. This file is where the two are held together for the two fixed slots, in the
// same spirit as `warp_coverage_test.cpp`:
//
//   * every `kParamNoiseType` value the reference's parameter model knows must map onto a colour the
//     NOISE engine implements — a name with no engine behind it is a silent drop;
//   * the same for `kParamShape` on the SUB, **including the sixth name the schema does not carry**:
//     the reference's own init state says the control has six steps and its default is `Sine`, and the
//     schema's five values are the ones the corpus stores. Both halves are asserted, so the finding
//     cannot quietly stop being true;
//   * neither slot offers a warp menu, which is why there is no warp to implement on slots 3 and 4
//     (ticket 25's measurement, restated against the schema as ticket 40's acceptance asks).

#include <catch2/catch_test_macros.hpp>

#include "schema.h"

#include "noise_osc.h"
#include "sub_osc.h"

#include <set>
#include <string>
#include <vector>

using namespace xerum::model;

namespace
{

/// Every value the schema allows on a string leaf, or empty when the leaf is not a menu.
///
/// `Schema::describe` answers for a **normalised** path, so a concrete one (`Oscillator4/SubOsc4/…`) is
/// normalised first — otherwise it answers "no such leaf" for every concrete path, which is how the
/// first draft of this file passed an absence check vacuously.
std::vector<std::string> vocabulary (std::string_view concreteOrSchemaPath)
{
    std::vector<std::string> names;

    const auto* leaf = Schema::instance().describe (
        Schema::instance().normalise (concreteOrSchemaPath));

    if (leaf == nullptr || leaf->kind != LeafKind::String || leaf->values == nullptr)
        return names;

    for (std::size_t i = 0; i < leaf->valueCount; ++i)
        names.emplace_back (leaf->values[i]);

    return names;
}

bool describes (std::string_view concreteOrSchemaPath)
{
    return Schema::instance().describe (Schema::instance().normalise (concreteOrSchemaPath)) != nullptr;
}

} // namespace

TEST_CASE ("every kParamNoiseType value the schema carries is a colour the engine implements",
           "[noise][model]")
{
    const auto names = vocabulary ("Oscillator{N}/NoiseOsc{N}/plainParams/kParamNoiseType");

    // The vocabulary is exactly four, and the manual's table names the same four.
    REQUIRE (names.size() == 4);
    CHECK (std::set<std::string> (names.begin(), names.end())
           == std::set<std::string> { "Brown", "Geiger", "Pink", "White" });

    for (const auto& name : names)
    {
        INFO (name);
        REQUIRE (xerum::dsp::noiseTypeFromName (name).has_value());
    }

    // And nothing is implemented that the reference does not offer, which is the other direction of the
    // same claim: a colour xerum invented would be a substitution nobody asked for.
    for (const auto colour : { xerum::dsp::NoiseType::White, xerum::dsp::NoiseType::Pink,
                               xerum::dsp::NoiseType::Brown, xerum::dsp::NoiseType::Geiger })
        CHECK (std::find (names.begin(), names.end(), std::string (xerum::dsp::noiseTypeName (colour)))
               != names.end());
}

TEST_CASE ("the SUB's shape vocabulary is the schema's five plus the default the schema never stores",
           "[sub][model]")
{
    const std::string path = "Oscillator{N}/SubOsc4/plainParams/kParamShape";
    const auto names = vocabulary (path);

    // Five, because the writer omits a value equal to the default — the same rule every other sparse
    // leaf follows. The reference's own init state gives the sixth: `Sub Shape … 6 steps`, default
    // `Sine` (`xerum-host --dump-params` against Serum 2).
    REQUIRE (names.size() == 5);
    CHECK (std::set<std::string> (names.begin(), names.end())
           == std::set<std::string> { "kPulse", "kRoundRect", "kSaw", "kSquare", "kTriangle" });

    for (const auto& name : names)
    {
        INFO (name);
        REQUIRE (xerum::dsp::subShapeFromName (name).has_value());
    }

    // The sixth is implemented and is *not* in the schema's vocabulary, which is the finding rather than
    // a discrepancy: `Sine` is the default, so the corpus never had to store it.
    REQUIRE (xerum::dsp::subShapeFromName ("Sine").has_value());
    CHECK (std::find (names.begin(), names.end(), "Sine") == names.end());

    // Every implemented shape is either a stored name or that default — nothing invented.
    for (const auto shape : { xerum::dsp::SubShape::Sine, xerum::dsp::SubShape::RoundRect,
                              xerum::dsp::SubShape::Triangle, xerum::dsp::SubShape::Saw,
                              xerum::dsp::SubShape::Square, xerum::dsp::SubShape::Pulse })
    {
        const std::string token (xerum::dsp::subShapeName (shape));
        INFO (token);
        CHECK ((token == "Sine" || std::find (names.begin(), names.end(), token) != names.end()));
    }
}

TEST_CASE ("the reference's recorded default for the SUB's shape is Sine, when that table is available",
           "[sub][model]")
{
    // `generated/defaults_data.h` is derived from the reference instrument and is regenerated locally
    // rather than committed (tickets 10/38), so this clause is skipped loudly where it is absent — a skip
    // must never be able to look like a pass.
    const auto recorded = Schema::instance().defaultStringFor ("Oscillator{N}/SubOsc4/plainParams/kParamShape");

    if (recorded.empty())
        SKIP ("no defaults table in this build (ticket 38's generated data is local-only)");

    CHECK (recorded == "Sine");
}

TEST_CASE ("neither fixed slot offers a warp menu, which is why neither has a warp to implement",
           "[noise][sub][model]")
{
    // Ticket 25 measured this from the reference's parameter model; ticket 40's acceptance asks about the
    // warps on slots 3/4, and the answer is that the reference offers none there. Asserted rather than
    // assumed, and asserted as an absence: the *slot's* own parameter map is where a menu would live.
    for (const auto* slot : { "Oscillator3/NoiseOsc3", "Oscillator4/SubOsc4" })
        for (const auto* leaf : { "plainParams/kParamWarpMenu", "plainParams/kParamWarpMenu2",
                                  "plainParams/kParamWarp", "plainParams/kParamWarp2" })
        {
            INFO (slot << "/" << leaf);
            CHECK_FALSE (describes (std::string (slot) + "/" + leaf));
        }

    // The engine side of the same claim: the two slots' own settings carry no warp field at all, so
    // there is nothing that *could* be silently left unimplemented. The check is that the leaves a
    // preset stores for them are the ones the engine reads.
    for (const auto* leaf : { "Oscillator3/NoiseOsc3/plainParams/kParamColor",
                              "Oscillator3/NoiseOsc3/plainParams/kParamOneShot",
                              "Oscillator4/SubOsc4/plainParams/kParamContiguousPhase",
                              "Oscillator4/SubOsc4/plainParams/kParamInitialPhase" })
    {
        INFO (leaf);
        CHECK (describes (leaf));
    }
}
