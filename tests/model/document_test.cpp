// Tests for the preset document and its import report.
//
// These are the import contract's clauses as assertions. The trees are built directly rather than encoded
// to CBOR, because the question here is what the contract does with a given shape, not whether the wire
// format parses — that is cbor_test.cpp and legacy_test.cpp.

#include <catch2/catch_test_macros.hpp>

#include "document.h"

#include "cbor.h"
#include "container.h"
#include "decompress.h"
#include "legacy.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

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

Value integer (std::int64_t value)
{
    Value v;
    v.type = Value::Type::Int;
    v.integer = value;
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

Value list (std::initializer_list<Value> items)
{
    Value v;
    v.type = Value::Type::Array;
    v.items.assign (items.begin(), items.end());
    return v;
}

/// A wavetable slot selecting a shipped engine, with one real parameter.
Value wavetableSlot (const std::string& engine = "WTOsc", const std::string& warp = "kPD_OSC")
{
    return object ({ { engine + "0", object ({ { "plainParams",
                        object ({ { "kParamWarpMenu", text (warp) } }) } }) },
                     { "GranularOsc0", object ({ { "plainParams", text ("default") } }) },
                     { "SpectralOsc0", object ({ { "plainParams", text ("default") } }) },
                     { "MultiSampleOsc0", object ({ { "plainParams", text ("default") } }) },
                     { "SampleOsc0", object ({ { "plainParams", text ("default") } }) } });
}

const Schema& schema() { return Schema::instance(); }
const SupportCatalogue& catalogue() { return SupportCatalogue::v1(); }

LoadResult load (const Value& payload, std::string_view version = "4.0")
{
    return loadPreset (payload, version, schema(), catalogue());
}

} // namespace

TEST_CASE ("a preset we can fully reproduce imports with an empty report", "[document]")
{
    // "Empty, not absent": a clean import still produces a report, and it is the caller's evidence that
    // nothing was dropped because there was nothing to drop.
    const auto result = load (object ({ { "Oscillator0", wavetableSlot() } }));

    REQUIRE (result.ok);
    CHECK (result.report.clean());
    CHECK (result.report.entries.empty());
    CHECK (result.document.parameterCount() > 0);
    CHECK (result.document.schemaVersion == "4.0");
}

TEST_CASE ("the default sentinel contributes nothing and complains about nothing", "[document]")
{
    // Every unselected engine carries it. If it were treated as a parameter, every preset would produce
    // one bogus "unknown parameter" entry per unselected engine.
    const auto result = load (object ({ { "Oscillator0", wavetableSlot() } }));

    REQUIRE (result.ok);
    CHECK (result.report.clean());
    CHECK (result.document.parameters.find ("Oscillator{N}/GranularOsc{N}/plainParams") == result.document.parameters.end());
}

TEST_CASE ("an engine we cannot synthesise is reported at high severity", "[document]")
{
    const auto result = load (object ({ { "Oscillator0", wavetableSlot ("SpectralOsc") } }));

    REQUIRE (result.ok);
    CHECK (result.report.count (Severity::High) == 1);
    CHECK (result.report.worst() == Severity::High);

    const auto& entry = result.report.entries.front();
    CHECK (entry.item == "SpectralOsc");
    CHECK (entry.modulePath == "Oscillator0");
    CHECK (entry.reason.find ("cannot synthesise") != std::string::npos);
}

TEST_CASE ("a warp family we do not ship is named in the report", "[document]")
{
    const auto shipped = load (object ({ { "Oscillator0", wavetableSlot ("WTOsc", "kSync") } }));
    CHECK (shipped.report.clean());

    const auto missing = load (object ({ { "Oscillator0", wavetableSlot ("WTOsc", "kSomethingNew") } }));
    REQUIRE (missing.ok);
    CHECK (missing.report.count (Severity::High) == 1);
    CHECK (missing.report.entries.front().item == "kSomethingNew");
}

TEST_CASE ("an unknown module or parameter is reported, and the rest still imports", "[document]")
{
    const auto result = load (object ({ { "Oscillator0", wavetableSlot() },
                                        { "ModuleFromTheFuture", object ({ { "plainParams", object ({}) } }) } }));

    REQUIRE (result.ok);
    CHECK (result.report.count (Severity::Medium) >= 1);

    bool namedTheModule = false;
    for (const auto& entry : result.report.entries)
        if (entry.item == "ModuleFromTheFuture")
            namedTheModule = true;

    CHECK (namedTheModule);

    // We understood the rest: an unrecognised module does not poison the whole document.
    CHECK (result.document.parameterCount() > 0);
}

