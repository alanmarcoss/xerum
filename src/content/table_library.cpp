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

/// Is this error simply "there is nothing there"?
///
/// It matters, because the two have opposite meanings here (ticket `56`): **absence** is an ordinary miss —
/// a preset naming an asset no root has is the import contract's every-day case — while a path that *is*
/// there and will not be read is a refusal, and a refusal is reported. `ENOENT` and `ENOTDIR` are how a
/// filesystem spells absence, both from a walk over a directory that is not there and from a stat of a path
/// that is not; libc++ already folds both into `file_type::not_found` for the stat, and this is the same
/// distinction made where the error code is all there is.
bool isAbsence (const std::error_code& error)
{
    return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory;
}

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
///
/// `refusal` is set when a directory could not be listed at all — a refusal, which is a fact about the
/// content root rather than about the spelling, and one the caller reports (ticket `56`).
bool spelledExactly (const std::filesystem::path& root, const std::filesystem::path& suffix,
                     ContentProblem& refusal)
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

        if (error && ! isAbsence (error))
        {
            refusal = { current, "could not be listed (" + error.message() + ")" };
            return false;
        }

        if (exact.empty())
            return false;

        current = exact;
    }

    return true;
}

/// A case-insensitive lookup inside one directory, for a filesystem that is case-*sensitive*.
std::filesystem::path findByIgnoringCase (const std::filesystem::path& root, const std::string& relative,
                                          ContentProblem& refusal)
{
    std::filesystem::path current = root;

    std::size_t start = 0;

    while (start <= relative.size())
    {
        const auto slash = relative.find ('/', start);
        const auto piece = relative.substr (start, slash == std::string::npos ? std::string::npos : slash - start);

        if (piece.empty())
            return {};

        // Non-throwing, like every other question this file asks the filesystem: a path it may not look at
        // is a refusal to report, not an exception to unwind through a host (ticket `56`).
        std::error_code failing;
        const bool present = std::filesystem::exists (current / piece, failing);

        if (failing && ! isAbsence (failing))
        {
            refusal = { current, "could not be read (" + failing.message() + ")" };
            return {};
        }

        if (! present)
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
            {
                if (error && ! isAbsence (error))
                    refusal = { current, "could not be listed (" + error.message() + ")" };

                return {};
            }

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

void TableLibrary::noteProblem (const std::filesystem::path& path, std::string reason) const
{
    if (hasProblemFor (path))
        return;

    problems_.push_back ({ path, std::move (reason) });
}

bool TableLibrary::hasProblemFor (const std::filesystem::path& path) const
{
    return std::any_of (problems_.begin(), problems_.end(),
                        [&path] (const ContentProblem& problem) { return problem.path == path; });
}

void TableLibrary::addRoot (const std::filesystem::path& root)
{
    if (std::find (roots_.begin(), roots_.end(), root) != roots_.end() || hasProblemFor (root))
        return;

    std::error_code error;
    const auto status = std::filesystem::status (root, error);

    if (! std::filesystem::exists (status))
    {
        // Not there: an ordinary candidate — a machine that has not installed the reference has one — and
        // saying so would be noise. A refusal that happened to the path's *parent* leaves the type as
        // `not_found` too, so the error is the thing that tells them apart (ticket `56`).
        if (error && ! isAbsence (error))
            noteProblem (root, "the content root could not be examined (" + error.message() + ")");

        return;
    }

    roots_.push_back (root);
}

std::optional<std::filesystem::path> TableLibrary::findAsset (std::string_view reference,
                                                              std::string_view subdirectory,
                                                              Resolution& resolution, std::string& error) const
{
    resolution = Resolution::Missing;
    error.clear();

    const auto relative = normaliseReference (reference);

    if (relative.empty())
    {
        error = "empty asset reference";
        return std::nullopt;
    }

    // The reference's own layout: a table resolves under `Tables` and an impulse under `Impulses`, and both
    // fall back to the reference exactly as the preset spelled it.
    const auto candidates = { std::filesystem::path (subdirectory) / relative, std::filesystem::path (relative) };

    for (const auto& root : roots_)
    {
        // A path inside a root that will not be read — a refused root, a directory with its permissions
        // taken away — is a fact about the content root, and it is recorded *and skipped*: the roots after
        // it are still asked (ticket `56`). A stall is not recorded here at all: it never returns.
        ContentProblem refusal;

        for (const auto& suffix : candidates)
        {
            const auto candidate = root / suffix;

            // **The `error_code` overload, everywhere on this path.** A stat that fails for anything but
            // "not there" — a permission, a symlink loop, an I/O error — used to throw a
            // `filesystem_error` out of the constructor and abort the host; the crash report is
            // `__throw_filesystem_error` under `TableLibrary::loadTable`. It is a value now.
            std::error_code failing;
            const bool present = std::filesystem::exists (candidate, failing);

            if (failing && ! isAbsence (failing))
            {
                noteProblem (candidate, "could not be read (" + failing.message() + ")");
                continue;
            }

            if (! present)
                continue;

            resolution = spelledExactly (root, suffix, refusal) ? Resolution::Exact : Resolution::CaseInsensitive;

            // A refusal met on the way to a hit is still a refusal (ticket `56`): the file resolved, but
            // something above it would not be listed, and that is worth saying rather than swallowing.
            if (! refusal.path.empty())
                noteProblem (refusal.path, refusal.reason);

            return candidate;
        }

        // Exact failed everywhere under this root; try ignoring case before moving on.
        for (const auto& suffix : candidates)
        {
            const auto found = findByIgnoringCase (root / suffix.parent_path(), suffix.filename().string(),
                                                   refusal);

            std::error_code failing;
            const bool present = ! found.empty() && std::filesystem::exists (found, failing);

            if (failing && ! isAbsence (failing))
                noteProblem (found, "could not be read (" + failing.message() + ")");

            if (! present)
                continue;

            resolution = Resolution::CaseInsensitive;
            return found;
        }

        if (! refusal.path.empty())
            noteProblem (refusal.path, refusal.reason);
    }

    resolution = Resolution::Missing;
    error = "not found in " + std::to_string (roots_.size()) + " content root(s)";

    if (! problems_.empty())
        error += "; " + std::to_string (problems_.size()) + " content path(s) could not be read";

    return std::nullopt;
}

TableLookup TableLibrary::loadTable (std::string_view reference) const
{
    Resolution resolution;
    std::string error;
    const auto found = findAsset (reference, "Tables", resolution, error);

    if (! found.has_value())
    {
        TableLookup lookup;
        lookup.resolution = resolution;
        lookup.error = error;
        return lookup;
    }

    const auto key = found->string();

    if (const auto cached = cache_.find (key); cached != cache_.end())
    {
        // The *spelling* is a fact about the reference that asked, not about the file: a hit through the
        // case-insensitive pass is reported as one even when another reference already read that same file
        // exactly. A read that failed stays failed — that is a fact about the file.
        auto lookup = cached->second;

        if (lookup.found())
            lookup.resolution = resolution;

        return lookup;
    }

    std::ifstream in (*found, std::ios::binary);

    // The **non-throwing** size call, deliberately: a specious entry in a content root — a FIFO, a socket,
    // anything the filesystem will not size — used to throw out of here, and an exception is not a report.
    // It is an unreadable asset like any other (ticket `56`).
    std::error_code sizing;
    const auto size = std::filesystem::file_size (*found, sizing);

    if (sizing)
    {
        TableLookup lookup;
        lookup.resolution = Resolution::Unreadable;
        lookup.path = *found;
        lookup.error = "could not read " + found->string() + " (" + sizing.message() + ")";
        cache_.emplace (key, lookup);
        return lookup;
    }

    std::vector<std::byte> bytes (static_cast<std::size_t> (size));

    if (in.good() && ! bytes.empty())
    {
        in.read (reinterpret_cast<char*> (bytes.data()), (std::streamsize) bytes.size());

        const auto parsed = xerum::dsp::parseWavetable (bytes);

        TableLookup lookup;
        lookup.path = *found;
        lookup.resolution = resolution;

        if (parsed.ok)
            lookup.table = parsed.table;
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
    lookup.path = *found;
    lookup.error = "could not read " + found->string();
    cache_.emplace (key, lookup);
    return lookup;
}

AssetLookup TableLibrary::loadAsset (std::string_view reference, std::string_view subdirectory) const
{
    Resolution resolution;
    std::string error;
    const auto found = findAsset (reference, subdirectory, resolution, error);

    AssetLookup lookup;

    if (! found.has_value())
    {
        lookup.resolution = resolution;
        lookup.error = error;
        return lookup;
    }

    const auto key = found->string();

    if (const auto cached = assetCache_.find (key); cached != assetCache_.end())
    {
        auto cachedLookup = cached->second;

        if (cachedLookup.found())
            cachedLookup.resolution = resolution;

        return cachedLookup;
    }

    lookup.path = *found;
    lookup.resolution = resolution;

    std::error_code sizing;
    const auto size = std::filesystem::file_size (*found, sizing);

    if (sizing)
    {
        lookup.resolution = Resolution::Unreadable;
        lookup.error = "could not read " + found->string() + " (" + sizing.message() + ")";
        assetCache_.emplace (key, lookup);
        return lookup;
    }

    std::ifstream in (*found, std::ios::binary);
    std::vector<std::byte> bytes (static_cast<std::size_t> (size));

    if (! in.good() || bytes.empty())
    {
        lookup.resolution = Resolution::Unreadable;
        lookup.error = "could not read " + found->string();
        assetCache_.emplace (key, lookup);
        return lookup;
    }

    in.read (reinterpret_cast<char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));

    lookup.bytes = std::move (bytes);
    assetCache_.emplace (key, lookup);
    return lookup;
}

} // namespace xerum::content
