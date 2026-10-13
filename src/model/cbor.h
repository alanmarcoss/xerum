// xerum::model — CBOR, as the reference container uses it.
//
// The payload of a current preset is a CBOR document: a map of module names to module trees. This is
// the reader for it, scoped to what the format actually requires rather than to the whole of RFC 8949.
//
// Two decisions worth stating, because both are load-bearing:
//
//  * **Order is preserved** in maps, and duplicate keys are kept. A preset is a serialised
//    document, not a hash table, and silently collapsing duplicate keys would lose the fact that it
//    contained them.
//  * **Nothing throws.** A malformed payload is an ordinary outcome that the import report describes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xerum::model
{

struct CborValue
{
    enum class Type { Null, Bool, Int, Float, String, Bytes, Array, Map };

    Type type { Type::Null };

    bool boolean { false };
    std::int64_t integer { 0 };
    double real { 0.0 };
    std::string text;          ///< String contents, or Bytes contents (binary-safe)

    std::vector<CborValue> items;                                  ///< Array elements
    std::vector<std::pair<std::string, CborValue>> entries;         ///< Map pairs, in file order

    bool isMap() const   { return type == Type::Map; }
    bool isArray() const { return type == Type::Array; }
    bool isText() const  { return type == Type::String; }
    bool isNumber() const { return type == Type::Int || type == Type::Float; }

    /// Look a key up. Returns nullptr when absent — an absent key means "at default" in this model, so
    /// it is a normal answer rather than an error (ticket 01).
    const CborValue* find (std::string_view key) const;

    /// The value as a double, whether it arrived as an integer or a float. The reference writes both,
    /// including `float64` where a naive reader expects `float32` (ticket 01), so readers must accept
    /// either at any leaf.
    bool numberAsDouble (double& out) const;
};

struct CborResult
{
    bool ok { false };
    CborValue value;
    std::string error;
    std::size_t bytesRead { 0 };
};

/// Decode one CBOR document starting at the beginning of `input`.
///
/// `bytesRead` reports exactly how much was consumed, so the caller can assert the payload was consumed
/// in full — a trailing-byte check that has already caught real format mistakes.
CborResult decodeCbor (std::span<const std::byte> input);

/// The name of a type, for diagnostics and report entries.
std::string_view toString (CborValue::Type type);

} // namespace xerum::model