TEST_CASE ("schema versions are accepted or reported, never refused", "[document]")
{
    SECTION ("a version we know")
    {
        const auto result = load (object ({ { "Oscillator0", wavetableSlot() } }), "6.0");
        CHECK (result.report.clean());
        CHECK (result.document.schemaVersion == "6.0");
    }

    SECTION ("a version from the future is parsed with a report")
    {
        const auto result = load (object ({ { "Oscillator0", wavetableSlot() } }), "7.1");
        REQUIRE (result.ok);                       // parsed, not refused
        CHECK (result.report.count (Severity::Medium) == 1);
        CHECK (result.document.parameterCount() > 0);
    }

    SECTION ("no version at all is reported rather than assumed")
    {
        const auto result = load (object ({ { "Oscillator0", wavetableSlot() } }), "");
        REQUIRE (result.ok);
        CHECK (result.report.count (Severity::Medium) == 1);
    }
}

TEST_CASE ("asset references are collected, not resolved", "[document]")
{
    // Resolution needs a filesystem, which the model layer deliberately does not touch. Collecting the
    // references here is what lets the caller resolve them where it knows what is installed.
    const auto result = load (object ({ { "Oscillator0", object ({
        { "WTOsc0", object ({ { "plainParams", object ({ { "kParamWarpMenu", text ("kPD_OSC") } }) },
                              { "relativePathToWT", text ("/Analog/Basic Shapes.wav") } }) } }) } }));

    REQUIRE (result.ok);
    REQUIRE (result.document.assetReferences.size() == 1);
    CHECK (result.document.assetReferences.front() == "/Analog/Basic Shapes.wav");
}

TEST_CASE ("effects are reported by class, and the unused ones are deferred", "[document]")
{
    const auto rack = [] (const std::string& className)
    {
        return object ({ { "FX", list ({ object ({ { className, object ({}) } }) }) } });
    };

    SECTION ("a shipped effect is silent")
    {
        const auto result = load (object ({ { "FXRack0", rack ("FXReverb") } }));
        CHECK (result.report.count (Severity::High) == 0);
    }

    SECTION ("the names are the ones the corpus actually uses")
    {
        // `FXConv`, `FXUtils` and `FXHyperD` — not the `FXConvolve`/`FXUtility`/`FXHyper` this layer
        // first guessed at, and which no preset contains. A guessed name produces a report entry for a
        // file that is perfectly ordinary.
        for (const auto* className : { "FXConv", "FXUtils", "FXHyperD", "FXSplit", "FXSplit3", "FXSplitMS" })
        {
            const auto result = load (object ({ { "FXRack0", rack (className) } }));
            CHECK (result.report.count (Severity::Medium) == 0);
        }
    }

    SECTION ("an effect class we have never seen is reported, not assumed")
    {
        const auto result = load (object ({ { "FXRack0", rack ("FXFromTheFuture") } }));
        CHECK (result.report.count (Severity::Medium) == 1);
    }
}

TEST_CASE ("the same complaint is not repeated for every instance", "[document]")
{
    // Sixteen LFOs missing the same parameter is one fact about the schema, not sixteen.
    const auto result = load (object ({ { "Oscillator0", wavetableSlot ("WTOsc", "kSomethingNew") },
                                        { "Oscillator1", wavetableSlot ("WTOsc", "kSomethingNew") } }));

    REQUIRE (result.ok);
    CHECK (result.report.count (Severity::High) == 1);
}

TEST_CASE ("loading a preset never merges onto what was loaded before", "[document]")
{
    // Absence means default, so merging would leave the previous preset's values in every parameter the
    // new one does not mention — producing a preset no author ever wrote.
    const auto first = load (object ({ { "Oscillator0", wavetableSlot() } })).document;
    const auto second = load (object ({ { "Env0", object ({ { "plainParams", object ({ { "kParamAttack", number (0.5) } }) } }) } })).document;

    REQUIRE (first.parameterCount() > 0);
    REQUIRE (second.parameterCount() > 0);

    const auto applied = neverMerges (first, second);

    CHECK (applied.parameterCount() == second.parameterCount());
    CHECK (applied.parameters == second.parameters);

    for (const auto& [path, value] : applied.parameters)
        CHECK (first.parameters.find (path) == first.parameters.end());
}

