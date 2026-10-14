// Modulation coverage, measured against the reference's own vocabulary and its factory bank.
//
// Ticket 27's two measurable criteria:
//
//   * "the corpus is walked: every slot in every preset resolves its source and destination, and
//     anything unresolved reports rather than being ignored" — so this walks every slot of all 626
//     presets and holds each row to both halves of the claim: the engine's own vocabulary
//     (`xerum::dsp`), and the parameter model a user's import report is built from (`xerum::model`).
//     Where the two disagree, the disagreement is named;
//   * "unimplemented sources report by name" — so this measures how much of the corpus's modulation the
//     shipped set actually evaluates, and checks that what is left has a name and reaches the caller.
//
// It lives beside the model tests because it needs both halves, in the way `filter_coverage_test.cpp`
// does: the engine owns the vocabulary, the schema owns the parameter model, and the walk is the one
// place where a claim in either is compared to the other.

#include <catch2/catch_test_macros.hpp>

#include "cbor.h"
#include "container.h"
#include "decompress.h"
#include "document.h"
#include "legacy.h"
#include "schema.h"

#include "generated/schema_data.h"

#include "modulation.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace xerum::model;
using namespace xerum::dsp;

namespace
{

/// The reference's **factory** bank: 626 presets, which is the population every number in ticket 03 and
/// ticket 12 is measured over. `Presets/` also holds `User/Default.SerumPreset` — the reference's own
/// working default, not factory content — so walking the parent would count 627 and quietly move every
/// count in this file.
constexpr const char* kCorpusRoot = "/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Presets/Factory";

/// The schema's own module path for a destination class's parameter, so this can ask the parameter model
/// the same question the engine's vocabulary answers.
///
/// One class needs an alias, and it is a correspondence rather than a guess: `Arp` destinations name the
/// arp's parameters (`kParamRate`, `kParamGate`, `kParamChance`, `kParamVeloTarget`, `kParamWrapPhantomNote`,
/// `kParamTransposeRange`), and the preset model stores exactly those under `ArpClip{N}` — the module the
/// arp's settings live in. The walk asserts that correspondence below, so the alias cannot quietly relax
/// into a wildcard.
std::string schemaPrefix (std::string_view destinationClass)
{
    if (destinationClass.starts_with ("FX"))
        return "FXRack{N}/FX[]/" + std::string (destinationClass) + "/plainParams/";

    if (destinationClass == "Arp")
        return "ArpClip{N}/plainParams/";

    for (const auto* engine : { "WTOsc", "SampleOsc", "MultiSampleOsc", "GranularOsc", "SpectralOsc",
                                "NoiseOsc" })
        if (destinationClass == engine)
            return "Oscillator{N}/" + std::string (engine) + "{N}/plainParams/";

    return std::string (destinationClass) + "{N}/plainParams/";
}

/// The fields of a modulation row the **engine** acts on — the other half of the fact
/// `src/model/document.cpp`'s `kAppliedRowKeys`/`kAppliedRowParamKeys` states, written here from the
/// contract's side: one entry per member of `dsp::ModSlot`. The walk below classifies every field the
/// document carries and holds this list and the loader's to each other, so a field the engine stops
/// acting on (or a field the loader stops applying) fails here rather than in a user's ears.
///
/// The loader's own item for a row complaint is the vocabulary this reads back: `ModSlot{N}/<field>` for
/// a field or a row's state, and the parameter model's leaf path for a destination parameter the model
/// cannot describe.
constexpr std::string_view kAppliedRowFields[] = {
    "source",                // ModSlot::source and ::auxSource
    "destModuleTypeString",  // ModSlot::destinationClass
    "destModuleID",          // ModSlot::destinationInstance
    "destModuleParamID",     // ModSlot::destinationParameter
    "destModuleParamName",   // ModSlot::destinationName
    "plainParams/kParamAmount",
    "plainParams/kParamBipolar",
    "plainParams/kParamBypass",
    "plainParams/kParamCurveIn",
    "plainParams/kParamCurveOut",
};

bool isAppliedRowField (std::string_view field)
{
    for (const auto applied : kAppliedRowFields)
        if (applied == field)
            return true;

    return false;
}

/// Which field of its row a concrete document path belongs to.
///
/// `ModSlot3/plainParams/kParamOut` -> `plainParams/kParamOut` (the field), `ModSlot3/flex[0]/xVals[4]`
/// -> `flex` (the subtree's root, since a drawn curve is one field however many points it holds) and
/// `ModSlot3/source[0]` -> `source`. This is the granularity a report can usefully talk about: sixty rows
/// storing the same unapplied field is one fact about the field.
std::string rowFieldOf (std::string_view path, std::string_view rowModule)
{
    const auto relative = path.substr (rowModule.size() + 1);

    if (relative.starts_with ("plainParams/"))
    {
        const auto parameter = relative.substr (std::string_view ("plainParams/").size());
        return "plainParams/" + std::string (parameter.substr (0, parameter.find ('/')));
    }

    const auto bracket = relative.find ('[');
    const auto slash = relative.find ('/');
    const auto end = bracket != std::string_view::npos && (slash == std::string_view::npos || bracket < slash)
                         ? bracket
                         : slash;

    return std::string (relative.substr (0, end));
}

struct DecodedPreset
{
    bool ok { false };
    LoadResult result;
    /// The decoded payload, kept because the point-modification side table is a **top-level array** and
    /// the document's walk only descends into top-level maps — see `pointModRecords` below.
    Value payload;
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
    out.payload = decoded.value;
    out.ok = out.result.ok;
    return out;
}

std::string moduleOf (std::string_view path)
{
    const auto at = path.find ('/');
    return std::string (path.substr (0, at == std::string_view::npos ? path.size() : at));
}

double numberOr (const PresetDocument& document, const std::string& path, double fallback)
{
    const auto it = document.parameters.find (path);

    if (it == document.parameters.end())
        return fallback;

    double value = fallback;
    return it->second.numberAsDouble (value) ? value : fallback;
}

std::string stringOr (const PresetDocument& document, const std::string& path)
{
    const auto it = document.parameters.find (path);

    return it != document.parameters.end() && it->second.isText() ? it->second.text : std::string {};
}

bool present (const PresetDocument& document, const std::string& path)
{
    return document.parameters.find (path) != document.parameters.end();
}

/// Whether any leaf under `prefix` is in the document.
bool anyUnder (const PresetDocument& document, const std::string& prefix)
{
    for (const auto& [path, value] : document.parameters)
        if (path.starts_with (prefix))
            return true;

    return false;
}

/// One matrix row of one preset, rebuilt from the document exactly as an engine loader would build it.
struct Row
{
    std::string preset;
    std::string module;
    std::string version;
    ModSlot slot;
    bool hasDrawnCurve { false };
    bool hasOutputTrim { false };
    bool hasAuxInvert { false };
};

std::vector<Row> rowsOf (const DecodedPreset& preset, const std::filesystem::path& file)
{
    const auto& document = preset.result.document;
    std::set<std::string> modules;

    for (const auto& [path, value] : document.parameters)
        if (path.starts_with ("ModSlot"))
            modules.insert (moduleOf (path));

    std::vector<Row> rows;

    for (const auto& module : modules)
    {
        Row row;
        row.preset = file.filename().string();
        row.module = module;
        row.version = document.schemaVersion;
        row.slot.used = true;
        row.slot.source = (int) numberOr (document, module + "/source[0]", 0.0);
        row.slot.auxSource = (int) numberOr (document, module + "/source[1]", 0.0);
        row.slot.amount = numberOr (document, module + "/plainParams/kParamAmount", 0.0);
        row.slot.bipolar = numberOr (document, module + "/plainParams/kParamBipolar", 0.0) != 0.0;
        row.slot.bypassed = numberOr (document, module + "/plainParams/kParamBypass", 0.0) != 0.0;
        row.slot.curveIn = numberOr (document, module + "/plainParams/kParamCurveIn", 1.0);
        row.slot.curveOut = numberOr (document, module + "/plainParams/kParamCurveOut", 1.0);
        row.slot.destinationClass = stringOr (document, module + "/destModuleTypeString");
        row.slot.destinationInstance = (int) numberOr (document, module + "/destModuleID", 0.0);
        row.slot.destinationParameter = (int) numberOr (document, module + "/destModuleParamID", 0.0);
        row.slot.destinationName = stringOr (document, module + "/destModuleParamName");
        // `flex` holds two curves — main and aux — and either may be the empty object, so the presence of
        // the drawn curve is "any flex leaf", not "flex[0] has points".
        row.hasDrawnCurve = anyUnder (document, module + "/flex[");
        row.hasOutputTrim = present (document, module + "/plainParams/kParamOut");
        row.hasAuxInvert = present (document, module + "/plainParams/kParamAuxInverted");
        rows.push_back (row);
    }

    return rows;
}

/// The rack depth a preset's own `FXRack{r}/FX` array declares — the only place the reference records it,
/// since the parameter model carries no instance counts.
std::map<int, int> rackDepths (const PresetDocument& document)
{
    std::map<int, int> depth;

    for (const auto& [path, value] : document.parameters)
    {
        if (! path.starts_with ("FXRack"))
            continue;

        const auto fxAt = path.find ("/FX[");

        if (fxAt == std::string::npos)
            continue;

        const auto close = path.find (']', fxAt);

        if (close == std::string::npos)
            continue;

        const auto rack = std::stoi (path.substr (6, fxAt - 6));
        const auto slot = std::stoi (path.substr (fxAt + 4, close - fxAt - 4));

        depth[rack] = std::max (depth[rack], slot + 1);
    }

    return depth;
}

/// One `lfoPointModAssignments` record, as ticket 03 measured its shape.
struct PointModRecord
{
    int bus { 0 };
    int lfo { 0 };
    int lfoType { 0 };
    int point { 0 };
    int target { 0 };
};

/// The point-modification side table, read from the **payload**.
///
/// It has to be read from there rather than from a `PresetDocument`, and that is a finding rather than a
/// convenience: `lfoPointModAssignments` is a top-level **array**, and `loadPreset`'s walk descends only
/// into top-level maps (`document.cpp`: `if (! module.isMap()) continue;`), so the table never reaches a
/// document — and unlike a parameter it does not even draw a report entry, because that `continue` is
/// taken before any note is made. Ticket 27's business is the matrix's half of the bus, so it reads the
/// table itself and the count below records what the document carried, for whoever owns the import path.
std::vector<PointModRecord> pointModRecords (const Value& payload)
{
    std::vector<PointModRecord> records;
    const auto* list = payload.find ("lfoPointModAssignments");

    if (list == nullptr || ! list->isArray())
        return records;

    for (const auto& entry : list->items)
    {
        if (! entry.isMap())
            continue;

        PointModRecord record;

        for (const auto* field : { "busID", "lfoID", "lfoType", "pointID", "target" })
        {
            const auto* value = entry.find (field);
            double number = 0.0;

            if (value == nullptr || ! value->numberAsDouble (number))
                continue;

            const auto asInt = (int) number;

            if (std::string_view (field) == "busID")
                record.bus = asInt;
            else if (std::string_view (field) == "lfoID")
                record.lfo = asInt;
            else if (std::string_view (field) == "lfoType")
                record.lfoType = asInt;
            else if (std::string_view (field) == "pointID")
                record.point = asInt;
            else
                record.target = asInt;
        }

        records.push_back (record);
    }

    return records;
}

/// Every input at its maximum, so a row contributes whatever source it names. This is what makes "the bus
/// is actually driven" a measurement rather than a restatement of the wiring.
ModulationInputs allOnes()
{
    ModulationInputs in;
    in.modWheel = 1.0;
    in.pitchBend = 1.0;
    in.expressionX = 1.0;
    in.expressionY = 1.0;
    in.expressionZ = 1.0;
    in.aftertouch = 1.0;
    in.polyAftertouch = 1.0;
    in.velocity = 1.0;
    in.releaseVelocity = 1.0;
    in.noteNumber = 1.0;
    in.noiseOsc = 1.0;
    in.noteOnRandom = 1.0;
    in.noteOnRandomDiscrete = 1.0;
    in.noteOnAlternate = 1.0;
    in.fixed = 1.0;
    in.activeVoices = 1.0;
    in.voiceIndex = 1.0;
    in.voiceMod1 = 1.0;
    in.voiceMod2 = 1.0;
    in.subOsc = 1.0;

    for (auto& value : in.macro)
        value = 1.0;
    for (auto& value : in.envelope)
        value = 1.0;
    for (auto& value : in.lfo)
        value = 1.0;
    for (auto& value : in.lfoY)
        value = 1.0;
    for (auto& value : in.osc)
        value = 1.0;
    for (auto& value : in.voiceFilter)
        value = 1.0;

    return in;
}

struct UnresolvedRow
{
    std::string preset;
    std::string module;
    std::string destinationClass;
    std::string reason;        ///< from the engine, or empty when only the model complained
    std::string modelItem;     ///< the import report's item, or empty
};

/// A destination the engine can address but the parameter model cannot describe: the address resolves, so
/// the matrix computes a value for it, and what is missing is that nothing tells a user what it is.
struct ModelGap
{
    std::string preset;
    std::string module;
    std::string destinationClass;
    std::string destinationName;
    std::string reportItem;    ///< empty when the import report is silent about the parameter
};

/// Everything the corpus says, gathered once: 626 presets, decoded and walked once.
struct CorpusFacts
{
    bool present { false };
    std::size_t presets { 0 };
    std::size_t rows { 0 };
    std::size_t bypassed { 0 };
    std::size_t rowsWithAux { 0 };
    std::size_t rowsWithDrawnCurve { 0 };
    std::size_t rowsWithOutputTrim { 0 };
    std::size_t rowsWithAuxInvert { 0 };

