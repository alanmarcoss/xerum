// Tests for the container framing.
//
// The hand-built cases pin down the quirks — the trailing-NUL metadata, the length field, the codec
// number — so they stay pinned. The corpus walk at the end is the real one: it reads every container
// the reference ships and insists the payload decodes exactly, skipping (loudly) on a machine with no
// reference installed, per docs/test-strategy.md.

#include <catch2/catch_test_macros.hpp>

#include "cbor.h"
#include "container.h"
#include "decompress.h"
#include "json.h"
#include "legacy.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace xerum::model;

namespace
{

/// Build a container by hand, so each test says exactly which part of the layout it is exercising.
std::vector<std::byte> buildContainer (std::string_view metadata, unsigned codec, std::uint32_t declared,
                                       std::string_view payload, bool nulTerminated = false)
{
    std::vector<std::byte> bytes;

    auto append = [&] (const void* data, std::size_t size)
    {
        const auto* p = static_cast<const std::byte*> (data);
        bytes.insert (bytes.end(), p, p + size);
    };

    append (kContainerMagic, kContainerMagicSize);

    const std::uint64_t metadataLength = metadata.size() + (nulTerminated ? 1u : 0u);
    append (&metadataLength, sizeof (metadataLength));
    append (metadata.data(), metadata.size());

    if (nulTerminated)
        bytes.push_back (std::byte { 0 });

    append (&declared, sizeof (declared));
    append (&codec, sizeof (codec));
    append (payload.data(), payload.size());

    return bytes;
}

constexpr std::string_view kSimpleMetadata =
    R"({"fileType":"SerumPreset","presetName":"Test","version":4.0})";

std::vector<std::byte> asBytes (std::string_view text)
{
    return std::vector<std::byte> (reinterpret_cast<const std::byte*> (text.data()),
                                   reinterpret_cast<const std::byte*> (text.data() + text.size()));
}

constexpr std::string_view kReferenceContentRoot =
    "/Library/Audio/Presets/Xfer Records/Serum 2 Presets";

} // namespace

TEST_CASE ("a container's framing reads back", "[container]")
{
    const auto bytes = buildContainer (kSimpleMetadata, 2, 1234, "pretend-zstd");
    const auto result = readContainer (bytes);

    REQUIRE (result.ok);
    CHECK (result.error.empty());
    CHECK (result.fileType == "SerumPreset");
    CHECK (result.presetName == "Test");
    CHECK (result.schemaVersion == "4.0");
    CHECK (result.codec == Codec::Zstd);
    CHECK (result.declaredSize == 1234);
    CHECK (result.metadataSlack == 0);
    CHECK (result.metadataJson == kSimpleMetadata);

    REQUIRE (result.payload.size() == 12);
    CHECK (std::memcmp (result.payload.data(), "pretend-zstd", 12) == 0);
}

TEST_CASE ("metadata that counts a trailing NUL still reads", "[container]")
{
    // 12 files in the corpus do this. A strict JSON parser rejects them with "extra data", so the
    // framing slices by the declared length and the parser tolerates the terminator — and the slack is
    // reported rather than absorbed.
    const auto bytes = buildContainer (kSimpleMetadata, 2, 99, "payload", true);
    const auto result = readContainer (bytes);

    REQUIRE (result.ok);
    CHECK (result.metadataSlack == 1);
    CHECK (result.fileType == "SerumPreset");
    CHECK (result.declaredSize == 99);
}

TEST_CASE ("schema versions normalise to one decimal place", "[container]")
{
    // The metadata carries the version as a JSON *number*, and `4` is not `"4.0"`. Getting this wrong
    // made every real preset report as a future release (found by the ticket 17 prototype).
    CHECK (normaliseVersion (4.0) == "4.0");
    CHECK (normaliseVersion (3.0) == "3.0");
    CHECK (normaliseVersion (6.0) == "6.0");

    const auto integerVersion = buildContainer (R"({"version":4})", 2, 10, "x");
    CHECK (readContainer (integerVersion).schemaVersion == "4.0");

    const auto stringVersion = buildContainer (R"({"version":"5.0"})", 2, 10, "x");
    CHECK (readContainer (stringVersion).schemaVersion == "5.0");

    // No version at all is empty, not a guess.
    const auto absent = buildContainer (R"({"fileType":"SerumPreset"})", 2, 10, "x");
    CHECK (readContainer (absent).schemaVersion.empty());
}

TEST_CASE ("metadata survives the round trip verbatim", "[container]")
{
    // We interpret two fields and preserve the rest, because re-writing a preset must not drop a field
    // we did not understand (ticket 14). Emoji in a preset name is the case that breaks a naive scan.
    const std::string metadata =
        R"({"fileType":"SerumPreset","presetName":"\u00e9\uD83C\uDFB9 name","version":4.0,)"
        R"("presetDescription":"line\nbreak","unknownField":{"nested":true}})";

    const auto bytes = buildContainer (metadata, 2, 42, "payload");
    const auto result = readContainer (bytes);

    REQUIRE (result.ok);
    CHECK (result.metadataJson == metadata);                 // byte-for-byte

    const auto parsed = decodeJson (result.metadataJson);
    REQUIRE (parsed.ok);
    CHECK (parsed.value.stringOr ("presetName") == "\u00e9\U0001F3B9 name");
    CHECK (parsed.value.find ("unknownField") != nullptr);   // preserved, not dropped
}

