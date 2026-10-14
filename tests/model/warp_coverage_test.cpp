// Warp coverage, measured against the reference's own vocabulary and against its factory bank.
//
// Ticket `25`'s two measurable criteria, and they are two views of one claim:
//
//   * over the **vocabulary** — every warp mode name the reference's parameter model knows for the
//     wavetable oscillator: is it implemented, and if not, does the import report *name* it?
//   * over the **corpus** — the same question weighted by how often the factory bank actually uses each
//     mode, because covering 54 of 59 names says nothing if the five uncovered ones are the five most
//     used.
//
// This sits beside the model tests rather than the DSP ones because it needs both sides at once: the
// schema owns the vocabulary, the import report owns the claim, and the warp unit owns the truth about
// what is implemented. The claim and the truth are two lists in two libraries that deliberately do not
// link each other, so this file is where they are held together — a drift between them is either a
// silent drop (claimed, not implemented) or a false report (implemented, not claimed), and both are
// bugs this catches.

#include <catch2/catch_test_macros.hpp>

#include "cbor.h"
#include "container.h"
#include "decompress.h"
#include "document.h"
#include "legacy.h"
#include "schema.h"

#include "warp.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace xerum::model;

namespace
{

/// The four leaves the wavetable oscillator's warp menu lives on. Two menus and two variant knobs,
/// because the reference exposes two warp slots per oscillator.
const std::vector<std::string> kWarpLeaves = { "Oscillator{N}/WTOsc{N}/plainParams/kParamWarpMenu",
                                               "Oscillator{N}/WTOsc{N}/plainParams/kParamWarpMenu2",
                                               "Oscillator{N}/WTOsc{N}/plainParams/kParamWarpVar",
                                               "Oscillator{N}/WTOsc{N}/plainParams/kParamWarpVar2" };

/// Every warp mode name the schema allows on a wavetable oscillator.
std::vector<std::string> warpVocabulary()
{
    std::set<std::string> names;

    for (const auto& path : kWarpLeaves)
    {
        const auto* leaf = Schema::instance().describe (path);

        if (leaf == nullptr || leaf->kind != LeafKind::String || leaf->values == nullptr)
            continue;      // the variant knobs are floats, not menus; only the menus carry names

        for (std::size_t i = 0; i < leaf->valueCount; ++i)
            names.emplace (leaf->values[i]);
    }

    return { names.begin(), names.end() };
}

/// What the *catalogue* claims, which is the claim the import report acts on. Read through the same
/// prefix test the report uses, so this measures the report's behaviour and not a restatement of it.
bool catalogueClaims (std::string_view name)
{
    for (const auto& prefix : SupportCatalogue::v1().warpPrefixes)
        if (name.starts_with (prefix))
            return true;

    return false;
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

/// A minimal preset carrying one warp mode on the first wavetable oscillator.
Value presetUsing (const std::string& warpMode)
{
    return object ({ { "Oscillator0",
                       object ({ { "WTOsc0", object ({ { "plainParams",
                                                        object ({ { "kParamWarpMenu",
                                                                    text (warpMode) } }) } }) } }) } });
}

/// Does importing a preset that asks for this warp mode produce a report entry *naming* the mode?
bool reportNames (const std::string& warpMode)
{
    const auto result = loadPreset (presetUsing (warpMode), "4.0", Schema::instance(),
                                    SupportCatalogue::v1());

    if (! result.ok)
        return false;

    return std::any_of (result.report.entries.begin(), result.report.entries.end(),
                        [&] (const ReportEntry& entry) { return entry.item == warpMode; });
}

struct DecodedPreset
{
    bool ok { false };
    LoadResult result;
};

/// The whole pipeline for one file: container, decompression, decoding, document.
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

TEST_CASE ("the wavetable warp vocabulary is classified, and nothing is claimed that is not implemented",
           "[warp-coverage]")
{
    // The vocabulary comes from the reference's own parameter model, not from a list typed out here: a
    // mode the reference offers that we have never heard of has to be classified too, and this is how it
    // gets noticed.
    const auto vocabulary = warpVocabulary();

    REQUIRE (vocabulary.size() > 40);   // this is the reference's 59 modes; a shrink means the schema broke

    std::vector<std::string> unimplemented;
    std::size_t implemented = 0;

    for (const auto& name : vocabulary)
    {
        const auto mode = xerum::dsp::warpModeFromName (name);

        if (mode == xerum::dsp::WarpMode::Unknown)
        {
            unimplemented.push_back (name);
            continue;
        }

        ++implemented;

        // Implemented means the family is real, not that the name was recognised.
        CHECK (xerum::dsp::warpModeFamilyName (mode) != "unknown");

        // A mode the unit implements must be claimed by the catalogue, or the import report complains
        // about something we can in fact do.
        CHECK (catalogueClaims (name));
    }

    for (const auto& name : unimplemented)
    {
        // ...and a mode the unit does *not* implement must not be claimed, or the preset is silently
        // wrong: the catalogue says we ship it and nothing in the engine acts on it.
        CHECK_FALSE (catalogueClaims (name));

        // `kNoWarp` is not a mode, so it can never appear here.
        CHECK (name != "kNoWarp");
    }

    UNSCOPED_INFO ("vocabulary " << vocabulary.size() << ", implemented " << implemented << " ("
                                 << (100.0 * static_cast<double> (implemented)
                                     / static_cast<double> (vocabulary.size()))
                                 << "%), unimplemented " << unimplemented.size());

    for (const auto& name : unimplemented)
        UNSCOPED_INFO ("  unimplemented: " << name);

    // The set is named rather than counted, so that shipping one more mode is a deliberate edit to this
    // list and not an accident. Each of the five is unimplemented for a stated reason:
    //
    //   kDLM        undocumented in the shipped guide (ticket 02)
    //   kEvenOdd    semantics not established
    //   kRemap_*    needs the editable remap curve graph, which v1 does not build
    CHECK (unimplemented == std::vector<std::string> { "kDLM", "kEvenOdd", "kRemap_1", "kRemap_3",
                                                       "kRemap_4" });

    // And the cut has to hold: a v1 that cannot do 90% of the wavetable oscillator's warp names is not
    // the v1 ticket 12 cut.
    CHECK (implemented * 100 / vocabulary.size() >= 90);
}

TEST_CASE ("every unimplemented warp mode is named in the import report", "[warp-coverage]")
{
    // The contract's rule is that nothing is dropped silently. "Some warp was not applied" is not
    // actionable; naming the mode is, so the assertion is on the name, not on the entry count.
    for (const auto& name : warpVocabulary())
    {
        const auto mode = xerum::dsp::warpModeFromName (name);

        if (mode == xerum::dsp::WarpMode::Unknown)
            CHECK (reportNames (name));
        else
            CHECK_FALSE (reportNames (name));
    }
}

TEST_CASE ("an unimplemented warp mode is reported at high severity, with its module path",
           "[warp-coverage]")
{
    // Severity matters: a warp that was not applied changes the sound, so it is not something to mention
    // in passing. The module path is what lets a user find the oscillator it was on.
    const auto result = loadPreset (presetUsing ("kRemap_3"), "4.0", Schema::instance(),
                                    SupportCatalogue::v1());

    REQUIRE (result.ok);

    const auto found = std::find_if (result.report.entries.begin(), result.report.entries.end(),
                                     [] (const ReportEntry& entry) { return entry.item == "kRemap_3"; });

    REQUIRE (found != result.report.entries.end());
    CHECK (found->severity == Severity::High);
    CHECK (found->modulePath == "Oscillator0/WTOsc0");
    CHECK (found->reason == "warp family is not shipped in v1");
}

TEST_CASE ("the reference offers warp in two slots per wavetable oscillator, and nowhere else",
           "[warp-coverage]")
{
    // Ticket 25's acceptance says "warp works on all three oscillator slots and on NOISE where the
    // reference offers it". Both halves are facts about the reference's parameter model rather than
    // things to assume, and the answers are different from what the wording suggests:
    //
    //   * the three oscillator slots are `Oscillator0/1/2`, each with its own `WTOsc` instance, and every
    //     instance carries the *same* two warp slots — so "all three slots" costs nothing beyond the
    //     oscillator owning its warps, which is what would otherwise be the bug;
    //   * the noise and sub oscillators have **no warp at all** in the reference's model, so there is
    //     nothing to implement for that half of the sentence.
    const auto& schema = Schema::instance();

    for (const auto* leaf : { "kParamWarpMenu", "kParamWarpMenu2" })
    {
        const auto path = std::string ("Oscillator{N}/WTOsc{N}/plainParams/") + leaf;
        REQUIRE (schema.describe (path) != nullptr);
    }

    // Instances are the same shape: slot 2 of oscillator 3 normalises onto the same parameter, which is
    // what makes "all three slots" true rather than three special cases.
    for (const auto* instance : { "0", "1", "2" })
    {
        const auto concrete = std::string ("Oscillator") + instance + "/WTOsc" + instance
                              + "/plainParams/kParamWarpMenu";
        CHECK (schema.normalise (concrete) == "Oscillator{N}/WTOsc{N}/plainParams/kParamWarpMenu");
    }

    // And the engines that have none.
    for (const auto* engine : { "NoiseOsc", "SubOsc" })
    {
        const auto path = std::string ("Oscillator{N}/") + engine + "{N}/plainParams/kParamWarpMenu";
        CHECK (schema.describe (path) == nullptr);
    }
}

TEST_CASE ("warp coverage over the factory bank, weighted by use", "[warp-coverage][corpus]")
{
    // The measurement ticket 25 asks to have recorded. Mode *names* covered is one number; what a user
    // hears is the other one, and the two differ as soon as the uncovered modes are rare or common.
    const std::filesystem::path root = kCorpusRoot;

    if (! std::filesystem::exists (root))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    std::map<std::string, std::size_t> uses;
    std::size_t presets = 0;
    std::size_t bothSlots = 0;        // wavetable oscillators, which is what the two warp slots are for
    std::size_t bothSlotsAnyEngine = 0;

    for (const auto& entry : std::filesystem::recursive_directory_iterator (root))
    {
        if (! entry.is_regular_file() || entry.path().extension() != ".SerumPreset")
            continue;

        const auto decoded = decodePreset (entry.path());

        if (! decoded.ok)
            continue;

        ++presets;

        // Every warp slot on every oscillator, which is how the reference's own usage counts are taken.
        bool wtSlot1 = false;
        bool wtSlot2 = false;
        bool anySlot1 = false;
        bool anySlot2 = false;

        for (const auto& [path, value] : decoded.result.document.parameters)
        {
            if (value.type != Value::Type::String || value.text == "kNoWarp")
                continue;

            // The flags are per engine, not per preset: a granular oscillator using slot 1 while a
            // wavetable oscillator uses slot 2 is not a wavetable oscillator using both slots, and
            // counting it as one would inflate exactly the number this test exists to measure.
            const auto wavetableSlot = path.find ("/WTOsc") != std::string::npos;

            if (path.ends_with ("/plainParams/kParamWarpMenu"))
            {
                ++uses[value.text];
                anySlot1 = true;
                wtSlot1 = wtSlot1 || wavetableSlot;
            }
            else if (path.ends_with ("/plainParams/kParamWarpMenu2"))
            {
                ++uses[value.text];
                anySlot2 = true;
                wtSlot2 = wtSlot2 || wavetableSlot;
            }

        }

        if (wtSlot1 && wtSlot2)
            ++bothSlots;

        if (anySlot1 && anySlot2)
            ++bothSlotsAnyEngine;
    }

    REQUIRE (presets > 0);

    std::size_t occurrences = 0;
    std::size_t coveredOccurrences = 0;
    std::map<std::string, std::size_t> unimplementedUses;

    for (const auto& [name, count] : uses)
    {
        occurrences += count;

        if (xerum::dsp::warpModeFromName (name) == xerum::dsp::WarpMode::Unknown)
            unimplementedUses[name] = count;
        else
            coveredOccurrences += count;
    }

    const auto share = occurrences > 0 ? 100.0 * static_cast<double> (coveredOccurrences)
                                             / static_cast<double> (occurrences)
                                       : 0.0;

    UNSCOPED_INFO ("corpus: " << presets << " presets, " << uses.size() << " distinct warp modes, "
                              << occurrences << " warp slots used");
    UNSCOPED_INFO ("presets using both warp slots at once: " << bothSlots << " wavetable, "
                                                             << bothSlotsAnyEngine << " any engine");

    // The number that decided the oscillator carries *two* warp slots rather than one: a quarter of the
    // bank would otherwise be half a preset.
    CHECK (bothSlots > 100);
    UNSCOPED_INFO ("occurrence-weighted coverage: " << coveredOccurrences << "/" << occurrences << " ("
                                                    << share << "%)");

    for (const auto& [name, count] : unimplementedUses)
    {
        UNSCOPED_INFO ("  unimplemented in use: " << name << " (" << count << " uses)");

        // Used in the bank *and* unimplemented means it has to be reported — and it is, per mode rather
        // than per preset, so a user sees the name.
        CHECK (reportNames (name));
    }

    // Ticket 12 cut the families behind ~90% of usage. The measured number is the check on that cut.
    CHECK (share > 90.0);
}
