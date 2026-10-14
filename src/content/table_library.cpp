#include "table_library.h"

#include <algorithm>
#include <fstream>

namespace xerum::content
{

std::string_view toString (Resolution resolution)
{
    switch (resolution)
    {
        case Resolution::Exact:           return "exact";
        case Resolution::CaseInsensitive: return "case-insensitive";
        case Resolution::Missing:         return "missing";
        case Resolution::Unreadable:      return "unreadable";
    }
    return "missing";
}

namespace
{

std::string normaliseReference (std::string_view reference)
{
    std::string out (reference);

    for (auto& c : out)
        if (c == '\\')
            c = '/';               // packs authored on Windows spell paths the other way

    while (! out.empty() && out.front() == '/')
        out.erase (out.begin());   // leading separators are relative to nothing

    return out;
}

/// Does the path on disk spell every segment exactly as the reference did?
///
/// This is the only way to tell an exact hit from a lucky one. On a case-insensitive volume — which is
/// what macOS gives you by default — `exists()` succeeds for `basic shapes.WAV` whether or not the file is
/// spelled that way, so asking the filesystem is not enough: the *directory entries* have to be compared.
/// Without this the CaseInsensitive resolution would be dead code on the machine most likely to need it.
bool spelledExactly (const std::filesystem::path& root, const std::filesystem::path& suffix)
{
    std::filesystem::path current = root;

    for (const auto& segment : suffix)
    {
        std::error_code error;
        std::filesystem::path exact;

        for (const auto& entry : std::filesystem::directory_iterator (current, error))
            if (entry.path().filename() == segment)
            {
                exact = entry.path();
                break;
            }

        if (exact.empty())
            return false;

        current = exact;
    }

    return true;
}

/// A case-insensitive lookup inside one directory, for a filesystem that is case-*sensitive*.
std::filesystem::path findByIgnoringCase (const std::filesystem::path& root, const std::string& relative)
{
    std::filesystem::path current = root;

    std::size_t start = 0;

    while (start <= relative.size())
    {
        const auto slash = relative.find ('/', start);
        const auto piece = relative.substr (start, slash == std::string::npos ? std::string::npos : slash - start);

        if (piece.empty())
            return {};

        if (! std::filesystem::exists (current / piece))
        {
            std::error_code error;
            std::filesystem::path match;

            for (const auto& entry : std::filesystem::directory_iterator (current, error))
            {
                auto name = entry.path().filename().string();
                std::string lowerName, lowerPiece;

                for (const auto c : name) lowerName.push_back (static_cast<char> (std::tolower (c)));
                for (const auto c : piece) lowerPiece.push_back (static_cast<char> (std::tolower (c)));

                if (lowerName == lowerPiece)
                {
                    match = entry.path();
                    break;
                }
            }

            if (match.empty())
                return {};

            current = match;
        }
        else
        {
            current /= piece;
        }

        if (slash == std::string::npos)
            break;

        start = slash + 1;
    }

    return current;
}

} // namespace

void TableLibrary::addRoot (const std::filesystem::path& root)
{
    std::error_code error;

    if (! std::filesystem::exists (root, error))
        return;

    if (std::find (roots_.begin(), roots_.end(), root) == roots_.end())
        roots_.push_back (root);
}

TableLookup TableLibrary::loadTable (std::string_view reference) const
{
    const auto relative = normaliseReference (reference);

    if (relative.empty())
    {
        TableLookup lookup;
        lookup.error = "empty asset reference";
        return lookup;
    }

    // The reference's own layout: table references resolve under `Tables`.
    const auto candidates = { std::filesystem::path ("Tables") / relative, std::filesystem::path (relative) };

    for (const auto& root : roots_)
    {
        for (const auto& suffix : candidates)
        {
            const auto candidate = root / suffix;

            if (std::filesystem::exists (candidate))
            {
                const bool exactSpelling = spelledExactly (root, suffix);
                const auto key = candidate.string();

                if (const auto cached = cache_.find (key); cached != cache_.end())
                    return cached->second;

                std::ifstream in (candidate, std::ios::binary);
                std::vector<std::byte> bytes ((std::size_t) std::filesystem::file_size (candidate));

                if (in.good() && ! bytes.empty())
                {
                    in.read (reinterpret_cast<char*> (bytes.data()), (std::streamsize) bytes.size());

                    const auto parsed = xerum::dsp::parseWavetable (bytes);

                    TableLookup lookup;
                    lookup.path = candidate;

                    if (parsed.ok)
                    {
                        lookup.resolution = exactSpelling ? Resolution::Exact : Resolution::CaseInsensitive;
                        lookup.table = parsed.table;
                        lookup.needsUnimplementedInterpolation =
                            parsed.table.interpolation == xerum::dsp::FrameInterpolation::SpectralMorph;
                    }
                    else
                    {
                        lookup.resolution = Resolution::Unreadable;
                        lookup.error = parsed.error;
                    }

                    cache_.emplace (key, lookup);
                    return lookup;
                }

                TableLookup lookup;
                lookup.resolution = Resolution::Unreadable;
                lookup.path = candidate;
                lookup.error = "could not read " + candidate.string();
                cache_.emplace (key, lookup);
                return lookup;
            }
        }

        // Exact failed everywhere under this root; try ignoring case before moving on.
        for (const auto& suffix : candidates)
        {
            const auto found = findByIgnoringCase (root / suffix.parent_path(), suffix.filename().string());

            if (! found.empty() && std::filesystem::exists (found))
            {
                const auto key = found.string();

                if (const auto cached = cache_.find (key); cached != cache_.end())
                {
                    auto lookup = cached->second;
                    lookup.resolution = Resolution::CaseInsensitive;
                    return lookup;
                }

                std::ifstream in (found, std::ios::binary);
                std::vector<std::byte> bytes ((std::size_t) std::filesystem::file_size (found));

                if (in.good() && ! bytes.empty())
                {
                    in.read (reinterpret_cast<char*> (bytes.data()), (std::streamsize) bytes.size());

                    const auto parsed = xerum::dsp::parseWavetable (bytes);

                    TableLookup lookup;
                    lookup.path = found;
                    lookup.resolution = parsed.ok ? Resolution::CaseInsensitive : Resolution::Unreadable;

                    if (parsed.ok)
                    {
                        lookup.table = parsed.table;
                        cache_.emplace (key, lookup);
                    }
                    else
                    {
                        lookup.error = parsed.error;
                    }

                    return lookup;
                }
            }
        }
    }

    TableLookup lookup;
    lookup.resolution = Resolution::Missing;
    lookup.error = "not found in " + std::to_string (roots_.size()) + " content root(s)";
    return lookup;
}

} // namespace xerum::content
