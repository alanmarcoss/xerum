// xerum::model — CBOR, as the reference container uses it.
//
// The payload of a current preset is a CBOR document: a map of module names to module trees. This is the
// reader for it, scoped to what the format actually requires rather than to the whole of RFC 8949.
//
// Two decisions worth stating, because both are load-bearing:
//
//  * **Order is preserved** in maps, and duplicate keys are kept. A preset is a serialised document,
//    not a hash table, and silently collapsing duplicate keys would lose the fact that it contained them.
//  * **Nothing throws.** A malformed payload is an ordinary outcome that the import report describes.

#pragma once

#include "value.h"

#include <cstddef>
#include <span>
#include <string>

namespace xerum::model
{

/// Decode one CBOR document starting at the beginning of `input`.
///
/// `bytesRead` reports exactly how much was consumed, so the caller can assert the payload was read in
/// full — a trailing-byte check that has already caught real format mistakes.
ValueResult decodeCbor (std::span<const std::byte> input);

} // namespace xerum::model
