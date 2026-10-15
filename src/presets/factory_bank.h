// xerum::presets — the factory bank (ticket 54).
//
// ## What this is
//
// A hundred-odd presets, authored here as data: each one is a name, a browsing category, a one-line
// description of what it is made of, and a `PresetDocument` — the sparse parameter map the product
// already speaks. Nothing here is captured from anywhere, and every wavetable a preset names is one
// xerum generated from a formula we wrote (docs/content-policy.md): the bank is self-contained and
// travels with the product.
//
// ## Where the bank lives, and why there
//
// The bank is content, so it lives under a **content root** — the same roots the plugin resolves
// wavetables from, in the same order (`XerumAudioProcessor::contentRoots`, docs/preset-import-contract.md
// §5):
//
//     <content root>/Factory/<Category>/<Name>.xerumpreset
//     <content root>/Factory/manifest.json
//
// `Factory/` rather than `Presets/`, and that is the whole of the reasoning: a reference install's own
// content root **contains `Presets/`** (its factory bank, which is licensed material and must never be
// browsed as ours), so a bank directory named `Presets` would be ambiguous the moment a machine had both
// installed. `Factory/` exists in no other content root, which is what makes "find the factory bank"
// a search rather than a guess.
//
// The file format is xerum's own block — the exact bytes `getStateInformation` writes and
// `setStateInformation` reads (`model::encodePresetBlock`), not the reference's container. A preset is
// therefore loaded through the product's own loader, and the extension `.xerumpreset` says so.
//
// **If the bank is absent** — a build with no content directory, or a user who removed it — the search
// finds nothing, and that is the honest answer for a browser to show: no factory presets, rather than a
// bank assembled out of whatever else happened to be in the roots. Nothing else in xerum depends on the
// bank being present (a preset's *tables* are what the audio path needs, and they resolve separately).
//
// ## What is asserted about it
//
// `tests/model/factory_bank_test.cpp` holds the bank to its own rules (count, unique names, valid
// categories, tables that exist in this repository, one parameter vocabulary) and to the bytes on disk;
// `plugin/tests/bank_test.cpp` loads every file through `setStateInformation`, renders it, and refuses
// two presets that render the same. Nothing asserts that they *sound* good — nobody here can hear.

#pragma once

#include "document.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace xerum::presets
{

/// The bank's directory below a content root. See the note above on why it is not `Presets`.
inline constexpr std::string_view kBankDirectory = "Factory";

/// The extension of one preset file. Ours, so a browser can never confuse one with the reference's
/// `.SerumPreset`, and so a stray file can be told from a factory preset by name alone.
inline constexpr std::string_view kPresetExtension = ".xerumpreset";

/// The bank's own manifest, at `<content root>/Factory/manifest.json`: every preset's name, category,
/// path, tables and description, so the browser (ticket 55) reads a file rather than reverse-engineering
/// the bank.
inline constexpr std::string_view kBankManifestName = "manifest.json";

/// The fewest presets the bank may hold. Ticket 54's acceptance is "at least a hundred"; the number is
/// here so the test that enforces it and the tool that reports it read the same one.
inline constexpr std::size_t kMinimumBankSize = 100;

/// The browsing categories, in the order a player moves through them. A preset's category is one of
/// these or it is not in the bank.
const std::vector<std::string_view>& categories();

struct FactoryPreset
{
    std::string name;           ///< what a player browses to, e.g. "Molten Sub"
    std::string category;       ///< one of `categories()`
    std::string description;    ///< what it is made of: the tables, and the choices that shape it
    model::PresetDocument document;
};

/// The bank, in catalogue order: category order, and within a category the order authored here.
/// Built once, on first use.
const std::vector<FactoryPreset>& bank();

/// Everything the bank found wrong with **itself** while it was built: a name used twice, a category
/// outside the eight, a warp mode or filter type the engine does not build, a matrix source v1 does not
/// evaluate. Empty is the only acceptable answer.
///
/// This exists because the failure it catches is otherwise invisible: a parameter whose token is outside
/// xerum's vocabulary resolves to the "not in xerum's set" sentinel, and the module it addresses then
/// quietly does nothing. The authoring tool refuses to write a bank with problems and both test suites
/// fail on a non-empty list, so the check cannot be forgotten.
std::vector<std::string> bankProblems();

/// One preset's file path relative to a content root: `Factory/<Category>/<Name>.xerumpreset`.
std::string presetFilePath (const FactoryPreset& preset);

/// A preset's file bytes: the document and an empty report, encoded as xerum's own block.
///
/// Empty on purpose. The report is what an *import* could not honour, and the bank is authored against
/// the engine's cut (ticket 12): a preset here that produced a report entry would be a preset asking for
/// something the product does not do, which the bank test treats as a failure.
std::vector<std::byte> encodePresetFile (const FactoryPreset& preset);

/// Every wavetable a preset names, as the preset spells them (`xerum/Sine.wav`), sorted and unique.
/// This is what the self-containment check walks: each of them must be a file in this repository.
std::vector<std::string> tablesOf (const FactoryPreset& preset);

} // namespace xerum::presets
