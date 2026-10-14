// xerum::plugin — the plugin's own identity.
//
// Ticket 30. Two things live here that hosts can see and that must never be borrowed from the
// reference instrument (ticket 11, ticket 13 D6):
//
//   * **The AU codes.** The reference registers `aumu` / `Xf2X` / `Xfer`. Registering the same type,
//     subtype and manufacturer would make the two plugins collide on any machine with both installed:
//     the AU registry keys on the triple, and a duplicate install replaces or shadows the other. So
//     xerum registers its own, recorded here and in `plugin/README.md`:
//
//         type          aumu                 (MusicDevice — the same *type*; that is the class of unit,
//                                              not an identity, and a synth must be `aumu`)
//         subtype       Xrm2                 (xerum; distinct from the reference's Xf2X)
//         manufacturer  Xerm                 (distinct from the reference's Xfer)
//
//     The VST3 side has no such registry; its identity is the bundle id and the class UID JUCE derives
//     from `PLUGIN_CODE`, so the same distinction holds there.
//
//   * **The plugin's name and bundle id.** `com.xerum.xerum`; the reference's is `com.xfer.serum2`.
//
// These are compile-time constants rather than `JucePlugin_*` macros on purpose: the processor and the
// bridge compile into the Catch2 test binary as well as into the plugin, and a test binary that has to
// satisfy the wrapper's macro set is a test binary that cannot be built without the wrappers.

#pragma once

namespace xerum::plugin::identity
{

/// The plugin's name as a host shows it.
inline constexpr const char* kName = "xerum";

/// The company name on the bundle.
inline constexpr const char* kManufacturer = "xerum";

/// `com.xerum.xerum`, not `com.xfer.serum2`.
inline constexpr const char* kBundleId = "com.xerum.xerum";

/// AU type/subtype/manufacturer. See the header note for why these and not the reference's.
inline constexpr const char* kAuType = "aumu";
inline constexpr const char* kAuSubtype = "Xrm2";
inline constexpr const char* kManufacturerCode = "Xerm";

/// The current state block's format version. A reader refuses anything it does not know rather than
/// guessing, because guessing at state is how a session silently loses a patch.
inline constexpr unsigned int kStateFormat = 1;

/// The state block's magic. Deliberately **not** the reference container's `XferJson`: a state block is
/// not a preset and must never be mistaken for one. Ticket `32` measured that a document-only write of
/// the reference's container aborts the reference on 172 of 626 presets, so xerum's state does not wear
/// the reference's clothes.
inline constexpr char kStateMagic[] = "xerum-state";
inline constexpr unsigned int kStateMagicSize = 12;   // includes the trailing NUL

} // namespace xerum::plugin::identity
