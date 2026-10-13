// xerum::model — the container every reference asset is wrapped in.
//
// Layout, as characterised in the tracker's recon and ticket 01:
//
//     0        "XferJson\0"                 9 bytes
//     9        uint64 little-endian         length of the metadata block
//     17       JSON metadata                `jsonLen` bytes
//     +jsonLen uint32 little-endian         the payload's uncompressed size
//     +4       uint32 little-endian         codec: 0 = zlib, 2 = zstd
//     +8       the compressed stream        to end of file
//
// Two quirks a naive reader gets wrong, both observed in the real corpus:
//
//   * **The metadata block may carry a trailing NUL**, with `jsonLen` counting it. Strict JSON parsing
//     then fails with "extra data" on 12 files in the corpus. The block is sliced by `jsonLen` so the
//     framing stays correct, and parsed tolerantly.
//   * **The metadata is preserved verbatim.** We interpret two fields and keep the rest untouched, so
//     re-writing a preset does not silently drop a field we did not understand (ticket 14).

#pragma once

#include "decompress.h"

#include <cstddef>
#include <span>
#include <string>

namespace xerum::model
{

struct ContainerResult
{
    bool ok { false };
    std::string error;

    std::string metadataJson;      ///< the metadata block exactly as it appeared
    std::string fileType;          ///< e.g. "SerumPreset"
    std::string schemaVersion;     ///< normalised to one decimal place, e.g. "4.0"
    std::string presetName;
    std::string presetAuthor;

    Codec codec { Codec::Unknown };
    std::size_t declaredSize { 0 };   ///< the payload's uncompressed size, as the container claims it

    std::span<const std::byte> payload;   ///< the compressed stream, still compressed

    /// `jsonLen` minus the bytes the metadata actually consumed. Non-zero on the legacy files that
    /// count a trailing NUL, so the quirk is visible rather than silently absorbed.
    std::size_t metadataSlack { 0 };
};

/// Read the container framing. Does not decompress and does not parse the payload — those are the
/// caller's business, so a framing error and a payload error stay distinguishable.
ContainerResult readContainer (std::span<const std::byte> bytes);

/// Normalise a schema version to one decimal place: `4` becomes `"4.0"`.
///
/// The metadata carries the version as a JSON *number*, and `4` does not equal `"4.0"`. A naive
/// comparison made every real preset look like it came from a future release (found by the ticket 17
/// prototype), so normalisation lives here where both the reader and the importer can share it.
std::string normaliseVersion (double version);

/// The container's magic, exposed so tests and tools do not each spell it out.
inline constexpr char kContainerMagic[] = "XferJson";
inline constexpr std::size_t kContainerMagicSize = 9;      // includes the trailing NUL
inline constexpr std::size_t kContainerHeaderSize = 17;    // magic + the length field

} // namespace xerum::model
