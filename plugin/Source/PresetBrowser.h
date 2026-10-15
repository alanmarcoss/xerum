// xerum::plugin — moving through a hundred presets (ticket 55).
//
// ## The question this answers
//
// Ticket `54` produced a bank of 104 presets under `<content root>/Factory/`. A bank nobody can move
// through is a folder, so this is the other half: **categories as the first cut, previous/next for
// auditioning in place, search by name, and the keyboard doing all of it.** The design already gives it a
// home — `docs/design/xerum-direction-warm.html` carries a preset field in the header (and the design
// system's seven-row table for it names the type and the box), and the design's wavetable slot is the
// vocabulary for a field with two stepper arrows.
//
// ## Over the manifest, never over the filenames
//
// `BankCatalog` is read from `<content root>/Factory/manifest.json` — the bank's own contract, which
// names every preset's category and the order a player moves through the categories in. A browser that
// listed a directory instead would offer whatever happened to be on disk, including a file the loader
// rejects, and the ticket's rule is the opposite: **a preset the browser offers is one that loads.**
// The reader is `xerum::presets::readBankManifest`, in the bank's own module, so the browser and the
// bank cannot disagree about the shape of the bank.
//
// ## Two doors, and they must not look alike
//
// xerum has exactly one way for a user's **own** preset to come in — the `Import .SerumPreset…` button,
// which opens a file dialog on the reference's format and reports what it could not honour in the import
// bar at the bottom. This browser is the other thing entirely: it shows **xerum's factory bank**, in the
// manifest's order, and it never touches the filesystem outside `<content root>/Factory/`. The
// distinction is carried in four places a user can see, and `plugin/tests/interface_test.cpp` asserts
// each of them rather than trusting the prose:
//
//   1. **the words.** The browser is titled `Factory`, and every name it offers is spelled on the header
//      field as `<Category> · <Name>`; a preset that arrived by any other route has no category and so no
//      prefix. The import button says `.SerumPreset`, and nothing in the browser does;
//   2. **the list.** Every entry the browser can offer is a `.xerumpreset` under `Factory/`, resolved from
//      the manifest — asserted as a property of the list, not of the code that built it;
//   3. **the surface.** Import is an explicit file action that leaves a report in the bar at the bottom;
//      the browser is a panel over the page, and leaves nothing behind;
//   4. **the door.** The browser has no file dialog at all. The only way to a file it did not get from the
//      manifest is the import button.
//
// ## It must not cost the fit
//
// Ticket `53` packed the composition into the window it opens at, and its test measures the page's height
// against the viewport's. So the browser is an **overlay**: a child of the editor, laid out over the
// strip and the composed page and hidden until it is asked for. It changes no panel's bounds and no
// section's height — `plugin/tests/interface_test.cpp` measures the page with the browser open and with
// it shut and requires the same number. The two header arrows are the design's own wavetable-slot arrows
// at the design's own 16px, and they are laid out **inside the width the header already gave the preset
// field**, so the header's other contents keep their rectangles.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "factory_bank.h"

#include <functional>
#include <string>
#include <vector>

namespace xerum::plugin::ui
{

//==============================================================================
/// The factory bank as the browser reads it: the manifest's own order, and one resolved file per entry.
///
/// `ok == false` and an empty `presets` is the honest answer for a machine with no bank — the browser
/// then says so instead of showing an empty list that looks like a search that matched nothing.
struct BankCatalog
{
    bool ok { false };
    std::string error;                    ///< why there is no bank, when there is none
    juce::File root;                      ///< the content root `Factory/manifest.json` was found under

    std::vector<std::string> categories;  ///< in the manifest's order — the order a player moves through
    std::vector<xerum::presets::BankManifestPreset> presets;

    int size() const { return static_cast<int> (presets.size()); }
    bool isEmpty() const { return presets.empty(); }

    /// The preset's file, or a file that does not exist. Never a path built outside the content root.
    juce::File fileFor (int index) const;

    /// The manifest index holding this name, or -1. What makes "is the loaded preset one of ours, and
    /// which one" a question with an answer rather than a guess.
    int indexOfName (const juce::String& name) const;
};

/// Find the bank the way the product finds content — the first content root holding
/// `Factory/manifest.json`, in `XerumAudioProcessor::contentRoots()`'s own order — and read it.
BankCatalog readBankCatalog (const std::vector<juce::String>& contentRoots);

//==============================================================================
/// The header's preset field: the design's monospaced name box, with the design's two stepper arrows
/// inside it, and the field itself as the way into the browser.
///
/// The text is `<Category> · <Name>` for a factory preset and just `<Name>` for one that arrived any
/// other way, which is the distinction ticket `55` requires be visible: the factory bank says where it is
/// filed, and a user's own preset cannot.
class PresetField final : public juce::Component
{
public:
    PresetField();

