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

    /// True when the table asks for a frame interpolation the engine cannot do yet. The oscillator falls
    /// back to a linear crossfade, which is a **substitution**, and the import contract says substitutions
    /// are reported rather than made quietly. 74 of the 288 factory tables are in this position.
    bool needsUnimplementedInterpolation { false };

    bool found() const { return resolution == Resolution::Exact || resolution == Resolution::CaseInsensitive; }
};

class TableLibrary
{
public:
    /// Add a root, searched in the order added. Duplicates and non-existent roots are ignored, so a
    /// caller can offer every plausible root without checking first.
    void addRoot (const std::filesystem::path& root);
    const std::vector<std::filesystem::path>& roots() const { return roots_; }

    /// Resolve and load a reference as a preset spells it — usually something like
    /// `/Analog/Basic Shapes.wav`, resolved under a root's `Tables` directory.
    TableLookup loadTable (std::string_view reference) const;

    /// Drop the cache. Needed when the content on disk changes underneath a running instance.
    void clearCache() const { cache_.clear(); }

private:
    std::vector<std::filesystem::path> roots_;

    /// Keyed by the *resolved* path, so two references that land on the same file are read once. A factory
    /// table is up to 2 MB and the engine asks for the same handful constantly.
    mutable std::map<std::string, TableLookup> cache_;
};

} // namespace xerum::content
