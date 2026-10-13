#include "container.h"

#include "json.h"

#include <cstdio>
#include <cstring>

namespace xerum::model
{

namespace
{

std::uint32_t readU32 (std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    std::memcpy (&value, bytes.data() + offset, sizeof (value));
    return value;
}

std::uint64_t readU64 (std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    std::memcpy (&value, bytes.data() + offset, sizeof (value));
    return value;
}

} // namespace

std::string normaliseVersion (double version)
{
    char buffer[32] = {};
    std::snprintf (buffer, sizeof (buffer), "%.1f", version);
    return buffer;
}

ContainerResult readContainer (std::span<const std::byte> bytes)
{
    ContainerResult result;

    if (bytes.size() < kContainerHeaderSize + 8)
    {
        result.error = "file is too short to hold a container header";
        return result;
    }

    if (std::memcmp (bytes.data(), kContainerMagic, kContainerMagicSize - 1) != 0
        || static_cast<char> (bytes[kContainerMagicSize - 1]) != '\0')
    {
        result.error = "not a reference container: magic does not match";
        return result;
    }

    const auto metadataLength = readU64 (bytes, 9);

    // A declared length that cannot fit is rejected before any slicing, so a corrupt or hostile length
    // can never index past the buffer.
    if (metadataLength > bytes.size() || kContainerHeaderSize + metadataLength + 8 > bytes.size())
    {
        result.error = "metadata length runs past the end of the file";
        return result;
    }

    const auto metadataBytes = bytes.subspan (kContainerHeaderSize, static_cast<std::size_t> (metadataLength));
    result.metadataJson.assign (reinterpret_cast<const char*> (metadataBytes.data()), metadataBytes.size());

    const auto metadata = decodeJson (result.metadataJson);

    if (! metadata.ok)
    {
        result.error = "metadata is not readable JSON: " + metadata.error;
        return result;
    }

    if (! metadata.value.isObject())
    {
        result.error = "metadata is not a JSON object";
        return result;
    }

    result.metadataSlack = metadataBytes.size() - metadata.bytesRead;

    result.fileType = metadata.value.stringOr ("fileType");
    result.presetName = metadata.value.stringOr ("presetName");
    result.presetAuthor = metadata.value.stringOr ("presetAuthor");

    if (const auto* version = metadata.value.find ("version"); version != nullptr)
    {
        if (version->isNumber())
            result.schemaVersion = normaliseVersion (version->number);
        else if (version->isString())
            result.schemaVersion = version->text;   // already textual; leave it as the file had it
    }

    const auto payloadHeaderAt = kContainerHeaderSize + static_cast<std::size_t> (metadataLength);
    result.declaredSize = readU32 (bytes, payloadHeaderAt);
    result.codec = codecFromNumber (readU32 (bytes, payloadHeaderAt + 4));
    result.payload = bytes.subspan (payloadHeaderAt + 8);

    if (result.payload.empty())
    {
        result.error = "container declares a payload but carries none";
        return result;
    }

    if (result.codec == Codec::Unknown)
    {
        result.error = "container uses a codec this reader does not know";
        return result;
    }

    if (result.declaredSize == 0)
    {
        result.error = "container declares a zero-length payload";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace xerum::model
