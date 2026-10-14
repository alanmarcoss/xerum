// xerum::plugin — reading a preset, and writing xerum's own state block. See PresetState.h.

#include "PresetState.h"

#include "PluginIdentity.h"
#include "cbor.h"
#include "cbor_write.h"
#include "container.h"
#include "decompress.h"
#include "legacy.h"
#include "schema.h"

#include <cstring>
#include <sstream>

namespace xerum::plugin
{

namespace
{

model::Value textValue (std::string text)
{
    model::Value value;
    value.type = model::Value::Type::String;
    value.text = std::move (text);
    return value;
}

model::Value intValue (const std::int64_t number)
{
    model::Value value;
    value.type = model::Value::Type::Int;
    value.integer = number;
    return value;
}

int severityCode (const model::Severity severity)
{
    switch (severity)
    {
        case model::Severity::Info:   return 0;
        case model::Severity::Medium: return 1;
        case model::Severity::High:   return 2;
    }

    return 0;
}

model::Severity severityFromCode (const std::int64_t code)
{
    if (code == 2)
        return model::Severity::High;

    if (code == 1)
        return model::Severity::Medium;

    return model::Severity::Info;
}

void appendUint32 (std::vector<std::byte>& out, const unsigned int value)
{
    for (int i = 0; i < 4; ++i)
        out.push_back (static_cast<std::byte> ((value >> (8 * i)) & 0xffu));
}

unsigned int readUint32 (const std::byte* bytes)
{
    unsigned int value = 0;

    for (int i = 0; i < 4; ++i)
        value |= static_cast<unsigned int> (std::to_integer<unsigned char> (bytes[i])) << (8 * i);

    return value;
}

} // namespace

LoadedPreset loadPresetPayload (const model::Value& payload, std::string_view schemaVersion)
{
    LoadedPreset result;

    try
    {
        auto loaded = model::loadPreset (payload, schemaVersion, model::Schema::instance(),
                                        model::SupportCatalogue::v1());

        result.ok = loaded.ok;
        result.error = loaded.error;
        result.document = std::move (loaded.document);
        result.report = std::move (loaded.report);
        result.reportEntriesFromModel = static_cast<int> (result.report.entries.size());
    }
    catch (const std::exception& e)
    {
        // `decompress` documents `std::bad_alloc` as the one exception it may propagate, and the model
        // layer's own promise is that nothing else throws. Neither may cross the plugin boundary, so it
        // is caught and reported like any other failure to read a file.
        result.ok = false;
        result.error = std::string ("reading the preset threw: ") + e.what();
    }
    catch (...)
    {
        result.ok = false;
        result.error = "reading the preset threw an unknown exception";
    }

    return result;
}

LoadedPreset loadPresetFile (const juce::File& file)
{
    LoadedPreset result;

    juce::MemoryBlock block;

    if (! file.loadFileAsData (block))
    {
        result.error = "could not read " + file.getFullPathName().toStdString();
        return result;
    }

    const auto* data = static_cast<const std::byte*> (block.getData());
    const std::span<const std::byte> bytes { data, block.getSize() };

    try
    {
        const auto container = model::readContainer (bytes);

        if (! container.ok)
        {
            result.error = container.error;
            return result;
        }

        const auto decompressed = model::decompress (container.payload, container.codec, container.declaredSize);

        if (! decompressed.ok)
        {
            result.error = decompressed.error;
            return result;
        }

        model::ValueResult decoded;

        if (container.codec == model::Codec::Zstd)
            decoded = model::decodeCbor (decompressed.bytes);
        else if (container.codec == model::Codec::Zlib)
            decoded = model::decodeLegacy (decompressed.bytes);
        else
        {
            result.error = "container codec is not one xerum reads";
            return result;
        }

        if (! decoded.ok)
        {
            result.error = decoded.error;
            return result;
        }

        result = loadPresetPayload (decoded.value, container.schemaVersion);

        if (result.ok && result.document.name.empty())
            result.document.name = container.presetName;

        return result;
    }
    catch (const std::exception& e)
    {
        result.ok = false;
        result.error = std::string ("reading the preset threw: ") + e.what();
    }
    catch (...)
    {
        result.ok = false;
        result.error = "reading the preset threw an unknown exception";
    }

    return result;
}

std::vector<std::byte> encodeStateBlock (const model::PresetDocument& document,
                                         const model::ImportReport& report,
                                         const int reportEntriesFromModel)
{
    model::Value root;
    root.type = model::Value::Type::Map;

    root.entries.emplace_back ("format", intValue (identity::kStateFormat));
    root.entries.emplace_back ("schemaVersion", textValue (document.schemaVersion));
    root.entries.emplace_back ("name", textValue (document.name));
    root.entries.emplace_back ("modelReportEntries", intValue (reportEntriesFromModel));

    model::Value parameters;
    parameters.type = model::Value::Type::Map;

    for (const auto& [path, value] : document.parameters)
        parameters.entries.emplace_back (path, value);

    root.entries.emplace_back ("parameters", std::move (parameters));

    model::Value assets;
    assets.type = model::Value::Type::Array;

    for (const auto& reference : document.assetReferences)
        assets.items.push_back (textValue (reference));

    root.entries.emplace_back ("assets", std::move (assets));

    model::Value items;
    items.type = model::Value::Type::Array;

    for (const auto& entry : report.entries)
    {
        model::Value item;
        item.type = model::Value::Type::Map;
        item.entries.emplace_back ("severity", intValue (severityCode (entry.severity)));
        item.entries.emplace_back ("modulePath", textValue (entry.modulePath));
        item.entries.emplace_back ("item", textValue (entry.item));
        item.entries.emplace_back ("reason", textValue (entry.reason));
        items.items.push_back (std::move (item));
    }

    root.entries.emplace_back ("report", std::move (items));

    // `encodeCbor` sorts map keys and uses definite-length containers, so the same document always
    // produces the same bytes — which is what makes a byte-identical re-render a meaningful test.
    auto payload = model::encodeCbor (root);

    std::vector<std::byte> out;
    out.reserve (identity::kStateMagicSize + 4 + payload.size());

    const auto* magic = reinterpret_cast<const std::byte*> (identity::kStateMagic);

    for (unsigned int i = 0; i < identity::kStateMagicSize; ++i)
        out.push_back (magic[i]);

    appendUint32 (out, identity::kStateFormat);
    out.insert (out.end(), payload.begin(), payload.end());
    return out;
}

LoadedPreset decodeStateBlock (const void* data, const int size)
{
    LoadedPreset result;

    if (data == nullptr || size < static_cast<int> (identity::kStateMagicSize) + 4)
    {
        result.error = "the state block is too small to be a xerum state block";
        return result;
    }

    const auto* bytes = static_cast<const std::byte*> (data);

    if (std::memcmp (bytes, identity::kStateMagic, identity::kStateMagicSize) != 0)
    {
        result.error = "the state block is not xerum's (magic does not match)";
        return result;
    }

    const auto version = readUint32 (bytes + identity::kStateMagicSize);

    if (version != identity::kStateFormat)
    {
        result.error = "the state block's format version is " + std::to_string (version) + ", this build writes "
                       + std::to_string (identity::kStateFormat);
        return result;
    }

    const std::span<const std::byte> payload { bytes + identity::kStateMagicSize + 4,
                                               static_cast<std::size_t> (size) - identity::kStateMagicSize - 4 };

    try
    {
        const auto decoded = model::decodeCbor (payload);

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

        if (const auto* format = decoded.value.find ("format");
            format == nullptr || ! format->isNumber() || format->integer != static_cast<std::int64_t> (identity::kStateFormat))
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

                model::ReportEntry entry;
                entry.severity = severityFromCode (item.find ("severity") != nullptr
                                                       ? item.find ("severity")->integer
                                                       : 0);
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

void addReportEntry (model::ImportReport& report, const model::Severity severity, std::string modulePath,
                     std::string item, std::string reason)
{
    model::ReportEntry entry;
    entry.severity = severity;
    entry.modulePath = std::move (modulePath);
    entry.item = std::move (item);
    entry.reason = std::move (reason);
    report.entries.push_back (std::move (entry));
}

std::string reportText (const model::ImportReport& report)
{
    std::ostringstream out;

    out << "high " << report.count (model::Severity::High) << "  medium "
        << report.count (model::Severity::Medium) << "  info " << report.count (model::Severity::Info) << "\n\n";

    if (report.clean())
    {
        // Per the contract: empty, not absent. Saying so is the difference between "nothing to report" and
        // "no report", and a user must be able to tell them apart.
        out << "Nothing to report: every module, parameter and asset this preset asks for is one xerum implements.\n";
        return out.str();
    }

    for (const auto& entry : report.entries)
        out << "[" << model::toString (entry.severity) << "] " << entry.modulePath << " · " << entry.item
            << "\n         " << entry.reason << "\n";

    return out.str();
}

} // namespace xerum::plugin
