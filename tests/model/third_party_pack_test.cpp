// Third-party pack robustness — ticket 42, at the loader's boundary.
//
// Every corpus number in this project comes from the 626 factory presets, which are internally
// consistent. A third-party pack is exactly the input that breaks that consistency: asset paths spelled
// with the wrong case, table references that name a file nobody shipped, schema versions from a release
// that does not exist yet, and parameters the schema has never heard of. The map's v1 acceptance ends
// *"import survives third-party packs"*, and until this file that claim rested on nothing.
//
// The trees here are built by hand and handed straight to `loadPreset`: the question is what the
// *importer* does with a shape the factory bank never produces, not whether a bytes-on-disk fixture
// parses. There is no reference content in anything this file constructs (ticket 10); where a test needs
// a real file it resolves one of ours by path and skips loudly when it is absent.
//
// The asset cases reach the filesystem, which the model layer deliberately does not — those live in the
// plugin's suite (`plugin/tests/third_party_test.cpp`), where resolution, the report and a render can all
// be observed together.

#include <catch2/catch_test_macros.hpp>

#include "document.h"
#include "schema.h"

#include <algorithm>
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

/// A wavetable slot selecting a shipped engine, with one real parameter and an asset reference.
Value wavetableSlot (const std::string& assetReference)
{
    return object ({ { "WTOsc0", object ({ { "plainParams",
                        object ({ { "kParamWarpMenu", text ("kPD_OSC") } }) },
                     { "relativePathToWT", text (assetReference) } }) },
                     { "GranularOsc0", object ({ { "plainParams", text ("default") } }) },
                     { "SpectralOsc0", object ({ { "plainParams", text ("default") } }) },
                     { "MultiSampleOsc0", object ({ { "plainParams", text ("default") } }) },
                     { "SampleOsc0", object ({ { "plainParams", text ("default") } }) } });
}

/// A wavetable slot whose `plainParams` hold exactly the members given.
Value wavetableWith (std::initializer_list<std::pair<std::string, Value>> params)
{
    return object ({ { "WTOsc0", object ({ { "plainParams", object (params) } }) } });
}

LoadResult load (const Value& payload, std::string_view version = "4.0")
{
    return loadPreset (payload, version, Schema::instance(), SupportCatalogue::v1());
}

/// The first entry whose item is exactly `item`, or nullptr.
const ReportEntry* entryFor (const LoadResult& result, std::string_view item)
{
    for (const auto& entry : result.report.entries)
        if (entry.item == item)
            return &entry;

    return nullptr;
}

const SupportCatalogue& catalogue() { return SupportCatalogue::v1(); }

} // namespace

TEST_CASE ("a schema version newer than anything we have seen imports with a report, not a refusal",
           "[third-party]")
{
    // 6.0 is the newest version the catalogue accepts. A third-party pack written by a reference a few
    // releases ahead carries something above it, and a loader that refuses it is useless against the
    // future — while one that accepts it silently hides the gap. Both are forbidden; the report is the
    // intended third option.
    REQUIRE (catalogue().schemaVersions.back() == "6.0");

    const auto future = load (object ({ { "Oscillator0", wavetableSlot ("/third-party/Basic Shapes.wav") } }),
                              "9.9");

    REQUIRE (future.ok);                                   // parsed, not refused
    CHECK (future.document.parameterCount() > 0);          // and the payload really was read

    const auto* version = entryFor (future, "9.9");
    REQUIRE (version != nullptr);                          // named, by the exact version string
    CHECK (version->severity == Severity::Medium);
    CHECK (version->modulePath == "preset");
    CHECK (version->reason.find ("not one of the ones we recognise") != std::string::npos);

    // The newest version we do know stays clean: the report is about the *unknown* version, not about
    // version reporting existing at all.
    CHECK (load (object ({ { "Oscillator0", wavetableSlot ("/third-party/Basic Shapes.wav") } }), "6.0")
               .report.clean());
}

