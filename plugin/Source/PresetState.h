// xerum::plugin — reading a preset, and writing xerum's own state block.
//
// ## Are preset import and state save/load the same mechanism?
//
// **One mechanism, two codecs, and the asymmetry between them is the whole answer.**
//
// The in-memory form is the same: a `PresetDocument` plus its `ImportReport`. Loading a preset builds
// one from the reference's container; restoring state builds one from xerum's own block; the processor
// applies both through one code path (`XerumAudioProcessor::applyDocument`). Nothing else in the plugin
// knows either file format.
//
// What differs is the *container*, and ticket 32 is why it must:
//
//   * a `.SerumPreset` is the **reference's** format — read-only here, and its payload carries structure
//     a document cannot hold (98 098 subtrees across the factory bank), so a document-only write aborts
//     the reference on 172 of 626 presets;
//   * a xerum state block is **ours**: a magic, a version, and CBOR of the document. It is deliberately
//     not shaped like a preset, so nobody can mistake one for the other and hand the reference a file
//     that crashes it.
//
// So: same in-memory form, same apply path, different codecs — and the state block is not an export
// format. `plugin/README.md` lists exactly what it therefore cannot round-trip.

#pragma once

#include <juce_core/juce_core.h>

#include "document.h"

#include <cstddef>
#include <string>
#include <vector>

namespace xerum::plugin
{

struct LoadedPreset
{
    bool ok { false };
    std::string error;
    model::PresetDocument document;
    model::ImportReport report;

    /// How many leading `report` entries came from the model's `loadPreset`, as opposed to the plugin's
    /// own additions (asset resolution, engine gaps). The plugin regenerates its own from the document, so
    /// it needs to know where its part starts — otherwise a state restore would carry them twice.
    int reportEntriesFromModel { 0 };
};

/// Read a `.SerumPreset` (or any container) into a document plus its report.
///
/// Never throws. The import contract's first clause is "MUST not fail, whatever the file contains", so a
/// damaged file is an error string and an empty document — and the caller surfaces it, because a preset
/// that did not load must say so rather than appear to have loaded silence.
LoadedPreset loadPresetFile (const juce::File& file);

/// Read a preset from a payload already in memory, the same path a file takes after its container is
/// read. Exposed for tests, which write their own fixtures through `writeContainer`.
LoadedPreset loadPresetPayload (const model::Value& payload, std::string_view schemaVersion);

/// xerum's state block: `identity::kStateMagic`, a format version, then CBOR of the document and report.
/// `reportEntriesFromModel` is stored so that a restore can tell the model's entries from the plugin's.
std::vector<std::byte> encodeStateBlock (const model::PresetDocument& document,
                                         const model::ImportReport& report, int reportEntriesFromModel);

/// Decode a state block. `ok == false` means it is not ours or is damaged; the caller keeps what it has.
LoadedPreset decodeStateBlock (const void* data, int size);

/// Append one entry. The plugin *completes* the model's report with the things only it knows: asset
/// resolution outcomes (contract §5) and the engine-level gaps the bridge does not apply.
void addReportEntry (model::ImportReport& report, model::Severity severity, std::string modulePath,
                     std::string item, std::string reason);

/// The report as text for the interface: a counts line, one line per entry, and — for a clean import —
/// "nothing to report". Per the contract a clean import still produces a report: **empty, not absent**,
/// and the two must not look the same.
std::string reportText (const model::ImportReport& report);

} // namespace xerum::plugin