    std::map<int, std::size_t> sourceRows;
    std::vector<int> sourcesWithoutName;

    std::size_t resolved { 0 };
    std::vector<UnresolvedRow> unresolved;
    std::vector<ModelGap> modelGaps;

    std::map<std::string, std::size_t> discreteRows;
    std::map<std::string, std::size_t> continuousRows;

    /// (class, param id) -> the names the corpus gives it; more than one means the id moved between
    /// schema versions.
    std::map<std::pair<std::string, int>, std::set<std::string>> namesByClassId;
    /// (version, class, param id) -> the names given; this must always be exactly one.
    std::map<std::tuple<std::string, std::string, int>, std::set<std::string>> namesByVersionClassId;
    std::map<std::string, std::size_t> versions;

    std::size_t fxRows { 0 };
    std::size_t fxRowsOutsideRack { 0 };

    std::size_t pointModPresets { 0 };
    std::size_t pointModRecords { 0 };
    std::size_t pointModRecordsInDocument { 0 };   ///< expected 0: see `pointModRecords`
    std::map<int, std::size_t> pointModTargets;
    std::map<int, std::size_t> pointModTypes;
    std::size_t pointModWithoutDriver { 0 };
    std::size_t pointModDriven { 0 };
    std::size_t pointModSilent { 0 };
    std::vector<std::string> pointModDead;

