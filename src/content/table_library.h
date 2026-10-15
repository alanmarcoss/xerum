// xerum::content — finding and loading assets from a content root.
//
// This is where a preset's asset reference meets the filesystem, and where the resolution rules from the
// import contract live:
//
//   * leading `/` and `//` are stripped before joining;
//   * **both `/` and `\` are separators**, because third-party packs are often authored on Windows;
//   * **exact match first, then case-insensitive**, because a pack that resolves in the reference must
//     resolve here;
//   * a miss is **reported with a reason**, never substituted silently.
//
// A case-insensitive hit is not a failure, but it is not nothing either — it is reported as such, because
// a pack that only works by luck of a filesystem comparison is worth knowing about.

#pragma once

#include "../dsp/wavetable.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xerum::content
{

enum class Resolution
{
    Exact,             ///< found at the path the preset named
    CaseInsensitive,   ///< found, but only by ignoring case
    Missing,           ///< not found in any root
    Unreadable,        ///< found, but could not be read or parsed
};

std::string_view toString (Resolution resolution);

struct TableLookup
{
    Resolution resolution { Resolution::Missing };
    std::string error;              ///< why, when it is not Exact or CaseInsensitive
    std::filesystem::path path;     ///< what it resolved to, when it did
    xerum::dsp::Wavetable table;

    bool found() const { return resolution == Resolution::Exact || resolution == Resolution::CaseInsensitive; }
};

/// A reference resolved to **bytes**, for an asset this layer does not parse.
///
/// Ticket `64`. An impulse response is an audio file the convolver reads, not a wavetable, so the
/// resolution rules are the ones `loadTable` applies — the same walk, the same absence/refusal
/// distinction — and only the read is generic: whoever asked decodes the bytes. Which codec that takes is
/// deliberately not this layer's business; it links no JUCE and touches no decoder.
struct AssetLookup
{
    Resolution resolution { Resolution::Missing };
    std::string error;              ///< why, when it is not Exact or CaseInsensitive
    std::filesystem::path path;     ///< what it resolved to, when it did
    std::vector<std::byte> bytes;

    bool found() const { return resolution == Resolution::Exact || resolution == Resolution::CaseInsensitive; }
};

/// The subdirectory the reference instrument's own content tree keeps its impulse responses under.
/// `Impulses/Factory/Long/Crisp.flac` is a reference as a preset spells it, and the `Impulses/` join is
/// this layer's — exactly as `Tables/` is for a wavetable.
inline constexpr std::string_view kImpulseDirectory = "Impulses";

/// A content path that is **there and cannot be read**, and why.
///
/// Ticket `56`. A root that simply is not there is an ordinary candidate — a machine without the
/// reference instrument has one — and stays silent. A root that exists and refuses to be read is
/// different: it is the largest thing that can fail to load, and the import contract's rule is that
/// nothing is dropped silently. So it is recorded here, carried with the resolution, and reported.
///
/// What this is *not* is a whole explanation of a stall: a directory that will not answer a read
/// (a consent prompt nobody has answered, a network volume, a sleeping drive) never returns an error
/// at all. That case is the caller's to bound, and `OffThreadWork` is how.
struct ContentProblem
{
    std::filesystem::path path;
    std::string reason;
};

class TableLibrary
{
public:
    /// Add a root, searched in the order added. Duplicates and non-existent roots are ignored, so a
    /// caller can offer every plausible root without checking first. A root that is there and refuses to
    /// be read is recorded in `problems()` rather than dropped.
    void addRoot (const std::filesystem::path& root);
    const std::vector<std::filesystem::path>& roots() const { return roots_; }

    /// Every content path this library has found unreadable, in the order it found them. Read by whoever
    /// reports the resolution's outcome, which is why a lookup does not have to be the thing that fails
    /// for a refusal to be visible.
    const std::vector<ContentProblem>& problems() const { return problems_; }

    /// Resolve and load a reference as a preset spells it — usually something like
    /// `/Analog/Basic Shapes.wav`, resolved under a root's `Tables` directory.
    TableLookup loadTable (std::string_view reference) const;

    /// Resolve a reference to the **bytes** of the file it names, under `subdirectory` inside a root first
    /// and then under the root itself, exactly as `loadTable` does. The absence/refusal distinction is
    /// `loadTable`'s too: nothing there is reported here as anything else.
    AssetLookup loadAsset (std::string_view reference, std::string_view subdirectory) const;

    /// Drop the caches. Needed when the content on disk changes underneath a running instance.
    void clearCache() const
    {
        cache_.clear();
        assetCache_.clear();
    }

private:
    std::vector<std::filesystem::path> roots_;

    /// Mutable for the same reason `cache_` is: a lookup is a read of the library that *learns*
    /// something, and what it learns about a root — that it refuses — belongs to the library rather than
    /// to the call that happened to find out.
    mutable std::vector<ContentProblem> problems_;

    /// Record a refusal once, so a library asked the same question repeatedly does not grow a problem
    /// list — or re-stat a directory that is already known to refuse. Const, because a lookup is a read of
    /// the library that learns something, and `problems_` is mutable for that reason.
    void noteProblem (const std::filesystem::path& path, std::string reason) const;

    bool hasProblemFor (const std::filesystem::path& path) const;

    /// Keyed by the *resolved* path, so two references that land on the same file are read once. A factory
    /// table is up to 2 MB and the engine asks for the same handful constantly.
    mutable std::map<std::string, TableLookup> cache_;

    /// The bytes cache, keyed the same way and for the same reason: an impulse response is up to a few MB
    /// and a preset that names one asks for it on every load.
    mutable std::map<std::string, AssetLookup> assetCache_;

    /// One reference's **path**, under `subdirectory` inside a root and then under the root itself: the
    /// exact-spelling walk first, then the case-insensitive one, in root order. Absence and refusal are
    /// told apart here and recorded on the library; whether the caller then parses a wavetable or copies
    /// raw bytes is not this function's business, which is the whole of ticket `64`'s share of it.
    ///
    /// `resolution` and `error` are set even when nothing is found — `Missing` with the roots' count, or
    /// `Unreadable` with the reason one root gave.
    std::optional<std::filesystem::path> findAsset (std::string_view reference, std::string_view subdirectory,
                                                    Resolution& resolution, std::string& error) const;
};

} // namespace xerum::content
