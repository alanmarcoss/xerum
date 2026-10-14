#include "write.h"

#include "cbor_write.h"
#include "container.h"
#include "zstd_write.h"

#include <cstdio>
#include <cstring>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
// CommonCrypto's MD5 is deprecated in favour of CryptoKit, which is Swift-first and awkward from here.
// This is not a security use: MD5 is the reference's preset *key*, we are required to match it exactly,
// and a "stronger" hash would simply fail to match. Rolling our own would be worse than using the
// system's.
#include <CommonCrypto/CommonDigest.h>
#pragma clang diagnostic pop

namespace xerum::model
{

std::string md5Hex (std::span<const std::byte> bytes)
{
    unsigned char digest[CC_MD5_DIGEST_LENGTH] = {};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_MD5 (bytes.data(), static_cast<CC_LONG> (bytes.size()), digest);
#pragma clang diagnostic pop

    char text[CC_MD5_DIGEST_LENGTH * 2 + 1] = {};

    for (int i = 0; i < CC_MD5_DIGEST_LENGTH; ++i)
        std::snprintf (text + i * 2, 3, "%02x", digest[i]);

    return text;
}

namespace
{

/// One path segment: a name, plus an array index when the segment carries one.
struct Segment
{
    std::string name;
    bool isArrayElement { false };
    std::size_t index { 0 };
};

std::vector<Segment> splitPath (std::string_view path)
{
    std::vector<Segment> segments;
    std::string current;

    auto flush = [&]
    {
        if (current.empty())
            return;

        Segment segment;
        const auto bracket = current.find ('[');

        if (bracket == std::string::npos)
        {
            segment.name = current;
        }
        else
        {
            segment.name = current.substr (0, bracket);
            segment.isArrayElement = true;
            segment.index = static_cast<std::size_t> (std::stoul (current.substr (bracket + 1)));
        }

        segments.push_back (std::move (segment));
        current.clear();
    };

    for (const auto c : path)
    {
        if (c == '/')
            flush();
        else
            current.push_back (c);
    }

    flush();
    return segments;
}

Value& descend (Value& node, const Segment& segment, bool terminal)
{
    if (segment.name.empty())
        return node;

    if (! segment.isArrayElement)
    {
        for (auto& [key, child] : node.entries)
            if (key == segment.name)
            {
                if (terminal && ! child.isMap() && ! child.isArray())
                    child = Value {};       // a leaf we are overwriting: replace it wholesale

                return child;
            }

        node.entries.emplace_back (segment.name, Value {});
        auto& child = node.entries.back().second;

        if (! terminal)
            child.type = Value::Type::Map;

        return child;
    }

    // An array element: find or create the array, then the slot.
    Value* array = nullptr;

    for (auto& [key, child] : node.entries)
        if (key == segment.name)
            array = &child;

    if (array == nullptr)
    {
        node.entries.emplace_back (segment.name, Value {});
        array = &node.entries.back().second;
    }

    if (! array->isArray())
    {
        *array = Value {};
        array->type = Value::Type::Array;
    }

    if (array->items.size() <= segment.index)
        array->items.resize (segment.index + 1);

    auto& element = array->items[segment.index];

    // An array element that is not the last segment has to become a container *here*. Leaving it Null
    // and then populating `entries` on it produces a value whose type says "nothing" while holding a
    // tree — and the encoder, which switches on type, emits null and drops the whole subtree. That is
    // how every parameter inside an effect slot disappeared on write without a single error.
    if (! terminal && ! element.isMap() && ! element.isArray())
    {
        element = Value {};
        element.type = Value::Type::Map;
    }

    return element;
}

} // namespace

Value documentToTree (const PresetDocument& document)
{
    Value root;
    root.type = Value::Type::Map;

    for (const auto& [path, value] : document.parameters)
    {
        const auto segments = splitPath (path);

        if (segments.empty())
            continue;

        Value* node = &root;

        for (std::size_t i = 0; i + 1 < segments.size(); ++i)
            node = &descend (*node, segments[i], false);

        auto& leaf = descend (*node, segments.back(), true);
        leaf = value;
    }

    return root;
}

namespace
{

/// Replace the `hash` field's value, leaving every other byte of the metadata alone.
///
/// A textual edit rather than a re-serialisation on purpose: re-emitting the JSON through our own writer
/// would reformat the whole block and lose the exact layout — including fields we do not interpret,
/// which is the thing preservation is *for*.
std::string withHash (std::string_view metadataJson, const std::string& hash)
{
    std::string metadata (metadataJson);
    const std::string key = "\"hash\"";
    const auto at = metadata.find (key);

    if (at == std::string::npos)
    {
        const auto brace = metadata.rfind ('}');

        if (brace == std::string::npos)
            return metadata;

        const bool empty = metadata.find_first_not_of (" \t\n\r{") == std::string::npos
                        || metadata.find_first_not_of (" \t\n\r{") > brace;
        metadata.insert (brace, std::string (empty ? "" : ",") + "\"hash\":\"" + hash + "\"");
        return metadata;
    }

    const auto valueStart = metadata.find ('"', at + key.size());
    const auto valueEnd = valueStart == std::string::npos ? std::string::npos
                                                          : metadata.find ('"', valueStart + 1);

    if (valueStart == std::string::npos || valueEnd == std::string::npos)
        return metadata;

    metadata.replace (valueStart + 1, valueEnd - valueStart - 1, hash);
    return metadata;
}

void appendLittle (std::vector<std::byte>& out, std::uint64_t value, int width)
{
    for (int i = 0; i < width; ++i)
        out.push_back (static_cast<std::byte> ((value >> (8 * i)) & 0xFF));
}

} // namespace

WriteResult writeContainer (const PresetDocument& document, std::string_view metadataJson)
{
    WriteResult result;

    const auto tree = documentToTree (document);
    const auto plain = encodeCbor (tree);

    if (plain.empty())
    {
        result.error = "encoded an empty payload";
        return result;
    }

    const auto stream = encodeStoredZstdFrame (plain);
    const auto hash = md5Hex (stream);

    std::string metadata;

    if (metadataJson.empty())
    {
        metadata = "{\"fileType\":\"SerumPreset\",\"presetName\":\"" + document.name
                 + "\",\"product\":\"Serum2\",\"version\":"
                 + (document.schemaVersion.empty() ? std::string ("4.0") : document.schemaVersion)
                 + ",\"hash\":\"" + hash + "\"}";
    }
    else
    {
        metadata = withHash (metadataJson, hash);
    }

    auto& out = result.bytes;

    static constexpr char magic[] = "XferJson";
    out.insert (out.end(), reinterpret_cast<const std::byte*> (magic),
                reinterpret_cast<const std::byte*> (magic + kContainerMagicSize));

    appendLittle (out, metadata.size(), 8);
    out.insert (out.end(), reinterpret_cast<const std::byte*> (metadata.data()),
                reinterpret_cast<const std::byte*> (metadata.data() + metadata.size()));

    appendLittle (out, plain.size(), 4);          // the payload's uncompressed size
    appendLittle (out, 2, 4);                     // codec 2: zstd

    out.insert (out.end(), stream.begin(), stream.end());

    result.ok = true;
    return result;
}

} // namespace xerum::model