    std::size_t presetsQueried { 0 };
    std::vector<std::string> unimplementedReportMismatches;

    // --- the general rule of ticket 39, measured -----------------------------------------------------
    //
    // **Every field of every modulation row either resolves to something the engine acts on, or appears
    // in the report.** Both halves are computed here and they are independent statements: the fields from
    // the `PresetDocument` (again with the vocabulary in this file, not the loader's), the report from
    // `loadPreset`. `rowReportMismatches` is where they are compared, preset by preset.
    std::map<std::string, std::size_t> appliedFieldRows;   ///< a field the engine acts on -> rows storing it
    std::map<std::string, std::size_t> rowFieldRows;       ///< a field it does not -> rows storing it
    std::size_t rowsWithSourceBeyondTwo { 0 };

    /// One import-report entry about a modulation row. The loader's item vocabulary is the classification:
    /// `ModSlot{N}/<field>` for a field or a row's own state, and the parameter model's leaf path for a
    /// destination parameter the model cannot describe.
    struct RowReport
    {
        std::string preset;
        std::string item;
        Severity severity { Severity::Info };
        std::string reason;
    };

    std::vector<RowReport> rowReports;
    std::set<std::string> destinationReportItems;   ///< the model's leaves the report names
    std::size_t destinationReportEntries { 0 };
    std::size_t destinationMissingEntries { 0 };
    std::size_t sourceBeyondTwoEntries { 0 };

    /// Entries naming a field the engine *does* act on, or a field no row of that preset stores: either
    /// would be the loader inventing a complaint, which is the mirror image of the silence this ticket is
    /// about. Must be zero.
    std::size_t unclassifiedRowReportEntries { 0 };

