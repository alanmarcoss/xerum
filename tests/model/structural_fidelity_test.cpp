// What a `PresetDocument` can and cannot represent — ticket 36's enumeration, as tests.
//
// The ticket asked for every structural fact the model cannot hold to be either represented or listed
// with the reason it is accepted. These are the four candidates it named, plus the one this work found:

#include <catch2/catch_test_macros.hpp>

#include "cbor.h"
#include "container.h"
#include "document.h"
#include "schema.h"
#include "write.h"

#include <string>

using namespace xerum::model;

namespace
{

Value text (std::string value)
{
    Value v;
    v.type = Value::Type::String;
    v.text = std::move (value);
    return v;
}

Value number (double value)
{
    Value v;
    v.type = Value::Type::Float;
    v.real = value;
    return v;
}

Value object (std::initializer_list<std::pair<std::string, Value>> members)
{
    Value v;
    v.type = Value::Type::Map;
    for (const auto& [key, value] : members)
        v.entries.emplace_back (key, value);
    return v;
}

LoadResult load (const Value& payload)
{
    return loadPreset (payload, "4.0", Schema::instance(), SupportCatalogue::v1());
}

bool wasReported (const LoadResult& result, std::string_view reasonFragment)
{
    for (const auto& entry : result.report.entries)
        if (entry.reason.find (reasonFragment) != std::string::npos)
            return true;

    return false;
}

/// Reload a document through the writer, which is the only way to see what a write would lose.
LoadResult roundTrip (const PresetDocument& document)
{
    const auto written = writeContainer (document, {});
    REQUIRE (written.ok);

    const auto container = readContainer (written.bytes);
    REQUIRE (container.ok);

    const auto payload = decompress (container.payload, container.codec, container.declaredSize);
    REQUIRE (payload.ok);

    const auto decoded = decodeCbor (payload.bytes);
    REQUIRE (decoded.ok);

    const auto result = loadPreset (decoded.value, container.schemaVersion, Schema::instance(),
                                    SupportCatalogue::v1());
    REQUIRE (result.ok);
    return result;
}

} // namespace

TEST_CASE ("a parameter outside the schema is reported and retained, so a write cannot lose it",
           "[structural]")
{
    // The one real loss this ticket's enumeration found. Reporting it satisfies "nothing is silently
    // dropped" for a load — but dropping it from the document means the preset writes back missing a
    // field the reference put there, which is losing data we were shown. Both, then: named in the
    // report *and* kept as an opaque value.
    const auto result = load (object ({ { "Oscillator0",
                                         object ({ { "WTOsc0",
                                                     object ({ { "plainParams",
                                                                 object ({ { "kParamVolume", number (0.5) },
                                                                           { "kParamFromTheFuture",
                                                                             number (0.25) } }) } }) } }) } }));

    REQUIRE (result.ok);

    CHECK (wasReported (result, "parameter is not in the schema"));

    const auto found = result.document.parameters.find ("Oscillator0/WTOsc0/plainParams/kParamFromTheFuture");
    REQUIRE (found != result.document.parameters.end());
    CHECK (found->second.real == 0.25);

    // And it survives the round trip, which is the claim that matters.
    const auto reread = roundTrip (result.document);

    REQUIRE (reread.document.parameters.find ("Oscillator0/WTOsc0/plainParams/kParamFromTheFuture")
             != reread.document.parameters.end());
    CHECK (reread.document.parameters.at ("Oscillator0/WTOsc0/plainParams/kParamFromTheFuture").real
           == 0.25);
}

TEST_CASE ("the default sentinel and an empty override map are equivalent, and both are accepted",
           "[structural]")
{
    // `plainParams` is either a sparse override map or the string `"default"`. A document cannot tell
    // them apart — both store nothing — and that is *accepted* rather than fixed, because they mean the
    // same thing: every parameter in the module sits at its schema default. The difference is a spelling,
    // not a state.
    const auto asSentinel = load (object ({ { "ModSlot0", object ({ { "plainParams", text ("default") } }) } }));
    const auto asEmpty = load (object ({ { "ModSlot0", object ({ { "plainParams", object ({}) } }) } }));

    REQUIRE (asSentinel.ok);
    REQUIRE (asEmpty.ok);

    CHECK (asSentinel.document.parameters.empty());
    CHECK (asEmpty.document.parameters.empty());

    // Neither is a complaint: the sentinel sits where a parameter map belongs, and reporting it would
    // fill the report with one bogus entry per unselected engine in every preset.
    CHECK_FALSE (wasReported (asSentinel, "parameter is not in the schema"));
}

