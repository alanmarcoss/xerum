// xerum::plugin — the interface, in code. See XerumAudioProcessorEditor.h.

#include "XerumAudioProcessorEditor.h"

#include "InterfaceLook.h"
#include "PluginIdentity.h"
#include "XerumAudioProcessor.h"

#include <algorithm>
#include <map>

namespace xerum::plugin
{

namespace
{
// Ticket `51`: the window is the design system's, so the chrome above and below the page is the design's
// own: a 34px header (rounded up to 38 to seat the preset field and the readouts without clipping), the
// tab strip under it, and the 22px import bar at the bottom.
constexpr int kHeaderHeight = 38;
constexpr int kMargin = 10;
constexpr int kStripHeight = 30;
constexpr int kReportBarHeight = 22;
constexpr int kDrawerHeight = 300;
constexpr int kWordmarkWidth = 84;
constexpr int kImportWidth = 168;
constexpr int kStatusWidth = 260;

constexpr int kMinWidth = 900;
constexpr int kMinHeight = 600;
constexpr int kDefaultWidth = 1080;
constexpr int kDefaultHeight = 640;
constexpr int kMaxWidth = 2400;
constexpr int kMaxHeight = 1600;
} // namespace

std::atomic<int> XerumAudioProcessorEditor::created_ { 0 };
std::atomic<int> XerumAudioProcessorEditor::alive_ { 0 };

int XerumAudioProcessorEditor::instancesCreated() { return created_.load (std::memory_order_relaxed); }
int XerumAudioProcessorEditor::instancesAlive() { return alive_.load (std::memory_order_relaxed); }

//==============================================================================
XerumAudioProcessorEditor::XerumAudioProcessorEditor (XerumAudioProcessor& processor)
    : AudioProcessorEditor (&processor),
      processor_ (processor),
      plan_ (ui::planInterface (processor.bridge().specs(), ui::designedComposition()))
{
    // The interface's own existence, counted. A host opening the interface is the only thing that moves
    // these, and `plugin/tests/interface_test.cpp` asserts it.
    created_.fetch_add (1, std::memory_order_relaxed);
    alive_.fetch_add (1, std::memory_order_relaxed);

    schemaLeaves_ = static_cast<int> (model::Schema::instance().leafCount());

    // One panel per group and one control per parameter, both straight out of the plan. Nothing below
    // names a parameter: that is the point, and it is what `controlCount()`/`controlFor()` let a test
    // check without reading this file.
    for (const auto& panelPlan : plan_.panels)
    {
        auto* panel = panels_.add (new ui::ParameterPanel (panelPlan.title));

        for (const auto* spec : panelPlan.specs)
        {
            auto* parameter = processor_.bridge().parameters()[static_cast<std::size_t> (spec->paramIndex)];
            panel->addControl (std::make_unique<ui::ParameterControl> (*spec, *parameter,
                                                                      processor_.bridge().choices (spec->choiceSet)));
        }

        // The design's wavetable slot, in an oscillator panel's head. The arrows step the slot's own
        // frame position — the one per-slot table control the model has — and a slot without one gets the
        // field and no arrows rather than arrows that write nothing.
        if (panelPlan.wavetableSlot >= 0)
        {
            auto slot = std::make_unique<ui::WavetableSlot>();
            auto* raw = slot.get();

            if (auto* position = panelPlan.positionRow >= 0 ? panel->controlAtRow (panelPlan.positionRow)
                                                            : nullptr)
                if (auto* knob = position->knob())
                {
                    raw->onStep = [knob] (const int step)
                    { knob->setValue (knob->getValue() + step, juce::sendNotificationSync); };
                }

            raw->setSteppable (raw->onStep != nullptr);
            panel->setWavetableSlot (std::move (slot));
        }

        addChildComponent (*panel);
    }

    // A voice filter's panel carries the filter's **drawn response inside itself**, beside its knobs —
    // the design's `filter-viz` — because the plane is that filter's display and it edits the very
    // response it draws (ticket `45`).
    for (int slot = 0; slot < dsp::kFilterSlots; ++slot)
    {
        auto* plane = planes_.add (new ui::PoleZeroPlane());

        plane->setEditable (true);
        plane->onSectionChanged = [this, slot] (const int section, const dsp::PoleZeroSection& value)
        { processor_.setPoleZeroSection (slot, section, value); };

        const auto module = "VoiceFilter" + std::to_string (slot);

        for (int index = 0; index < static_cast<int> (plan_.panels.size()); ++index)
            if (plan_.panels[static_cast<std::size_t> (index)].module == module)
                panels_[index]->setInlineDisplay (plane);
    }

    // The matrix. **The one section the table does not derive**: ticket `45` gave the bridge a modulation
    // parameter surface, but it is deliberately kept out of `specs()` so `InterfacePlan` does not turn it
    // into a page of 512 controls — a 512-row page is not a matrix.
    matrixViewport_.setViewedComponent (&matrix_, false);
    matrixViewport_.setScrollBarsShown (true, true);
    matrix_.setSize (ui::ModulationMatrixView::preferredWidth(), ui::ModulationMatrixView::preferredHeight());

    // **Ticket 45: the matrix is the editor.** Its eight editable columns bind to the matrix parameters
    // the bridge now declares, so a cell writes a value the engine reads rather than a value nothing
    // reads. The other four columns stay painted from the document.
    matrix_.bindToBridge (processor_.bridge());

    // The composition, as data reaching the screen: the plan's sections, the plan's panels, and the matrix.
    std::vector<ui::ParameterPanel*> panelPointers;

    for (auto* panel : panels_)
        panelPointers.push_back (panel);

    composition_.setComposition (plan_.sections, panelPointers);
    composition_.setMatrix (&matrixViewport_);
    compositionViewport_.setViewedComponent (&composition_, false);
    compositionViewport_.setScrollBarsShown (true, false);
    addAndMakeVisible (compositionViewport_);

    // A strip item — a macro in the bottom row — is drawn the way the design draws one: a bare knob with
    // its label, no panel around it. The section's kind is what says so.
    for (const auto& section : plan_.sections)
        if (section.kind == ui::SectionKind::Strip)
            for (const auto panel : section.panels)
                panels_[panel]->setCompact (true);

    {
        // The strip, from the sections in the composition's own order. Titles are xerum's own words for
        // the bands — not the reference's, and not the plan's group names.
        std::vector<juce::String> titles;

        sectionModules_.clear();

        for (const auto& section : plan_.sections)
        {
            titles.push_back (juce::String (section.title));

            std::vector<juce::String> modules;

            for (const auto panel : section.panels)
                modules.push_back (juce::String (plan_.panels[static_cast<std::size_t> (panel)].module));

            if (section.kind == ui::SectionKind::Matrix)
                modules.push_back ("ModSlot");

            sectionModules_.push_back (std::move (modules));
        }

        strip_.setSections (std::move (titles));
    }

    strip_.onSelect = [this] (const int index) { scrollToSection (index); };
    addAndMakeVisible (strip_);

    // The report surface: the drawer first, so it is *under* the bar it opens from and over the page.
    reportViewport_.setScrollBarsShown (true, false);
    reportViewport_.setViewedComponent (&reportView_, false);
    addChildComponent (reportViewport_);

    reportBar_.onToggleRequested = [this] { setReportDrawerOpen (! reportBar_.drawerOpen()); };
    addAndMakeVisible (reportBar_);

    import_.setButtonText ("Import .SerumPreset…");

    // The hint ticket `31` put on a permanent status line now lives on the button that needs it: the
    // design's header has no spare line, and a tooltip is where "what does this do" belongs. The status
    // label is left for what the import actually did.
    import_.setTooltip ("presets load through the host; this button imports a Serum 2 preset file");
    import_.onClick = [this] { importPreset(); };
    import_.setColour (juce::TextButton::buttonColourId, ui::look::surface2);
    import_.setColour (juce::TextButton::textColourOffId, ui::look::fgDim);
    import_.setColour (juce::TextButton::textColourOnId, ui::look::fg);
    addAndMakeVisible (import_);

    status_.setJustificationType (juce::Justification::centredRight);
    status_.setFont (ui::look::monoFont (ui::look::type::readoutSm));
    status_.setColour (juce::Label::textColourId, ui::look::fgMuted);
    addAndMakeVisible (status_);

    setResizable (true, true);
    setResizeLimits (kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);

    // Ticket `53`: the window opens at the size the composed page **actually needs** at the default width,
    // so that "the whole instrument, at once, with nothing to scroll" is a property of the window rather
    // than of a scroll position. The number is the layout's own — the same total `resized` computes, not a
    // second estimate that could drift from it — and it is clamped to the resize limits, so a table that
    // outgrows the screen opens a maximum-size window that scrolls rather than one off the screen.
    //
    // The first `setSize` is what lays the page out at all (`resized` is what calls `layout`), and it also
    // keeps the old behaviour for a composition that is *shorter* than the default window: the page then
    // fills the window, and the window stays the default.
    setSize (kDefaultWidth, kDefaultHeight);
    setSize (kDefaultWidth, juce::jlimit (kMinHeight, kMaxHeight,
                                          composition_.totalHeight() + kHeaderHeight + kStripHeight
                                              + kReportBarHeight));

    processor_.addChangeListener (this);
    scrollToSection (0);
    refreshReport();
}

XerumAudioProcessorEditor::~XerumAudioProcessorEditor()
{
    processor_.removeChangeListener (this);
    alive_.fetch_sub (1, std::memory_order_relaxed);
}

int XerumAudioProcessorEditor::controlCount() const
{
    int total = 0;

    for (const auto* panel : panels_)
        total += panel->rows();

    return total;
}

int XerumAudioProcessorEditor::boundControlCount() const
{
    return controlCount() + (matrix_.isBound() ? matrix_.boundCellCount() : 0);
}

ui::ParameterControl* XerumAudioProcessorEditor::controlFor (const std::string_view id) const
{
    for (auto* panel : panels_)
        if (auto* control = panel->controlFor (id))
            return control;

    return nullptr;
}

juce::String XerumAudioProcessorEditor::sectionTitle (const int index) const
{
    const auto& sections = composition_.sections();

    return index >= 0 && index < static_cast<int> (sections.size())
               ? juce::String (sections[static_cast<std::size_t> (index)].title)
               : juce::String();
}

juce::String XerumAudioProcessorEditor::sectionId (const int index) const
{
    const auto& sections = composition_.sections();

    return index >= 0 && index < static_cast<int> (sections.size())
               ? juce::String (sections[static_cast<std::size_t> (index)].id)
               : juce::String();
}

std::vector<juce::String> XerumAudioProcessorEditor::sectionModules (const int index) const
{
    return index >= 0 && index < static_cast<int> (sectionModules_.size())
               ? sectionModules_[static_cast<std::size_t> (index)]
               : std::vector<juce::String>();
}

const ui::SectionPlan* XerumAudioProcessorEditor::sectionPlan (const int index) const
{
    const auto& sections = composition_.sections();

    return index >= 0 && index < static_cast<int> (sections.size())
               ? &sections[static_cast<std::size_t> (index)]
               : nullptr;
}

int XerumAudioProcessorEditor::sectionForModule (const juce::String& module) const
{
    for (int index = 0; index < sectionCount(); ++index)
        for (const auto& candidate : sectionModules (index))
            if (candidate == module)
                return index;

    return -1;
}

bool XerumAudioProcessorEditor::sectionIsMatrix (const int index) const
{
    const auto& sections = composition_.sections();

    return index >= 0 && index < static_cast<int> (sections.size())
           && sections[static_cast<std::size_t> (index)].kind == ui::SectionKind::Matrix;
}

ui::ParameterPanel* XerumAudioProcessorEditor::panelForGroup (const std::string& group) const
{
    for (int index = 0; index < static_cast<int> (plan_.panels.size()); ++index)
        if (plan_.panels[static_cast<std::size_t> (index)].title == group)
            return panels_[index];

    return nullptr;
}

const ui::PoleZeroPlane& XerumAudioProcessorEditor::poleZeroPlane (const int filterSlot) const
{
    return *planes_[filterSlot];
}

ui::PoleZeroPlane& XerumAudioProcessorEditor::poleZeroPlane (const int filterSlot)
{
    return *planes_[filterSlot];
}

void XerumAudioProcessorEditor::scrollToSection (const int index)
{
    if (index < 0 || index >= sectionCount())
        return;

    currentSection_ = index;
    strip_.setSelected (index);

    // The one page is taller than the window, so a tab is a scroll: the section's own top, which is what
    // makes the strip a map of the composition rather than a menu of hidden pages.
    const auto bounds = composition_.sectionBounds (index);
    compositionViewport_.setViewPosition (0, juce::jmax (0, bounds.getY()));
    repaint();
}

void XerumAudioProcessorEditor::setReportDrawerOpen (const bool open)
{
    reportBar_.setDrawerOpen (open);
    reportViewport_.setVisible (open);
    resized();
    repaint();
}

void XerumAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshReport();
}