    /// Presets where the report and the walk disagree, with the symmetric difference spelled out.
    std::vector<std::string> rowReportMismatches;
};

const CorpusFacts& corpusFacts()
{
    static const CorpusFacts facts = []
    {
        CorpusFacts out;
        const std::filesystem::path root = kCorpusRoot;

        if (! std::filesystem::exists (root))
            return out;

        out.present = true;

        for (const auto& entry : std::filesystem::recursive_directory_iterator (root))
        {
            if (! entry.is_regular_file() || entry.path().extension() != ".SerumPreset")
                continue;

            const auto decoded = decodePreset (entry.path());

            if (! decoded.ok)
                continue;

            ++out.presets;
            ++out.versions[decoded.result.document.schemaVersion];

            const auto rows = rowsOf (decoded, entry.path());
            const auto depths = rackDepths (decoded.result.document);
            const auto& report = decoded.result.report;

            // The document's half of the general rule: every field of every row, classified by this
            // file's own vocabulary. Read from the document, so nothing the loader believes about itself
            // can make this agree with the report by construction.
            std::map<std::string, std::set<std::string>> rowModulesByField;   // field -> the rows storing it
            std::set<std::string> unappliedFields;
            std::set<std::string> expectedRowItems;
            std::set<std::string> rowsBeyondTwo;

            for (const auto& [path, value] : decoded.result.document.parameters)
            {
                if (! path.starts_with ("ModSlot"))
                    continue;

                const auto rowModule = moduleOf (path);
                const auto field = rowFieldOf (path, rowModule);

                // `source[2]` and beyond: the pair's shape is two, and the engine reads two. Checked
                // before the classification below, because `source` is one of the fields it applies.
                if (field == "source")
                {
                    const auto indexAt = path.find ('[', rowModule.size());

                    if (indexAt != std::string::npos && std::stoi (path.substr (indexAt + 1)) >= 2)
                    {
                        rowsBeyondTwo.insert (rowModule);
                        expectedRowItems.insert ("ModSlot{N}/source[]");
                    }
                }

                if (isAppliedRowField (field))
                {
                    rowModulesByField[field].insert (rowModule);
                    continue;
                }

                unappliedFields.insert (field);
                rowModulesByField[field].insert (rowModule);
                expectedRowItems.insert ("ModSlot{N}/" + field);
            }

            for (const auto& [field, modules] : rowModulesByField)
                if (isAppliedRowField (field))
                    out.appliedFieldRows[field] += modules.size();
                else
                    out.rowFieldRows[field] += modules.size();

            out.rowsWithSourceBeyondTwo += rowsBeyondTwo.size();

            std::vector<ModSlot> slots;

            for (const auto& row : rows)
            {
                ++out.rows;

                if (row.slot.bypassed)
                    ++out.bypassed;

                if (row.slot.auxSource != 0)
                    ++out.rowsWithAux;

                if (row.hasDrawnCurve)
                    ++out.rowsWithDrawnCurve;

                if (row.hasOutputTrim)
                    ++out.rowsWithOutputTrim;

                if (row.hasAuxInvert)
                    ++out.rowsWithAuxInvert;

                ++out.sourceRows[row.slot.source];

                if (modSourceName (row.slot.source).empty())
                    out.sourcesWithoutName.push_back (row.slot.source);

                out.namesByClassId[{ row.slot.destinationClass, row.slot.destinationParameter }]
                    .insert (row.slot.destinationName);
                out.namesByVersionClassId[{ row.version, row.slot.destinationClass,
                                            row.slot.destinationParameter }]
                    .insert (row.slot.destinationName);

                // Resolution, in two parts, because they are two different claims:
                //
                //   * the **address** — class, instance and a named parameter — which is what the matrix
                //     needs, and what `modDestinationUnresolvedReason` answers;
                //   * the **parameter model**, which is what an import report needs, and which the schema
                //     owns. A destination parameter the model cannot describe is a gap in what a user is
                //     told, and the walk names it rather than folding it into "resolved".
                const auto reason = modDestinationUnresolvedReason (row.slot);
                std::string modelItem;

                if (reason.empty())
                {
                    const auto leaf = schemaPrefix (row.slot.destinationClass) + row.slot.destinationName;
                    const bool described = Schema::instance().describe (leaf) != nullptr;

                    if (! described)
                    {
                        // What the report must carry for this row is the leaf the parameter model lacks,
                        // and the loader builds that same path from its own table — so a mapping that
                        // drifted on either side fails here rather than mis-resolving in silence.
                        expectedRowItems.insert (leaf);

                        for (const auto& reportEntry : report.entries)
                            if (reportEntry.item == leaf)
                                modelItem = reportEntry.item;

                        out.modelGaps.push_back ({ row.preset, row.module, row.slot.destinationClass,
                                                   row.slot.destinationName, modelItem });
                    }
                }
                else
                {
                    // A row nothing can address. What is missing is a *destination*, so that is the item
                    // the report has to carry — and nothing else in the loader can notice, because an
                    // absent field has no shape to validate. The loader's gate is mirrored exactly: a row
                    // that names neither a source nor a class stores no modulation at all.
                    if (row.slot.source != 0 || ! row.slot.destinationName.empty())
                        expectedRowItems.insert ("ModSlot{N}/destination");

                    for (const auto& reportEntry : report.entries)
                        if (reportEntry.item == "ModSlot{N}/destination")
                            modelItem = reportEntry.item;

                    out.unresolved.push_back ({ row.preset, row.module, row.slot.destinationClass, reason,
                                                modelItem });
                }

                if (reason.empty())
                    ++out.resolved;

                if (modDestinationIsDiscrete (row.slot.destinationClass, row.slot.destinationName))
                    ++out.discreteRows[row.slot.destinationClass];
                else
                    ++out.continuousRows[row.slot.destinationClass];

                // An FX slot index has to fit inside the rack the preset itself declares.
                if (row.slot.destinationClass.starts_with ("FX"))
                {
                    ++out.fxRows;

                    const auto rack = row.slot.destinationInstance / 100;
                    const auto slot = row.slot.destinationInstance % 100;
                    const auto declared = depths.find (rack);

                    if (declared == depths.end() || slot >= declared->second)
                        ++out.fxRowsOutsideRack;
                }

                slots.push_back (row.slot);
            }

            // --- the report's half of the general rule -----------------------------------------------
            //
            // Everything `loadPreset` said about this preset's modulation rows, classified by the item
            // vocabulary it promises, and compared with what the document says. As a *set*, because
            // `note` collapses one complaint per field per preset — and because the claim is "the set of
            // silently dropped items is empty", which is a statement about sets.
            std::set<std::string> reportedRowItems;

            for (const auto& reportEntry : report.entries)
            {
                if (reportEntry.modulePath != "ModSlot{N}")
                    continue;

                reportedRowItems.insert (reportEntry.item);
                out.rowReports.push_back ({ entry.path().filename().string(), reportEntry.item,
                                            reportEntry.severity, reportEntry.reason });
            }

            if (reportedRowItems != expectedRowItems)
            {
                std::string detail = entry.path().filename().string() + " —";

                for (const auto& item : reportedRowItems)
                    if (! expectedRowItems.contains (item))
                        detail += " reported, but no row stores it: " + item + ";";

                for (const auto& item : expectedRowItems)
                    if (! reportedRowItems.contains (item))
                        detail += " stored, but not reported: " + item + ";";

                out.rowReportMismatches.push_back (detail);
            }

            for (const auto& item : reportedRowItems)
            {
                if (! item.starts_with ("ModSlot{N}/"))
                {
                    ++out.destinationReportEntries;
                    out.destinationReportItems.insert (item);
                    continue;
                }

                const auto field = item.substr (std::string_view ("ModSlot{N}/").size());

                if (field == "destination")
                    ++out.destinationMissingEntries;
                else if (field == "source[]")
                    ++out.sourceBeyondTwoEntries;
                else if (isAppliedRowField (field) || ! unappliedFields.contains (field))
                    ++out.unclassifiedRowReportEntries;
            }

            // The reporting mechanism, per preset: what the engine says is in use and unevaluated must be
            // exactly what the document uses and the shipped set does not cover — no false complaint, and
            // no silent drop.
            std::set<int> expected;

            for (const auto& row : rows)
            {
                if (row.slot.bypassed || ! modDestinationUnresolvedReason (row.slot).empty())
                    continue;

                if (! modSourceIsImplemented (row.slot.source))
                    expected.insert (row.slot.source);

                if (row.slot.auxSource != 0 && ! modSourceIsImplemented (row.slot.auxSource))
                    expected.insert (row.slot.auxSource);
            }

            ModulationMatrix matrix;
            matrix.prepare (48000.0, 64);
            matrix.setSlots (slots);

            const auto reported = matrix.unimplementedSourcesInUse();

            if (std::set<int> (reported.begin(), reported.end()) != expected)
                out.unimplementedReportMismatches.push_back (entry.path().filename().string());

            ++out.presetsQueried;

            // The one true bus: `lfoPointModAssignments` fans `LFOPointModBus{n}.kParamValue` out to LFO
            // points. The matrix's half of that is the bus as a destination, so the check is that a
            // record's bus is driven by a real row of the same preset and that the value arrives.
            const auto records = pointModRecords (decoded.payload);

            for (const auto& [path, value] : decoded.result.document.parameters)
                if (path.starts_with ("lfoPointModAssignments[") && path.ends_with ("/busID"))
                    ++out.pointModRecordsInDocument;

            if (records.empty())
                continue;

            ++out.pointModPresets;
            matrix.beginBlock (allOnes(), 64);
            matrix.beginBlock (allOnes(), 64);

            for (const auto& record : records)
            {
                ++out.pointModRecords;
                ++out.pointModTargets[record.target];
                ++out.pointModTypes[record.lfoType];

                bool driven = false;

                for (const auto& row : rows)
                    if (row.slot.destinationClass == "LFOPointModBus"
                        && row.slot.destinationInstance == record.bus)
                        driven = true;

                if (! driven)
                {
                    ++out.pointModWithoutDriver;
                    out.pointModDead.push_back (entry.path().filename().string() + " bus"
                                                + std::to_string (record.bus));
                }
                else if (matrix.rampFor ("LFOPointModBus", record.bus, 0).end != 0.0)
                    out.pointModDriven++;
                else
                    ++out.pointModSilent;   // wired, but to a row whose depth is zero
            }
        }

        return out;
    }();

    return facts;
}

const CorpusFacts& requireCorpus()
{
    const auto& facts = corpusFacts();

    if (! facts.present)
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    return facts;
}

std::vector<const generated::Leaf*> leavesUnder (std::string_view prefix)
{
    std::vector<const generated::Leaf*> found;

    for (const auto& leaf : generated::kLeaves)
        if (leaf.path.starts_with (prefix))
            found.push_back (&leaf);

    return found;
}

} // namespace