TEST_CASE ("module presence with no parameters is not represented, and does not need to be", "[structural]")
{
    // A module can exist in the file with nothing in it. Absent and present-but-empty both mean "all
    // defaults", so the write emits nothing and the reference reads the same state. Accepted.
    const auto result = load (object ({ { "Env0", object ({ { "plainParams", text ("default") } }) },
                                        { "Env1", object ({}) } }));

    REQUIRE (result.ok);
    CHECK (result.document.parameters.empty());
    CHECK (result.report.worst() != Severity::High);
}

TEST_CASE ("a subtree nested deeper than the loader's limit is reported, not dropped in silence",
           "[structural]")
{
    // The loader refuses beyond 24 levels. It used to return without a word — the only silent drop left
    // in the whole loader, against a contract whose rule is that the set of silent drops is empty.
    // Dormant for the factory bank (deepest path: 8 levels) and live for anything deeper.
    Value deepest = object ({ { "leaf", number (1.0) } });

    for (int i = 0; i < 30; ++i)
        deepest = object ({ { "nested", std::move (deepest) } });

    const auto result = load (object ({ { "Flex0", std::move (deepest) } }));

    REQUIRE (result.ok);
    CHECK (wasReported (result, "nested deeper than 24 levels"));
}

TEST_CASE ("array length survives when the elements carry parameters, and not when they do not",
           "[structural]")
{
    // An array's length is implied by the highest index that stored something. A trailing element with
    // no parameters of its own leaves nothing behind, so the written array is shorter. Accepted: an
    // absent element and an empty one are the same state to a reader, and the reference's own reader
    // treats a missing index as default.
    const auto result = load (object ({ { "ArpClip0",
                                         object ({ { "clip",
                                                     object ({ { "notes",
                                                                 Value { [] {
                                                                             Value array;
                                                                             array.type = Value::Type::Array;
                                                                             array.items.push_back (object ({ { "channel", number (3.0) } }));
                                                                             array.items.push_back (object ({ { "strumType", text ("kEven") } }));
                                                                             return array;
                                                                         }() } } }) } }) } }));

    REQUIRE (result.ok);

    CHECK (result.document.parameters.count ("ArpClip0/clip/notes[0]/channel") == 1);
    CHECK (result.document.parameters.count ("ArpClip0/clip/notes[1]/strumType") == 1);

    const auto reread = roundTrip (result.document);

    // The elements that held a parameter come back, at the same indices — which is what makes an array's
    // *content* addressable even though its length is not stored.
    CHECK (reread.document.parameters.count ("ArpClip0/clip/notes[0]/channel") == 1);
    CHECK (reread.document.parameters.count ("ArpClip0/clip/notes[1]/strumType") == 1);
}

TEST_CASE ("a preset asking for spectral morphing after load is reported, not silently crossfaded",
           "[structural]")
{
    // Ticket 37 measured what the `clm` interpolation modes do, and it is not what the guide says: modes
    // 2/3/4 are a **plain linear crossfade**, which is what xerum already does. The real spectral morph
    // — magnitude interpolation with shortest-arc phase, on the embedded-table path — is asked for by a
    // *preset parameter*, `interpolateAfterLoad == 2`, and exactly one factory preset sets it.
    //
    // That value is an ordinary schema leaf, so it loads quietly and nothing acts on it. Silent it is
    // not allowed to be: the preset's sound is materially wrong, which is `high`.
    Value integer;
    integer.type = Value::Type::Int;
    integer.integer = 2;

    const auto result = load (object ({ { "Oscillator0",
                                         object ({ { "WTOsc0",
                                                     object ({ { "plainParams", object ({}) },
                                                               { "interpolateAfterLoad", integer } }) } }) } }));

    REQUIRE (result.ok);
    CHECK (wasReported (result, "spectral morphing after load"));
    CHECK (result.report.worst() == Severity::High);

    // And the neighbouring values are not complaints: 0 and 1 are the modes we do implement.
    for (const auto value : { 0LL, 1LL })
    {
        Value ok;
        ok.type = Value::Type::Int;
        ok.integer = value;

        const auto plain = load (object ({ { "Oscillator0",
                                             object ({ { "WTOsc0",
                                                         object ({ { "plainParams", object ({}) },
                                                                   { "interpolateAfterLoad", ok } }) } }) } }));

        CHECK_FALSE (wasReported (plain, "spectral morphing after load"));
    }
}
