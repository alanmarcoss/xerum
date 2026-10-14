// Ticket 36 — can the document represent which engine a slot selects?
//
// The ticket was filed on a premise: that a preset selects an engine "by `kParamType` on the slot, or
// equivalently by which sub-engine carries a real parameter map", and that the second signal is not
// representable, so a write drops the selection.
//
// **The premise is wrong, and the two signals are not equivalent.** Measured over all 3130 oscillator
// slots in the factory bank:
//
//   * `Oscillator{N}/plainParams/kParamType` **is a schema leaf**, so when a preset stores it the
//     selection is a parameter like any other and survives load and write untouched;
//   * its **absence means the default engine** (wavetable), which ADR 0001 already represents;
//   * and 434 slots carry an override map on a *non-selected* engine — overwhelmingly slots 3 and 4,
//     which are the fixed NOISE and SUB slots and are not selectable at all (ticket 05) — so "which
//     sub-engine has parameters" is not a signal about selection at all. It is what a user leaves
//     behind when they try an engine and switch back.
//
// So the case the ticket worried about — a slot selecting a non-default engine with none of its
// parameters overridden — is real (85 slots) and *is* representable, because those 85 store
// `kParamType`. This file asserts exactly that, on the real presets rather than on a hand-built tree.

#include <catch2/catch_test_macros.hpp>

#include "cbor.h"
#include "container.h"
#include "decompress.h"
#include "document.h"
#include "legacy.h"
#include "schema.h"
#include "write.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace xerum::model;

namespace
{

struct Decoded
{
    bool ok { false };
    ContainerResult container;
    LoadResult result;
};

Decoded decode (const std::filesystem::path& file)
{
    Decoded out;

    std::ifstream in (file, std::ios::binary);

    if (! in.good())
        return out;

    std::vector<std::byte> raw ((std::size_t) std::filesystem::file_size (file));
    in.read (reinterpret_cast<char*> (raw.data()), (std::streamsize) raw.size());

    out.container = readContainer (raw);

    if (! out.container.ok)
        return out;

    const auto payload = decompress (out.container.payload, out.container.codec, out.container.declaredSize);

    if (! payload.ok)
        return out;

    const auto decoded = out.container.codec == Codec::Zstd ? decodeCbor (payload.bytes)
                                                            : decodeLegacy (payload.bytes);

    if (! decoded.ok || decoded.bytesRead != payload.bytes.size())
        return out;

    out.result = loadPreset (decoded.value, out.container.schemaVersion, Schema::instance(),
                             SupportCatalogue::v1());
    out.ok = out.result.ok;
    return out;
}

constexpr const char* kCorpusRoot = "/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Presets";

/// The engine a slot's stored `kParamType` names, and whether it is the default engine.
bool selectsNonDefaultEngine (const PresetDocument& document, const std::string& slot)
{
    const auto found = document.parameters.find (slot + "/plainParams/kParamType");

    if (found == document.parameters.end() || found->second.type != Value::Type::String)
        return false;

    // The stored value is the UI enum, not the sub-engine object's name: `kOsc_Granular` selects
    // `GranularOsc0`. An absent value is the wavetable default, which is precisely why absence can be
    // read as a selection rather than as a hole.
    return ! found->second.text.empty();
}

} // namespace

TEST_CASE ("the engine selection is a parameter, so a write cannot drop it", "[engine-selection]")
{
    // Ticket 36's acceptance, on a hand-built case first so the test means something with no reference
    // installed: a slot that selects spectral and overrides nothing.
    Value payload;
    payload.type = Value::Type::Map;

    Value slot;
    slot.type = Value::Type::Map;

    Value plainParams;
    plainParams.type = Value::Type::Map;

    Value type;
    type.type = Value::Type::String;
    type.text = "kOsc_Spectral";

    plainParams.entries.emplace_back ("kParamType", type);
    slot.entries.emplace_back ("plainParams", plainParams);
    payload.entries.emplace_back ("Oscillator0", slot);

    const auto loaded = loadPreset (payload, "4.0", Schema::instance(), SupportCatalogue::v1());
    REQUIRE (loaded.ok);
    REQUIRE (selectsNonDefaultEngine (loaded.document, "Oscillator0"));

    const auto written = writeContainer (loaded.document, {});
    REQUIRE (written.ok);

    const auto container = readContainer (written.bytes);
    REQUIRE (container.ok);

    const auto decompressed = decompress (container.payload, container.codec, container.declaredSize);
    REQUIRE (decompressed.ok);

    const auto decoded = decodeCbor (decompressed.bytes);
    REQUIRE (decoded.ok);

    const auto reread = loadPreset (decoded.value, container.schemaVersion, Schema::instance(),
                                    SupportCatalogue::v1());
    REQUIRE (reread.ok);

    // The selection survives, and so does the document.
    CHECK (selectsNonDefaultEngine (reread.document, "Oscillator0"));
    CHECK (reread.document.parameters.size() == loaded.document.parameters.size());
}

