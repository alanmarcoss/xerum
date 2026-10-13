// xerum::model — decompression.
//
// The reference container wraps its payload in one of two codecs. This is the layer that turns those
// bytes back into something a parser can read, and it is deliberately small: the container always
// declares the uncompressed size, so neither codec needs a streaming interface.
//
// Nothing here throws. A corrupt or truncated payload is an ordinary outcome the import report exists
// to describe, and an exception crossing a plugin boundary is a crash in someone else's DAW (ticket 15).

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace xerum::model
{

/// The codecs the reference container uses. `Zstd` covers every `.SerumPreset` in the wild; `Zlib`
/// appears only in the older container variant, which carries a legacy text stream instead of CBOR.
enum class Codec { Zlib, Zstd, Unknown };

Codec codecFromNumber (unsigned int number);

struct DecompressResult
{
    bool ok { false };
    std::vector<std::byte> bytes;
    std::string error;
};

/// Decompress exactly `expectedSize` bytes from `input`.
///
/// The expected size is not a hint — it is the container's own declaration, and passing the wrong one
/// must fail rather than silently truncate or overrun. `std::bad_alloc` is the one exception this may
/// propagate; everything else is reported through `error`.
DecompressResult decompress (std::span<const std::byte> input, Codec codec, std::size_t expectedSize);

/// The version of the vendored zstd decoder.
///
/// Exposed so a test can catch an unannounced regeneration of `third_party/zstd` rather than assume it
/// was harmless, and so diagnostics can say which decoder produced a result. Keeping it here rather
/// than letting callers reach into the vendored shim keeps the vendor's layout out of our public code.
std::string_view zstdVersion();

} // namespace xerum::model
