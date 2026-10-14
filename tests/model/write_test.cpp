// Tests for writing a document back into a container.
//
// The round trip is the interesting part, and it is deliberately routed through the *real* reader rather
// than through a private path: a writer that only satisfies itself proves nothing.

#include <catch2/catch_test_macros.hpp>

#include "cbor.h"
#include "container.h"
#include "decompress.h"
#include "document.h"
#include "write.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace xerum::model;

namespace
{

Value text (std::string value) { Value v; v.type = Value::Type::String; v.text = std::move (value); return v; }
Value number (double value) { Value v; v.type = Value::Type::Float; v.real = value; return v; }

Value object (std::initializer_list<std::pair<std::string, Value>> members)
{
    Value v;
    v.type = Value::Type::Map;
    for (const auto& [key, value] : members)
        v.entries.emplace_back (key, value);
    return v;
}

/// A preset with several instances of the same module, a nested engine, an asset reference and a
/// parameter whose name ends in a digit — the shapes that have broken this layer before.
///
/// Built with statements rather than one nested initialiser list: brace counting in a fixture is how you
/// spend twenty minutes on a compiler error instead of a bug.
Value richPreset()
{
    Value root;
    root.type = Value::Type::Map;

    auto warpParams = [] (const std::string& warp)
    {
        return object ({ { "plainParams", object ({ { "kParamWarpMenu", text (warp) } }) } });
    };

    // A wavetable slot: two real parameters under `plainParams`, plus an asset reference as a sibling.
    Value wt0 = object ({ { "plainParams", object ({ { "kParamWarpMenu", text ("kSync") },
                                                      { "kParamTablePos", number (5.0) } }) } });
    wt0.entries.emplace_back ("relativePathToWT", text ("/Analog/Basic Shapes.wav"));

    Value osc0;
    osc0.type = Value::Type::Map;
    osc0.entries.emplace_back ("WTOsc0", wt0);

    Value osc1;
    osc1.type = Value::Type::Map;
    osc1.entries.emplace_back ("WTOsc1", warpParams ("kPD_OSC"));

    Value env0;
    env0.type = Value::Type::Map;
    env0.entries.emplace_back ("plainParams", object ({ { "kParamAttack", number (1.5) },
                                                        { "kParamCurve1", number (0.25) } }));

    Value env1;
    env1.type = Value::Type::Map;
    env1.entries.emplace_back ("plainParams", object ({ { "kParamAttack", number (9.0) } }));

    root.entries.emplace_back ("Oscillator0", osc0);
    root.entries.emplace_back ("Oscillator1", osc1);
    root.entries.emplace_back ("Env0", env0);
    root.entries.emplace_back ("Env1", env1);

    return root;
}

LoadResult load (const Value& payload, std::string_view version = "4.0")
{
    return loadPreset (payload, version, Schema::instance(), SupportCatalogue::v1());
}

} // namespace

