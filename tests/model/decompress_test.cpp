// Tests for the decompression layer.
//
// The fixtures are ours — a payload we wrote, compressed in each codec the container uses. Nothing here
// needs the reference installed, which is the point: this layer has to be verifiable on a bare clone.

#include <catch2/catch_test_macros.hpp>

#include "decompress.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace xerum::model;

namespace
{

std::vector<std::byte> readFile (const std::string& name)
{
    const auto path = std::filesystem::path (XERUM_FIXTURE_DIR) / name;
    std::ifstream in (path, std::ios::binary);
    REQUIRE (in.good());                                   // a missing fixture is a broken checkout

    std::vector<std::byte> bytes;
    in.seekg (0, std::ios::end);
    bytes.resize (static_cast<std::size_t> (in.tellg()));
    in.seekg (0, std::ios::beg);
    in.read (reinterpret_cast<char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
    return bytes;
}

std::string asText (const std::vector<std::byte>& bytes)
{
    return std::string (reinterpret_cast<const char*> (bytes.data()), bytes.size());
}

} // namespace

TEST_CASE ("codec numbers map to the codecs the container uses", "[decompress]")
{
    CHECK (codecFromNumber (0) == Codec::Zlib);
    CHECK (codecFromNumber (2) == Codec::Zstd);

    // Anything else is unknown rather than a guess: a new codec appearing in a preset must be reported,
    // not silently treated as one we recognise.
    CHECK (codecFromNumber (1) == Codec::Unknown);
    CHECK (codecFromNumber (99) == Codec::Unknown);
}

TEST_CASE ("zstd payloads decompress", "[decompress]")
{
    const auto expected = readFile ("payload.txt");
    const auto compressed = readFile ("payload.zst");

    const auto result = decompress (compressed, Codec::Zstd, expected.size());

    REQUIRE (result.ok);
    CHECK (result.error.empty());
    CHECK (result.bytes == expected);
}

TEST_CASE ("zlib payloads decompress", "[decompress]")
{
    const auto expected = readFile ("payload.txt");
    const auto compressed = readFile ("payload.zlib");

    const auto result = decompress (compressed, Codec::Zlib, expected.size());

    REQUIRE (result.ok);
    CHECK (result.bytes == expected);
}

TEST_CASE ("the vendored zstd is the pinned release", "[decompress]")
{
    // If this fails, someone regenerated the vendored file from a different tag and did not update
    // third_party/zstd/README.md. That is not a catastrophe, but it must be noticed rather than assumed.
    CHECK (zstdVersion() == "1.5.7");
}

TEST_CASE ("a truncated frame is reported, not crashed on", "[decompress]")
{
    const auto expected = readFile ("payload.txt");
    const auto truncated = readFile ("payload.truncated.zst");

    const auto result = decompress (truncated, Codec::Zstd, expected.size());

    // A corrupt preset is an ordinary outcome: the import report describes it. What must never happen
    // is a crash, or a short buffer that looks like success and becomes a truncated parameter set.
    CHECK_FALSE (result.ok);
    CHECK_FALSE (result.error.empty());
    CHECK (result.bytes.empty());
}

TEST_CASE ("the container's declared size is enforced", "[decompress]")
{
    const auto expected = readFile ("payload.txt");
    const auto compressed = readFile ("payload.zst");

    SECTION ("too small")
    {
        const auto result = decompress (compressed, Codec::Zstd, expected.size() / 2);
        CHECK_FALSE (result.ok);
        CHECK_FALSE (result.error.empty());
    }

    SECTION ("too large")
    {
        const auto result = decompress (compressed, Codec::Zstd, expected.size() * 4);
        CHECK_FALSE (result.ok);
    }

    SECTION ("exactly right is the only thing that passes")
    {
        CHECK (decompress (compressed, Codec::Zstd, expected.size()).ok);
    }
}

TEST_CASE ("degenerate inputs are refused", "[decompress]")
{
    const auto expected = readFile ("payload.txt");
    const auto compressed = readFile ("payload.zst");

    CHECK_FALSE (decompress ({}, Codec::Zstd, 100).ok);                       // empty payload
    CHECK_FALSE (decompress (compressed, Codec::Zstd, 0).ok);                 // zero-length claim
    CHECK_FALSE (decompress (compressed, Codec::Unknown, expected.size()).ok);
    CHECK_FALSE (decompress (compressed, Codec::Zlib, expected.size()).ok);   // right bytes, wrong codec
}
