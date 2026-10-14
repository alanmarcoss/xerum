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
// Known gap, recorded rather than hidden: a document stores *parameters*, so an engine selection that
// carries no parameter overrides is not reconstructed. A preset whose slot selects an engine and sets
// nothing on it will re-read as though it selected the default. Closing that needs the engine selection
// to live in the document, which is a change to `PresetDocument` rather than to this writer.

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