TEST_CASE ("a parameter the schema does not describe is reported by name and retained",
           "[third-party]")
{
    // Ticket 36/21 made retention the rule: reporting alone satisfies "nothing is silently dropped" for
    // a *load*, but dropping the value would make the *write* lose a field the reference put there. A
    // third-party pack from a newer reference is full of these.
    const auto result = load (object ({ { "Oscillator0", wavetableWith ({
        { "kParamVolume", number (0.5) },
        { "kParamFromTheFuture", number (0.25) } }) } }));

    REQUIRE (result.ok);

    const auto normalised = "Oscillator{N}/WTOsc{N}/plainParams/kParamFromTheFuture";
    const auto* reported = entryFor (result, normalised);
    REQUIRE (reported != nullptr);
    CHECK (reported->severity == Severity::Medium);
    CHECK (reported->reason.find ("not in the schema") != std::string::npos);

    // Retained under its **concrete** path, at its value — so a write reproduces what the reference put
    // there even though we did not interpret it.
    const auto found = result.document.parameters.find ("Oscillator0/WTOsc0/plainParams/kParamFromTheFuture");
    REQUIRE (found != result.document.parameters.end());
    CHECK (found->second.real == 0.25);
}

TEST_CASE ("a case-mismatched asset reference survives the loader verbatim", "[third-party]")
{
    // The model layer does not touch the filesystem, so it cannot resolve — but it must not *normalise*
    // either: the resolver compares the reference's spelling against the directory entries to tell an
    // exact hit from a lucky one (ticket 24), and a loader that lower-cased the reference would destroy
    // the evidence it needs. This is the model's half of the case-mismatch contract.
    const auto result = load (object ({ { "Oscillator0", wavetableSlot ("/Third-Party/Basic Shapes.WAV") } }));

    REQUIRE (result.ok);
    REQUIRE (result.document.assetReferences.size() == 1);
    CHECK (result.document.assetReferences.front() == "/Third-Party/Basic Shapes.WAV");
}

TEST_CASE ("a third-party-shaped preset imports as a whole, and everything it asks for is named",
           "[third-party]")
{
    // One preset that combines the failure modes the factory bank never produces — the shape the map's
    // acceptance actually means. It must still import (contract §1), produce a document, and name every
    // item it could not honour (contract §2).
    const auto payload = object ({
        { "Oscillator0", object ({
            { "WTOsc0", object ({ { "plainParams",
                object ({ { "kParamVolume", number (0.8) },
                          { "kParamWarpMenu", text ("kSomethingNew") },
                          { "kParamFromTheFuture", number (3.0) } }) },
                { "relativePathToWT", text ("/Third-Party/basic shapes.WAV") } }) },
            // The engines a pack leaves as the "default" sentinel when a slot is unselected.
            { "SpectralOsc0", object ({ { "plainParams", text ("default") } }) },
            { "GranularOsc0", object ({ { "plainParams", text ("default") } }) },
            { "MultiSampleOsc0", object ({ { "plainParams", text ("default") } }) },
            { "SampleOsc0", object ({ { "plainParams", text ("default") } }) } }) },
        // A module from a newer reference.
        { "ModuleFromTheFuture", object ({ { "plainParams", object ({}) } }) } });

    const auto result = load (payload, "7.4");

    REQUIRE (result.ok);
    CHECK (result.document.parameterCount() > 0);
    CHECK (result.report.worst() == Severity::High);

    // Each item is named, by its own spelling: the future version, the future module, the future
    // parameter (under its normalised path), and the warp we do not ship.
    CHECK (entryFor (result, "7.4") != nullptr);
    CHECK (entryFor (result, "ModuleFromTheFuture") != nullptr);
    CHECK (entryFor (result, "Oscillator{N}/WTOsc{N}/plainParams/kParamFromTheFuture") != nullptr);
    CHECK (entryFor (result, "kSomethingNew") != nullptr);

    // And the parameter it did not interpret is still there to be written back.
    CHECK (result.document.parameters.count ("Oscillator0/WTOsc0/plainParams/kParamFromTheFuture") == 1);
    CHECK (result.document.assetReferences.size() == 1);
}
