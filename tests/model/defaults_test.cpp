// Tests for parameter defaults (ticket 38).
//
// Where defaults come from, settled with evidence: **the reference's own init state**, read out of the
// real plugin by `xerum-host --dump-params` (2623 parameters, of which 543 are the genuine automatable
// surface). A preset never stores a default, so no default can be read from the corpus — and the value
// the factory bank uses most often is a *mode*, which these tests measure rather than assume.
//
// The captured table is **not committed** (ticket 10: nothing derived from the reference is committed).
// The generator that produces it ships — `tools/reference-init-defaults.py` — and so does this file,
// which has two halves:
//
//   * what the *model* promises about defaults, which holds on any machine, reference or not;
//   * what a *capture* and the *corpus* say, measured only when they are present and skipped loudly
//     when they are not (the same rule every other corpus test follows).
//
// Nothing here asserts that a particular default value is correct: that is the fidelity layer's job,
// against our own init render, once an engine exists. What it asserts is that an unestablished default
// is refused rather than returned as a plausible zero, and that the premise the defaults table exists
// for — absence as the ordinary case — is true of the factory bank.

#include <catch2/catch_test_macros.hpp>

#include "generated/schema_data.h"

#include "cbor.h"
#include "container.h"
#include "decompress.h"
#include "document.h"
#include "json.h"
#include "legacy.h"
#include "schema.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace xerum::model;

namespace
{

/// Where a capture lives when one has been taken. Outside the repository, always: see ticket 10.
constexpr const char* kDefaultsEnvVar = "XERUM_REFERENCE_DEFAULTS";
constexpr const char* kPresetRoot = "/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Presets";
constexpr const char* kLeafCountExpected = "776";

std::filesystem::path defaultsTablePath()
{
    if (const char* explicitPath = std::getenv (kDefaultsEnvVar); explicitPath != nullptr && *explicitPath != '\0')
        return explicitPath;

    if (const char* home = std::getenv ("HOME"); home != nullptr && *home != '\0')
        return std::filesystem::path (home) / ".xerum-fixtures/params38/defaults.json";

    return {};
}

/// Read and JSON-decode the captured defaults table. `nullptr` when no capture is present.
JsonResult readDefaultsTable (std::filesystem::path& used)
{
    used = defaultsTablePath();

    if (used.empty() || ! std::filesystem::exists (used))
        return {};

    std::ifstream in (used, std::ios::binary);
    std::string text ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());

    return decodeJson (text);
}

/// Decode one preset file the way ticket 22's pipeline does.
LoadResult loadFile (const std::filesystem::path& file)
{
    std::ifstream in (file, std::ios::binary);
    std::vector<std::byte> raw ((std::size_t) std::filesystem::file_size (file));
    in.read (reinterpret_cast<char*> (raw.data()), (std::streamsize) raw.size());

    LoadResult failure;
    const auto container = readContainer (raw);

    if (! container.ok)
        return failure;

    const auto payload = decompress (container.payload, container.codec, container.declaredSize);

    if (! payload.ok)
        return failure;

    const auto decoded = container.codec == Codec::Zstd ? decodeCbor (payload.bytes)
                                                        : decodeLegacy (payload.bytes);

    if (! decoded.ok || decoded.bytesRead != payload.bytes.size())
        return failure;

    return loadPreset (decoded.value, container.schemaVersion, Schema::instance(),
                       SupportCatalogue::v1());
}

const JsonValue* member (const JsonValue& object, std::string_view key)
{
    return object.find (key);
}

double numberOr (const JsonValue& object, std::string_view key, double fallback)
{
    const auto* found = object.find (key);
    return found != nullptr && found->isNumber() ? found->number : fallback;
}

} // namespace

