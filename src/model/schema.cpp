#include "schema.h"

#include "generated/schema_data.h"

#include <algorithm>
#include <cctype>

namespace xerum::model
{

namespace
{

LeafKind kindFromString (std::string_view k)
{
    if (k == "float")       return LeafKind::Float;
    if (k == "int")         return LeafKind::Int;
    if (k == "bool")        return LeafKind::Bool;
    if (k == "string")      return LeafKind::String;
    if (k == "null")        return LeafKind::Null;
    if (k == "empty-map")   return LeafKind::EmptyMap;
    if (k == "empty-array") return LeafKind::EmptyArray;
    return LeafKind::Unknown;
}

bool isDigits (std::string_view s)
{
    return ! s.empty()
        && std::all_of (s.begin(), s.end(), [] (unsigned char c) { return std::isdigit (c) != 0; });
}

/// Match a taxonomy pattern against a concrete module name. `{N}` is one or more digits.
bool matchesPattern (std::string_view pattern, std::string_view name)
{
    const auto marker = pattern.find ("{N}");

    if (marker == std::string_view::npos)
        return pattern == name;

    const auto prefix = pattern.substr (0, marker);
    const auto suffix = pattern.substr (marker + 3);

    if (name.size() <= prefix.size() + suffix.size() - (suffix.empty() ? 0 : 1))
        return false;

    if (! name.starts_with (prefix))
        return false;

    if (! suffix.empty() && ! name.ends_with (suffix))
        return false;

    const auto middle = name.substr (prefix.size(), name.size() - prefix.size() - suffix.size());
    return isDigits (middle);
}

const std::vector<std::string> g_versions = []
{
    std::vector<std::string> v;
    v.reserve (std::size (generated::kVersions));
    for (auto s : generated::kVersions)
        v.emplace_back (s);
    return v;
}();

} // namespace

std::string_view toString (LeafKind kind)
{
    switch (kind)
    {
        case LeafKind::Float:      return "float";
        case LeafKind::Int:        return "int";
        case LeafKind::Bool:       return "bool";
        case LeafKind::String:     return "string";
        case LeafKind::Null:       return "null";
        case LeafKind::EmptyMap:   return "empty-map";
        case LeafKind::EmptyArray: return "empty-array";
        case LeafKind::Unknown:    return "unknown";
    }
    return "unknown";
}

std::string_view toString (DefaultProvenance p)
{
    return p == DefaultProvenance::Authored ? "authored" : "unestablished";
}

const Schema& Schema::instance()
{
    static const Schema schema;
    return schema;
}

const std::vector<std::string>& Schema::versions() { return g_versions; }

std::size_t Schema::leafCount() const          { return generated::kLeafCount; }
std::size_t Schema::modulePatternCount() const { return std::size (generated::kModulePatterns); }

bool Schema::recognisesModule (std::string_view moduleName) const
{
    for (auto pattern : generated::kModulePatterns)
        if (matchesPattern (pattern, moduleName))
            return true;

    return false;
}

const LeafDescriptor* Schema::describe (std::string_view normalisedPath) const
{
    if (normalisedPath.empty())
        return nullptr;

    // The generated table is sorted by path, so this is a binary search rather than a scan. Anything
    // not found is simply unknown — an ordinary outcome, not an error.
    const auto* begin = generated::kLeaves;
    const auto* end   = begin + generated::kLeafCount;

    const auto it = std::lower_bound (begin, end, normalisedPath,
                                      [] (const generated::Leaf& leaf, std::string_view key)
                                      { return leaf.path < key; });

    if (it == end || it->path != normalisedPath)
        return nullptr;

    // The generated record and the public descriptor have the same shape; reinterpret the fields
    // explicitly so the generator can change its layout without silently breaking the API.
    static thread_local LeafDescriptor descriptor;
    descriptor.path = it->path;
    descriptor.kind = kindFromString (it->kind);
    descriptor.hasRange = it->hasRange;
    descriptor.min = it->min;
    descriptor.max = it->max;
    descriptor.values = it->values;
    descriptor.valueCount = it->valueCount;
    descriptor.versionMask = it->versionMask;
    descriptor.defaultProvenance = it->defaultProvenance == "authored"
                                 ? DefaultProvenance::Authored
                                 : DefaultProvenance::Unestablished;
    return &descriptor;
}

bool Schema::presentIn (const LeafDescriptor& leaf, std::string_view version) const
{
    const auto& all = versions();

    for (std::size_t bit = 0; bit < all.size(); ++bit)
        if (all[bit] == version)
            return (leaf.versionMask & (1u << bit)) != 0;

    return false;
}

std::optional<double> Schema::defaultFor (std::string_view normalisedPath) const
{
    const auto* leaf = describe (normalisedPath);

    if (leaf == nullptr || leaf->defaultProvenance != DefaultProvenance::Authored)
        return std::nullopt;

    // No authored defaults exist yet. When they do, they are read from a table in the generated
    // data — deliberately not from `min` or the corpus mode, both of which would be a guess.
    return std::nullopt;
}

std::string Schema::normalise (std::string_view concretePath)
{
    std::string out;
    out.reserve (concretePath.size());

    for (std::size_t i = 0; i < concretePath.size(); ++i)
    {
        const auto c = concretePath[i];

        if (c == '/')
        {
            if (out.empty())            // drop a leading separator; paths are relative to the root
                continue;

            if (out.back() != '/')
                out.push_back (c);

            continue;
        }

        if (c == '[')
        {
            while (i < concretePath.size() && concretePath[i] != ']')
                ++i;

            out += "[]";
            continue;
        }

        if (std::isdigit (static_cast<unsigned char> (c)) != 0)
        {
            auto end = i;
            while (end < concretePath.size() && std::isdigit (static_cast<unsigned char> (concretePath[end])) != 0)
                ++end;

            // An index only means an index when it follows a name. A bare number is data.
            if (! out.empty() && std::isalnum (static_cast<unsigned char> (out.back())) != 0)
                out += "{N}";
            else
                out.append (concretePath.substr (i, end - i));

            i = end - 1;
            continue;
        }

        out.push_back (c);
    }

    while (! out.empty() && out.back() == '/')
        out.pop_back();

    return out;
}

} // namespace xerum::model