//==============================================================================
juce::String XerumAudioProcessorEditor::tableNameFor (const model::PresetDocument& document, const int slot)
{
    const auto prefix = "Oscillator" + std::to_string (slot) + "/WTOsc" + std::to_string (slot);

    // An inline table takes precedence over a path reference in the contract (§5), so the name says which
    // one the slot is actually rendering from.
    if (document.parameters.find (prefix + "/embeddedWTData[0]") != document.parameters.end())
        return "inline";

    const auto found = document.parameters.find (prefix + "/relativePathToWT");

    if (found == document.parameters.end() || ! found->second.isText())
        return {};

    const auto& path = found->second.text;
    const auto slash = path.find_last_of ("/\\");

    return juce::String (slash == std::string::npos ? path : path.substr (slash + 1));
}

void XerumAudioProcessorEditor::updateWavetableHeads()
{
    const auto document = processor_.document();

    for (const auto& panelPlan : plan_.panels)
    {
        if (panelPlan.wavetableSlot < 0)
            continue;

        for (auto* panel : panels_)
            if (panel->title() == panelPlan.title)
                if (auto* slot = panel->wavetableSlot())
                    slot->setTableName (tableNameFor (document, panelPlan.wavetableSlot));
    }
}

void XerumAudioProcessorEditor::refreshReport()
{
    // Read back rather than echo: the report is the processor's, and after a state restore it is the one
    // the preset produced when it was first loaded.
    const auto report = processor_.report();
    const auto document = processor_.document();

    presetName_ = juce::String (document.name);

    // The width first, because how many lines an entry needs depends on it, then the height the rows
    // turned out to need — never shorter than the drawer, so the panel reads as a panel.
    const auto width = juce::jmax (80, reportViewport_.getMaximumVisibleWidth());

    reportView_.setSize (width, 1);
    reportView_.setReport (report, processor_.lastError(), processor_.contentRoots());
    reportView_.setSize (width, juce::jmax (reportView_.preferredHeight(),
                                            reportViewport_.getMaximumVisibleHeight()));

    // The bar is the surface that is always there, so it is updated even when the drawer is shut.
    reportBar_.setReport (reportView_.summaryText(), presetName_, report, processor_.lastError());

    // The two drawn surfaces, from the same document the loader produced and the engine was given.
    matrix_.setRows (ui::modulationRowsFromDocument (document));

    for (int slot = 0; slot < planes_.size(); ++slot)
    {
        const auto type = document.parameters.find ("VoiceFilter" + std::to_string (slot)
                                                    + "/plainParams/kParamType");
        planes_[slot]->setFilter (slot, type != document.parameters.end() && type->second.isText()
                                            ? juce::String (type->second.text)
                                            : juce::String());
        planes_[slot]->setSections (poleZeroFromDocument (document, slot));
    }

    // The knobs' second arc, from the same rows: a knob whose parameter a modulation row addresses draws
    // one, and a knob nothing addresses draws none.
    updateModulationArcs();

    // The oscillator heads, from the same document: the table a preset names, or `inline`, or nothing.
    updateWavetableHeads();

    // The strip's dots: where the import went wrong, on the section it went wrong in.
    updateSectionSeverity (report);
}