TEST_CASE ("every leaf either refuses a default or holds one it can actually hold", "[defaults]")
{
    // The invariant that makes an unestablished default useful rather than merely honest. `defaultFor`
    // returning nullopt is correct; `defaultFor` returning a plausible-looking 0 would be a lie that no
    // downstream test could see. This walks all 776 leaves so the guarantee is total rather than spot
    // checked, and it keeps holding when a table is established: a value that *is* established must lie
    // inside the leaf's own range.
    const auto& schema = Schema::instance();

    std::size_t unestablished = 0;
    std::size_t established = 0;
    std::size_t stringLeaves = 0;

    for (const auto* leaf = generated::kLeaves; leaf != generated::kLeaves + generated::kLeafCount; ++leaf)
    {
        const auto* described = schema.describe (leaf->path);

        // The generated table and the lookup agree about every row they both know about.
        REQUIRE (described != nullptr);
        CHECK (toString (described->defaultProvenance) == leaf->defaultProvenance);

        const auto value = schema.defaultFor (leaf->path);

        if (described->defaultProvenance == DefaultProvenance::Unestablished)
        {
            ++unestablished;
            CHECK_FALSE (value.has_value());
            continue;
        }

        ++established;

        // A default is either a number the leaf can hold, or explicitly absent.
        if (value.has_value())
        {
            if (described->hasRange)
            {
                CHECK (*value >= described->min);
                CHECK (*value <= described->max);
            }

            CHECK (described->kind != LeafKind::String);

            if (described->kind == LeafKind::String)
                ++stringLeaves;
        }
    }

    CHECK (unestablished + established == schema.leafCount());
    CHECK (schema.leafCount() == 776);

    // The state the model is in today, named rather than implied: defaults are *generated* from the
    // reference's init state, not authored into the committed schema, so the committed table carries
    // none. If this ever changes, the generated table was replaced by an authored one and the
    // provenance in `src/model/generated/schema_data.h` moved with it — which is a decision, not a
    // detail, and should show up here.
    UNSCOPED_INFO ("established " << established << ", unestablished " << unestablished
                                  << ", string leaves " << stringLeaves);
    CHECK (established == 0);
}

TEST_CASE ("a captured reference init table is consistent with the schema it claims to address",
           "[defaults]")
{
    // Runs only where a capture exists. Measured, not assumed: every path must resolve to a leaf, every
    // normalised value must be normalised, and every entry must say where it came from. A table that
    // silently held a path the schema does not know would be a table that cannot be applied.
    std::filesystem::path used;
    const auto table = readDefaultsTable (used);

    if (! table.ok)
        SKIP ("no captured defaults table at " << (used.empty() ? "(no HOME)" : used.string())
              << " — capture one with tools/reference-init-defaults.py (docs/test-strategy.md)");

    const auto& schema = Schema::instance();

    REQUIRE (table.value.isObject());

    std::size_t entries = 0;
    std::size_t presetUnits = 0;
    std::size_t normalisedOnly = 0;
    std::size_t textOnly = 0;
    std::size_t outsideTheObservedRange = 0;
    std::vector<std::string> outsideExamples;

    for (const auto& [path, entry] : table.value.members)
    {
        ++entries;

        const auto* leaf = schema.describe (path);
        REQUIRE (leaf != nullptr);

        CHECK (member (entry, "provenance") != nullptr);
        CHECK (entry.stringOr ("provenance") == "reference-init");
        CHECK (path == entry.stringOr ("path"));

        const auto* instances = member (entry, "instances");
        REQUIRE (instances != nullptr);
        REQUIRE (instances->isObject());
        CHECK_FALSE (instances->members.empty());

        for (const auto& [instance, observed] : instances->members)
        {
            const auto value = numberOr (observed, "normalised", -1.0);
            CHECK (value >= 0.0);
            CHECK (value <= 1.0);
            CHECK (member (observed, "displayText") != nullptr);
            CHECK (observed.stringOr ("pairing") != "");
        }

        const auto state = entry.stringOr ("valueState");
        const bool hasPresetUnits = member (entry, "presetUnitDefault") != nullptr
                                    && member (entry, "presetUnitDefault")->isNumber();

        if (state == "preset-units")
        {
            ++presetUnits;
            REQUIRE (hasPresetUnits);

            const auto converted = numberOr (entry, "presetUnitDefault", 0.0);

            CHECK (std::isfinite (converted));

            // `min`/`max` in the schema are the range the *corpus* was observed to use, and a default
            // is by definition never stored — so a default can legitimately sit outside it. That is not
            // a defect to check away; it is the evidence that the corpus cannot yield defaults, and it
            // is asserted below rather than swallowed here.
            if (leaf->hasRange && (converted < leaf->min || converted > leaf->max))
            {
                ++outsideTheObservedRange;

                if (outsideExamples.size() < 6)
                    outsideExamples.push_back (path + " default " + std::to_string (converted)
                                               + " outside observed [" + std::to_string (leaf->min)
                                               + ", " + std::to_string (leaf->max) + "]");
            }
        }
        else if (state == "normalised-only")
        {
            ++normalisedOnly;
            CHECK (leaf->kind != LeafKind::String);
        }
        else
        {
            ++textOnly;
            // A string leaf's default cannot be a double: only the reference's own display text
            // carries it, and the enum id the preset stores is ticket 14's mapping to establish.
            CHECK (leaf->kind == LeafKind::String);
        }
    }

    UNSCOPED_INFO ("captured table: " << entries << " leaves, " << presetUnits << " in preset units, "
                                      << normalisedOnly << " normalised only, " << textOnly
                                      << " text only — from " << used.string());

    for (const auto& example : outsideExamples)
        UNSCOPED_INFO ("default outside the corpus-observed range: " << example);

    // The measured overlap of ticket 38: the reference's automatable surface addresses 116 of the
    // model's 776 leaves, so a capture of any size below that would mean the crosswalk regressed.
    CHECK (entries >= 100);
    CHECK (textOnly + normalisedOnly + presetUnits == entries);

    // And the consequence that makes this table necessary rather than convenient: some defaults lie
    // outside every value the corpus ever stored for them, precisely because a stored value is a
    // non-default. A defaults table inferred from the bank could not have produced those numbers.
    UNSCOPED_INFO ("established defaults outside the corpus-observed range: " << outsideTheObservedRange
                                                                             << " of " << presetUnits);
    CHECK (outsideTheObservedRange > 0);
}