TEST_CASE ("a real factory preset imports without a flood of false complaints", "[document][corpus]")
{
    // The tests above build trees by hand, and a hand-built tree can agree with a bug. This one reads a
    // real preset end to end — container, decompression, CBOR, document — and asserts the report is
    // *quiet*. That is precisely what a mistaken path breaks: dropping `plainParams` from a path makes
    // every parameter in the file resolve to nothing and produces one complaint per parameter.
    const std::filesystem::path file =
        "/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Presets/Factory/Bass/808/808 - Straight.SerumPreset";

    if (! std::filesystem::exists (file))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    std::ifstream in (file, std::ios::binary);
    std::vector<std::byte> raw ((std::size_t) std::filesystem::file_size (file));
    in.read (reinterpret_cast<char*> (raw.data()), (std::streamsize) raw.size());

    const auto container = readContainer (raw);
    REQUIRE (container.ok);

    const auto payload = decompress (container.payload, container.codec, container.declaredSize);
    REQUIRE (payload.ok);

    const auto decoded = decodeCbor (payload.bytes);
    REQUIRE (decoded.ok);
    REQUIRE (decoded.bytesRead == payload.bytes.size());

    const auto result = loadPreset (decoded.value, container.schemaVersion, schema(), catalogue());

    REQUIRE (result.ok);

    // A handful of genuine complaints is expected — this preset uses engines and effects v1 does not
    // ship. Hundreds would mean the paths are wrong, not that the preset is unusual.
    for (const auto& entry : result.report.entries)
        UNSCOPED_INFO (toString (entry.severity) << " " << entry.modulePath << " " << entry.item << ": " << entry.reason);

    CHECK (result.report.entries.size() < 24);
    CHECK (result.report.count (Severity::Medium) < 12);

    // And the parameters really did land: hundreds of them, addressed by schema path.
    // Cross-validated rather than bounded. The Python decoder — written independently, hours earlier,
    // by a different process — reports **912** parameters and **4** asset references for this file under
    // the same traversal rules. Two implementations agreeing exactly is the strongest evidence available
    // short of running the synth, and it is what caught the instance-collapsing bug above.
    CHECK (result.document.parameterCount() == 912);
    CHECK (result.document.assetReferences.size() == 4);
    CHECK (result.document.schemaVersion == "4.0");

}
TEST_CASE ("every factory preset produces a document and a report", "[document][corpus]")
{
    // The acceptance criterion that needs the whole bank rather than one file: a report for *every*
    // preset, including the ones with nothing to report. It also answers a question the map has been
    // carrying since the cut: how much of the factory bank actually imports without material loss.
    const std::filesystem::path root =
        "/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Presets";

    if (! std::filesystem::exists (root))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    std::size_t presets = 0;
    std::size_t cleanImports = 0;
    std::size_t withHighSeverity = 0;
    std::size_t failed = 0;
    std::map<std::string, std::size_t> reasons;

    for (const auto& entry : std::filesystem::recursive_directory_iterator (root))
    {
        if (! entry.is_regular_file() || entry.path().extension() != ".SerumPreset")
            continue;

        ++presets;

        std::ifstream in (entry.path(), std::ios::binary);
        std::vector<std::byte> raw ((std::size_t) std::filesystem::file_size (entry.path()));
        in.read (reinterpret_cast<char*> (raw.data()), (std::streamsize) raw.size());

        const auto container = readContainer (raw);

        if (! container.ok)
        {
            ++failed;
            UNSCOPED_INFO (entry.path().filename().string() << ": " << container.error);
            continue;
        }

        const auto payload = decompress (container.payload, container.codec, container.declaredSize);

        if (! payload.ok)
        {
            ++failed;
            UNSCOPED_INFO (entry.path().filename().string() << ": " << payload.error);
            continue;
        }

        const auto decoded = container.codec == Codec::Zstd ? decodeCbor (payload.bytes)
                                                            : decodeLegacy (payload.bytes);

        if (! decoded.ok || decoded.bytesRead != payload.bytes.size())
        {
            ++failed;
            UNSCOPED_INFO (entry.path().filename().string() << ": " << decoded.error);
            continue;
        }

        const auto result = loadPreset (decoded.value, container.schemaVersion, schema(), catalogue());

        if (! result.ok)
        {
            ++failed;
            continue;
        }

        // The criterion: a report exists for every preset. An empty one is a report.
        if (result.report.clean())
            ++cleanImports;
        else if (result.report.worst() == Severity::High)
            ++withHighSeverity;

        for (const auto& reportEntry : result.report.entries)
            if (reportEntry.severity == Severity::High)
                ++reasons[reportEntry.item];
    }

    // A summary, not just a pass: these numbers are the answer to "does the cut hold up".
    UNSCOPED_INFO ("presets " << presets << ", clean " << cleanImports << ", with a high-severity item "
                              << withHighSeverity << ", failed " << failed);

    std::vector<std::pair<std::size_t, std::string>> ranked;

    for (const auto& [item, count] : reasons)
        ranked.emplace_back (count, item);

    std::sort (ranked.begin(), ranked.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });

    for (std::size_t i = 0; i < ranked.size() && i < 10; ++i)
        UNSCOPED_INFO ("  high: " << ranked[i].second << " (" << ranked[i].first << " presets)");

    CHECK (presets == 626);
    CHECK (failed == 0);
    CHECK (cleanImports + withHighSeverity <= presets);
}

TEST_CASE ("severity counts and the worst case are available for a user-facing summary", "[document]")
{
    const auto result = load (object ({ { "Oscillator0", wavetableSlot ("SpectralOsc") },
                                        { "ModuleFromTheFuture", object ({ { "plainParams", object ({}) } }) } }));

    REQUIRE (result.ok);
    CHECK (result.report.count (Severity::High) == 1);
    CHECK (result.report.count (Severity::Medium) == 1);
    CHECK (result.report.count (Severity::Info) == 0);
    CHECK (result.report.worst() == Severity::High);
    CHECK_FALSE (result.report.clean());

    CHECK (toString (Severity::High) == "high");
    CHECK (toString (Severity::Medium) == "medium");
    CHECK (toString (Severity::Info) == "info");
}
