// The rack topology across the reference's whole corpus, re-derived here rather than trusted.
//
// Ticket 04 validated one model of the flat `FX` array — **serial top-to-bottom, with a splitter
// claiming the next `sum(kParamModuleCount1..3)` slots as parallel bands, and nothing nested** — and
// reported that it consumes **942 of 942** non-empty racks with zero leftovers. This file re-derives
// that result in our own code, over the same files, and reports anything the walk cannot account for.
//
// Two things it also pins down, because the first one changes the model and the second is the cut:
//
//   * an **absent `kParamModuleCount` means zero**, not one. Ticket 04 wrote that a band was never
//     empty; the corpus says otherwise, and 14 racks overrun their own array under the other reading.
//     The test walks the corpus twice — once each way — so the correction is demonstrated, not asserted.
//   * the **usage table itself**: the per-class counts below are `.scratch/xerum/tools/corpus-usage.py`'s
//     numbers, reproduced from our decoder, and the shipped/deferred split is checked against them.
//
// It lives beside the model tests because it needs both halves: the container pipeline owns the bytes,
// and `xerum::dsp` owns the class table the cut is stated in.

#include <catch2/catch_test_macros.hpp>

#include "cbor.h"
#include "container.h"
#include "decompress.h"
#include "document.h"
#include "legacy.h"
#include "schema.h"
#include "value.h"

#include "fx_rack.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace xerum::model;

namespace
{

constexpr const char* kReferenceRoot = "/Library/Audio/Presets/Xfer Records/Serum 2 Presets";

constexpr std::string_view kCountNames[3] { "kParamModuleCount1", "kParamModuleCount2", "kParamModuleCount3" };

/// Decode one container into the format-neutral tree both wire formats produce. Returns false for
/// anything unreadable, which the caller counts rather than throwing over — the corpus contains
/// transient files and the walk must survive them.
bool decodeFile (const std::filesystem::path& file, Value& out, bool& legacy)
{
    std::error_code error;
    const auto size = std::filesystem::file_size (file, error);

    if (error)
        return false;

    std::ifstream in (file, std::ios::binary);

    if (! in.good())
        return false;

    std::vector<std::byte> raw (static_cast<std::size_t> (size));
    in.read (reinterpret_cast<char*> (raw.data()), static_cast<std::streamsize> (raw.size()));

    const auto container = readContainer (raw);

    if (! container.ok)
        return false;

    const auto payload = decompress (container.payload, container.codec, container.declaredSize);

    if (! payload.ok)
        return false;

    legacy = container.codec != Codec::Zstd;
    const auto decoded = legacy ? decodeLegacy (payload.bytes) : decodeCbor (payload.bytes);

    if (! decoded.ok || decoded.bytesRead != payload.bytes.size())
        return false;

    out = decoded.value;
    return true;
}

/// A slot's numeric `type`. Anything outside 0…15 is a class the model has never seen.
std::size_t slotClassId (const Value& entry)
{
    const Value* type = entry.find ("type");
    double value = 0.0;

    if (type != nullptr && type->numberAsDouble (value) && value >= 0.0)
        return static_cast<std::size_t> (value);

    return 16;
}

/// A splitter's band count. `absent` is the rule under test: the corpus needs **zero**, and reading it
/// as one makes 14 racks claim more slots than they hold.
double bandCount (const Value& entry, std::string_view name, double absent)
{
    for (const auto& [key, value] : entry.entries)
    {
        if (! value.isMap())
            continue;

        const Value* params = value.find ("plainParams");

        if (params == nullptr || ! params->isMap())
            continue;

        const Value* count = params->find (name);
        double number = 0.0;

        if (count != nullptr && count->numberAsDouble (number))
            return number;
    }

    return absent;
}

struct Stats
{
    std::size_t racks { 0 };
    std::size_t legacyRacks { 0 };
    std::size_t assetRacks { 0 };
    std::size_t overrun { 0 };
    std::size_t nested { 0 };
    std::size_t unrecognised { 0 };
    std::size_t undecoded { 0 };
    std::size_t maxSlots { 0 };
    std::map<std::string, std::size_t> presetUses {};
    std::map<std::string, std::size_t> corpusUses {};