void XerumAudioProcessorEditor::updateModulationArcs()
{
    // A parameter is modulated when a matrix row addresses it, and the arc says by how much. The address
    // is the row's own three parts — class, instance, parameter name — and the path they compose is the
    // very `ParamSpec::modelPath` the controls are built from, so the two cannot drift.
    std::map<std::string, double> depth;

    for (int index = 0; index < matrix_.rowCount(); ++index)
    {
        const auto& row = matrix_.row (index);

        if (! row.stored || row.slot.bypassed || row.slot.source == 0)
            continue;

        // A row the engine does not apply contributes nothing, so it modulates nothing: the arc is drawn
        // for the rows that are *heard*, which is what the matrix's own note column already reports.
        if (! dsp::modDestinationUnresolvedReason (row.slot).empty())
            continue;

        const auto path = row.slot.destinationClass + std::to_string (row.slot.destinationInstance)
                          + "/plainParams/" + row.slot.destinationName;

        depth[path] += row.slot.amount / 100.0;
    }

    for (int index = 0; index < static_cast<int> (plan_.panels.size()); ++index)
    {
        for (const auto* spec : plan_.panels[static_cast<std::size_t> (index)].specs)
        {
            auto* control = panels_[index]->controlFor (spec->id);

            if (control == nullptr)
                continue;

            if (auto* knob = control->knob())
            {
                const auto found = depth.find (spec->modelPath);
                knob->setModulationDepth (found == depth.end() ? 0.0 : found->second);
            }
        }
    }
}

