// xerum::model — writing a document back into a container.
//
// The write half of the import contract. Three commitments, and each is a deliberate choice:
//
//  * **Codec 2, zstd, always.** Writing the legacy codec would be easier (we have its grammar and zlib
//    is on the system) but a current reference expects the modern form, and emitting an obsolete format
//    from a new writer is how a compatibility quirk becomes permanent.
//  * **The metadata is preserved, except the hash.** Unknown fields survive byte-for-byte, because a
//    newer reference may have written things we do not understand and dropping them loses someone's
//    preset notes. The `hash` is recomputed because it identifies the payload, and the payload changed.
//  * **Deterministic output.** The same document always produces the same bytes.
//
// **A known gap was recorded here and is now withdrawn.** The note claimed that "a document stores
// *parameters*, so an engine selection that carries no parameter overrides is not reconstructed". That
// was written on the assumption that selection lives in the file's structure. It does not: as ticket 36
// measured, `Oscillator{N}/plainParams/kParamType` **is a schema leaf**, absence of it means the
// wavetable default (which ADR 0001 already represents), and every preset that selects a non-default
// engine stores it. Of 3130 oscillator slots in the factory bank, 475 select a non-default engine; the
// case the note worried about — selected, nothing overridden — occurs and round-trips, verified across
// 262 presets in `tests/model/engine_selection_test.cpp`.
//
// The 434 slots that *look* like the gap — a non-wavetable engine carrying an override map — are slots 3
// and 4, the fixed NOISE and SUB slots that are not selectable at all, plus stale overrides from an
// engine the user tried and left. "Which sub-engine has parameters" is not a signal about selection.

#pragma once

#include "document.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace xerum::model
{

struct WriteResult
{
    bool ok { false };
    std::string error;
    std::vector<std::byte> bytes;
};

/// Rebuild the payload tree from a document's concrete parameter paths.
///
/// The inverse of the flattening the importer does, and the place the `[index]` segments have to be
/// honoured: an effect rack's slots are an array, not a map with numeric keys.
Value documentToTree (const PresetDocument& document);

/// Write a document as a container.
///
/// `metadataJson` is the metadata block to preserve. Pass the one the preset was read with so unknown
/// fields survive; pass empty to have a minimal one synthesised.
WriteResult writeContainer (const PresetDocument& document, std::string_view metadataJson = {});

/// The lowercase hex MD5 of some bytes — the reference's `hash`, which is the MD5 of its compressed
/// payload stream. Exposed because both the writer and any test of it need to agree on it.
std::string md5Hex (std::span<const std::byte> bytes);

} // namespace xerum::model
