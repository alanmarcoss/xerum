// The offline wavetable authoring tool (ticket 34).
//
// xerum ships only content we author, and content is added *through this tool* so that what ships is
// what the tool generated rather than whatever was in a working directory (docs/content-policy.md).
// The tool:
//
//   * evaluates the formulas in `xerum::dsp::starterLibrary()` and writes each table as RIFF/WAVE in
//     the format ticket 24 reads (the `clm ` chunk carries the interpolation mode and the factory flag);
//   * writes the provenance manifest — every shipped file, with its origin, licence, formula and hash;
//   * `--verify` re-generates every table in memory and refuses to pass if the bytes on disk are not
//     exactly what the formulas produce, so the tree cannot drift from its source.
//
// It is not product code: nothing in the plugin links this, and it never writes outside the content
// root it is given — in particular it refuses to write into the reference instrument's content tree.
//
//   xerum-wavetable-author --out content --write --manifest content/manifest.json
//   xerum-wavetable-author --verify

#include "authoring.h"
#include "wavetable_write.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

// The provenance line every table this tool writes carries, and the licence it ships under.
constexpr std::string_view kOrigin =
    "authored for xerum: generated from the formula recorded here by tools/wavetable-author.cpp";

using namespace xerum::authoring;

std::string interpolationName (xerum::dsp::FrameInterpolation interpolation)
{
    return std::string (xerum::dsp::toString (interpolation));
}

struct Entry
{
    const xerum::dsp::TableSpec* spec { nullptr };
    std::vector<std::byte> bytes;
    std::filesystem::path path;
};

bool generate (const std::filesystem::path& out, std::vector<Entry>& entries, std::string& error)
{
    for (const auto& spec : xerum::dsp::starterLibrary())
    {
        xerum::dsp::Wavetable table;
        std::string buildError;

        if (! xerum::dsp::buildTable (spec, table, buildError))
        {
            error = spec.name + ": " + buildError;
            return false;
        }

        const auto encoded = xerum::dsp::encodeWavetable (table, xerum::dsp::encodeOptionsFor (spec));

        if (! encoded.ok)
        {
            error = spec.name + ": " + encoded.error;
            return false;
        }

        entries.push_back ({ &spec, encoded.bytes, out / "Tables" / spec.relativePath });
    }

    return true;
}

void printCatalogue()
{
    for (const auto& spec : xerum::dsp::starterLibrary())
        std::cout << std::left << std::setw (16) << spec.name
                  << std::setw (30) << ("Tables/" + spec.relativePath)
                  << std::setw (8) << spec.frameCount
                  << std::setw (16) << interpolationName (spec.interpolation)
                  << (spec.fallback ? "fallback  " : "          ")
                  << spec.formula << '\n';
}

std::string manifestJson (const std::vector<Entry>& entries)
{
    std::ostringstream out;
    out << "{\n";
    out << "  \"policy\": \"docs/content-policy.md\",\n";
    out << "  \"generatedBy\": \"tools/wavetable-author.cpp\",\n";
    out << "  \"licence\": \"" << kLicence << "\",\n";
    out << "  \"origin\": \"" << escapeJson (kOrigin) << "\",\n";
    out << "  \"note\": \"Every file below is content xerum authored; regenerating it with the "
           "recorded command must reproduce these bytes exactly. tools/check-content-provenance.py "
           "fails the release when a shipped file is absent from this list, and verifies, where the "
           "reference instrument is installed, that no file here is byte-identical to one of its files.\",\n";
    out << "  \"command\": \"xerum-wavetable-author --out content --write --manifest content/manifest.json\",\n";
    out << "  \"tables\": [\n";

    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const auto& entry = entries[i];
        const auto& spec = *entry.spec;

        out << "    {\n";
        out << "      \"path\": \"" << escapeJson ((std::filesystem::path ("Tables") / spec.relativePath).generic_string()) << "\",\n";
        out << "      \"name\": \"" << escapeJson (spec.name) << "\",\n";
        out << "      \"origin\": \"" << escapeJson (kOrigin) << "\",\n";
        out << "      \"licence\": \"" << kLicence << "\",\n";
        out << "      \"formula\": \"" << escapeJson (spec.formula) << "\",\n";
        out << "      \"description\": \"" << escapeJson (spec.description) << "\",\n";
        out << "      \"frames\": " << spec.frameCount << ",\n";
        out << "      \"frameSize\": " << xerum::dsp::kDefaultFrameSize << ",\n";
        out << "      \"interpolation\": \"" << interpolationName (spec.interpolation) << "\",\n";
        out << "      \"encoding\": \"" << (spec.sixteenBit ? "pcm-s16" : "ieee-float32") << "\",\n";
        out << "      \"bytes\": " << entry.bytes.size() << ",\n";

        if (spec.fallback)
            out << "      \"fallback\": true,\n";

        out << "      \"sha256\": \"" << sha256Hex (entry.bytes) << "\"\n";
        out << "    }" << (i + 1 == entries.size() ? "\n" : ",\n");
    }

    out << "  ]\n";
    out << "}\n";
    return out.str();
}