TEST_CASE ("md5 matches the reference's hash for known inputs", "[write]")
{
    // Checked against published vectors, because a hash that is merely self-consistent would happily key
    // every preset the same way.
    const auto of = [] (std::string_view s)
    {
        return md5Hex (std::span<const std::byte> (reinterpret_cast<const std::byte*> (s.data()), s.size()));
    };

    CHECK (of ("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK (of ("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK (of ("The quick brown fox jumps over the lazy dog") == "9e107d9d372bb6826bd81d3542a419d6");
}

TEST_CASE ("a document writes to a container that reads back equal", "[write]")
{
    const auto original = load (richPreset());
    REQUIRE (original.ok);
    REQUIRE (original.document.parameterCount() > 0);

    const auto written = writeContainer (original.document);
    REQUIRE (written.ok);

    // Through the real reader, not a shortcut.
    const auto container = readContainer (written.bytes);
    REQUIRE (container.ok);
    CHECK (container.codec == Codec::Zstd);
    CHECK (container.fileType == "SerumPreset");

    const auto payload = decompress (container.payload, container.codec, container.declaredSize);
    REQUIRE (payload.ok);

    const auto decoded = decodeCbor (payload.bytes);
    REQUIRE (decoded.ok);
    CHECK (decoded.bytesRead == payload.bytes.size());

    const auto reread = load (decoded.value, container.schemaVersion);
    REQUIRE (reread.ok);

    // The criterion: equal documents, which means every instance kept its own value.
    CHECK (reread.document.parameters == original.document.parameters);
    CHECK (reread.document.parameters.size() > 6);

    auto before = original.document.assetReferences;
    auto after = reread.document.assetReferences;
    std::sort (before.begin(), before.end());
    std::sort (after.begin(), after.end());
    CHECK (before == after);

    // Instances did not collapse on the way through.
    CHECK (reread.document.parameters.count ("Env0/plainParams/kParamAttack") == 1);
    CHECK (reread.document.parameters.count ("Env1/plainParams/kParamAttack") == 1);
    CHECK (reread.document.parameters.count ("Env0/plainParams/kParamCurve1") == 1);
}

TEST_CASE ("the metadata's hash is recomputed and everything else is preserved", "[write]")
{
    const auto original = load (richPreset());
    const std::string metadata =
        R"({"fileType":"SerumPreset","hash":"00000000000000000000000000000000",)"
        R"("presetName":"A name we must not lose","authorNote":"a field we do not understand","version":4.0})";

    const auto written = writeContainer (original.document, metadata);
    REQUIRE (written.ok);

    const auto container = readContainer (written.bytes);
    REQUIRE (container.ok);

    // The hash identifies the payload, and the payload changed — so it must have been recomputed.
    const auto expected = md5Hex (container.payload);
    CHECK (container.metadataJson.find ("\"hash\":\"" + expected + "\"") != std::string::npos);
    CHECK (container.metadataJson.find ("00000000000000000000000000000000") == std::string::npos);

    // And a field we do not interpret survived byte-for-byte, which is what preservation is for.
    CHECK (container.metadataJson ==
           std::string ("{\"fileType\":\"SerumPreset\",\"hash\":\"" + expected + "\","
                        "\"presetName\":\"A name we must not lose\","
                        "\"authorNote\":\"a field we do not understand\",\"version\":4.0}"));
    CHECK (container.presetName == "A name we must not lose");
}

TEST_CASE ("a hash field that is absent is added rather than the metadata rewritten", "[write]")
{
    const auto original = load (richPreset());
    const auto written = writeContainer (original.document, R"({"fileType":"SerumPreset","version":4.0})");

    REQUIRE (written.ok);
    const auto container = readContainer (written.bytes);
    REQUIRE (container.ok);

    CHECK (container.metadataJson.find ("\"hash\":\"" + md5Hex (container.payload) + "\"") != std::string::npos);
    CHECK (container.metadataJson.find ("\"fileType\":\"SerumPreset\"") != std::string::npos);
    CHECK (container.metadataJson.find ("\"version\":4.0") != std::string::npos);
}

TEST_CASE ("writing is deterministic", "[write]")
{
    // The same document must produce the same bytes, or nothing downstream can cache, diff or compare.
    const auto document = load (richPreset()).document;

    const auto first = writeContainer (document);
    const auto second = writeContainer (document);

    REQUIRE (first.ok);
    REQUIRE (second.ok);
    CHECK (first.bytes == second.bytes);
}

TEST_CASE ("a written container's declared size is the encoded payload's", "[write]")
{
    const auto document = load (richPreset()).document;
    const auto written = writeContainer (document);
    REQUIRE (written.ok);

    const auto container = readContainer (written.bytes);
    REQUIRE (container.ok);

    const auto payload = decompress (container.payload, container.codec, container.declaredSize);
    REQUIRE (payload.ok);

    const auto decoded = decodeCbor (payload.bytes);
    REQUIRE (decoded.ok);
    CHECK (decoded.bytesRead == container.declaredSize);
}

TEST_CASE ("a real factory preset survives a round trip", "[write][corpus]")
{
    // The hand-built tree above proves the mechanics. This proves the contract on a file the reference
    // actually wrote: 912 parameters in, 912 out, addressed identically.
    const std::filesystem::path file =
        "/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Presets/Factory/Bass/808/808 - Straight.SerumPreset";

    if (! std::filesystem::exists (file))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    std::ifstream in (file, std::ios::binary);
    std::vector<std::byte> raw ((std::size_t) std::filesystem::file_size (file));
    in.read (reinterpret_cast<char*> (raw.data()), (std::streamsize) raw.size());

    const auto original = readContainer (raw);
    REQUIRE (original.ok);

    const auto payload = decompress (original.payload, original.codec, original.declaredSize);
    REQUIRE (payload.ok);

    const auto decoded = decodeCbor (payload.bytes);
    REQUIRE (decoded.ok);

    const auto first = load (decoded.value, original.schemaVersion);
    REQUIRE (first.ok);
    REQUIRE (first.document.parameterCount() == 912);

    // Out, then back in through the real reader, preserving the metadata block the reference wrote.
    const auto written = writeContainer (first.document, original.metadataJson);
    REQUIRE (written.ok);

    const auto container = readContainer (written.bytes);
    REQUIRE (container.ok);
    CHECK (container.schemaVersion == original.schemaVersion);
    CHECK (container.presetName == original.presetName);          // preserved from the metadata

    const auto roundPayload = decompress (container.payload, container.codec, container.declaredSize);
    REQUIRE (roundPayload.ok);

    const auto roundDecoded = decodeCbor (roundPayload.bytes);
    REQUIRE (roundDecoded.ok);
    CHECK (roundDecoded.bytesRead == roundPayload.bytes.size());

    const auto second = load (roundDecoded.value, container.schemaVersion);
    REQUIRE (second.ok);

    CHECK (second.document.parameterCount() == first.document.parameterCount());
    CHECK (second.document.parameters == first.document.parameters);

    auto before = first.document.assetReferences;
    auto after = second.document.assetReferences;
    std::sort (before.begin(), before.end());
    std::sort (after.begin(), after.end());
    CHECK (before == after);
}