    /// The loaded preset's name, as the document spells it. Empty draws `init`, the design's word for
    /// nothing loaded.
    void setName (juce::String name);
    const juce::String& name() const { return name_; }

    /// The category the loaded preset is filed under in the factory bank, or empty when the loaded preset
    /// is not one of the bank's. Never a guess: the editor reads it out of the catalogue by name.
    void setFactoryCategory (juce::String category);
    const juce::String& factoryCategory() const { return category_; }

    /// What the field draws, category prefix included — the same string the evidence reads.
    juce::String displayText() const;

    /// ◀ and ▶, one step each: -1 and +1.
    std::function<void (int)> onStep;

    /// The field itself was clicked: open the browser.
    std::function<void()> onOpen;

    juce::Rectangle<int> fieldBounds() const;
    juce::Rectangle<int> textBounds() const;
    juce::Rectangle<int> arrowBounds (int step) const;

    void paint (juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;

private:
    juce::String name_;
    juce::String category_;
    int hovered_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetField)
};

//==============================================================================
/// The bank as a panel over the page: a category row as the first cut, a search field, one line per
/// preset, and the selected preset's own description under the list.
///
/// **Auditioning is the point**, so a selection change **loads**: stepping the list with the arrow keys is
/// what "hold one key and hear them change" means, and it goes through `onLoad`, which the editor wires
/// to `XerumAudioProcessor::loadPresetFromFile`. There is no second code path — a preset chosen here is
/// loaded exactly as one chosen anywhere else.
class PresetBrowser final : public juce::Component
{
public:
    PresetBrowser();

    void setCatalog (BankCatalog catalog);
    const BankCatalog& catalog() const { return catalog_; }

    /// Selected index into the manifest, or -1. The list's cursor and the loaded preset are the same
    /// thing: `setSelectedIndex` loads unless told not to.
    int selectedIndex() const { return selected_; }
    bool setSelectedIndex (int index, bool notify = true);

    /// Step one preset **through the whole bank, in the manifest's order**, wrapping at both ends. This is
    /// what the header's arrows and the editor's `[` / `]` do.
    bool stepPreset (int step);

    /// Step one row through what the filter is currently showing, wrapping. What ↑ and ↓ do.
    bool selectNext (int step);

    const juce::String& search() const { return search_; }

    /// Set the query and re-filter. The selection follows only if it is still visible, so typing never
    /// changes what you are hearing.
    void setSearch (juce::String text);

    /// -1 is `All`, otherwise an index into `catalog().categories`.
    int categoryIndex() const { return category_; }
    void setCategoryIndex (int index, bool notify = true);

    /// The manifest indices the filter is showing, in manifest order, and how many rows that is.
    const std::vector<int>& visibleRows() const { return visible_; }
    int visibleCount() const { return static_cast<int> (visible_.size()); }

    /// Where a manifest index sits in the visible list, or -1.
    int rowOfIndex (int index) const;

    /// Which visible row the list's first line is drawing — what scrolling moves.
    int firstVisibleRow() const { return topRow_; }
    int rowsVisible() const { return rowsVisible_; }

    /// A selection change that must be loaded. The editor answers by loading through the product's own
    /// loader and calling `setSelectedIndex(index, false)` back.
    std::function<void (int)> onLoad;

    /// The browser was dismissed (Return, Escape, or the field again).
    std::function<void()> onClose;

    bool keyPressed (const juce::KeyPress& key) override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

    void paint (juce::Graphics& graphics) override;
    void resized() override;

    //==============================================================================
    // The rectangles, so "where did the browser land" is a measurement rather than a look.

    juce::Rectangle<int> headingBounds() const { return heading_; }
    juce::Rectangle<int> searchBounds() const { return searchBox_; }
    juce::Rectangle<int> categoryBounds (int index) const;   ///< index -1 is the `All` pill
    juce::Rectangle<int> listBounds() const { return list_; }
    juce::Rectangle<int> rowBounds (int row) const;          ///< in this component's coordinates
    juce::Rectangle<int> footerBounds() const { return footer_; }

    int rowHeight() const;

private:
    /// One line of the bank, in the design's own scale for a table row.
    static constexpr int kRowHeight = 20;

    void refilter (bool keepSelection);
    void clampScroll();
    int rowAtPoint (juce::Point<int> position) const;
    juce::String footerDescription() const;

    BankCatalog catalog_;
    std::vector<int> visible_;
    juce::String search_;
    int category_ { -1 };
    int selected_ { -1 };
    int topRow_ { 0 };
    int rowsVisible_ { 0 };
    int hoveredRow_ { -1 };

    juce::Rectangle<int> heading_;
    juce::Rectangle<int> searchBox_;
    juce::Rectangle<int> categoryRow_;
    std::vector<juce::Rectangle<int>> categoryPills_;
    juce::Rectangle<int> list_;
    juce::Rectangle<int> footer_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBrowser)
};

} // namespace xerum::plugin::ui
