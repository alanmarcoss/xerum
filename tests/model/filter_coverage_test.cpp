// Filter coverage, measured against the reference's own vocabulary, its factory bank, and its
// pole/zero payloads.
//
// Ticket `26`'s two measurable criteria:
//
//   * "types covering ~90% of corpus filter usage resolve to a model; the rest report" — so this
//     measures the vocabulary, the usage, and whether the report names what is left;
//   * "a PZ corpus file loads from a preset and renders, with the eight sections asserted" — with the
//     correction ticket `04` established: presets do not *reference* PZ files, they **inline** the same
//     object, so the payload arrives in the document rather than through a path.
//
// It lives beside the model tests because it needs both halves: the schema owns the vocabulary, the
// import report owns the claim, and the filter unit owns the truth.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "cbor.h"
#include "container.h"
#include "decompress.h"
#include "document.h"
#include "legacy.h"
#include "schema.h"

#include "filter.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace xerum::model;

namespace
{

std::vector<std::string> filterVocabulary()
{
    const auto* leaf = Schema::instance().describe ("VoiceFilter{N}/plainParams/kParamType");

    REQUIRE (leaf != nullptr);
    REQUIRE (leaf->values != nullptr);

    std::vector<std::string> names;

    for (std::size_t i = 0; i < leaf->valueCount; ++i)
        names.emplace_back (leaf->values[i]);

    return names;
}

/// What the catalogue claims, read the way the report reads it — exact names, because the filter
/// vocabulary is closed and carries no suffixes. This measures the report's behaviour rather than
/// restating it.
bool catalogueClaims (std::string_view name)
{
    const auto& models = SupportCatalogue::v1().filterModels;
    return std::find (models.begin(), models.end(), name) != models.end();
}

Value text (std::string value)
{
    Value v;
    v.type = Value::Type::String;
    v.text = std::move (value);
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

Value presetWithFilter (const std::string& filterType)
{
    return object ({ { "VoiceFilter0",
                       object ({ { "plainParams",
                                   object ({ { "kParamType", text (filterType) } }) } }) } });
}

bool reportNamesFilter (const std::string& filterType)
{
    const auto result = loadPreset (presetWithFilter (filterType), "4.0", Schema::instance(),
                                    SupportCatalogue::v1());

    if (! result.ok)
        return false;

    return std::any_of (result.report.entries.begin(), result.report.entries.end(),
                        [&] (const ReportEntry& entry) { return entry.item == filterType; });
}

struct DecodedPreset
{
    bool ok { false };
    LoadResult result;
};

DecodedPreset decodePreset (const std::filesystem::path& file)
{
    DecodedPreset out;

    std::ifstream in (file, std::ios::binary);

    if (! in.good())
        return out;

    std::vector<std::byte> raw ((std::size_t) std::filesystem::file_size (file));
    in.read (reinterpret_cast<char*> (raw.data()), (std::streamsize) raw.size());

    const auto container = readContainer (raw);

    if (! container.ok)
        return out;

    const auto payload = decompress (container.payload, container.codec, container.declaredSize);

    if (! payload.ok)
        return out;

    const auto decoded = container.codec == Codec::Zstd ? decodeCbor (payload.bytes)
                                                        : decodeLegacy (payload.bytes);

    if (! decoded.ok || decoded.bytesRead != payload.bytes.size())
        return out;

    out.result = loadPreset (decoded.value, container.schemaVersion, Schema::instance(),
                             SupportCatalogue::v1());
    out.ok = out.result.ok;
    return out;
}

constexpr const char* kCorpusRoot = "/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Presets";

} // namespace

TEST_CASE ("the filter vocabulary is classified, and the catalogue claims exactly what we build",
           "[filter-coverage]")
{
    const auto vocabulary = filterVocabulary();

    REQUIRE (vocabulary.size() == 83);      // ticket 04 counted 83 distinct type strings

    std::vector<std::string> unimplemented;
    std::size_t implemented = 0;

    for (const auto& name : vocabulary)
    {
        if (xerum::dsp::VoiceFilter::implements (name))
            ++implemented;
        else
            unimplemented.push_back (name);

        // The claim and the truth have to agree in both directions. A name the catalogue claims but the
        // unit cannot build is a silent drop; one the unit builds but the catalogue does not claim is a
        // false complaint in a report users are meant to trust.
        CHECK (catalogueClaims (name) == xerum::dsp::VoiceFilter::implements (name));
    }

    UNSCOPED_INFO ("filter vocabulary: " << vocabulary.size() << ", implemented " << implemented << " ("
                                         << (100.0 * static_cast<double> (implemented)
                                             / static_cast<double> (vocabulary.size()))
                                         << "%), unimplemented " << unimplemented.size());

    for (const auto& name : unimplemented)
        UNSCOPED_INFO ("  unimplemented: " << name);

    CHECK (implemented * 100 / vocabulary.size() >= 70);
}

TEST_CASE ("every unimplemented filter type is named in the import report", "[filter-coverage]")
{
    for (const auto& name : filterVocabulary())
    {
        if (xerum::dsp::VoiceFilter::implements (name))
            CHECK_FALSE (reportNamesFilter (name));
        else
            CHECK (reportNamesFilter (name));
    }
}

TEST_CASE ("filter type coverage over the factory bank, weighted by use", "[filter-coverage][corpus]")
{
    const std::filesystem::path root = kCorpusRoot;

    if (! std::filesystem::exists (root))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    std::map<std::string, std::size_t> uses;
    std::size_t presets = 0;

    for (const auto& entry : std::filesystem::recursive_directory_iterator (root))
    {
        if (! entry.is_regular_file() || entry.path().extension() != ".SerumPreset")
            continue;

        const auto decoded = decodePreset (entry.path());

        if (! decoded.ok)
            continue;

        ++presets;

        for (const auto& [path, value] : decoded.result.document.parameters)
        {
            if (value.type != Value::Type::String)
                continue;

            if (path.ends_with ("/plainParams/kParamType") && path.find ("VoiceFilter") != std::string::npos)
                ++uses[value.text];
        }
    }

    REQUIRE (presets > 0);

    std::size_t occurrences = 0;
    std::size_t covered = 0;
    std::map<std::string, std::size_t> missing;

    for (const auto& [name, count] : uses)
    {
        occurrences += count;

        if (xerum::dsp::VoiceFilter::implements (name))
            covered += count;
        else
            missing[name] = count;
    }

    const auto share = occurrences > 0 ? 100.0 * static_cast<double> (covered)
                                             / static_cast<double> (occurrences)
                                       : 0.0;

    UNSCOPED_INFO ("corpus: " << presets << " presets, " << uses.size() << " distinct filter types, "
                              << occurrences << " filter slots used");
    UNSCOPED_INFO ("occurrence-weighted coverage: " << covered << "/" << occurrences << " (" << share
                                                    << "%)");

    std::vector<std::pair<std::size_t, std::string>> ranked;

    for (const auto& [name, count] : missing)
        ranked.emplace_back (count, name);

    std::sort (ranked.begin(), ranked.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });

