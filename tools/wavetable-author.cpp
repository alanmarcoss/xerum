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

constexpr std::string_view kReferenceRoot = "/Library/Audio/Presets/Xfer Records/Serum 2 Presets/";
constexpr std::string_view kLicence = "AGPL-3.0-or-later";
constexpr std::string_view kOrigin =
    "authored for xerum: generated from the formula recorded here by tools/wavetable-author.cpp";

// ------------------------------------------------------------------------------------ SHA-256

/// A plain SHA-256, so the manifest can say "this file is exactly what the tool produced". Present so a
/// stale or hand-edited content file is detectable without a compiler; not a security boundary.
class Sha256
{
public:
    Sha256()
    {
        state_ = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    }

    void update (const std::byte* data, std::size_t size)
    {
        for (std::size_t i = 0; i < size; ++i)
        {
            buffer_[bufferLength_++] = static_cast<unsigned char> (data[i]);

            if (bufferLength_ == 64)
                compress();
        }

        length_ += size;
    }

    std::string hex()
    {
        const std::uint64_t bitLength = length_ * 8;

        // 0x80, then zeros, then the 64-bit big-endian length, total a multiple of 64.
        const std::size_t padding = bufferLength_ < 56 ? 56 - bufferLength_ : 120 - bufferLength_;

        std::array<unsigned char, 128> pad {};
        pad[0] = 0x80;

        for (std::size_t i = 0; i < 8; ++i)
            pad[padding + i] = static_cast<unsigned char> ((bitLength >> (56 - 8 * i)) & 0xFFu);

        // `update` folds into the length, which is no longer read, so this is safe.
        update (reinterpret_cast<const std::byte*> (pad.data()), padding + 8);

        std::ostringstream out;

        for (const auto word : state_)
            out << std::hex << std::setw (8) << std::setfill ('0') << word;

        return out.str();
    }

private:
    static std::uint32_t rotateRight (std::uint32_t value, unsigned count)
    {
        return (value >> count) | (value << (32 - count));
    }

    void compress()
    {
        static constexpr std::uint32_t k[64] =
        {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };

        std::uint32_t w[64] {};

        for (std::size_t i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t> (buffer_[4 * i]) << 24)
                 | (static_cast<std::uint32_t> (buffer_[4 * i + 1]) << 16)
                 | (static_cast<std::uint32_t> (buffer_[4 * i + 2]) << 8)
                 | static_cast<std::uint32_t> (buffer_[4 * i + 3]);

        for (std::size_t i = 16; i < 64; ++i)
        {
            const std::uint32_t s0 = rotateRight (w[i - 15], 7) ^ rotateRight (w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotateRight (w[i - 2], 17) ^ rotateRight (w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        auto [a, b, c, d, e, f, g, h] = state_;

        for (std::size_t i = 0; i < 64; ++i)
        {
            const std::uint32_t s1 = rotateRight (e, 6) ^ rotateRight (e, 11) ^ rotateRight (e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + choose + k[i] + w[i];
            const std::uint32_t s0 = rotateRight (a, 2) ^ rotateRight (a, 13) ^ rotateRight (a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;

            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;

        bufferLength_ = 0;
    }

    std::array<std::uint32_t, 8> state_ {};
    unsigned char buffer_[64] {};
    std::size_t bufferLength_ { 0 };
    std::uint64_t length_ { 0 };
};

std::string sha256Hex (const std::vector<std::byte>& bytes)
{
    Sha256 hash;
    hash.update (bytes.data(), bytes.size());
    return hash.hex();
}

// ------------------------------------------------------------------------------------ helpers

std::string escapeJson (std::string_view text)
{
    std::string out;

    for (const char c : text)
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char> (c) < 0x20)
                {
                    char buffer[7];
                    std::snprintf (buffer, sizeof (buffer), "\\u%04x", static_cast<unsigned char> (c));
                    out += buffer;
                }
                else
                {
                    out += c;
                }
        }

    return out;
}

bool readFile (const std::filesystem::path& path, std::vector<std::byte>& bytes)
{
    std::ifstream in (path, std::ios::binary);

    if (! in)
        return false;

    in.seekg (0, std::ios::end);
    const auto size = in.tellg();

    if (size < 0)
        return false;

    in.seekg (0, std::ios::beg);
    bytes.resize (static_cast<std::size_t> (size));

    if (! bytes.empty())
        in.read (reinterpret_cast<char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));

    return in.good() || in.eof();
}

bool writeFile (const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
    std::error_code error;
    std::filesystem::create_directories (path.parent_path(), error);

    std::ofstream out (path, std::ios::binary | std::ios::trunc);

    if (! out)
        return false;

    out.write (reinterpret_cast<const char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
    return out.good();
}

/// The tool writes only into the content root it is handed. Writing into another application's install
/// would corrupt it, and the reference maintains a database over that directory (docs/content-policy.md).
bool insideReferenceRoot (const std::filesystem::path& candidate)
{
    std::error_code error;
    const auto reference = std::filesystem::weakly_canonical (kReferenceRoot, error);

    if (error || ! std::filesystem::exists (reference))
        return false;

    auto relative = std::filesystem::relative (std::filesystem::weakly_canonical (candidate, error), reference, error);

    return ! error && ! relative.empty() && relative.native()[0] != '.';
}

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
