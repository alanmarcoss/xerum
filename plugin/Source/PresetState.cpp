// xerum::plugin — reading a preset, and writing xerum's own state block. See PresetState.h.

#include "PresetState.h"

#include "PluginIdentity.h"
#include "cbor.h"
#include "container.h"
#include "decompress.h"
#include "legacy.h"
#include "preset_block.h"
#include "schema.h"

#include <sstream>

namespace xerum::plugin
{

// The block's format is one implementation in the model layer (`model::encodePresetBlock` /
// `decodePresetBlock`), because the factory bank ships documents in the same form and the offline
// authoring tool has to write them without JUCE. The **identity** stays the plugin's: these asserts are
// what stop the tool's copy of the magic and the version from drifting away from the product's.
static_assert (model::kPresetBlockMagic == std::string_view (identity::kStateMagic),
               "the model layer's preset-block magic must be the plugin identity's state magic");
static_assert (model::kPresetBlockMagic.size() + 1 == identity::kStateMagicSize,
               "the state magic is NUL-terminated on disk, and the model layer writes that terminator");
static_assert (model::kPresetBlockFormat == identity::kStateFormat,
               "the model layer's preset-block version must be the plugin identity's state format");

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
    // One encoder, in the model layer: the factory bank's files are documents in exactly this block, and
    // the offline tool that writes them links no JUCE (`src/model/preset_block.h` explains the split).
    return model::encodePresetBlock (document, report, reportEntriesFromModel, identity::kStateMagic,
                                     identity::kStateFormat);
}

LoadedPreset decodeStateBlock (const void* data, const int size)
{
    LoadedPreset result;

    if (data == nullptr || size < 0)
    {
        result.error = "the state block is too small to be a xerum state block";
        return result;
    }

    const auto* bytes = static_cast<const std::byte*> (data);
    const auto decoded = model::decodePresetBlock ({ bytes, static_cast<std::size_t> (size) },
                                                   identity::kStateMagic, identity::kStateFormat);

    result.ok = decoded.ok;
    result.error = decoded.error;
    result.document = std::move (decoded.document);
    result.report = std::move (decoded.report);
    result.reportEntriesFromModel = decoded.reportEntriesFromModel;
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