    for (std::size_t i = 0; i < ranked.size() && i < 12; ++i)
    {
        UNSCOPED_INFO ("  used but unbuilt: " << ranked[i].second << " (" << ranked[i].first << ")");

        // Used in the bank and not built means it has to be reported, by name.
        CHECK (reportNamesFilter (ranked[i].second));
    }

    // Ticket 12's target for the family cut, and the number to record in the ticket.
    CHECK (share > 85.0);
}

TEST_CASE ("a pole/zero filter loads from a real preset and renders", "[filter-coverage][corpus]")
{
    const std::filesystem::path root = kCorpusRoot;

    if (! std::filesystem::exists (root))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    // Ticket 04: `PZ_SVF` inlines the drawn response under the module's `PZs` key, and no preset
    // references a `PZ Filter/*` file. So the payload arrives in the document, which is what this reads.
    std::size_t presetsWithInlinePz = 0;
    std::size_t sectionsSeen = 0;
    std::size_t identitySections = 0;
    std::size_t identitySectionsThatAreType2 = 0;

    for (const auto& entry : std::filesystem::recursive_directory_iterator (root))
    {
        if (! entry.is_regular_file() || entry.path().extension() != ".SerumPreset")
            continue;

        const auto decoded = decodePreset (entry.path());

        if (! decoded.ok)
            continue;

        const auto& parameters = decoded.result.document.parameters;

        // Which slot, if any, is a `PZ_SVF` with an inline response.
        for (const auto& [path, value] : parameters)
        {
            if (value.type != Value::Type::String || value.text != "PZ_SVF")
                continue;

            const auto slot = path.substr (0, path.find ("/plainParams"));

            if (parameters.find (slot + "/PZs[0]/fp[0]") == parameters.end())
                continue;

            ++presetsWithInlinePz;

            xerum::dsp::PoleZeroDescription description;

            for (std::size_t object = 0; object < xerum::dsp::PoleZeroDescription::kMaxObjects; ++object)
            {
                const auto prefix = slot + "/PZs[" + std::to_string (object) + "]";

                if (parameters.find (prefix + "/fp[0]") == parameters.end())
                    break;

                double poleFrequency[xerum::dsp::PoleZeroDescription::kSectionsPerObject] {};
                double zeroFrequency[xerum::dsp::PoleZeroDescription::kSectionsPerObject] {};
                double poleQ[xerum::dsp::PoleZeroDescription::kSectionsPerObject] {};
                double zeroQ[xerum::dsp::PoleZeroDescription::kSectionsPerObject] {};
                double gain[xerum::dsp::PoleZeroDescription::kSectionsPerObject] {};
                int type[xerum::dsp::PoleZeroDescription::kSectionsPerObject] {};

                const auto read = [&] (const std::string& field, double* out)
                {
                    for (std::size_t i = 0; i < xerum::dsp::PoleZeroDescription::kSectionsPerObject; ++i)
                    {
                        const auto found = parameters.find (prefix + "/" + field + "[" + std::to_string (i)
                                                            + "]");

                        if (found != parameters.end() && found->second.type == Value::Type::Float)
                            out[i] = found->second.real;
                    }
                };

                read ("fp", poleFrequency);
                read ("fz", zeroFrequency);
                read ("qp", poleQ);
                read ("qz", zeroQ);
                read ("gain", gain);

                for (std::size_t i = 0; i < xerum::dsp::PoleZeroDescription::kSectionsPerObject; ++i)
                {
                    const auto found = parameters.find (prefix + "/types[" + std::to_string (i) + "]");

                    if (found != parameters.end() && found->second.type == Value::Type::Int)
                        type[i] = static_cast<int> (found->second.integer);
                }

                xerum::dsp::appendPoleZeroObject (description, poleFrequency, zeroFrequency, poleQ, zeroQ,
                                                  gain, type);
            }

            // Eight sections per object, and up to four objects — never a partial object.
            REQUIRE (description.count > 0);
            REQUIRE (description.count % xerum::dsp::PoleZeroDescription::kSectionsPerObject == 0);
            REQUIRE (description.count <= xerum::dsp::PoleZeroDescription::kMaxSections);

            for (std::size_t i = 0; i < description.count; ++i)
            {
                const auto& section = description.sections[i];
                ++sectionsSeen;

                // Ticket 04's reading of the redundant `types` label, checked on the real payloads
                // rather than on a synthetic one.
                if (section.poleFrequency == section.zeroFrequency && section.poleQ == section.zeroQ)
                {
                    ++identitySections;

                    if (section.type == 2)
                        ++identitySectionsThatAreType2;
                }
            }

            xerum::dsp::VoiceFilter filter;
            filter.prepare (48000.0);
            filter.setSpec (xerum::dsp::filterSpecFromName ("PZ_SVF"));
            filter.setPoleZero (description);

            double peak = 0.0;

            for (int i = 0; i < 4096; ++i)
            {
                const auto y = static_cast<double> (
                    filter.process (static_cast<float> (0.5 * std::sin (0.02 * static_cast<double> (i)))));

                REQUIRE (std::isfinite (y));
                peak = std::max (peak, std::abs (y));
            }

            UNSCOPED_INFO (entry.path().filename().string() << ": " << description.count
                                                            << " sections, peak " << peak);

            // It renders, and it is a filter rather than a wire: a drawn response that is exactly a
            // pass-through would mean the sections were read but never applied.
            CHECK (peak > 0.0);
        }
    }

    UNSCOPED_INFO ("presets with an inline pole/zero response: " << presetsWithInlinePz << ", sections "
                                                                 << sectionsSeen << ", identity geometry "
                                                                 << identitySections << " of which type 2: "
                                                                 << identitySectionsThatAreType2);

    // Ticket 04 found 7 factory slots, and this is the same corpus.
    CHECK (presetsWithInlinePz >= 1);
    CHECK (sectionsSeen > 0);

    // The redundant label agrees with the geometry where the geometry is identity — which is what makes
    // it safe to read the response from the geometry and keep `types` as a label. Not exactly, though,
    // and the gap is worth having measured rather than assumed: ticket `04` saw 128 of 149, and on these
    // payloads it is 233 of 234. So the label is close to derivable and is not authoritative — which is
    // exactly why the response comes from the geometry.
    if (identitySections > 0)
    {
        UNSCOPED_INFO ("identity geometry that is labelled type 2: " << identitySectionsThatAreType2 << " of "
                                                                     << identitySections);

        CHECK (identitySectionsThatAreType2 * 100 >= identitySections * 90);
    }
}
