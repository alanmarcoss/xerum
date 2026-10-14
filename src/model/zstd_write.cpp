#include "zstd_write.h"

namespace xerum::model
{

namespace
{

/// The largest payload a single block may carry, from the format's own limit on Block_Size.
constexpr std::size_t kMaxBlockSize = 128 * 1024;

void putLittle (std::vector<std::byte>& out, std::uint64_t value, int width)
{
    for (int i = 0; i < width; ++i)
        out.push_back (static_cast<std::byte> ((value >> (8 * i)) & 0xFF));
}

} // namespace

std::vector<std::byte> encodeStoredZstdFrame (std::span<const std::byte> input)
{
    std::vector<std::byte> out;
    out.reserve (input.size() + 32);

    // Magic, little-endian in the file: the value 0xFD2FB528.
    out.push_back (std::byte { 0x28 });
    out.push_back (std::byte { 0xB5 });
    out.push_back (std::byte { 0x2F });
    out.push_back (std::byte { 0xFD });

    // Frame_Header_Descriptor: Frame_Content_Size_flag = 2 (four bytes), Single_Segment_flag = 1,
    // Content_Checksum_flag = 0, Dictionary_ID_flag = 0.
    out.push_back (std::byte { 0xA0 });

    putLittle (out, input.size(), 4);

    std::size_t offset = 0;

    do
    {
        const auto remaining = input.size() - offset;
        const auto thisBlock = remaining < kMaxBlockSize ? remaining : kMaxBlockSize;
        const bool last = offset + thisBlock >= input.size();

        // Block_Header, three bytes little-endian: bit 0 last, bits 1–2 type (0 = raw), bits 3–23 size.
        const std::uint32_t header = (last ? 1u : 0u) | (0u << 1) | (static_cast<std::uint32_t> (thisBlock) << 3);
        putLittle (out, header, 3);

        out.insert (out.end(), input.begin() + static_cast<std::ptrdiff_t> (offset),
                    input.begin() + static_cast<std::ptrdiff_t> (offset + thisBlock));

        offset += thisBlock;
    }
    while (offset < input.size());

    return out;
}

} // namespace xerum::model