TEST_CASE ("the corpus walk decodes the whole factory bank", "[modulation-coverage]")
{
    const auto& facts = requireCorpus();

    REQUIRE (facts.presets == 626);
    REQUIRE (facts.rows > 13000);

    UNSCOPED_INFO ("corpus: " << facts.presets << " presets, " << facts.rows << " matrix rows");
    UNSCOPED_INFO ("  bypassed rows: " << facts.bypassed << ", rows with an aux source: "
                                       << facts.rowsWithAux);

    for (const auto& [version, count] : facts.versions)
        UNSCOPED_INFO ("  schema " << version << ": " << count << " presets");

    // Three row attributes the frozen `ModSlot` contract has no field for. They are not silently applied,
    // and this is where that is recorded rather than left implicit: `flex` is the drawn curve (the
    // contract's own words: curves "live beside the row, not in it"), `kParamOut` is the OUTPUT trim, and
    // `kParamAuxInverted` is the aux invert. The matrix evaluates `curveIn`/`curveOut` in their place, and
    // the counts are asserted so they cannot grow unnoticed.
    //
    // 453 and not 504: 504 rows carry a `flex` array, but 51 of them hold no points at all (one curve
    // empty in 439 rows, both empty in 51), and a sparse document has nothing to record for an empty
    // curve. So 453 is what a document can see, and 504 is what the payload holds.
    UNSCOPED_INFO ("  rows carrying a drawn `flex` curve: " << facts.rowsWithDrawnCurve
                                                            << ", `kParamOut`: " << facts.rowsWithOutputTrim
                                                            << ", `kParamAuxInverted`: "
                                                            << facts.rowsWithAuxInvert);

    CHECK (facts.rowsWithDrawnCurve == 453);
    CHECK (facts.rowsWithOutputTrim == 156);
    CHECK (facts.rowsWithAuxInvert == 52);
}

TEST_CASE ("every slot in every preset resolves its source", "[modulation-coverage]")
{
    const auto& facts = requireCorpus();

    // No preset stores source 0, so a source of 0 in a *used* row would be a decode failure rather than a
    // legal "no source", and every value has to have a name.
    CHECK (facts.sourcesWithoutName.empty());
    CHECK (facts.sourceRows.find (0) == facts.sourceRows.end());
    CHECK (facts.sourceRows.size() == 49);   // ticket 03 counted 49 distinct values in the corpus

    std::size_t occurrences = 0;
    std::size_t covered = 0;
    std::map<int, std::size_t> missing;

    for (const auto& [source, count] : facts.sourceRows)
    {
        occurrences += count;

        if (modSourceIsImplemented (source))
            covered += count;
        else
            missing[source] = count;
    }

    const auto share = 100.0 * static_cast<double> (covered) / static_cast<double> (occurrences);

    UNSCOPED_INFO ("main-source rows: " << occurrences << ", covered by the shipped set: " << covered
                                        << " (" << share << "%)");

    for (const auto& [source, count] : missing)
    {
        UNSCOPED_INFO ("  used but not evaluated: " << modSourceName (source) << " (" << source << "), "
                                                    << count << " rows");

        // Reportable by name is the whole requirement: a user cannot act on "a source was ignored".
        CHECK_FALSE (modSourceName (source).empty());
    }

    // Ticket 12's cut was the head covering 90%; shipping every source the contract carries covers all but
    // `NoteOn Rand2`, whose second random value the contract has no field for.
    CHECK (share > 99.0);
    CHECK (missing == std::map<int, std::size_t> { { 22, 15 } });
}

