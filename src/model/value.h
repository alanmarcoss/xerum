// xerum::model — the value tree.
//
// Both of the container's payload formats decode into this one shape: current files carry CBOR, and the
// legacy codec carries a tagged typecode stream. Downstream — the document layer, the import report —
// should not care which wire format a preset arrived in, so the tree is format-neutral and deliberately
// not named after either. See cbor.h and legacy.h.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xerum::model
{

struct Value
{
    enum class Type { Null, Bool, Int, Float, String, Bytes, Array, Map };

    Type type { Type::Null };

    bool boolean { false };
    std::int64_t integer { 0 };
    double real { 0.0 };
    std::string text;          ///< String contents, or Bytes contents (binary-safe)

    std::vector<Value> items;                                  ///< Array elements
    std::vector<std::pair<std::string, Value>> entries;         ///< Map pairs, in file order

    bool isMap() const   { return type == Type::Map; }
    bool isArray() const { return type == Type::Array; }
    bool isText() const  { return type == Type::String; }
    bool isNumber() const { return type == Type::Int || type == Type::Float; }

    /// Look a key up. Returns nullptr when absent — an absent key means "at default" in this model, so
    /// it is a normal answer rather than an error (ticket 01).
    const Value* find (std::string_view key) const;

    /// A string member's value, or `fallback` when absent or not a string.
    std::string stringOr (std::string_view key, std::string fallback = {}) const;

    /// The value as a double, whether it arrived as an integer or a float. Both formats mix the two,
    /// so readers must accept either at any leaf.
    bool numberAsDouble (double& out) const;
};

struct ValueResult
{
    bool ok { false };
    Value value;
    std::string error;
    std::size_t bytesRead { 0 };
};

/// The name of a type, for diagnostics and report entries.
std::string_view toString (Value::Type type);

} // namespace xerum::model
