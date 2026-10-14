// xerum::model — the preset document and its import report.
//
// This is docs/preset-import-contract.md, made executable. A decoded preset becomes a
// `PresetDocument`: a sparse set of parameter values addressed by schema path, plus the assets it
// references. Nothing about that is allowed to be lossy — anything we could not honour appears in the
// `ImportReport` with a severity, a module path and a reason.
//
// Two rules from the contract are load-bearing and are expressed here rather than left to callers:
//
//  * **Nothing is dropped silently.** Every engine, warp, filter, effect, module, parameter and asset a
//    preset asks for and we cannot honour produces a report entry. The set of silent drops is empty.
//  * **A clean import still produces a report** — empty, not absent. "No report" and "nothing to report"
//    are different states.
//
// The reset-to-defaults half of the load semantics is *not* here: a document is the sparse override set
// a preset carries, and it is the engine that applies those onto its defaults. What this layer does
// provide is `neverMerges`, so the no-merge rule is stated in code rather than only in prose.

#pragma once

#include "schema.h"
#include "value.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace xerum::model
{

enum class Severity
{
    Info,     ///< we coped, and how — worth knowing, not worth worrying about
    Medium,   ///< something was present that we did not interpret
    High      ///< the preset's sound is materially wrong or missing
};

std::string_view toString (Severity severity);

struct ReportEntry
{
    Severity severity { Severity::Info };
    std::string modulePath;   ///< where it is, so a user can find it
    std::string item;         ///< what the preset asked for
    std::string reason;       ///< why we could not honour it
};

struct ImportReport
{
    std::vector<ReportEntry> entries;

    std::size_t count (Severity severity) const;
    bool clean() const { return entries.empty(); }

    /// Highest severity present, for a caller that wants one word to show a user.
    Severity worst() const;
};

/// What v1 can actually reproduce. Everything outside this catalogue is reported rather than substituted.
///
/// The **effect set** is ticket 29's cut, measured: 14 of the corpus's 16 real classes cover 99% of
/// effect occurrences, and the two rarest (`FXSplit3`, `FXSplitMS`) are deferred by name. The
/// **filter models** are ticket 12's families, which ticket `26` has since replaced with the exact
/// name table ticket `26` implements.
struct SupportCatalogue
{
    std::vector<std::string> schemaVersions;      ///< accepted schema versions
    std::vector<std::string> engines;             ///< module-name stems, e.g. "WTOsc"
    std::vector<std::string> deferredEngines;     ///< recognised, deliberately not synthesised in v1
    std::vector<std::string> warpPrefixes;        ///< shipped warp families, by mode-name prefix
    std::vector<std::string> filterModels;        ///< shipped filter families, by type-name prefix
    std::vector<std::string> effects;             ///< shipped effect classes
    std::vector<std::string> deferredEffects;     ///< recognised, not shipped

    static const SupportCatalogue& v1();
};

struct PresetDocument
{
    std::string name;
    std::string schemaVersion;                       ///< normalised, e.g. "4.0"
    /// Concrete path -> value.
    ///
    /// **May contain paths the schema does not describe.** A parameter we could not interpret is kept
    /// as an opaque value so that writing the preset back reproduces what the reference put there; the
    /// report still names it. Losing it would make a load faithful and a write lossy, which the contract
    /// does not allow.
    std::map<std::string, Value> parameters;
    std::vector<std::string> assetReferences;        ///< paths the preset refers to, unresolved here

    std::size_t parameterCount() const { return parameters.size(); }
};

struct LoadResult
{
    bool ok { false };
    std::string error;
    PresetDocument document;
    ImportReport report;
};

/// Turn a decoded payload tree into a document and its report.
///
/// `schemaVersion` comes from the container's metadata, not from the payload — ticket 17's prototype
/// found that reading it from the wrong place silently mis-reports every preset.
LoadResult loadPreset (const Value& payload, std::string_view schemaVersion, const Schema& schema,
                       const SupportCatalogue& catalogue);

/// Returns only `replacement`'s entries, discarding `existing`'s.
///
/// A function this trivial exists because the rule it encodes is not trivial: loading a preset must
/// **not** merge onto what was already loaded. Because absence means *default*, merging would leave the
/// previous preset's values in every parameter the new one does not mention, producing a preset no author
/// ever wrote. Stating it as a named function means a test can assert it and a reader can find it.
PresetDocument neverMerges (const PresetDocument& existing, const PresetDocument& replacement);

} // namespace xerum::model