void usage()
{
    std::cout <<
        "xerum-wavetable-author - offline wavetable authoring tool (ticket 34)\n"
        "\n"
        "  --out DIR         content root to write into (default: content)\n"
        "  --write           write every table under DIR/Tables (the default action)\n"
        "  --manifest [PATH] write the provenance manifest (default PATH: DIR/manifest.json)\n"
        "  --verify          check every file on disk is exactly what the formulas generate\n"
        "  --list            list the catalogue and exit\n"
        "  --help            this text\n";
}

} // namespace

int main (int argc, char** argv)
{
    std::filesystem::path out = "content";
    std::filesystem::path manifest;
    bool write = false;
    bool verify = false;
    bool list = false;
    bool manifestRequested = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];

        if (argument == "--out" && i + 1 < argc)
            out = argv[++i];
        else if (argument == "--manifest")
        {
            manifestRequested = true;

            if (i + 1 < argc && argv[i + 1][0] != '-')
                manifest = argv[++i];
        }
        else if (argument == "--write")
            write = true;
        else if (argument == "--verify")
            verify = true;
        else if (argument == "--list")
            list = true;
        else if (argument == "--help" || argument == "-h")
        {
            usage();
            return 0;
        }
        else
        {
            std::cerr << "unknown argument: " << argument << "\n";
            usage();
            return 2;
        }
    }

    if (list)
    {
        printCatalogue();
        return 0;
    }

    if (! write && ! verify && ! manifestRequested)
        write = true;   // the default action

    if (insideReferenceRoot (std::filesystem::absolute (out)))
    {
        std::cerr << "refusing to write into the reference instrument's content tree: "
                  << std::filesystem::absolute (out) << "\n";
        return 1;
    }

    std::vector<Entry> entries;
    std::string error;

    if (! generate (out, entries, error))
    {
        std::cerr << "generating content failed: " << error << "\n";
        return 1;
    }

    if (write)
    {
        for (const auto& entry : entries)
        {
            if (! writeFile (entry.path, entry.bytes))
            {
                std::cerr << "could not write " << entry.path << "\n";
                return 1;
            }

            std::cout << "wrote " << entry.path << " (" << entry.bytes.size() << " bytes)\n";
        }
    }

    if (manifestRequested)
    {
        const auto path = manifest.empty() ? out / "manifest.json" : manifest;

        std::error_code code;
        std::filesystem::create_directories (path.parent_path(), code);

        std::ofstream stream (path, std::ios::trunc);

        if (! stream)
        {
            std::cerr << "could not write " << path << "\n";
            return 1;
        }

        stream << manifestJson (entries);
        stream.close();
        std::cout << "wrote " << path << " (" << entries.size() << " entries)\n";
    }

    if (verify)
    {
        std::size_t failures = 0;

        for (const auto& entry : entries)
        {
            std::vector<std::byte> onDisk;

            if (! readFile (entry.path, onDisk))
            {
                std::cerr << "missing: " << entry.path << "\n";
                ++failures;
                continue;
            }

            if (onDisk.size() != entry.bytes.size())
            {
                std::cerr << "size differs: " << entry.path << " (" << onDisk.size() << " on disk, "
                          << entry.bytes.size() << " generated)\n";
                ++failures;
                continue;
            }

            if (std::memcmp (onDisk.data(), entry.bytes.data(), onDisk.size()) != 0)
            {
                std::cerr << "bytes differ: " << entry.path << "\n";
                ++failures;
                continue;
            }

            std::cout << "ok " << entry.path << "\n";
        }

        if (failures != 0)
        {
            std::cerr << failures << " of " << entries.size()
                      << " shipped tables do not match what the tool generates\n";
            return 1;
        }

        std::cout << "verified " << entries.size() << " tables against their formulas\n";
    }

    return 0;
}