TEST_CASE ("absence is the ordinary case in the factory bank", "[defaults][corpus]")
{
    // The reason a defaults table is load-bearing rather than a nicety. "Absence means default" only
    // matters if absence is common, so this measures it across the whole bank instead of asserting it:
    // a preset stores the parameters it was saved with, and omits the rest.
    const std::filesystem::path root = kPresetRoot;

    if (! std::filesystem::exists (root))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    const auto& schema = Schema::instance();

    std::map<std::string, std::size_t> presetsStoringLeaf;
    std::size_t presets = 0;
    std::size_t factoryPresets = 0;
    std::size_t failed = 0;
    std::size_t storedParameters = 0;
    std::size_t storedPathsOutsideSchema = 0;

    for (const auto& file : std::filesystem::recursive_directory_iterator (root))
    {
        if (! file.is_regular_file() || file.path().extension() != ".SerumPreset")
            continue;

        ++presets;

        if (file.path().string().find ("/Factory/") != std::string::npos)
            ++factoryPresets;

        const auto result = loadFile (file.path());

        if (! result.ok)
        {
            ++failed;
            UNSCOPED_INFO (file.path().filename().string() << ": " << result.error);
            continue;
        }

        std::set<std::string> seen;

        for (const auto& [path, value] : result.document.parameters)
        {
            ++storedParameters;

            const auto normalised = schema.normalise (path);

            if (schema.describe (normalised) == nullptr)
            {
                ++storedPathsOutsideSchema;
                continue;
            }

            seen.insert (normalised);
        }

        for (const auto& leafPath : seen)
            ++presetsStoringLeaf[leafPath];
    }

    REQUIRE (presets > 600);
    CHECK (failed == 0);
    CHECK (storedPathsOutsideSchema == 0);

    std::size_t everyPreset = 0;
    std::size_t fewerThanHalf = 0;
    std::size_t atMostTenPercent = 0;

    for (const auto* leaf = generated::kLeaves; leaf != generated::kLeaves + generated::kLeafCount; ++leaf)
    {
        const auto stored = presetsStoringLeaf[std::string (leaf->path)];

        if (stored == presets)
            ++everyPreset;

        if (stored * 2 < presets)
            ++fewerThanHalf;

        if (stored * 10 <= presets)
            ++atMostTenPercent;
    }

    UNSCOPED_INFO ("presets " << presets << " (" << factoryPresets << " of them factory)"
                              << ", parameters stored " << storedParameters
                              << " (" << (storedParameters / presets) << " per preset)"
                              << ", leaves stored by every preset " << everyPreset
                              << ", by fewer than half " << fewerThanHalf
                              << ", by at most a tenth " << atMostTenPercent
                              << " of " << schema.leafCount());

    // The characteristic property: omission is not an edge case. Fewer than a tenth of the model's
    // leaves appear in every preset, and most appear in fewer than half — so a loader that read
    // absence as "nothing happened here" would get the majority of every preset wrong.
    CHECK (everyPreset < 60);
    CHECK (fewerThanHalf > 600);
    CHECK (atMostTenPercent > 400);

    // And a parameter written by every preset is rare enough that the figure above cannot be an
    // artefact of the presets being small.
    CHECK (storedParameters > 20 * presets);
    CHECK (kLeafCountExpected == std::string ("776"));
}

