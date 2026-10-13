// Tests for the CBOR reader.
//
// The inputs are hand-written CBOR byte sequences rather than fixture files: CBOR is designed to be
// written by hand, so a test can say exactly which encoding it is exercising — including the awkward
// ones (`float64` at a leaf, indefinite-length containers) that a round-trip through an encoder would
// never produce on purpose.

#include <catch2/catch_test_macros.hpp>

#include "cbor.h"

#include <cstdlib>
#include <string_view>

using namespace xerum::model;

namespace
{

std::vector<std::byte> fromHex (std::string_view hex)
{
    std::vector<std::byte> out;
    out.reserve (hex.size() / 2);

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

} // namespace

TEST_CASE ("a definite-length map decodes into its entries, in order", "[cbor]")
{
    // {"a": 1, "b": "two", "c": [1, 2, 3]}
    const auto bytes = fromHex ("a361610161626374776f616383010203");
    const auto result = decodeCbor (bytes);

    REQUIRE (result.ok);
    CHECK (result.bytesRead == bytes.size());     // consumed in full, no trailing bytes
    REQUIRE (result.value.isMap());
    REQUIRE (result.value.entries.size() == 3);

    // Order is the file's order. A preset is a document, not a hash table.
    CHECK (result.value.entries[0].first == "a");
    CHECK (result.value.entries[1].first == "b");
    CHECK (result.value.entries[2].first == "c");

    const auto* a = result.value.find ("a");
    REQUIRE (a != nullptr);
    CHECK (a->type == CborValue::Type::Int);
    CHECK (a->integer == 1);

    const auto* b = result.value.find ("b");
    REQUIRE (b != nullptr);
    CHECK (b->type == CborValue::Type::String);
    CHECK (b->text == "two");

    const auto* c = result.value.find ("c");
    REQUIRE (c != nullptr);
    REQUIRE (c->isArray());
    CHECK (c->items.size() == 3);
    CHECK (c->items[2].integer == 3);

    CHECK (result.value.find ("missing") == nullptr);
}

TEST_CASE ("indefinite-length containers decode", "[cbor]")
{
    // The real payload uses these, so a reader that only handles definite lengths reads a subset of
    // the corpus and fails on the rest. Map with indefinite length, one entry, then break.
    // { "k": [1, 2] } with both the map and the array indefinite.
    const auto bytes = fromHex ("bf616b9f0102ffff");
    const auto result = decodeCbor (bytes);

    REQUIRE (result.ok);
    CHECK (result.bytesRead == bytes.size());
    REQUIRE (result.value.isMap());

    const auto* k = result.value.find ("k");
    REQUIRE (k != nullptr);
    REQUIRE (k->isArray());
    REQUIRE (k->items.size() == 2);
    CHECK (k->items[0].integer == 1);
    CHECK (k->items[1].integer == 2);
}

TEST_CASE ("both float widths decode at a leaf", "[cbor]")
{
    // The reference writes float32 and float64, sometimes in the same document. A reader that assumes
    // one width silently mis-reads the other.
    const auto f32 = decodeCbor (fromHex ("fa3f800000"));   // 1.0
    REQUIRE (f32.ok);
    CHECK (f32.value.type == CborValue::Type::Float);
    CHECK (f32.value.real == 1.0);

    const auto f64 = decodeCbor (fromHex ("fb3ff0000000000000"));   // 1.0
    REQUIRE (f64.ok);
    CHECK (f64.value.type == CborValue::Type::Float);
    CHECK (f64.value.real == 1.0);
}

TEST_CASE ("integers are both signed and unsigned", "[cbor]")
{
    CHECK (decodeCbor (fromHex ("00")).value.integer == 0);
    CHECK (decodeCbor (fromHex ("1817")).value.integer == 23);
    CHECK (decodeCbor (fromHex ("190100")).value.integer == 256);
    CHECK (decodeCbor (fromHex ("20")).value.integer == -1);
    CHECK (decodeCbor (fromHex ("3863")).value.integer == -100);
}

TEST_CASE ("numbers read as doubles whichever way they arrived", "[cbor]")
{
    double out = 0.0;

    const auto asInt = decodeCbor (fromHex ("182a"));   // 42
    REQUIRE (asInt.ok);
    REQUIRE (asInt.value.numberAsDouble (out));
    CHECK (out == 42.0);

    const auto asFloat = decodeCbor (fromHex ("fb3ff0000000000000"));
    REQUIRE (asFloat.ok);
    REQUIRE (asFloat.value.numberAsDouble (out));
    CHECK (out == 1.0);

    const auto asNull = decodeCbor (fromHex ("f6"));
    REQUIRE (asNull.ok);
    CHECK_FALSE (asNull.value.numberAsDouble (out));
}

TEST_CASE ("duplicate keys are preserved rather than collapsed", "[cbor]")
{
    // A preset containing a duplicate key is worth surfacing, not silently squashing: collapsing it
    // here would hide the fact from every layer above.
    const auto result = decodeCbor (fromHex ("a2616101616102"));

    REQUIRE (result.ok);
    REQUIRE (result.value.entries.size() == 2);
    CHECK (result.value.entries[0].second.integer == 1);
    CHECK (result.value.entries[1].second.integer == 2);
    CHECK (result.value.find ("a")->integer == 1);   // find returns the first
}

TEST_CASE ("truncated input is an error, never a crash", "[cbor]")
{
    // A map claiming three entries but carrying one.
    const auto result = decodeCbor (fromHex ("a3616101"));

    CHECK_FALSE (result.ok);
    CHECK_FALSE (result.error.empty());
    CHECK (result.value.entries.empty());

    // An empty input is an error too, not an empty document.
    const auto empty = decodeCbor ({});
    CHECK_FALSE (empty.ok);
    CHECK_FALSE (empty.error.empty());
}

TEST_CASE ("trailing bytes are reported by the count, not swallowed", "[cbor]")
{
    // A complete document followed by junk. Decoding succeeds, but `bytesRead` tells the caller the
    // document ended early — which is how a format mistake gets caught rather than absorbed.
    const auto bytes = fromHex ("a1616101" "ffffff");
    const auto result = decodeCbor (bytes);

    REQUIRE (result.ok);
    CHECK (result.bytesRead == 4);
    CHECK (result.bytesRead < bytes.size());
}

TEST_CASE ("absurd nesting is refused instead of exhausting the stack", "[cbor]")
{
    // 200 nested arrays, which no preset contains and a corrupt file might claim.
    std::vector<std::byte> bytes (200, std::byte { 0x81 });

    const auto result = decodeCbor (bytes);

    CHECK_FALSE (result.ok);
    CHECK (result.error.find ("nesting") != std::string::npos);
}

TEST_CASE ("type names are available for reports", "[cbor]")
{
    CHECK (toString (CborValue::Type::Map) == "map");
    CHECK (toString (CborValue::Type::Float) == "float");
    CHECK (toString (CborValue::Type::String) == "string");
}
