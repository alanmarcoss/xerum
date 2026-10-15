// xerum::model — xerum's own preset block. See preset_block.h.

#include "preset_block.h"

#include "cbor.h"
#include "cbor_write.h"

#include <cstring>
#include <exception>

namespace xerum::model
{

namespace
{

Value textValue (std::string text)
{
    Value value;
    value.type = Value::Type::String;
    value.text = std::move (text);
    return value;
}

Value intValue (const std::int64_t number)
{
    Value value;
    value.type = Value::Type::Int;
    value.integer = number;
    return value;
}

int severityCode (const Severity severity)
{
    switch (severity)
    {
        case Severity::Info:   return 0;
        case Severity::Medium: return 1;
        case Severity::High:   return 2;
    }

    return 0;
}

Severity severityFromCode (const std::int64_t code)
{
    if (code == 2)
        return Severity::High;

    if (code == 1)
        return Severity::Medium;

    return Severity::Info;
}

std::byte byteOf (const unsigned int value)
{
    return static_cast<std::byte> (value & 0xffu);
}

} // namespace

std::vector<std::byte> encodePresetBlock (const PresetDocument& document, const ImportReport& report,
                                          const int reportEntriesFromModel, const std::string_view magic,
                                          const unsigned int format)
{
    Value root;
    root.type = Value::Type::Map;

    root.entries.emplace_back ("format", intValue (static_cast<std::int64_t> (format)));
    root.entries.emplace_back ("schemaVersion", textValue (document.schemaVersion));
    root.entries.emplace_back ("name", textValue (document.name));
    root.entries.emplace_back ("modelReportEntries", intValue (reportEntriesFromModel));

    Value parameters;
    parameters.type = Value::Type::Map;

    for (const auto& [path, value] : document.parameters)
        parameters.entries.emplace_back (path, value);

    root.entries.emplace_back ("parameters", std::move (parameters));

    Value assets;
    assets.type = Value::Type::Array;

    for (const auto& reference : document.assetReferences)
        assets.items.push_back (textValue (reference));

    root.entries.emplace_back ("assets", std::move (assets));

    Value items;
    items.type = Value::Type::Array;

    for (const auto& entry : report.entries)
    {
        Value item;
        item.type = Value::Type::Map;
        item.entries.emplace_back ("severity", intValue (severityCode (entry.severity)));
        item.entries.emplace_back ("modulePath", textValue (entry.modulePath));
        item.entries.emplace_back ("item", textValue (entry.item));
        item.entries.emplace_back ("reason", textValue (entry.reason));
        items.items.push_back (std::move (item));
    }

    root.entries.emplace_back ("report", std::move (items));

    const auto payload = encodeCbor (root);

    std::vector<std::byte> out;
    out.reserve (magic.size() + 1 + 4 + payload.size());

    for (const auto character : magic)
        out.push_back (static_cast<std::byte> (static_cast<unsigned char> (character)));

    // The magic is a string constant on the writing side, so its terminator is part of the block and a
    // reader comparing `magic.size() + 1` bytes sees a C string. Encoding it here rather than asking
    // every caller to pass the NUL keeps the two writers from disagreeing about it.
    out.push_back (std::byte { 0 });

    for (int i = 0; i < 4; ++i)
        out.push_back (byteOf ((format >> (8 * i)) & 0xffu));

    out.insert (out.end(), payload.begin(), payload.end());
    return out;
}

BlockResult decodePresetBlock (const std::span<const std::byte> bytes, const std::string_view magic,
                               const unsigned int format)
{
    BlockResult result;

    const auto headerSize = magic.size() + 1 + 4;

    if (bytes.size() < headerSize)
    {
        result.error = "the state block is too small to be a xerum state block";
        return result;
    }

    if (std::memcmp (bytes.data(), magic.data(), magic.size()) != 0
        || std::to_integer<unsigned char> (bytes[magic.size()]) != 0)
    {
        result.error = "the state block is not xerum's (magic does not match)";
        return result;
    }

    unsigned int version = 0;

    for (int i = 0; i < 4; ++i)
        version |= static_cast<unsigned int> (std::to_integer<unsigned char> (bytes[magic.size() + 1 + static_cast<std::size_t> (i)]))
                   << (8 * i);

    if (version != format)
    {
        result.error = "the state block's format version is " + std::to_string (version) + ", this build writes "
                       + std::to_string (format);
        return result;
    }

    const std::span<const std::byte> payload { bytes.data() + headerSize, bytes.size() - headerSize };

    try
    {
        const auto decoded = decodeCbor (payload);

        if (! decoded.ok)
        {
            result.error = "the state block's payload is damaged: " + decoded.error;
            return result;
        }

        if (! decoded.value.isMap())
        {
            result.error = "the state block is not a map";
            return result;
        }

        if (const auto* marker = decoded.value.find ("format");
            marker == nullptr || ! marker->isNumber() || marker->integer != static_cast<std::int64_t> (format))
        {
            result.error = "the state block has no usable format marker";
            return result;
        }

        result.document.schemaVersion = decoded.value.stringOr ("schemaVersion");
        result.document.name = decoded.value.stringOr ("name");

        if (const auto* split = decoded.value.find ("modelReportEntries"); split != nullptr && split->isNumber())
            result.reportEntriesFromModel = static_cast<int> (split->integer);

        if (const auto* parameters = decoded.value.find ("parameters"); parameters != nullptr && parameters->isMap())
            for (const auto& [path, value] : parameters->entries)
                result.document.parameters.emplace (path, value);

        if (const auto* assets = decoded.value.find ("assets"); assets != nullptr && assets->isArray())
            for (const auto& asset : assets->items)
                if (asset.isText())
                    result.document.assetReferences.push_back (asset.text);

        if (const auto* items = decoded.value.find ("report"); items != nullptr && items->isArray())
        {
            for (const auto& item : items->items)
            {
                if (! item.isMap())
                    continue;

                const auto* severity = item.find ("severity");

                ReportEntry entry;
                entry.severity = severityFromCode (severity != nullptr && severity->isNumber() ? severity->integer : 0);
                entry.modulePath = item.stringOr ("modulePath");
                entry.item = item.stringOr ("item");
                entry.reason = item.stringOr ("reason");
                result.report.entries.push_back (std::move (entry));
            }
        }

        result.ok = true;
    }
    catch (const std::exception& e)
    {
        result.ok = false;
        result.error = std::string ("reading the state block threw: ") + e.what();
    }
    catch (...)
    {
        result.ok = false;
        result.error = "reading the state block threw an unknown exception";
    }

    return result;
}

} // namespace xerum::model
