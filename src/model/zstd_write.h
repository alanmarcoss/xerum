// xerum::model — writing a zstd frame without a compressor.
//
// We vendored only zstd's **decoder**, because reading is what a preset reader needs and the full library
// is twice the size. Writing a container still has to produce a valid zstd frame, and it turns out it
// can: zstd's own format has a Raw_Block type, so a frame can carry its content uncompressed.
//
// So this emits a real, standard-conformant zstd frame that any decoder reads, at a compression ratio of
// exactly 1.0. That is the honest trade: correctness now, and preset files a few kilobytes larger than
// they need to be, with the upgrade path being to link an encoder rather than to change any format.
//
// Frame layout used here:
//
//     28 B5 2F FD          magic
//     A0                   Frame_Header_Descriptor: FCS field 4 bytes, single segment, no checksum
//     <u32 LE>             Frame_Content_Size
//     blocks…              each: 3-byte header (last, type, size) then the bytes
//
// Single-segment means no window descriptor, and the content size is what lets a decoder allocate once.

#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace xerum::model
{

/// Wrap `input` in a valid zstd frame using raw blocks. Never fails.
std::vector<std::byte> encodeStoredZstdFrame (std::span<const std::byte> input);

} // namespace xerum::model