    std::size_t totalPresetUses() const
    {
        std::size_t total = 0;

        for (const auto& [name, count] : presetUses)
            total += count;

        return total;
    }
};

/// One pass over a rack's flat array, in the format's own grammar: a slot is consumed, and a splitter
/// consumes its bands' slots next. Returns how far the walk reached, which for a well-formed array is
/// the end.
std::size_t walkSlots (const std::vector<Value>& slots, Stats& stats, double absentCount, bool countUsage)
{
    std::size_t position = 0;

    while (position < slots.size())
    {
        const Value& entry = slots[position];
        ++position;

        const auto id = slotClassId (entry);

        if (id >= 16)
        {
            ++stats.unrecognised;
            continue;
        }

        const auto cls = xerum::dsp::fxClassFromTypeId (static_cast<int> (id));

        if (countUsage)
            ++stats.corpusUses[std::string (xerum::dsp::fxClassName (cls))];

        if (! xerum::dsp::fxClassIsSplitter (cls))
            continue;

        for (int band = 0; band < xerum::dsp::fxSplitterBandCount (cls); ++band)
        {
            const auto count = static_cast<std::size_t> (
                std::max (bandCount (entry, kCountNames[static_cast<std::size_t> (band)], absentCount), 0.0));

            if (position + count > slots.size())
            {
                ++stats.overrun;
                return slots.size();
            }

            // A splitter inside a band would be nesting, which ticket 04 found nowhere in the corpus.
            for (std::size_t i = position; i < position + count; ++i)
                if (xerum::dsp::fxClassIsSplitter (
                        xerum::dsp::fxClassFromTypeId (static_cast<int> (slotClassId (slots[i])))))
                    ++stats.nested;

            position += count;
        }
    }

    return position;
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

Value list (std::vector<Value> items)
{
    Value v;
    v.type = Value::Type::Array;
    v.items = std::move (items);
    return v;
}

Value number (double value)
{
    Value v;
    v.type = Value::Type::Float;
    v.real = value;
    return v;
}

Value integer (long long value)
{
    Value v;
    v.type = Value::Type::Int;
    v.integer = value;
    return v;
}

/// A payload with one rack holding one slot of `className`.
Value presetWithEffect (std::string_view className, int typeId)
{
    return object ({ { "FXRack0",
                       object ({ { "FX",
                                   list ({ object ({ { std::string (className), object ({}) },
                                                     { "type", integer (typeId) } }) }) } }) } });
}

bool reportNamesHigh (const LoadResult& result, std::string_view item)
{
    return std::any_of (result.report.entries.begin(), result.report.entries.end(),
                        [&] (const ReportEntry& entry)
                        { return entry.severity == Severity::High && entry.item == item; });
}

} // namespace

TEST_CASE ("the class table, the catalogue and the cut are one statement", "[fx-topology]")
{
    const auto& catalogue = SupportCatalogue::v1();

    REQUIRE (catalogue.effects.size() + catalogue.deferredEffects.size() == 16);

    for (int id = 0; id < 16; ++id)
    {
        const auto cls = xerum::dsp::fxClassFromTypeId (id);
        const std::string name (xerum::dsp::fxClassName (cls));

        const bool shipped = xerum::dsp::fxClassIsShipped (cls);
        const bool listed = std::find (catalogue.effects.begin(), catalogue.effects.end(), name)
                            != catalogue.effects.end();
        const bool deferred = std::find (catalogue.deferredEffects.begin(), catalogue.deferredEffects.end(), name)
                              != catalogue.deferredEffects.end();

        // The catalogue is the report's claim; the DSP is the truth. They have to agree both ways, or a
        // preset is either silently dropped or falsely complained about.
        CHECK (listed == shipped);
        CHECK (deferred == ! shipped);
        CHECK (listed != deferred);
    }

    REQUIRE (catalogue.deferredEffects.size() == 2);
    CHECK (catalogue.deferredEffects[0] == "FXSplit3");
    CHECK (catalogue.deferredEffects[1] == "FXSplitMS");
}

TEST_CASE ("the two deferred classes report by name, and shipped ones stay quiet", "[fx-topology]")
{
    const auto& catalogue = SupportCatalogue::v1();

    for (const auto cls : xerum::dsp::deferredFxClasses())
    {
        const std::string name (xerum::dsp::fxClassName (cls));
        const auto result = loadPreset (presetWithEffect (name, xerum::dsp::fxTypeId (cls)), "4.0",
                                        Schema::instance(), catalogue);

        REQUIRE (result.ok);
        UNSCOPED_INFO ("deferred class " << name << " reports: " << result.report.entries.size() << " entr(ies)");
        CHECK (reportNamesHigh (result, name));
    }

    for (const auto cls : xerum::dsp::shippedFxClasses())
    {
        const std::string name (xerum::dsp::fxClassName (cls));
        const auto result = loadPreset (presetWithEffect (name, xerum::dsp::fxTypeId (cls)), "4.0",
                                        Schema::instance(), catalogue);

        REQUIRE (result.ok);
        CHECK (result.report.count (Severity::High) == 0);
    }
}

TEST_CASE ("every parameter the corpus stores for an effect is one the effect knows", "[fx-topology][corpus]")
{
    const std::filesystem::path root = kReferenceRoot;

    if (! std::filesystem::exists (root))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    // class name -> the distinct `kParam...` names (and other stored names) the corpus uses for it,
    // with one sample value each.
    struct Sample
    {
        std::string name;
        double value { 0.0 };
        bool isText { false };
        std::string text {};
    };

    std::map<std::string, std::map<std::string, Sample>> vocabulary;
    std::size_t slots = 0;

    const auto visit = [&] (const std::filesystem::path& file)
    {
        Value payload;
        bool legacy = false;

        if (! decodeFile (file, payload, legacy) || ! payload.isMap())
            return;

        for (const auto& [rackKey, module] : payload.entries)
        {
            if (! rackKey.starts_with ("FXRack") || ! module.isMap())
                continue;

            const Value* fx = module.find ("FX");

            if (fx == nullptr || ! fx->isArray())
                continue;

            for (const auto& entry : fx->items)
            {
                if (! entry.isMap())
                    continue;

                if (entry.find ("type") == nullptr)
                    continue;

                const Value* type = entry.find ("type");
                double typeValue = 0.0;

                if (type == nullptr || ! type->numberAsDouble (typeValue))
                    continue;

                const auto id = static_cast<std::size_t> (typeValue);

                if (id >= 16)
                    continue;

                const std::string className (xerum::dsp::fxClassName (xerum::dsp::fxClassFromTypeId (
                    static_cast<int> (id))));
                auto& samples = vocabulary[className];
                ++slots;

                for (const auto& [key, value] : entry.entries)
                {
                    if (! value.isMap())
                        continue;

                    const Value* params = value.find ("plainParams");

                    const auto record = [&] (const std::string& name, const Value& leaf)
                    {
                        if (samples.count (name) != 0)
                            return;

                        Sample sample;
                        sample.name = name;

                        if (leaf.isText())
                        {
                            sample.isText = true;
                            sample.text = leaf.text;
                        }
                        else if (! leaf.numberAsDouble (sample.value))
                            return;

                        samples.emplace (name, sample);
                    };

                    if (params != nullptr && params->isMap())
                        for (const auto& [name, leaf] : params->entries)
                            record (name, leaf);
                }
            }
        }
    };

    // The **factory** bank: 626 presets, which is what ticket 04 and `corpus-usage.py` measured. The
    // sibling `User/` tree is where a user's own presets live — and where a concurrent process writes
    // them — so counting it would make this test about whatever else is running on the machine.
    for (const auto& entry : std::filesystem::recursive_directory_iterator (root / "Presets" / "Factory"))
        if (entry.is_regular_file() && entry.path().extension() == ".SerumPreset")
            visit (entry.path());

    for (const auto& entry : std::filesystem::recursive_directory_iterator (root / "Effect Chains"))
        if (entry.is_regular_file() && entry.path().extension() == ".SerumFXRack")
            visit (entry.path());

    REQUIRE (! vocabulary.empty());

    // Every class the corpus uses is a class the table knows, and every parameter name the corpus
    // stores for a shipped class is one the effect accepts. A name it rejected would be reported as an
    // unknown parameter — in every ordinary preset that uses it.
    for (const auto& [className, samples] : vocabulary)
    {
        const auto cls = xerum::dsp::fxClassFromName (className);
        REQUIRE (cls != xerum::dsp::FxClass::Unknown);

        if (! xerum::dsp::fxClassIsShipped (cls))
            continue;

        xerum::dsp::FxRack rack;
        rack.prepare (48000.0, 512);

        xerum::dsp::FxSlot slot;
        slot.cls = cls;

        for (const auto& [name, sample] : samples)
        {
            if (xerum::dsp::fxClassIsSplitter (cls)
                && (name == "kParamModuleCount1" || name == "kParamModuleCount2"
                    || name == "kParamModuleCount3"))
                continue;   // re-added below as zero, so the splitter claims no bands

            if (sample.isText)
                slot.texts.emplace_back (name, sample.text);
            else
                slot.numbers.emplace_back (name, sample.value);
        }

        if (xerum::dsp::fxClassIsSplitter (cls))
            for (const auto* name : { "kParamModuleCount1", "kParamModuleCount2", "kParamModuleCount3" })
                slot.numbers.emplace_back (name, 0.0);

        rack.setSlots ({ slot });

        for (const auto& unresolved : rack.unresolved())
            UNSCOPED_INFO (className << " stores a name the effect does not know: " << unresolved);

        CHECK (rack.unresolved().empty());
    }

    UNSCOPED_INFO ("effect slots walked: " << slots << " across " << vocabulary.size() << " classes");
    CHECK (vocabulary.size() == 16);
}

TEST_CASE ("every rack in the reference's corpus reconstructs, and the usage table follows", "[fx-topology][corpus]")
{
    const std::filesystem::path root = kReferenceRoot;

    if (! std::filesystem::exists (root))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    Stats stats;
    Stats permissive;   // the same walk read the other way: absence of a module count means one

    const auto visit = [&] (const std::filesystem::path& file, bool isPreset)
    {
        Value payload;
        bool legacy = false;

        if (! decodeFile (file, payload, legacy))
        {
            ++stats.undecoded;
            return;
        }

        if (! payload.isMap())
            return;

        for (const auto& [key, module] : payload.entries)
        {
            if (! key.starts_with ("FXRack") || ! module.isMap())
                continue;

            const Value* fx = module.find ("FX");

            if (fx == nullptr || ! fx->isArray() || fx->items.empty())
                continue;

            ++stats.racks;
            ++permissive.racks;

            if (legacy)
                ++stats.legacyRacks;

            if (! isPreset)
                ++stats.assetRacks;

            stats.maxSlots = std::max (stats.maxSlots, fx->items.size());

            // The usage table, per class, over presets alone — `corpus-usage.py`'s FX table.
            if (isPreset)
                for (const auto& entry : fx->items)
                {
                    const auto id = slotClassId (entry);

                    if (id < 16)
                        ++stats.presetUses[std::string (
                            xerum::dsp::fxClassName (xerum::dsp::fxClassFromTypeId (static_cast<int> (id))))];
                }

            walkSlots (fx->items, stats, 0.0, true);
            walkSlots (fx->items, permissive, 1.0, false);
        }
    };

    for (const auto& entry : std::filesystem::recursive_directory_iterator (root / "Presets" / "Factory"))
        if (entry.is_regular_file() && entry.path().extension() == ".SerumPreset")
            visit (entry.path(), true);

    for (const auto& entry : std::filesystem::recursive_directory_iterator (root / "Effect Chains"))
        if (entry.is_regular_file() && entry.path().extension() == ".SerumFXRack")
            visit (entry.path(), false);

    UNSCOPED_INFO ("racks: " << stats.racks << " non-empty (" << stats.assetRacks << " of them rack assets, "
                             << stats.legacyRacks << " of them legacy codec 0), longest FX array "
                             << stats.maxSlots << " slots");
    UNSCOPED_INFO ("leftovers/overruns: " << stats.overrun << ", nested splitters: " << stats.nested
                                          << ", unrecognised classes: " << stats.unrecognised
                                          << ", undecoded files: " << stats.undecoded);

    for (const auto& [name, count] : stats.corpusUses)
        UNSCOPED_INFO ("  " << name << ": " << count << " occurrences in the corpus");

    // The model, re-derived: every non-empty rack is consumed exactly, nothing nests, and no class
    // appears that the table does not know.
    CHECK (stats.overrun == 0);
    CHECK (stats.nested == 0);
    CHECK (stats.unrecognised == 0);
    CHECK (stats.undecoded == 0);

    // Ticket 04's published figure, independently: 942 non-empty racks across the presets and the
    // factory rack assets — the five legacy codec-0 rack assets included, which that tool did not read,
    // is 947.
    CHECK (stats.racks - stats.legacyRacks == 942);
    CHECK (stats.racks == 947);
    CHECK (stats.legacyRacks == 5);

    // The correction that makes the walk clean: read absent band counts as one and 14 racks claim more
    // slots than they hold. That is the evidence that absence means zero.
    UNSCOPED_INFO ("with an absent module count read as 1: " << permissive.overrun << " racks overrun");
    CHECK (permissive.overrun > 0);
    CHECK (permissive.overrun == 14);

    // The usage table itself, over the presets alone. These are `corpus-usage.py`'s numbers: if our
    // decoder and the tool disagree about how the corpus uses effects, one of them is wrong.
    CHECK (stats.totalPresetUses() == 4168);
    CHECK (stats.presetUses["FXEQ"] == 687);
    CHECK (stats.presetUses["FXComp"] == 540);
    CHECK (stats.presetUses["FXReverb"] == 505);
    CHECK (stats.presetUses["FXDelay"] == 448);
    CHECK (stats.presetUses["FXDistortion"] == 435);
    CHECK (stats.presetUses["FXFilter"] == 365);
    CHECK (stats.presetUses["FXSplitMS"] == 15);
    CHECK (stats.presetUses["FXSplit3"] == 19);

    // And the cut, derived from those numbers rather than from taste: the two deferred classes are the
    // two least-used of all sixteen, and the fourteen shipped ones carry more than 99 % of the use.
    const auto rarest = [&]
    {
        std::vector<std::pair<std::size_t, std::string>> ranked;

        for (const auto& [name, count] : stats.presetUses)
            ranked.emplace_back (count, name);

        std::sort (ranked.begin(), ranked.end());
        return ranked;
    }();

    REQUIRE (rarest.size() == 16);
    CHECK (rarest[0].second == "FXSplitMS");
    CHECK (rarest[1].second == "FXSplit3");

    std::size_t deferredUses = 0;

    for (const auto& [count, name] : rarest)
        if (name == "FXSplit3" || name == "FXSplitMS")
            deferredUses += count;

    const double shippedShare = 100.0 * static_cast<double> (stats.totalPresetUses() - deferredUses)
                              / static_cast<double> (stats.totalPresetUses());
    UNSCOPED_INFO ("the 14 shipped classes carry " << shippedShare << " % of preset effect use");
    CHECK (shippedShare >= 99.0);
}