TEST_CASE ("the factory bank's most common value is not the reference's default", "[defaults][corpus]")
{
    // The claim ticket 21 made and could not test: a mode is not a default. This measures it where the
    // two are directly comparable — the leaves whose units the capture has established — and reports the
    // disagreement rather than arguing for it.
    //
    // A preset that stores a leaf is stating a value *different* from the default (that is what sparse
    // override means), so the modal stored value should differ from the reference's init value wherever
    // the parameter is used expressively. Where it does not differ, the parameter is one the bank mostly
    // leaves alone.
    std::filesystem::path used;
    const auto table = readDefaultsTable (used);

    if (! table.ok)
        SKIP ("no captured defaults table at " << (used.empty() ? "(no HOME)" : used.string())
              << " — capture one with tools/reference-init-defaults.py (docs/test-strategy.md)");

    const std::filesystem::path root = kPresetRoot;

    if (! std::filesystem::exists (root))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    const auto& schema = Schema::instance();

    // Only leaves the capture established in the model's own units: comparing a normalised position
    // with a stored engineering value would be comparing two different scales.
    std::map<std::string, double> referenceDefault;

    for (const auto& [path, entry] : table.value.members)
        if (entry.stringOr ("valueState") == "preset-units")
            referenceDefault[path] = numberOr (entry, "presetUnitDefault", 0.0);

    REQUIRE_FALSE (referenceDefault.empty());

    // A histogram per comparable leaf only, so walking 626 presets costs nothing.
    std::map<std::string, std::map<double, std::size_t>> histogram;

    std::size_t presets = 0;

    for (const auto& file : std::filesystem::recursive_directory_iterator (root))
    {
        if (! file.is_regular_file() || file.path().extension() != ".SerumPreset")
            continue;

        ++presets;

        const auto result = loadFile (file.path());

        if (! result.ok)
            continue;

        for (const auto& [path, value] : result.document.parameters)
        {
            const auto normalised = schema.normalise (path);

            if (referenceDefault.find (normalised) == referenceDefault.end())
                continue;

            double number = 0.0;

            if (! value.numberAsDouble (number))
                continue;

            // Quantised to a millionth: corpus values are decimals written by a UI, and two encodings of
            // "the same" value should not split the count.
            const auto key = (double) std::llround (number * 1.0e6) / 1.0e6;
            ++histogram[normalised][key];
        }
    }

    REQUIRE (presets > 600);

    std::size_t compared = 0;
    std::size_t agree = 0;
    std::size_t differ = 0;

    for (const auto& [path, default_] : referenceDefault)
    {
        const auto found = histogram.find (path);

        if (found == histogram.end())
            continue;

        std::size_t best = 0;
        double modal = 0.0;

        for (const auto& [value, count] : found->second)
            if (count > best)
            {
                best = count;
                modal = value;
            }

        // Only where the bank uses the parameter enough for a mode to mean anything.
        if (best < 3)
            continue;

        ++compared;

        if (std::abs (modal - default_) <= 1.0e-6 * std::max (1.0, std::abs (default_)))
            ++agree;
        else
            ++differ;

        UNSCOPED_INFO (path << ": reference default " << default_ << ", corpus mode " << modal
                            << " (" << best << " presets)");
    }

    UNSCOPED_INFO ("comparable leaves " << compared << ", mode agrees " << agree
                                        << ", mode differs " << differ);

    CHECK (compared >= 8);
    CHECK (differ > agree);
}
