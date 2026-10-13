// xerum::model — the legacy payload format.
//
// Twelve files in the local corpus predate the CBOR payload and carry a compact tagged stream instead.
// They matter out of proportion to their number: they are real presets, and a reader that cannot open
// them is a reader that fails on someone's file.
//
// The grammar, as recovered in the tracker (ticket 01's answer and the byte-exact decoder on branch
// `research/04-serum2-filters-and-fx`):
//
//     { }          map open / close
//     [ ]          array open / close
//     i <u8 len>…  in KEY position: a map key, one-byte length prefix
//                  in VALUE position: an integer in one unsigned byte
//     I <u16 BE>   integer, two bytes big-endian
//     S <name>     a string value, spelled exactly like a key
//     D <8 bytes>  IEEE-754 double, BIG-endian — not little, which is the obvious mistake
//     T / F        boolean, a BARE tag with no payload — see the note in legacy.cpp, where the
//                  tracker's first reading of `F` as tag-plus-byte is corrected
//
// The context-dependent `i` is the part worth knowing: the same byte means a key at one end of a pair
// and a number at the other. A reader that treats it uniformly decodes half the file as garbage.
//
// Decodes into the same Value tree as CBOR, so nothing above this layer needs to know which format a
// preset arrived in.

#pragma once

#include "value.h"

#include <cstddef>
#include <span>
#include <string>

namespace xerum::model
{

/// Decode one legacy typecode stream.
///
/// `bytesRead` reports how much was consumed. The 9 legacy files that were decoded by hand all consume
/// their payload exactly, so a short read is a signal worth asserting on.
ValueResult decodeLegacy (std::span<const std::byte> input);

} // namespace xerum::model
