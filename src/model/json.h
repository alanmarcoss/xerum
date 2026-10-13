// xerum::model — JSON, for the container's metadata block.
//
// Deliberately small, and deliberately present rather than avoided. The metadata block is JSON, we
// interpret exactly two fields out of it (`fileType` and the schema `version`), and we preserve the
// whole block verbatim so nothing is lost — but "we only read two fields" is not a reason to hand-roll
// a string scan: a scan that trips over an escaped quote or a surrogate pair in a user's preset name
// produces a wrong answer rather than an error, and preset names do contain emoji.
//
// Nothing throws. Malformed metadata is reported, and the import contract decides what that means.

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xerum::model
{

struct JsonValue
{
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type { Type::Null };
    bool boolean { false };
    double number { 0.0 };
    std::string text;

    std::vector<JsonValue> items;                                  ///< Array elements
    std::vector<std::pair<std::string, JsonValue>> members;         ///< Object members, in order

    bool isObject() const { return type == Type::Object; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }

    /// Look a top-level member up. Returns nullptr when absent.
    const JsonValue* find (std::string_view key) const;

    /// A string member's value, or empty when absent or not a string.
    std::string stringOr (std::string_view key, std::string fallback = {}) const;
};

struct JsonResult
{
    bool ok { false };
    JsonValue value;
    std::string error;
    std::size_t bytesRead { 0 };
};

/// Decode a JSON document. Trailing whitespace and a trailing NUL are tolerated; anything else after
/// the document is reported through `bytesRead` rather than absorbed, so a caller can tell the
/// difference between a clean parse and one that stopped early.
JsonResult decodeJson (std::string_view input);

} // namespace xerum::model