void XerumAudioProcessorEditor::updateSectionSeverity (const model::ImportReport& report)
{
    strip_.clearSeverity();

    for (int index = 0; index < sectionCount(); ++index)
    {
        auto has = false;
        auto worst = model::Severity::Info;

        for (const auto& module : sectionModules (index))
            for (const auto& entry : report.entries)
            {
                const juce::String path (entry.modulePath);

                // `ModSlot{N}` is the matrix's module path in a report — the loader reports a row's fields
                // against the template row, because a row's identity is its slot rather than an instance.
                const auto matches = module == "ModSlot"
                                         ? path.startsWith ("ModSlot")
                                         : (path == module || path.startsWith (module + "/"));

                if (! matches)
                    continue;

                has = true;

                if (entry.severity > worst)
                    worst = entry.severity;
            }

        strip_.setSeverity (index, has, worst);
    }
}

void XerumAudioProcessorEditor::importPreset()
{
    // The decision ticket `31` names. xerum's own presets arrive through the host's preset mechanism —
    // state, which round-trips byte-identically (ticket 30) and exists on a machine with no reference
    // installed. This is the *import* path for the reference's format, so it is an explicit file action,
    // and the dialog only starts at the first content root that exists (the reference's tree on a machine
    // that has one, otherwise the user's home).
    chooser_ = std::make_unique<juce::FileChooser> (
        "Import a Serum 2 preset",
        ui::importStartDirectory (processor_.contentRoots(),
                                  juce::File::getSpecialLocation (juce::File::userHomeDirectory)),
        "*.SerumPreset");

    chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                           [this] (const juce::FileChooser& chooser)
                           {
                               const auto file = chooser.getResult();

                               if (! file.existsAsFile())
                                   return;

                               const auto result = processor_.loadPresetFromFile (file);

                               status_.setText (result.wasOk() ? "imported " + file.getFileName()
                                                               : "could not import " + file.getFileName(),
                                                juce::dontSendNotification);
                               refreshReport();
                           });
}

