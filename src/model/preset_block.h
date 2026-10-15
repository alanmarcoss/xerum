// xerum::model — xerum's own preset block: a document, its report, and a magic.
//
// ## Why this is in the model layer and not in the plugin
//
// `XerumAudioProcessor::getStateInformation` writes a preset as **xerum's own block** rather than a
// reference container (ticket 32), and the factory bank (ticket 54) ships documents in the very same
// form. Two copiers — the plugin and the offline authoring tool — would be two implementations
// of one format, so the format lives here, where the vocabulary it is made of lives, and each caller
// supplies its own magic and version.
//
// The **magic belongs to the caller**. It is the plugin's identity (`plugin/Source/PluginIdentity.h`,
// deliberately not the reference container's `XferJson`: a state block must never be mistaken for a
// preset the reference would read), and `plugin/Source/PresetState.cpp` static-asserts that the values
// the tool writes and the values the plugin writes are the same two values. That assert is what keeps
// this header from being a third copy of the format: there is one encoder, and the constants are
// checked against each other at compile time rather than copied.
//
// ## The layout
//
//     "xerum-state" 0x00            the magic, NUL-terminated on disk (it is a string constant there)
//     uint32, little-endian         the format version
//     CBOR (definite-length, keys sorted)
//
// The CBOR is a map: `format`, `schemaVersion`, `name`, `modelReportEntries`, `parameters`, `assets`
// and `report`. `encodeCbor` sorts keys and uses definite lengths, so a document always encodes to the
// same bytes — which is what makes "this bank file is exactly what the tool generated" a byte
// comparison rather than a claim.

#pragma once

#include "document.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xerum::model
{

/// The magic and version the factory bank's files carry, stated once so the authoring tool and the
/// release check can name them, and so the plugin's copies can be static-asserted against them.
///
/// `xerum::plugin::identity::kStateMagic` / `kStateFormat` are the authority — the plugin is what
/// defines its own identity — and `plugin/Source/PresetState.cpp` refuses to compile if these two
/// disagree with them.
inline constexpr std::string_view kPresetBlockMagic = "xerum-state";
inline constexpr unsigned int kPresetBlockFormat = 1;

/// Encode a document as xerum's block. `magic` is written NUL-terminated, so `magic` itself must not
/// contain a NUL. Never throws.
std::vector<std::byte> encodePresetBlock (const PresetDocument& document, const ImportReport& report,
                                          int reportEntriesFromModel, std::string_view magic,
                                          unsigned int format);

struct BlockResult
{
    bool ok { false };
    std::string error;
    PresetDocument document;
    ImportReport report;

    /// How many leading `report` entries came from the model's own reader, as opposed to a caller's
    /// additions. Written by `encodePresetBlock` and read back here, so a restore can tell its own
    /// entries from the ones the file carried.
    int reportEntriesFromModel { 0 };
};

/// Decode a block. `ok == false` means it is not ours, is a version this build does not read, or is
/// damaged — and `error` says which, in words a user can act on. Never throws.
BlockResult decodePresetBlock (std::span<const std::byte> bytes, std::string_view magic,
                               unsigned int format);

} // namespace xerum::model
