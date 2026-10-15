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
// ## Ticket `55`: the bank, and the two doors it must not blur
//
// Ticket `54` authored a factory bank of 104 presets; ticket `55` is the other half, so the header's
// preset field is a **component** (`ui::PresetField`) carrying the bank's two stepper arrows inside its
// own width, and `ui::PresetBrowser` is a panel over the strip and the page — a sibling of the composed
// page, so opening it moves nothing and the fit ticket `53` measured is untouched. A step **loads**,
// through `XerumAudioProcessor::loadPresetFromFile` — the same call the import button makes, which is why
// ticket `55` also taught that loader to read xerum's own block as well as the reference's container.
//
// The distinction the ticket requires be visible is the field's own text: a factory preset is spelled
// `Category · Name`, because the category comes from the **manifest** that files it, and a preset that
// arrived any other way carries no category at all. `plugin/Source/PresetBrowser.h` is the whole of it.
//
// ## The editor must not exist until a host opens it
//
// The headless harness is how the reference is driven and how this plugin is validated, so nothing here
// may be constructed eagerly. This class counts its own constructions (`instancesCreated`) and live
// instances (`instancesAlive`) so that the claim is a measurement rather than a promise: a test drives the
// processor through prepare, render and state restore and requires zero editors, and the count would fail
// if anyone made the editor eager. Nothing in this file is reachable from `processBlock`, and the two
// drawn surfaces read the loaded document on the **message thread** only.
//
// ## Ticket `59`: one fixed ratio, and the interface is scaled rather than re-flowed
//
// The composition used to be laid out **for its window**: the panels re-arranged with the width, and the
// design's `1fr` row grew with the height (ticket `53` measured the page needing 71 px less when the window
// was widened by 320, and the amplitude envelope taking it). The dev's report is that this is the wrong
// behaviour — the interface should hold its shape and scale — so:
//
//   * the composition is laid out **once**, at a nominal size, from its own content (`CompositionView`'s
//     `layout` takes a width and nothing else: there is no window in that call to re-flow against);
//   * that nominal size **is** the ratio: the design's baseline width and the page's own height at it plus
//     the design's chrome. Derived at construction like ticket `53`'s default window was, so a table that
//     grows moves the ratio rather than clipping;
//   * everything the editor shows is a child of one `Canvas`, which carries the whole interface at the
//     nominal size and is the **only** component whose transform ever changes. JUCE is explicit that this
//     is the editor's own transform's business — `juce_AudioProcessorEditor.cpp` asserts
//     `getTransform() == hostScaleTransform` and names this remedy in the same breath — and the host's
//     scaling has to keep working, so the editor's own transform stays the host's;
//   * a window at the ratio is filled exactly by the scaled canvas; a window a host forces **off** the ratio
//     is letterboxed — uniform scale, centred — rather than stretched, so nothing is ever distorted and the
//     page still fits. The editor's own resize is constrained to the ratio, so the standalone and any host
//     that honours the constrainer cannot produce an off-ratio window by dragging.
//
// The consequences, which are measured in `plugin/tests/interface_test.cpp` rather than argued here: every
// section's rectangle is a constant of the composition and every on-screen rectangle is that constant times
// one number; the design's control sizes bind at the nominal size and scale with it (ticket `51`'s "no
// control got smaller" is re-scoped to exactly that); and the page cannot scroll, because it is laid out at
// the size of the space it is given and the space is scaled with it (ticket `53`'s fit, kept and made
// structural).

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "EnvelopeCurve.h"
#include "ImportReportView.h"
#include "InterfacePlan.h"
#include "ModulationMatrixView.h"
#include "ParameterPanels.h"
#include "PoleZeroPlane.h"
#include "PresetBrowser.h"
#include "SectionStrip.h"
#include "off_thread_work.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xerum::plugin
{

class XerumAudioProcessor;

class XerumAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                        private juce::ChangeListener,
                                        private juce::KeyListener,
                                        private juce::Timer
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

    /// Which section the page is showing, and what a strip click does.
    int currentSection() const { return currentSection_; }

    /// Select a section. **It no longer scrolls**: ticket `59` lays the page out at exactly the size of the
    /// space it is given and scales that space with the window, so the whole instrument is on screen at
    /// every size and there is nothing to scroll to. The strip is a map that marks where you are rather
    /// than a menu of hidden pages — which it also was before, at a window the page fitted.
    void selectSection (int index);

    /// The composed page itself, so a test can read where each section and each panel landed. Every
    /// rectangle on it is in the **nominal page's** coordinates and is the same at every window size.
    const ui::CompositionView& composition() const { return composition_; }
    int compositionHeight() const { return composition_.totalHeight(); }

    /// The rectangle the page is laid out in — the window less the header, the strip, the report bar and the
    /// side margins. At the nominal size the page is exactly this tall (the nominal size is derived from it),
    /// which is what makes "the page fits" a property of the construction rather than a comparison.
    juce::Rectangle<int> compositionArea() const { return composition_.getBounds(); }

    //==============================================================================
    // Ticket `59`: the fixed ratio, as numbers.
    //
    // The interface is laid out at one size and scaled into the window, so the two things a geometry
    // measurement needs are *the size it was laid out at* and *the scale it is drawn at*. An on-screen
    // rectangle is the nominal one through both, and `sectionBoundsOnScreen` is that composition done once
    // so a caller does not have to repeat it.

    /// The size the whole interface is laid out at, derived at construction from the composition's own
    /// content: the design's baseline width (1080) and the page's height at it plus the design's chrome.
    /// Its ratio is the ratio the editor keeps — `aspectRatio()`.
    juce::Point<int> nominalSize() const { return nominalSize_; }

    /// The rectangle the whole interface is laid out in — the nominal size at the origin. Every rectangle
    /// the editor places is inside this one, at every window size, because it is the only rectangle the
    /// layout is given.
    juce::Rectangle<int> nominalBounds() const { return { 0, 0, nominalSize_.x, nominalSize_.y }; }

    /// The canvas every component of the interface hangs from (ticket `59`) — the one component that is
    /// scaled into the window, and the parent of the page, the strip, the bar, the browser and the header's
    /// controls. Exposed so a measurement can say what a component's parent *is* rather than assert a chain
    /// it cannot see.
    const juce::Component& interfaceCanvas() const { return canvas_; }

    /// The nominal canvas' proportions, `width / height`, which is the number the constructor gives the
    /// resize constrainer and the number `getWidth() / getHeight()` equals at every size the editor itself
    /// chooses.
    double aspectRatio() const;

    /// How much of the nominal canvas is drawn per window pixel, and where its top-left corner sits in the
    /// window. A window at the fixed ratio has an origin of nought and a scale of `width / nominalSize().x`;
    /// a size a host forces off the ratio is letterboxed, so the scale is the smaller of the two ratios and
    /// the origin centres what is left.
    float interfaceScale() const { return canvas_.scale(); }
    juce::Point<int> interfaceOrigin() const { return canvas_.origin(); }

    /// The page's own rectangle **on screen** — where the composition's coordinates start in the window.
    juce::Rectangle<float> pageBoundsOnScreen() const;

    /// A section's rectangle **on screen**. At any window this is the nominal rectangle times one number
    /// (`interfaceScale()`), which is the fixed ratio stated as a measurement: `relative` geometry cannot
    /// change because nothing but the scale does.
    juce::Rectangle<float> sectionBoundsOnScreen (int index) const;

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

    /// The amplitude envelope's **drawn** curve (ticket `59`). It is a read-out inside the envelope's own
    /// panel — the design's inline-display column, beside the four stage knobs and composed from them — so
    /// it is exposed the way the plane and the matrix are: a measurement reads its geometry rather than
    /// counting pixels, and nothing writes a parameter through it.
    const ui::EnvelopeCurve& envelopeCurve() const { return envelopeCurve_; }
    ui::EnvelopeCurve& envelopeCurve() { return envelopeCurve_; }

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

    //==============================================================================
    // Ticket `55`: moving through the bank.
    //
    // The bank is read once, at construction, from the manifest the first content root carries; the
    // header's preset field is the way in, `[` and `]` step it from anywhere, and a preset chosen here is
    // loaded through `XerumAudioProcessor::loadPresetFromFile` — the same call the import button makes,
    // so there is no second code path.

    /// The header's preset field: the loaded preset's name, and the two arrows that step the bank.
    const ui::PresetField& presetField() const { return presetField_; }
    ui::PresetField& presetField() { return presetField_; }

    /// The browser: an overlay over the strip and the page, hidden until it is asked for.
    const ui::PresetBrowser& browser() const { return browser_; }
    ui::PresetBrowser& browser() { return browser_; }

    /// The bank as the manifest describes it. `ok == false` is a machine with no bank.
    const ui::BankCatalog& bankCatalog() const { return catalog_; }

    /// The bank slot the loaded preset came from, or -1 when what is loaded is not one of the bank's.
    int bankIndex() const { return bankIndex_; }

    bool browserOpen() const { return browser_.isVisible(); }
    void setBrowserOpen (bool open);

    /// Step one preset through the **whole bank**, in the manifest's order, wrapping at both ends. One
    /// implementation, shared by the header's arrows and by `[` / `]`.
    bool stepPreset (int step);

    /// Load a bank slot through the product's own loader and read the result back into the interface.
    /// The browser offers a preset only from the manifest, so this is the whole of "a preset chosen in the
    /// browser is exactly a preset loaded any other way".
    bool loadBankPreset (int index);

    /// The header's rectangles, as they were laid out — the numbers the fit is made of, so the evidence
    /// reads them rather than guessing them from the drawing code.
    struct HeaderLayout
    {
        juce::Rectangle<int> wordmark;
        juce::Rectangle<int> preset;
        juce::Rectangle<int> coverage;
        juce::Rectangle<int> import;
        juce::Rectangle<int> status;
    };
    const HeaderLayout& headerLayout() const { return headerLayout_; }

    void paint (juce::Graphics& graphics) override;
    void resized() override;

    /// Ticket `55`'s gesture. `[` and `]` step the bank, `p` opens the browser — from anywhere in the
    /// window. Public because `juce::Component::keyPressed` is public and this is the same function: a
    /// host that adds the editor bare calls it, and so do the tests. The editor is also a `KeyListener`
    /// on its own top-level component (see below), because JUCE delivers a key by walking up the parent
    /// chain from whatever has focus and the top-level is where that walk arrives.
    bool keyPressed (const juce::KeyPress& key) override;
    bool keyPressed (const juce::KeyPress& key, juce::Component* origin) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void timerCallback() override;

    void parentHierarchyChanged() override;
    bool handleGlobalKey (const juce::KeyPress& key);

    /// The interface's paint, in **nominal** coordinates: the header's gradient, its wordmark and its
    /// coverage readout. Called by `Canvas`, which is the component that is actually scaled — the editor's
    /// own `paint` fills the window behind it and nothing else.
    void paintInterface (juce::Graphics& graphics);

    /// The whole interface, at the nominal size, scaled into the window (ticket `59`).
    ///
    /// Every component the editor shows is a child of this — panels, strip, page, report bar, browser — and
    /// every rectangle in that tree is a nominal one. It is the one component whose transform is ever
    /// touched, and that is not a preference: `juce_AudioProcessorEditor.cpp` asserts the **editor's** own
    /// transform is the host's scale factor and its comment names this remedy ("put the component you want
    /// to transform in a child of the editor and transform that instead"). The canvas is that child, and
    /// `scale()`/`origin()` are what the geometry measurements read.
    class Canvas final : public juce::Component
    {
    public:
        explicit Canvas (XerumAudioProcessorEditor& editor);

        void paint (juce::Graphics& graphics) override;

        /// The uniform scale and the window-space offset. `scale` is also the canvas's size in the editor's
        /// coordinates: the canvas is the nominal size, and the transform maps it into the window.
        void setScale (float scale, juce::Point<int> origin);
        float scale() const { return scale_; }
        juce::Point<int> origin() const { return origin_; }

    private:
        XerumAudioProcessorEditor& editor_;
        float scale_ { 1.0f };
        juce::Point<int> origin_ {};
    };

    void importPreset();
    void updateSectionSeverity (const model::ImportReport& report);
    void updateModulationArcs();
    void updateWavetableHeads();

    /// Read the bank, off this thread, and install it if it answers inside the budget (ticket 56).
    ///
    /// `Factory/manifest.json` is content, read through the same roots the wavetables come from, and this
    /// runs inside the host's `createEditor` — so a root that will not answer must not be able to keep the
    /// window from opening. The same bounded wait, the same worker, and the timer below installs a bank
    /// that answers late.
    void requestBankCatalog();

    /// Install a bank that arrived after its bounded wait. Message thread; the timer's own work.
    void installBankCatalog();

    /// Read the loaded document back into the header and the browser: the name, the category the bank
    /// files it under (or none, for a preset that is not one of the bank's), and the browser's cursor.
    void updateBankCursor();

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
    ui::SectionStrip strip_;

    juce::OwnedArray<ui::PoleZeroPlane> planes_;

    /// The amplitude envelope, drawn (ticket `59`): a read-out inside the envelope's own panel, the design's
    /// inline-display column, composed from the same four knobs the controls are. It holds no parameter and
    /// takes no click — the knobs still control and nothing was lost by adding it.
    ui::EnvelopeCurve envelopeCurve_;

    ui::ModulationMatrixView matrix_;
    juce::Viewport matrixViewport_;

    ui::ImportReportView reportView_;
    juce::Viewport reportViewport_;
    ui::ReportBar reportBar_;

    /// Ticket `55`: the header's preset field, and the browser over the strip and the page. The browser is
    /// **added last**, so it is the topmost child — a surface over the page rather than a window, which is
    /// what keeps it out of the layout: `resized` places it over the page area and it changes no panel's
    /// bounds, so the fit ticket `53` measured is untouched whether it is open or shut.
    ui::PresetField presetField_;
    ui::PresetBrowser browser_;

    /// The bank, as the manifest describes it, and which slot the loaded preset came from.
    ui::BankCatalog catalog_;
    int bankIndex_ { -1 };

    /// The bank read while it is in flight (ticket 56): the worker, the catalogue it is filling, and the
    /// flag that says it has. `catalog_` above is the interface's copy and is only ever written on this
    /// thread — at construction when the read answers in time, or by `installBankCatalog` when it does not.
    content::OffThreadWork bankWork_;
    std::shared_ptr<ui::BankCatalog> pendingCatalog_;
    std::shared_ptr<std::atomic<bool>> pendingCatalogFinished_;

    HeaderLayout headerLayout_;

    /// The component the editor listens to keys on — its own top-level, so the gesture works wherever the
    /// focus is. A safe pointer, because it is a component this editor does not own.
    juce::Component::SafePointer<juce::Component> keyListenerHost_;

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

    /// The whole interface at the nominal size, scaled into the window, and the nominal size itself
    /// (ticket `59`). `canvas_` is declared after the components it holds, so it is destroyed before them;
    /// a component's destructor detaches its children (`juce_Component.cpp`), which is what makes that
    /// order safe for both halves of the tree.
    Canvas canvas_;
    juce::Point<int> nominalSize_ {};

    static std::atomic<int> created_;
    static std::atomic<int> alive_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XerumAudioProcessorEditor)
};

} // namespace xerum::plugin
