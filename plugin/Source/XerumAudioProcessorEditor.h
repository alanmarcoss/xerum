// xerum::plugin — the interface.
//
// The interface's shape is designed here; the **binding is model-driven**, which is ticket `13`'s rule:
// every control is created from a `ParamSpec` and bound to the bridge's own parameter object, so adding a
// parameter to the schema adds a control, and the interface cannot be missing one the engine has.
// `ui::planInterface` places the panels in the design's composition; `ui::ParameterControl` builds and
// binds one parameter's control; `ui::ImportReportView` shows ticket `23`'s report.
//
// ## Ticket `52`: one composed page, arranged the way the design arranges it
//
// Ticket `43` settled the design, ticket `44` implemented its structure (one device per page behind a
// strip), and ticket `51` gave it the design's elements — the tokens, the knobs with their arcs, the
// pills, the chrome. The dev's reaction to `51` was that the elements are the design's but **the
// placements are not**, and that is right: the design specifies a *composition* — three oscillators
// across the top, noise and sub beneath them, the two filters with their response displays beside their
// knobs, the matrix under them, the effects and macros side by side at the bottom — and a panel-per-group
// grid cannot express it.
//
// So the composition became data (`ui::designedComposition`: an ordered list of sections) and the editor
// builds **one page** from it (`ui::CompositionView`), with the strip naming the sections and scrolling
// to them. The binding did not move: one control per parameter, from the model type; one panel per group;
// every group in exactly one section, with the groups the composition does not name landing in the
// default section rather than vanishing.
//
// The coverage readout in the header is part of the work rather than decoration: the bridge binds a
// fraction of the schema's leaves, and the interface says so on screen instead of implying that what is
// shown is all there is.
//
// ## The editor must not exist until a host opens it
//
// The headless harness is how the reference is driven and how this plugin is validated, so nothing here
// may be constructed eagerly. This class counts its own constructions (`instancesCreated`) and live
// instances (`instancesAlive`) so that the claim is a measurement rather than a promise: a test drives the
// processor through prepare, render and state restore and requires zero editors, and the count would fail
// if anyone made the editor eager. Nothing in this file is reachable from `processBlock`, and the two
// drawn surfaces read the loaded document on the **message thread** only.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ImportReportView.h"
#include "InterfacePlan.h"
#include "ModulationMatrixView.h"
#include "ParameterPanels.h"
#include "PoleZeroPlane.h"
#include "SectionStrip.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xerum::plugin
{

class XerumAudioProcessor;

class XerumAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                        private juce::ChangeListener
{
public:
    explicit XerumAudioProcessorEditor (XerumAudioProcessor& processor);
    ~XerumAudioProcessorEditor() override;

    //==============================================================================
    // The interface's own surface, for the tests and for the next reader.

    /// How many editors this process has constructed, and how many are alive right now. A host opening
    /// the interface is the only thing that moves these.
    static int instancesCreated();
    static int instancesAlive();

    /// The layout the editor built from the parameter table.
    const ui::InterfacePlan& plan() const { return plan_; }

    /// One control per parameter the bridge exposes — the coverage ticket `31` requires be assertable.
    int controlCount() const;
    ui::ParameterControl* controlFor (std::string_view id) const;

    /// `controlCount()` **plus the 512 bound matrix cells**: every control on screen, which is what the
    /// header's coverage readout counts. Ticket `45` — leaving the matrix cells out would understate what
    /// is bound by 512 now that the cells are real controls.
    int boundControlCount() const;

    /// How many schema leaves the interface *does not* bind. Shown in the header, so the gap is visible
    /// rather than implied away.
    int schemaLeafCount() const { return schemaLeaves_; }

    //==============================================================================
    // The composition: the design's sections, on one page.

    /// One per resolved section of the composition, in the design's order.
    int sectionCount() const { return composition_.sections().size(); }
    juce::String sectionTitle (int index) const;
    juce::String sectionId (int index) const;

    /// The model modules a section holds — what the strip's severity dot is keyed on, and what makes
    /// "which section is `VoiceFilter0` in" a question with an answer rather than a guess.
    std::vector<juce::String> sectionModules (int index) const;

    /// The section a model module landed in, or -1. The matrix's own module (`ModSlot`) is the matrix's
    /// section.
    int sectionForModule (const juce::String& module) const;

    /// A section's resolved plan — the panels it holds, in the table's order — or nullptr. The evidence
    /// reads this to say which group landed where.
    const ui::SectionPlan* sectionPlan (int index) const;

    /// True while the matrix is that section — the one section the table does not derive.
    bool sectionIsMatrix (int index) const;

    /// The strip: what a user clicks, and what says which section is showing.
    const ui::SectionStrip& sectionStrip() const { return strip_; }

    /// Which section the page is scrolled to, and what a strip click does.
    int currentSection() const { return currentSection_; }
    void scrollToSection (int index);

    /// The composed page itself and its scroll position, so a test can read where each section and each
    /// panel actually landed.
    const ui::CompositionView& composition() const { return composition_; }
    int compositionScrollY() const { return compositionViewport_.getViewPositionY(); }
    int compositionHeight() const { return composition_.totalHeight(); }

    /// The window the composed page is seen through. Ticket `53`'s acceptance is the page's height against
    /// this one, so the number the claim is made of is readable rather than inferred from the window's.
    int compositionViewportWidth() const { return compositionViewport_.getMaximumVisibleWidth(); }
    int compositionViewportHeight() const { return compositionViewport_.getMaximumVisibleHeight(); }

    /// True when the page is taller than the window it is in, which is the only thing that makes the page
    /// scroll. Ticket `53`: at the default window this is false — the whole instrument is on screen.
    ///
    /// The question is whether the bar is **visible**, not whether the viewport permits one:
    /// `Viewport::isVerticalScrollBarShown` returns the `setScrollBarsShown` flag, which is true here
    /// whether or not a bar is on screen — asking it would have "proved" no scrolling in the old build too.
    bool compositionScrolls() const
    {
        return const_cast<juce::Viewport&> (compositionViewport_).getVerticalScrollBar().isVisible();
    }

    /// The panel a group's controls are in, or nullptr. A group is a panel; a section holds several.
    ui::ParameterPanel* panelForGroup (const std::string& group) const;

    //==============================================================================
    // The two surfaces ticket `43` called hard.

    const ui::ModulationMatrixView& matrixView() const { return matrix_; }
    ui::ModulationMatrixView& matrixView() { return matrix_; }
    const ui::PoleZeroPlane& poleZeroPlane (int filterSlot) const;

    /// Non-const, because the plane **edits** since ticket `45`: a test drives a drag through it the way
    /// the mouse does, rather than reaching past it to the processor.
    ui::PoleZeroPlane& poleZeroPlane (int filterSlot);

    //==============================================================================
    // The report surface: a bar plus a drawer, never a modal.

    const ui::ImportReportView& reportView() const { return reportView_; }
    const ui::ReportBar& reportBar() const { return reportBar_; }
    bool reportDrawerOpen() const { return reportBar_.drawerOpen(); }
    void setReportDrawerOpen (bool open);

    /// Read the processor's report and show it. Called on a change message (a preset load or a state
    /// restore) and once at construction; public because the tests drive it without a message loop.
    void refreshReport();

    const juce::TextButton& importButton() const { return import_; }

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void importPreset();
    void updateSectionSeverity (const model::ImportReport& report);
    void updateModulationArcs();
    void updateWavetableHeads();

    /// The name the design's wavetable field shows for a slot: the preset's own asset name, `inline` for a
    /// preset carrying its table in its own data, or empty (`— off —`) for a slot with neither.
    static juce::String tableNameFor (const model::PresetDocument& document, int slot);

    XerumAudioProcessor& processor_;

    /// The design system's look-and-feel, set on this editor (ticket `53`). Every drawn surface beneath it
    /// — the pickers, the pills, the scroll bars JUCE creates for a viewport, a dropdown — resolves its
    /// colours from the palette rather than from JUCE's own dark scheme, whose `defaultFill` (#42a2c8) was
    /// on screen before this existed. Owned here, so it is destroyed with the window and never outlives the
    /// message manager.
    ui::look::XerumLookAndFeel lookAndFeel_;

    ui::InterfacePlan plan_;
    juce::OwnedArray<ui::ParameterPanel> panels_;
    ui::CompositionView composition_;
    juce::Viewport compositionViewport_;
    ui::SectionStrip strip_;

    juce::OwnedArray<ui::PoleZeroPlane> planes_;

    ui::ModulationMatrixView matrix_;
    juce::Viewport matrixViewport_;

    ui::ImportReportView reportView_;
    juce::Viewport reportViewport_;
    ui::ReportBar reportBar_;

    /// The model modules of each section, keyed the way the strip's dots are.
    std::vector<std::vector<juce::String>> sectionModules_;

    juce::TextButton import_ { "Import .SerumPreset…" };
    juce::Label status_;
    std::unique_ptr<juce::FileChooser> chooser_;

    /// What is loaded, drawn in the header's own field. Read from the document when the report is
    /// refreshed, so it is the same value the rest of the interface is showing.
    juce::String presetName_;

    int currentSection_ { 0 };
    int schemaLeaves_ { 0 };

    static std::atomic<int> created_;
    static std::atomic<int> alive_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XerumAudioProcessorEditor)
};

} // namespace xerum::plugin
