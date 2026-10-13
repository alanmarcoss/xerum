// xerum::model — the parameter schema.
//
// Owns the shape of the parameter model: which modules exist, which parameters they hold, what kind
// each parameter is, and which schema versions it appears in. It does not own values, does not read
// presets, and does not touch audio — see ADR 0001 and ticket 13 for why the seam sits here.
//
// Nothing in this header includes JUCE. The model layer is testable without a host, which is what
// makes the format suite cheap (ticket 15).

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xerum::model
{

/// What a leaf holds. `EmptyMap` and `EmptyArray` are real: the reference writes `{}` and `[]` for
/// containers it has nothing to say about, and treating those as "absent" would lose information.
enum class LeafKind { EmptyMap, EmptyArray, Float, Int, Bool, String, Null, Unknown };

/// Where a parameter's default came from.
///
/// A preset never stores a default, so no default can be read out of the corpus, and the value that
/// appears most often in the factory bank is a *mode*, not a default. Every leaf therefore starts
/// `Unestablished` until someone authors one and validates it behaviourally. Making this explicit is
/// the point: an unestablished default must never quietly behave like `0`.
enum class DefaultProvenance { Unestablished, Authored };

struct LeafDescriptor
{
    std::string_view path;          ///< `{N}` for an instance index, `[]` inside an array
    LeafKind kind { LeafKind::Unknown };
    bool hasRange { false };
    double min { 0.0 };
    double max { 0.0 };
    const std::string_view* values { nullptr };  ///< enum set for string leaves, else nullptr
    std::size_t valueCount { 0 };
    unsigned versionMask { 0 };     ///< bit 0 = 3.0, 1 = 4.0, 2 = 5.0, 3 = 6.0
    DefaultProvenance defaultProvenance { DefaultProvenance::Unestablished };
};

class Schema
{
public:
    static const Schema& instance();

    /// The schema versions xerum accepts, ascending: {"3.0", "4.0", "5.0", "6.0"}.
    static const std::vector<std::string>& versions();

    std::size_t leafCount() const;
    std::size_t modulePatternCount() const;

    /// Does this module name belong to the taxonomy? `ModSlot37` and `Env1` do; `Nonsense` does not.
    bool recognisesModule (std::string_view moduleName) const;

    /// Look up a normalised path. Returns nullptr for anything unknown — never throws, because an
    /// unknown parameter is a normal outcome that the import report exists to describe.
    const LeafDescriptor* describe (std::string_view normalisedPath) const;

    bool presentIn (const LeafDescriptor& leaf, std::string_view version) const;

    /// The default for a parameter, or nullopt when it is unestablished. Returns nullopt for an
    /// unknown path too — the caller distinguishes the two by asking `describe` first.
    std::optional<double> defaultFor (std::string_view normalisedPath) const;

    /// Convert a concrete path into its schema form:
    ///   `/Oscillator0/WTOsc1/plainParams/kParamWarpMenu` -> `Oscillator{N}/WTOsc{N}/plainParams/kParamWarpMenu`
    ///   `FXRack0/FX[2]/FXReverb/...`                     -> `FXRack{N}/FX[]/FXReverb/...`
    /// Leading `/` is dropped; index runs become `{N}`; bracketed indices become `[]`.
    static std::string normalise (std::string_view concretePath);
};

/// Human-readable name for a kind, for reports and diagnostics.
std::string_view toString (LeafKind kind);
std::string_view toString (DefaultProvenance provenance);

} // namespace xerum::model
