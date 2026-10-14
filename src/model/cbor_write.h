// xerum::model — writing CBOR.
//
// The encoder for the value tree, used when a document is written back out. Deliberately the narrowest
// thing that round-trips: **definite-length containers only**, and map keys emitted in sorted order.
//
// Two notes on those choices:
//
//  * The reference's own writer uses indefinite-length containers in places, and its key order is
//    whatever its serialiser produced. Neither matters for reading — both forms are valid CBOR — and
//    sorting makes our output deterministic, which is worth more than imitating byte layout we would
//    only be guessing at.
//  * Determinism here means the same tree always encodes to the same bytes. It does **not** claim a
//    byte-identical round trip against the reference's writer, and nothing should assume it.

#pragma once

#include "value.h"

#include <cstddef>
#include <span>
#include <vector>

namespace xerum::model
{

/// Encode a value as CBOR. Never throws; a value of an impossible shape cannot arise from the readers,
/// so there is no error channel to report through.
std::vector<std::byte> encodeCbor (const Value& value);

} // namespace xerum::model