TEST_CASE ("every slot in every preset resolves its destination, and the rest is reported",
           "[modulation-coverage]")
{
    const auto& facts = requireCorpus();

    // Two different claims, checked separately because they have different owners.
    //
    // The **address** (class, instance, named parameter) is what the matrix needs, and the only rows that
    // fail it are the six in `SY - Tetris` that kept a source and an amount and lost their destination
    // entirely — there is nothing to address, so the engine's own reason is their report.
    std::map<std::string, std::size_t> byClass;

    for (const auto& row : facts.unresolved)
        ++byClass[row.destinationClass.empty() ? std::string ("<no destination>") : row.destinationClass];

    UNSCOPED_INFO ("address-unresolved rows: " << facts.unresolved.size() << " of " << facts.rows
                                               << " (resolved: " << facts.resolved << ")");

    for (const auto& [name, count] : byClass)
        UNSCOPED_INFO ("  " << name << ": " << count);

    for (const auto& row : facts.unresolved)
        UNSCOPED_INFO ("  " << row.preset << " " << row.module << ": engine reason '" << row.reason << "'");

    // The acceptance, exactly: nothing is unresolved *and* unmentioned — the engine's reason names the
    // six, and so does the import report, which could not see them before this ticket because the field
    // they lost is *absent* and an absent field has no shape to validate.
    for (const auto& row : facts.unresolved)
    {
        CHECK_FALSE (row.reason.empty());
        CHECK_FALSE (row.modelItem.empty());
    }

    CHECK (facts.resolved + facts.unresolved.size() == facts.rows);
    CHECK (byClass.size() == 1);
    CHECK (byClass["<no destination>"] == 6);
    CHECK (facts.unresolved.size() == 6);

    // The **parameter model**, which is the import report's half. These rows' addresses resolve — the
    // matrix computes and hands back a value for them — but the parameter they name is not one the model
    // carries, so nothing can tell a user what it is. Two closed sets, both measured:
    //
    //   * `Global.kParamMasterTuning` — a parameter the reference clearly has (it is in the destination
    //     enum at id 1 and 128 rows target it) but no preset ever *stores* a value for, so no leaf for it
    //     was ever derived. A destination-only parameter;
    //   * `RetriggerState.<param>` — the class whose parameter enum ticket 03 could not locate in the
    //     reference either. Two of its seven names exist under `VoicePanel`, five nowhere at all.
    std::map<std::string, std::size_t> gapClasses;
    std::size_t named = 0;

    for (const auto& gap : facts.modelGaps)
    {
        ++gapClasses[gap.destinationClass];

        if (! gap.reportItem.empty())
            ++named;

        UNSCOPED_INFO ("  model gap: " << gap.preset << " " << gap.module << " -> "
                                       << gap.destinationClass << "." << gap.destinationName
                                       << (gap.reportItem.empty() ? " (the import report is silent)"
                                                                  : " (reported)"));
    }

    UNSCOPED_INFO ("model gaps: " << facts.modelGaps.size() << ", of which the import report names "
                                  << named);

    CHECK (gapClasses.size() == 2);
    CHECK (gapClasses["Global"] == 128);
    CHECK (gapClasses["RetriggerState"] == 7);
    CHECK (facts.modelGaps.size() == 135);

    for (const auto& gap : facts.modelGaps)
    {
        // Written out of the assertion because Catch2's decomposer refuses a top-level `||`.
        const bool expected = gap.destinationClass == "RetriggerState"
                              || (gap.destinationClass == "Global"
                                  && gap.destinationName == "kParamMasterTuning");
        CHECK (expected);
    }

    // **The acceptance, measured.** Every one of the 135 rows names its parameter in the import report,
    // so there is no third state between "resolvable" and "reported": the rows in `Global` and
    // `RetriggerState` are not silently applied to a destination nothing can consume, they are named.
    //
    // `medium` and not `high`, argued from the contract's own table. Its `high` examples — an
    // unsupported engine, warp, filter, effect or asset — are all *sound-producing components or assets
    // on the audio path*. A modulation row's destination is a **control signal**: the row's source,
    // polarity and depth all land, and what is missing is a name for where they land, which is exactly
    // §3's `medium` ("something was present that we did not interpret", whose examples are an unknown
    // module and an unknown parameter) and the severity the loader already gives a parameter the schema
    // does not describe.
    CHECK (named == 135);
    CHECK (named == facts.modelGaps.size());

    for (const auto& gap : facts.modelGaps)
        CHECK_FALSE (gap.reportItem.empty());

    // And the report's items are *exactly* those 135 parameters — the model's own leaf path for each,
    // which is what holds this file's class-to-module mapping and the loader's to each other. A mapping
    // that drifted on either side would name something no row of the corpus addresses, or fail to name
    // something one does.
    std::set<std::string> expectedDestinationItems;

    for (const auto& gap : facts.modelGaps)
        expectedDestinationItems.insert (schemaPrefix (gap.destinationClass) + gap.destinationName);

    for (const auto& item : facts.destinationReportItems)
        UNSCOPED_INFO ("  destination parameter named by the report: " << item);

    CHECK (facts.destinationReportItems == expectedDestinationItems);

    for (const auto& report : facts.rowReports)
        if (! report.item.starts_with ("ModSlot{N}/"))
            CHECK (report.severity == Severity::Medium);
}

