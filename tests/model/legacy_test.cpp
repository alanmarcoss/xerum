// Tests for the legacy typecode stream.
//
// Hand-written byte sequences, because the format's interesting behaviour is context-dependent: the same
// `i` byte is a key at one end of a map pair and a number at the other, and only a test written at that
// level can pin that down.

#include <catch2/catch_test_macros.hpp>

#include "legacy.h"

#include <string_view>

using namespace xerum::model;

namespace
{

std::vector<std::byte> fromHex (std::string_view hex)
{
    std::vector<std::byte> out;

    auto nibble = [] (char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        const int hi = nibble (hex[i]);
        const int lo = nibble (hex[i + 1]);
        REQUIRE (hi >= 0);
        REQUIRE (lo >= 0);
        out.push_back (static_cast<std::byte> ((hi << 4) | lo));
    }

    return out;
}

/// Text as bytes, for spelling an expectation.
std::string bytes (std::string_view text) { return std::string (text); }

} // namespace

TEST_CASE ("the same byte is a key or a number depending on its position", "[legacy]")
{
    // { "a": 5 }  ->  '{' 'i' 0x01 'a'  'i' 0x05  '}'
    const auto input = fromHex ("7b69016169057d");
    const auto result = decodeLegacy (input);

    REQUIRE (result.ok);
    CHECK (result.bytesRead == input.size());
    REQUIRE (result.value.isMap());
    REQUIRE (result.value.entries.size() == 1);

    CHECK (result.value.entries[0].first == "a");                 // the key
    CHECK (result.value.entries[0].second.type == Value::Type::Int);   // the value
    CHECK (result.value.entries[0].second.integer == 5);
}

TEST_CASE ("strings are spelled like keys, behind an S", "[legacy]")
{
    // { "s": "abc" }
    const auto input = fromHex ("7b690173" "536903616263" "7d");
    const auto result = decodeLegacy (input);

    REQUIRE (result.ok);
    CHECK (result.bytesRead == input.size());

    const auto* member = result.value.find ("s");
    REQUIRE (member != nullptr);
    CHECK (member->type == Value::Type::String);
    CHECK (member->text == "abc");
}

TEST_CASE ("both integer forms decode", "[legacy]")
{
    CHECK (decodeLegacy (fromHex ("69ff")).value.integer == 255);       // one byte
    CHECK (decodeLegacy (fromHex ("490100")).value.integer == 256);     // two bytes, big-endian
    CHECK (decodeLegacy (fromHex ("494100")).value.integer == 0x4100);
}

TEST_CASE ("doubles are big-endian", "[legacy]")
{
    // 1.0 as IEEE-754 big-endian: 3F F0 00 00 00 00 00 00. Read little-endian this would be a tiny
    // denormal — a plausible-looking number, which is exactly why the mistake is worth a test.
    const auto result = decodeLegacy (fromHex ("443ff0000000000000"));

    REQUIRE (result.ok);
    CHECK (result.value.type == Value::Type::Float);
    CHECK (result.value.real == 1.0);
}

TEST_CASE ("booleans and arrays decode", "[legacy]")
{
    // T and F are BARE tags. The tracker first recorded `F` as tag-plus-byte, which reads eleven of the
    // twelve legacy files and then swallows a key in the twelfth.
    const auto truthy = decodeLegacy (fromHex ("54"));
    REQUIRE (truthy.ok);
    CHECK (truthy.value.type == Value::Type::Bool);
    CHECK (truthy.value.boolean);

    const auto falsy = decodeLegacy (fromHex ("46"));
    REQUIRE (falsy.ok);
    CHECK (falsy.value.type == Value::Type::Bool);
    CHECK_FALSE (falsy.value.boolean);

    // The real case: { "isOpen": F, "numPoints": 1 }. A reader that consumes a byte after `F` eats the
    // next key and then fails on a well-formed file.
    // { "isOpen": F, "numPoints": 1 }
    const auto sequence = fromHex ("7b690669734f70656e4669096e756d506f696e747369017d");
    const auto decoded = decodeLegacy (sequence);
    REQUIRE (decoded.ok);
    CHECK (decoded.bytesRead == sequence.size());
    CHECK (decoded.value.find ("isOpen") != nullptr);
    CHECK (decoded.value.find ("isOpen")->boolean == false);
    CHECK (decoded.value.find ("numPoints") != nullptr);
    CHECK (decoded.value.find ("numPoints")->integer == 1);

    // [1, 2]
    const auto list = decodeLegacy (fromHex ("5b690169025d"));
    REQUIRE (list.ok);
    REQUIRE (list.value.isArray());
    CHECK (list.value.items.size() == 2);
    CHECK (list.value.items[1].integer == 2);
}

TEST_CASE ("nested maps decode as deep as the format goes", "[legacy]")
{
    // { "a": { "b": 7 } }
    const auto input = fromHex ("7b6901617b69016269077d7d");
    const auto result = decodeLegacy (input);

    REQUIRE (result.ok);
    CHECK (result.bytesRead == input.size());

    const auto* inner = result.value.find ("a");
    REQUIRE (inner != nullptr);
    REQUIRE (inner->isMap());
    CHECK (inner->find ("b")->integer == 7);
}

TEST_CASE ("a malformed legacy stream is reported, not crashed on", "[legacy]")
{
    SECTION ("unknown typecode")
    {
        const auto result = decodeLegacy (fromHex ("5a"));
        CHECK_FALSE (result.ok);
        CHECK (result.error.find ("typecode") != std::string::npos);
        CHECK (result.value.entries.empty());     // no half-built tree escapes
    }

    SECTION ("unterminated map")
    {
        const auto result = decodeLegacy (fromHex ("7b6901616905"));
        CHECK_FALSE (result.ok);
        CHECK_FALSE (result.error.empty());
    }

    SECTION ("a name that runs off the end")
    {
        const auto result = decodeLegacy (fromHex ("7b69ff4162"));
        CHECK_FALSE (result.ok);
    }

    SECTION ("empty input")
    {
        const auto result = decodeLegacy ({});
        CHECK_FALSE (result.ok);
    }

    SECTION ("a double with too few bytes")
    {
        const auto result = decodeLegacy (fromHex ("443f"));
        CHECK_FALSE (result.ok);
    }
}

TEST_CASE ("the legacy reader produces the same tree shape as the CBOR one", "[legacy]")
{
    // { "a": 5 } in the legacy grammar, and the same document as CBOR. The point of the shared Value
    // tree is that nothing above this layer can tell them apart.
    const auto legacy = decodeLegacy (fromHex ("7b69016169057d"));

    REQUIRE (legacy.ok);
    REQUIRE (legacy.value.isMap());
    CHECK (legacy.value.find ("a")->integer == 5);
    CHECK (legacy.value.find ("missing") == nullptr);      // absent is a normal answer, not an error
}
