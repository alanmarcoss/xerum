#include "decompress.h"

#include "../../third_party/zstd/shim.h"

#include <zlib.h>

#include <cstring>

namespace xerum::model
{

Codec codecFromNumber (unsigned int number)
{
    switch (number)
    {
        case 0: return Codec::Zlib;
        case 2: return Codec::Zstd;
        default: return Codec::Unknown;
    }
}

std::string_view zstdVersion() { return xerum_zstd_version(); }

DecompressResult decompress (std::span<const std::byte> input, Codec codec, std::size_t expectedSize)
{
    DecompressResult result;

    if (input.empty())
    {
        result.error = "empty payload";
        return result;
    }

    if (expectedSize == 0)
    {
        result.error = "container declared a zero-length payload";
        return result;
    }

    result.bytes.resize (expectedSize);

    switch (codec)
    {
        case Codec::Zstd:
        {
            const auto written = xerum_zstd_decompress (result.bytes.data(), expectedSize,
                                                        input.data(), input.size());

            if (written != expectedSize)
            {
                // Either the frame is corrupt or it does not hold what the container claimed. Both
                // matter, and neither may pass quietly: a short read here becomes a truncated
                // parameter set later, which is far harder to diagnose.
                result.bytes.clear();
                result.error = written == 0
                             ? std::string ("zstd: ") + xerum_zstd_error()
                             : "zstd decoded " + std::to_string (written)
                                   + " bytes but the container declared " + std::to_string (expectedSize);
                return result;
            }

            break;
        }

        case Codec::Zlib:
        {
            uLongf produced = static_cast<uLongf> (expectedSize);
            const auto status = ::uncompress (reinterpret_cast<Bytef*> (result.bytes.data()), &produced,
                                              reinterpret_cast<const Bytef*> (input.data()),
                                              static_cast<uLong> (input.size()));

            if (status != Z_OK || produced != expectedSize)
            {
                result.bytes.clear();
                result.error = std::string ("zlib: ")
                             + (status == Z_DATA_ERROR   ? "corrupt or not a zlib stream"
                                : status == Z_BUF_ERROR ? "decoded more than the container declared"
                                                        : ::zError (status))
                             + " (produced " + std::to_string (produced) + " of "
                             + std::to_string (expectedSize) + ")";
                return result;
            }

            break;
        }

        case Codec::Unknown:
        default:
            result.bytes.clear();
            result.error = "unknown codec";
            return result;
    }

    result.ok = true;
    return result;
}

} // namespace xerum::model
