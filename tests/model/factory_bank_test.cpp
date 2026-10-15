// The factory bank, held to its own rules (ticket 54).
//
// This is the half of the bank's verification that needs no plugin: the catalogue's shape, the
// parameters it may use, the tables it points at, and the **bytes on disk**. It runs on any machine,
// with no reference instrument installed, because everything the bank depends on is in this repository.
//
// What it deliberately does not do is render. "Every preset loads through the product's own loader and
// renders non-silent, and no two collapse to the same render" is the plugin suite's job
// (`plugin/tests/bank_test.cpp`), because only the product can answer it.
//
// Run alone with:  ctest --test-dir build-54 -R xerum-model-tests   (or the binary with "[bank]")

#include "factory_bank.h"
#include "json.h"
#include "preset_block.h"
#include "value.h"
#include "wavetable.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace
{

std::filesystem::path repositoryRoot()
{
#ifdef XERUM_REPO_ROOT
    return std::filesystem::path (XERUM_REPO_ROOT);
#else
    return std::filesystem::current_path();
#endif
}

std::filesystem::path contentRoot()
{
    return repositoryRoot() / "content";
}

std::vector<std::byte> readBytes (const std::filesystem::path& path)
{
    std::ifstream in (path, std::ios::binary);

    if (! in)
        return {};

    in.seekg (0, std::ios::end);
    const auto size = in.tellg();
    in.seekg (0, std::ios::beg);

    if (size <= 0)
        return {};

    std::vector<std::byte> bytes (static_cast<std::size_t> (size));
    in.read (reinterpret_cast<char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
    return bytes;
}

std::string readText (const std::filesystem::path& path)
{
    const auto bytes = readBytes (path);

    if (bytes.empty())
        return {};

    return { reinterpret_cast<const char*> (bytes.data()), bytes.size() };
}

/// Every file under the bank root, so a preset that ships without being in the bank — or the other way
/// round — is visible rather than ignored.
std::vector<std::filesystem::path> bankFiles()
{
    std::vector<std::filesystem::path> files;
    const auto root = contentRoot() / std::string (xerum::presets::kBankDirectory);

    std::error_code error;

    for (const auto& entry : std::filesystem::recursive_directory_iterator (root, error))
        if (entry.is_regular_file() && entry.path().extension() == xerum::presets::kPresetExtension)
            files.push_back (entry.path());

    std::sort (files.begin(), files.end());
    return files;
}

} // namespace

TEST_CASE ("the bank is a bank: enough presets, unique names, spread across the categories", "[model][bank]")
{
    using namespace xerum::presets;

    // The bank checks its own vocabulary as it is built — warp modes, filter types, noise colours, sub
    // shapes, matrix sources. A non-empty list here means a preset asks for something the engine does not
    // build, which resolves to the "not in xerum's set" sentinel and silently does nothing.
    const auto problems = bankProblems();
    INFO ("bank problems: " << (problems.empty() ? std::string ("none") : problems.front()));
    CHECK (problems.empty());

    const auto& presets = bank();

    REQUIRE (presets.size() >= kMinimumBankSize);

    std::set<std::string> names;
    std::map<std::string, int> perCategory;
    const auto& categories = xerum::presets::categories();

    REQUIRE (categories.size() >= 8);

    for (const auto& preset : presets)
    {
        INFO ("preset: " << preset.name);

        CHECK (! preset.name.empty());
        CHECK (! preset.description.empty());
        CHECK (names.insert (preset.name).second);

        // The category is one of the eight, and the browser groups by it.
        CHECK (std::find (categories.begin(), categories.end(), std::string_view (preset.category))
               != categories.end());

        ++perCategory[preset.category];

        // A preset must actually play something: at least one wavetable slot with a table.
        CHECK_FALSE (tablesOf (preset).empty());

        // The name has to survive becoming a file name.
        CHECK (preset.name.find ('/') == std::string::npos);
        CHECK (preset.name.find (':') == std::string::npos);
        CHECK (presetFilePath (preset).ends_with (std::string (kPresetExtension)));
    }

    // Every category a player browses by holds presets — a bank that put everything in one category
    // would pass the count and fail the point.
    for (const auto category : categories)
        CHECK (perCategory[std::string (category)] > 0);

    // And no single category is the bank: a hundred basses is not a bank.
    for (const auto& [category, count] : perCategory)
        CHECK (count * 3 <= static_cast<int> (presets.size()));
}

TEST_CASE ("every table a preset plays is one this repository ships", "[model][bank]")
{
    using namespace xerum::presets;

    int references = 0;

    for (const auto& preset : bank())
    {
        for (const auto& [path, value] : preset.document.parameters)
        {
            if (! path.ends_with ("/relativePathToWT"))
                continue;

            ++references;

            REQUIRE (value.isText());

            const auto& reference = value.text;
            INFO ("preset " << preset.name << " plays " << reference);

            // Never a rooted reference: `/Analog/Basic Shapes.wav` is the shape a reference preset's
            // asset takes, and one of those in the bank would be content we may not ship.
            CHECK_FALSE (reference.empty());
            CHECK (reference.front() != '/');

            // It resolves under this repository's own `Tables`, which is xerum's root — not the
            // reference instrument's.
            const auto file = contentRoot() / "Tables" / reference;
            CHECK (std::filesystem::exists (file));

            const auto bytes = readBytes (file);

            if (bytes.empty())
                continue;

            const auto parsed = xerum::dsp::parseWavetable (std::span<const std::byte> (bytes));
            REQUIRE (parsed.ok);

            // The frame the preset reads must exist in the table it names: `Wavetable::interpolated`
            // clamps at `frameCount - 1`, so a position past the end would silently read a different
            // frame from the one the preset was authored against.
            const auto module = path.substr (0, path.rfind ('/'));
            const auto position = preset.document.parameters.find (module + "/plainParams/kParamTablePos");

            if (position != preset.document.parameters.end() && parsed.table.frameCount > 1)
            {
                double frames = 0.0;
                REQUIRE (position->second.numberAsDouble (frames));
                CHECK (frames >= 1.0);
                CHECK (frames <= static_cast<double> (parsed.table.frameCount - 1));
            }
        }
    }

    CHECK (references >= static_cast<int> (bank().size()));
}

TEST_CASE ("the bank a preset file carries is what the bank encodes", "[model][bank]")
{
    using namespace xerum::presets;

    std::set<std::string> shipped;

    for (const auto& preset : bank())
    {
        const auto path = contentRoot() / presetFilePath (preset);
        shipped.insert (path.lexically_normal().string());

        INFO ("preset file: " << path.string());
        REQUIRE (std::filesystem::exists (path));

        const auto bytes = readBytes (path);
        REQUIRE_FALSE (bytes.empty());

        // What ships is what the encoder produces: a hand-edited or stale file fails here.
        CHECK (bytes == encodePresetFile (preset));
    }

    // And nothing else ships under the bank root: a preset file nobody declared is a preset the
    // manifest does not describe.
    for (const auto& file : bankFiles())
        CHECK (shipped.count (file.lexically_normal().string()) == 1);
}

TEST_CASE ("a preset file decodes to exactly the document the bank holds", "[model][bank]")
{
    using namespace xerum::presets;

    for (const auto& preset : bank())
    {
        const auto bytes = readBytes (contentRoot() / presetFilePath (preset));
        REQUIRE_FALSE (bytes.empty());

        const auto decoded = xerum::model::decodePresetBlock (std::span<const std::byte> (bytes),
                                                              xerum::model::kPresetBlockMagic,
                                                              xerum::model::kPresetBlockFormat);

        INFO ("preset: " << preset.name << (decoded.ok ? "" : " — " + decoded.error));
        REQUIRE (decoded.ok);

        CHECK (decoded.document.name == preset.name);
        CHECK (decoded.document.parameterCount() == preset.document.parameterCount());
        CHECK (decoded.document.parameters == preset.document.parameters);
        CHECK (decoded.document.assetReferences == preset.document.assetReferences);

        // The bank ships a clean block: no report entries, because a bank preset may not ask for anything
        // the product does not do.
        CHECK (decoded.report.entries.empty());
        CHECK (decoded.reportEntriesFromModel == 0);
    }
}

TEST_CASE ("the manifest describes the bank, and nothing else", "[model][bank]")
{
    using namespace xerum::presets;

    const auto path = contentRoot() / kBankDirectory / kBankManifestName;
    const auto text = readText (path);
    REQUIRE_FALSE (text.empty());

    const auto manifest = xerum::model::decodeJson (text);
    INFO (manifest.error);
    REQUIRE (manifest.ok);
    REQUIRE (manifest.value.isObject());

    // What ticket 55 reads: the file's own description, the categories in browsing order, and one entry
    // per preset with the tables it plays.
    const auto* element = manifest.value.find ("format");
    REQUIRE (element != nullptr);
    CHECK (element->stringOr ("magic") == std::string (xerum::model::kPresetBlockMagic));
    CHECK (element->stringOr ("extension") == std::string (kPresetExtension));

    const auto* listed = manifest.value.find ("categories");
    REQUIRE (listed != nullptr);
    REQUIRE (listed->items.size() == categories().size());

    for (std::size_t i = 0; i < categories().size(); ++i)
        CHECK (listed->items[i].text == std::string (categories()[i]));

    const auto* entries = manifest.value.find ("presets");
    REQUIRE (entries != nullptr);
    REQUIRE (entries->items.size() == bank().size());

    const auto count = manifest.value.find ("presetCount");
    REQUIRE (count != nullptr);
    CHECK (static_cast<std::size_t> (count->number) == bank().size());

    std::map<std::string, const FactoryPreset*> byPath;

    for (const auto& preset : bank())
        byPath[presetFilePath (preset)] = &preset;

    for (const auto& entry : entries->items)
    {
        REQUIRE (entry.isObject());

        const auto relative = entry.stringOr ("path");
        INFO ("manifest entry: " << relative);

        const auto found = byPath.find (relative);
        REQUIRE (found != byPath.end());

        const auto& preset = *found->second;

        CHECK (entry.stringOr ("name") == preset.name);
        CHECK (entry.stringOr ("category") == preset.category);
        CHECK (entry.stringOr ("description") == preset.description);
        CHECK_FALSE (entry.stringOr ("origin").empty());
        CHECK_FALSE (entry.stringOr ("licence").empty());
        CHECK (entry.stringOr ("sha256").size() == 64);

        const auto* tables = entry.find ("tables");
        REQUIRE (tables != nullptr);

        std::vector<std::string> listedTables;

        for (const auto& table : tables->items)
            listedTables.push_back (table.text);

        CHECK (listedTables == tablesOf (preset));

        const auto* size = entry.find ("bytes");
        REQUIRE (size != nullptr);
        CHECK (static_cast<std::size_t> (size->number) == encodePresetFile (preset).size());
    }
}

TEST_CASE ("the manifest reader the browser is built over agrees with the file, and refuses a bad one",
           "[model][bank]")
{
    using namespace xerum::presets;

    const auto path = contentRoot() / kBankDirectory / kBankManifestName;
    const auto text = readText (path);
    REQUIRE_FALSE (text.empty());

    // The reader ticket `55`'s browser and the plugin's catalogue actually use, held to the same file the
    // bank's own test reads: one reader, in the bank's own module, so the browser cannot hold a different
    // opinion of the bank than the bank does.
    const auto manifest = readBankManifest (text);

    INFO (manifest.error);
    REQUIRE (manifest.ok);

    REQUIRE (manifest.presets.size() == bank().size());
    REQUIRE (manifest.categories.size() == categories().size());
    CHECK_FALSE (manifest.origin.empty());
    CHECK_FALSE (manifest.licence.empty());

    for (std::size_t i = 0; i < categories().size(); ++i)
        CHECK (manifest.categories[i] == std::string (categories()[i]));

    for (std::size_t i = 0; i < manifest.presets.size(); ++i)
    {
        const auto& entry = manifest.presets[i];
        const auto& preset = bank()[i];

        INFO ("entry " << i << ": " << entry.name);

        CHECK (entry.name == preset.name);
        CHECK (entry.category == preset.category);
        CHECK (entry.path == presetFilePath (preset));
        CHECK (entry.description == preset.description);
        CHECK (entry.sha256.size() == 64);
        CHECK (entry.bytes == encodePresetFile (preset).size());
    }

    // The path a browser opens is the manifest's own spelling joined under a content root — not a second
    // spelling of it, so a preset cannot be listed under one name and looked for under another.
    CHECK (manifestPathUnder ("/root", manifest.presets.front())
           == "/root/" + manifest.presets.front().path);

    // **And a manifest that is not one is refused, not half-read.** Each of these is a failure a tolerant
    // reader would hide: a missing list, a preset with no path, and a category the manifest does not
    // declare — the last being the one that would put a row behind a pill the browser does not draw.
    {
        const auto refused = [] (const std::string& json, const std::string& what)
        {
            const auto result = readBankManifest (json);

            INFO ("refused \"" << what << "\" with: " << result.error);
            CHECK_FALSE (result.ok);
            CHECK_FALSE (result.error.empty());
            CHECK (result.presets.empty());

            return result.error;
        };

        CHECK (refused ("not json at all", "not JSON").find ("JSON") != std::string::npos);
        CHECK (refused (R"({"presets": []})", "no categories").find ("categories") != std::string::npos);
        CHECK (refused (R"({"categories": ["Bass"], "presets": [{"name": "x", "category": "Bass"}]})",
                        "no path")
                   .find ("path")
               != std::string::npos);
        CHECK (refused (R"({"categories": ["Bass"], "presets": [{"name": "x", "path": "p", "category": "Lead"}]})",
                        "an undeclared category")
                   .find ("Lead")
               != std::string::npos);
    }
}