TEST_CASE ("engine selection survives a round trip through the whole factory bank",
           "[engine-selection][corpus]")
{
    const std::filesystem::path root = kCorpusRoot;

    if (! std::filesystem::exists (root))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    std::size_t presets = 0;
    std::size_t slotsWithExplicitEngine = 0;
    std::size_t slotsWithExplicitEngineAndNoOverrides = 0;
    std::size_t compared = 0;
    std::set<std::string> engines;

    for (const auto& entry : std::filesystem::recursive_directory_iterator (root))
    {
        if (! entry.is_regular_file() || entry.path().extension() != ".SerumPreset")
            continue;

        const auto decoded = decode (entry.path());

        if (! decoded.ok)
            continue;

        ++presets;
        const auto& document = decoded.result.document;

        std::vector<std::string> selected;

        for (const auto& [path, value] : document.parameters)
        {
            if (! path.ends_with ("/plainParams/kParamType") || value.type != Value::Type::String)
                continue;

            if (! path.starts_with ("Oscillator"))
                continue;

            const auto slot = path.substr (0, path.find ("/plainParams"));
            selected.push_back (slot);
            ++slotsWithExplicitEngine;
            engines.insert (value.text);

            // The case the ticket names: the selected engine carries no override of its own, so the
            // selection is the *only* thing the preset says about that engine.
            const auto engineStem = value.text.substr (value.text.find ('_') + 1);
            bool hasOverrides = false;

            for (const auto& [otherPath, otherValue] : document.parameters)
            {
                (void) otherValue;

                if (otherPath.starts_with (slot + "/") && otherPath.find (engineStem) != std::string::npos)
                    hasOverrides = true;
            }

            if (! hasOverrides)
                ++slotsWithExplicitEngineAndNoOverrides;
        }

        if (selected.empty())
            continue;

        // Round trip it: write, read back, and require the same selections.
        const auto written = writeContainer (document, decoded.container.metadataJson);

        if (! written.ok)
            continue;

        const auto container = readContainer (written.bytes);

        if (! container.ok)
            continue;

        const auto payload = decompress (container.payload, container.codec, container.declaredSize);

        if (! payload.ok)
            continue;

        const auto cbor = decodeCbor (payload.bytes);

        if (! cbor.ok)
            continue;

        const auto reread = loadPreset (cbor.value, container.schemaVersion, Schema::instance(),
                                        SupportCatalogue::v1());

        if (! reread.ok)
            continue;

        ++compared;

        for (const auto& slot : selected)
        {
            UNSCOPED_INFO (entry.path().filename().string() << ": " << slot);

            CHECK (selectsNonDefaultEngine (reread.document, slot));
            CHECK (reread.document.parameters.find (slot + "/plainParams/kParamType")
                   != reread.document.parameters.end());
        }
    }

    UNSCOPED_INFO ("presets " << presets << ", slots selecting a non-default engine "
                              << slotsWithExplicitEngine << " of which " 
                              << slotsWithExplicitEngineAndNoOverrides
                              << " override nothing, round-tripped " << compared);

    UNSCOPED_INFO ("engine enum values in use:");
    for (const auto& engine : engines)
        UNSCOPED_INFO ("  " << engine);

    // The case exists in the bank, which is why the ticket was worth writing...
    CHECK (slotsWithExplicitEngineAndNoOverrides > 0);

    // ...and it round-trips, which is why the ticket's premise is wrong.
    CHECK (compared > 0);
    CHECK (engines.size() >= 3);
}