TEST_CASE ("every field of every modulation row reaches the engine or the report",
           "[modulation-coverage]")
{
    const auto& facts = requireCorpus();

    // Ticket 39's fourth acceptance, and the form of test `loadPreset` was missing: **every field of
    // every modulation row either resolves to something the engine acts on, or appears in the report**.
    //
    // Both halves are computed independently — the fields from the 626 `PresetDocument`s with this
    // file's own vocabulary, the report from `loadPreset` — and compared **preset by preset, as sets**.
    // That is stricter than a count: a field that loaded in silence is a missing item in the one preset
    // that stores it, and a complaint about a field the engine does act on is an extra item in the one
    // preset that carries it.
    for (const auto& mismatch : facts.rowReportMismatches)
        UNSCOPED_INFO ("  " << mismatch);

    CHECK (facts.rowReportMismatches.empty());
    CHECK (facts.unclassifiedRowReportEntries == 0);

    // The corpus's own vocabulary, counted in rows, so a field either side grows cannot pass unnoticed.
    UNSCOPED_INFO ("row fields the engine acts on:");
    for (const auto& [field, count] : facts.appliedFieldRows)
        UNSCOPED_INFO ("  " << field << ": " << count << " rows");

    UNSCOPED_INFO ("row fields the engine does not act on:");
    for (const auto& [field, count] : facts.rowFieldRows)
        UNSCOPED_INFO ("  " << field << ": " << count << " rows");

    UNSCOPED_INFO ("import-report entries about modulation rows: " << facts.rowReports.size());

    // Ten fields the engine reads, eleven it does not. The eleven are the finding: the ticket named
    // three (`flex`, `kParamOut`, `kParamAuxInverted`) because ticket 27 measured those; the general
    // rule found the smoothing, the delay, both curve-data blobs and the aux curve beside them.
    CHECK (facts.appliedFieldRows.size() == 10);
    CHECK (facts.rowFieldRows.size() == 11);

    CHECK (facts.appliedFieldRows.at ("source") == 13258);
    CHECK (facts.appliedFieldRows.at ("plainParams/kParamAmount") == 13213);
    CHECK (facts.appliedFieldRows.at ("plainParams/kParamBipolar") == 1549);
    CHECK (facts.appliedFieldRows.at ("plainParams/kParamBypass") == 24);
    CHECK (facts.appliedFieldRows.at ("plainParams/kParamCurveIn") == 277);
    CHECK (facts.appliedFieldRows.at ("plainParams/kParamCurveOut") == 51);

    CHECK (facts.rowFieldRows.at ("flex") == 453);
    CHECK (facts.rowFieldRows.at ("plainParams/kParamMainCurveData") == 423);
    CHECK (facts.rowFieldRows.at ("plainParams/kParamOut") == 156);
    CHECK (facts.rowFieldRows.at ("plainParams/kParamAuxCurveData") == 95);
    CHECK (facts.rowFieldRows.at ("plainParams/kParamAuxInverted") == 52);
    CHECK (facts.rowFieldRows.at ("plainParams/kParamSmoothRise") == 43);
    CHECK (facts.rowFieldRows.at ("plainParams/kParamSmoothFall") == 42);
    CHECK (facts.rowFieldRows.at ("plainParams/kParamSmoothLink") == 16);
    CHECK (facts.rowFieldRows.at ("plainParams/kParamDelayBeatSync") == 7);
    CHECK (facts.rowFieldRows.at ("plainParams/kParamDelayOffset") == 7);
    CHECK (facts.rowFieldRows.at ("plainParams/kParamAuxCurve") == 3);

    // The six rows in `SY - Tetris` that lost their destination: reported, and reported once, because
    // the complaint is about the row rather than about a parameter.
    CHECK (facts.destinationMissingEntries == 1);

    UNSCOPED_INFO ("entries naming a destination parameter the model cannot describe: "
                   << facts.destinationReportEntries << " over " << facts.destinationReportItems.size()
                   << " distinct parameters");

    // `source` is the reference's two-element pair in every one of 13,258 rows, so the third-element
    // complaint is not raised anywhere — and would be if a preset ever stored one.
    CHECK (facts.rowsWithSourceBeyondTwo == 0);
    CHECK (facts.sourceBeyondTwoEntries == 0);
}

TEST_CASE ("the discrete/continuous claim agrees with the parameter model", "[modulation-coverage]")
{
    // "The claim and the unit must agree" — `filter_coverage_test.cpp`'s drift test, for properties rather
    // than names. The engine decides discrete-versus-continuous from the parameter's name; the model
    // records what kind of value the reference stores there. A leaf the reference types as anything but a
    // number is an enum, and an enum has nothing to interpolate between.
    std::size_t checked = 0;
    std::size_t discrete = 0;
    std::vector<std::string> skipped;

    for (const auto* name : { "Oscillator", "VoiceFilter", "WTOsc", "MultiSampleOsc", "SampleOsc",
                              "GranularOsc", "SpectralOsc", "NoiseOsc", "Env", "LFO", "Macro",
                              "RoutingSlot", "LFOPointModBus", "Global", "Arp", "MidiClip", "VoicePanel",
                              "RetriggerState", "ModSlot", "FXEQ", "FXComp", "FXReverb", "FXDelay",
                              "FXDistortion", "FXFilter", "FXConv", "FXUtils", "FXChorus", "FXHyperD",
                              "FXBode", "FXPhaser", "FXFlanger", "FXSplit", "FXSplit3", "FXSplitMS" })
    {
        const auto prefix = schemaPrefix (name);
        const auto found = leavesUnder (prefix);

        if (found.empty())
        {
            skipped.emplace_back (name);
            continue;
        }

        for (const auto* leaf : found)
        {
            const auto parameter = std::string (leaf->path.substr (prefix.size()));

            if (! parameter.starts_with ("kParam"))
                continue;

            ++checked;

            const bool isEnum = leaf->kind != "float";
            const bool claim = modDestinationIsDiscrete (name, parameter);
            discrete += claim ? 1 : 0;

            INFO (name << "." << parameter << " is a '" << leaf->kind << "' and the engine says "
                       << (claim ? "discrete" : "continuous"));

            CHECK (claim == isEnum);
        }
    }

    UNSCOPED_INFO ("parameter-model leaves checked: " << checked << ", of which enums: " << discrete);

    for (const auto& name : skipped)
        UNSCOPED_INFO ("  no parameter leaves in the model to compare against: " << name);

    // `RetriggerState` is the one destination class the model cannot describe at all — which is exactly
    // why its rows are reported rather than resolved.
    CHECK (skipped == std::vector<std::string> { "RetriggerState" });
    CHECK (checked > 400);
    CHECK (discrete > 15);
}

TEST_CASE ("the destination vocabulary accounts for every effect class the catalogue does",
           "[modulation-coverage]")
{
    // Shipped **or** explicitly deferred — never absent. A destination class the catalogue does not
    // account for at all would be modulation onto an effect nobody has decided about, and a class the
    // catalogue ships but the engine cannot resolve would be a reported non-event.
    //
    // The shipped-versus-deferred split itself belongs to the FX rack (`fx_topology_test.cpp` holds this
    // catalogue to the engine's own class check); what this holds is the *set*.
    const auto& catalogue = SupportCatalogue::v1();
    std::set<std::string> accounted (catalogue.effects.begin(), catalogue.effects.end());

    for (const auto& effect : catalogue.deferredEffects)
        accounted.insert (effect);

    for (const auto* name : { "FXEQ", "FXComp", "FXReverb", "FXDelay", "FXDistortion", "FXFilter", "FXConv",
                              "FXUtils", "FXChorus", "FXHyperD", "FXBode", "FXPhaser", "FXFlanger",
                              "FXSplit", "FXSplit3", "FXSplitMS" })
    {
        CHECK (modDestinationClassKnown (name));
        CHECK (accounted.contains (name));
    }

    // Sixteen, and no more: the corpus's own vocabulary, so a class that appears here without appearing
    // in the catalogue (or the reverse) is a real disagreement rather than an omission.
    CHECK (accounted.size() == 16);
}