TEST_CASE ("a malformed container is refused with a reason", "[container]")
{
    SECTION ("not a container at all")
    {
        const auto bytes = asBytes ("this is not a preset file at all");
        const auto result = readContainer (bytes);
        CHECK_FALSE (result.ok);
        CHECK (result.error.find ("magic") != std::string::npos);
    }

    SECTION ("too short to hold a header")
    {
        const auto bytes = asBytes ("XferJson");
        const auto result = readContainer (bytes);
        CHECK_FALSE (result.ok);
        CHECK_FALSE (result.error.empty());
    }

    SECTION ("metadata length past the end")
    {
        auto bytes = buildContainer (kSimpleMetadata, 2, 10, "payload");
        const std::uint64_t absurd = 1ull << 40;
        std::memcpy (bytes.data() + 9, &absurd, sizeof (absurd));

        const auto result = readContainer (bytes);
        CHECK_FALSE (result.ok);
        CHECK (result.error.find ("past the end") != std::string::npos);
    }

    SECTION ("metadata that is not JSON")
    {
        const auto bytes = buildContainer ("not json at all", 2, 10, "payload");
        const auto result = readContainer (bytes);
        CHECK_FALSE (result.ok);
        CHECK (result.error.find ("JSON") != std::string::npos);
    }

    SECTION ("an unknown codec is reported, not guessed")
    {
        const auto bytes = buildContainer (kSimpleMetadata, 7, 10, "payload");
        const auto result = readContainer (bytes);
        CHECK_FALSE (result.ok);
        CHECK (result.error.find ("codec") != std::string::npos);
    }

    SECTION ("a zero-length payload claim")
    {
        const auto bytes = buildContainer (kSimpleMetadata, 2, 0, "payload");
        CHECK_FALSE (readContainer (bytes).ok);
    }
}

TEST_CASE ("every container the reference ships reads and decodes", "[container][corpus]")
{
    const std::filesystem::path root = kReferenceContentRoot;

    if (! std::filesystem::exists (root))
    {
        SKIP ("the reference's content tree is not installed here — this test needs it, and says so "
              "rather than passing quietly (docs/test-strategy.md)");
    }

    std::size_t containers = 0;
    std::size_t presets = 0;
    std::size_t cborDecoded = 0;
    std::size_t legacyCodec = 0;
    std::size_t legacyDecoded = 0;
    std::vector<std::string> failures;

    for (const auto& entry : std::filesystem::recursive_directory_iterator (root))
    {
        if (! entry.is_regular_file())
            continue;

        std::ifstream in (entry.path(), std::ios::binary);
        std::vector<std::byte> raw ((std::size_t) std::filesystem::file_size (entry.path()));
        in.read (reinterpret_cast<char*> (raw.data()), (std::streamsize) raw.size());

        if (raw.size() < kContainerHeaderSize
            || std::memcmp (raw.data(), kContainerMagic, kContainerMagicSize - 1) != 0)
            continue;   // not a container: the tree also holds .wav tables, .flac samples, .pdf, .png

        ++containers;
        if (entry.path().extension() == ".SerumPreset")
            ++presets;

        const auto container = readContainer (raw);

        if (! container.ok)
        {
            failures.push_back (entry.path().filename().string() + ": " + container.error);
            continue;
        }

        const auto payload = decompress (container.payload, container.codec, container.declaredSize);

        if (! payload.ok)
        {
            failures.push_back (entry.path().filename().string() + ": " + payload.error);
            continue;
        }

        if (container.codec == Codec::Zlib)
        {
            // The legacy typecode stream is a different grammar reaching the same Value tree.
            const auto legacy = decodeLegacy (payload.bytes);
            ++legacyCodec;

            if (! legacy.ok)
            {
                failures.push_back (entry.path().filename().string() + ": legacy: " + legacy.error);
                continue;
            }

            if (legacy.bytesRead != payload.bytes.size())
            {
                failures.push_back (entry.path().filename().string() + ": legacy consumed "
                                    + std::to_string (legacy.bytesRead) + " of "
                                    + std::to_string (payload.bytes.size()));
                continue;
            }

            ++legacyDecoded;
            continue;
        }

        const auto document = decodeCbor (payload.bytes);

        if (! document.ok)
        {
            failures.push_back (entry.path().filename().string() + ": CBOR: " + document.error);
            continue;
        }

        // The check that has caught real mistakes: the document must consume the payload exactly. A
        // short read here would mean the reader silently stopped early on some files.
        if (document.bytesRead != payload.bytes.size())
        {
            failures.push_back (entry.path().filename().string() + ": CBOR consumed "
                                + std::to_string (document.bytesRead) + " of "
                                + std::to_string (payload.bytes.size()));
            continue;
        }

        ++cborDecoded;
    }

    for (const auto& failure : failures)
        UNSCOPED_INFO (failure);

    CHECK (failures.empty());

    // The counts from ticket 01, asserted so a change in the input is noticed rather than absorbed.
    CHECK (containers == 1253);
    CHECK (presets == 626);
    CHECK (legacyCodec == 12);
    CHECK (legacyDecoded == legacyCodec);
    CHECK (cborDecoded + legacyDecoded == containers);   // the whole corpus reads
}