void XerumAudioProcessorEditor::paint (juce::Graphics& graphics)
{
    graphics.fillAll (ui::look::bg);

    // The header: the design's gradient from `surface2` to `surface`, closed by the border.
    {
        auto header = getLocalBounds().removeFromTop (kHeaderHeight);

        graphics.setGradientFill (juce::ColourGradient (ui::look::surface2, 0.0f,
                                                        static_cast<float> (header.getY()),
                                                        ui::look::surface, 0.0f,
                                                        static_cast<float> (header.getBottom()), false));
        graphics.fillRect (header);
        graphics.setColour (ui::look::border);
        graphics.fillRect (header.removeFromBottom (1));
    }

    auto header = getLocalBounds().removeFromTop (kHeaderHeight).reduced (kMargin, 0);

    // The wordmark: the design's logo mark — the accent, bold, letter-spaced.
    graphics.setColour (ui::look::accent);
    graphics.setFont (ui::look::bodyFont (ui::look::type::logo, true).withExtraKerningFactor (0.16f));
    graphics.drawText (juce::String (identity::kName).toUpperCase(), header.removeFromLeft (kWordmarkWidth),
                       juce::Justification::centredLeft, false);

    // The preset field: a monospaced name in a `bg` box, so what is loaded is always on screen.
    header.removeFromLeft (6);

    auto field = header.removeFromLeft (juce::jmin (240, header.getWidth() / 3)).reduced (0, 8);
    graphics.setColour (ui::look::bg);
    graphics.fillRoundedRectangle (field.toFloat(), static_cast<float> (ui::look::radiusPanel));
    graphics.setColour (ui::look::border);
    graphics.drawRoundedRectangle (field.toFloat().reduced (0.5f), static_cast<float> (ui::look::radiusPanel), 1.0f);
    graphics.setColour (ui::look::fgDim);
    graphics.setFont (ui::look::monoFont (10.0f));
    graphics.drawText (presetName_.isEmpty() ? juce::String ("init") : presetName_,
                       field.reduced (8, 0), juce::Justification::centredLeft, true);

    // **The honest measure of the gap**, on screen: how many of the schema's leaves this build binds. The
    // count is the bridge's own length, so it cannot drift from what is actually on the composition.
    {
        const auto reserved = kStatusWidth + kImportWidth + 24;
        auto coverage = header.removeFromLeft (juce::jmax (0, header.getWidth() - reserved));
        const auto counts = juce::String (boundControlCount()) + " of " + juce::String (schemaLeaves_);
        const auto font = ui::look::monoFont (10.0f);
        const auto countsWidth = juce::GlyphArrangement::getStringWidth (font, counts);

        graphics.setFont (font);
        graphics.setColour (ui::look::accent);
        graphics.drawText (counts, coverage.removeFromLeft (juce::jmin (coverage.getWidth(),
                                                                        static_cast<int> (countsWidth) + 2)),
                           juce::Justification::centredLeft, false);
        graphics.setColour (ui::look::fgMuted);
        graphics.drawText (" schema leaves bound", coverage, juce::Justification::centredLeft, true);
    }
}

void XerumAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    auto header = area.removeFromTop (kHeaderHeight).reduced (kMargin, 8);
    header.removeFromLeft (kWordmarkWidth + 6 + juce::jmin (240, header.getWidth() / 3));

    import_.setBounds (header.removeFromRight (juce::jmin (kImportWidth, juce::jmax (120, header.getWidth() / 3))));
    header.removeFromRight (8);
    status_.setBounds (header.removeFromRight (juce::jmin (kStatusWidth, juce::jmax (0, header.getWidth()))));

    strip_.setBounds (area.removeFromTop (kStripHeight));

    reportBar_.setBounds (area.removeFromBottom (kReportBarHeight));

    area = area.reduced (kMargin, 0);

    // The composed page. It is one page — the design's whole instrument — and ticket `53` packs it so that
    // it fits this window at the default size. The layout is given the viewport's **visible** size, and
    // both of those numbers depend on whether the scroll bars are showing, so it runs to a fixed point
    // rather than assuming either.
    compositionViewport_.setBounds (area);

    auto composeWidth = compositionViewport_.getMaximumVisibleWidth();
    auto composeHeight = compositionViewport_.getMaximumVisibleHeight();

    for (int pass = 0; pass < 4; ++pass)
    {
        composition_.layout (composeWidth, composeHeight);

        const auto visibleWidth = compositionViewport_.getMaximumVisibleWidth();
        const auto visibleHeight = compositionViewport_.getMaximumVisibleHeight();

        if (visibleWidth <= 0 || visibleHeight <= 0
            || (visibleWidth == composeWidth && visibleHeight == composeHeight))
            break;

        composeWidth = visibleWidth;
        composeHeight = visibleHeight;
    }

    // The drawer: the lower part of the page, over whatever the page is showing and above the bar. It is a
    // component of this window rather than a dialog, which is what "never a modal" means in practice.
    auto drawer = area.removeFromBottom (
        juce::jmin (kDrawerHeight, juce::jmax (kDrawerHeight / 2, area.getHeight() - 20)));

    reportViewport_.setBounds (drawer);
    reportViewport_.setVisible (reportBar_.drawerOpen());

    // The drawer re-wraps at its new width, so the rows' height follows the window.
    reportView_.setSize (juce::jmax (80, reportViewport_.getMaximumVisibleWidth()),
                         juce::jmax (reportView_.preferredHeight(), reportViewport_.getMaximumVisibleHeight()));
}

} // namespace xerum::plugin