TEST_CASE ("a destination id is version-dependent, so the name is the identity", "[modulation-coverage]")
{
    const auto& facts = requireCorpus();

    // Within one schema version, a (class, id) has exactly one name — otherwise the id would identify
    // nothing and the corpus would contradict itself.
    std::size_t pairsChecked = 0;

    for (const auto& [key, names] : facts.namesByVersionClassId)
    {
        ++pairsChecked;

        INFO (std::get<0> (key) << " " << std::get<1> (key) << "[" << std::get<2> (key) << "] names "
                                << names.size() << " parameters");

        CHECK (names.size() == 1);
    }

    // Across versions it moves: that is why the engine keys resolution and discreteness on the name.
    std::vector<std::pair<std::string, std::size_t>> moved;

    for (const auto& [classId, names] : facts.namesByClassId)
        if (names.size() > 1)
            moved.emplace_back (classId.first + "[" + std::to_string (classId.second) + "]", names.size());

    std::sort (moved.begin(), moved.end());

    for (const auto& [entry, count] : moved)
        UNSCOPED_INFO ("  same (class, id), " << count << " names across schema versions: " << entry);

    UNSCOPED_INFO ("(version, class, id) triples checked: " << pairsChecked);

    CHECK (pairsChecked > 300);

    // Measured: exactly two, both a parameter inserted at schema 4.0. `Oscillator[10]` is `kParamDetune`
    // at 3.0 and `kParamStart` at 4.0; `WTOsc[1]` is `kParamTablePos` at 3.0 and `kParamWarpVar` at 4.0.
    REQUIRE (moved.size() == 2);
    CHECK (moved[0].first == "Oscillator[10]");
    CHECK (moved[1].first == "WTOsc[1]");
}

TEST_CASE ("an FX destination fits inside the rack the preset declares", "[modulation-coverage]")
{
    const auto& facts = requireCorpus();

    UNSCOPED_INFO ("FX destination rows checked against their own rack's array: " << facts.fxRows);

    // The instance is `rackIndex * 100 + slotIndex`, and the schema carries no rack depth — so the check
    // is against the preset's own `FXRack{r}/FX` array, which is where the depth actually lives.
    CHECK (facts.fxRows > 3000);
    CHECK (facts.fxRowsOutsideRack == 0);
}

TEST_CASE ("the LFO point-mod bus is driven through the matrix", "[modulation-coverage]")
{
    const auto& facts = requireCorpus();

    UNSCOPED_INFO ("presets with `lfoPointModAssignments`: " << facts.pointModPresets);
    UNSCOPED_INFO ("  records: " << facts.pointModRecords << " in the payload, "
                                 << facts.pointModRecordsInDocument << " in a `PresetDocument`");

    // The table is a top-level **array** and the document walk descends only into top-level maps, so it
    // never reaches a document and draws no report entry either. Printed rather than asserted: this is a
    // finding about the import path (ticket 23's walk, not ticket 27's matrix), and turning it into an
    // expectation here would pin someone else's layer to a bug.
    CHECK (facts.pointModRecords == 170);

    for (const auto& [target, count] : facts.pointModTargets)
        UNSCOPED_INFO ("  target " << target << ": " << count << " records");

    for (const auto& [type, count] : facts.pointModTypes)
        UNSCOPED_INFO ("  lfoType " << type << ": " << count << " records");

    // Ticket 03: 67 presets, 170 records, targets 51/77/42, types 149 curve + 21 path.
    CHECK (facts.pointModPresets == 67);
    CHECK (facts.pointModRecords == 170);
    CHECK (facts.pointModTypes == std::map<int, std::size_t> { { 0, 149 }, { 1, 21 } });
    CHECK (facts.pointModTargets.at (0) == 51);
    CHECK (facts.pointModTargets.at (1) == 77);
    CHECK (facts.pointModTargets.at (2) == 42);

    // The matrix's half of the bus is wired: a record's bus is a destination in the same preset, and with
    // every source at maximum the value arrives through `rampFor`.
    //
    // The other half — the fan-out from `LFOPointModBus{n}.kParamValue` into LFO curve/path points — is
    // *not* wired, and the reason is structural rather than a deferral: point modulation rewrites an LFO's
    // drawn `curveData`/`pathData` point coordinates, and v1's LFOs arrive as values in
    // `ModulationInputs`, so there is no LFO point representation anywhere in xerum for the fan-out to
    // address. When the LFO layer lands it calls `rampFor ("LFOPointModBus", bus, 0)` for the bus value,
    // which is already correct — what is missing belongs to that layer, not to this one.
    CHECK (facts.pointModDriven > 150);
    CHECK (facts.pointModDriven + facts.pointModSilent + facts.pointModWithoutDriver
           == facts.pointModRecords);

    // Measured, and worth stating rather than smoothing over: **12 of the 170 records bind a bus that no
    // row of that preset drives**. Two of them have a stored `LFOPointModBus{n}/plainParams/kParamValue`
    // instead (a direct value rather than a modulation), and the rest are bindings whose source is gone.
    // The matrix reads them as zero, which is what the reference would do too — but a user should be told,
    // so they are named here.
    for (const auto& dead : facts.pointModDead)
        UNSCOPED_INFO ("  record with no driving row: " << dead);

    CHECK (facts.pointModWithoutDriver == 12);
    CHECK (facts.pointModDriven + facts.pointModSilent > 150);

    UNSCOPED_INFO ("  bus read non-zero through the matrix: " << facts.pointModDriven
                                                              << ", driven by a zero-depth row: "
                                                              << facts.pointModSilent
                                                              << ", not driven at all: "
                                                              << facts.pointModWithoutDriver);
}

TEST_CASE ("the unimplemented-source report matches the document, preset by preset", "[modulation-coverage]")
{
    const auto& facts = requireCorpus();

    // The mechanism the caller reports from: for every preset, `unimplementedSourcesInUse()` must be
    // exactly the sources that preset uses and this build does not evaluate.
    for (const auto& preset : facts.unimplementedReportMismatches)
        UNSCOPED_INFO ("  report mismatch in " << preset);

    CHECK (facts.unimplementedReportMismatches.empty());
    CHECK (facts.presetsQueried == 626);
}
