// xerum::plugin — the interface, in code. See XerumAudioProcessorEditor.h.

#include "XerumAudioProcessorEditor.h"

#include "InterfaceLook.h"
#include "PluginIdentity.h"
#include "XerumAudioProcessor.h"

#include <algorithm>

namespace xerum::plugin
{

namespace
{
constexpr int kHeaderHeight = 44;
constexpr int kMargin = 10;
constexpr int kStripHeight = 42;
constexpr int kReportBarHeight = 40;
constexpr int kDrawerHeight = 300;
constexpr int kColumns = 3;          ///< the most columns a page's rows are spread over
constexpr int kControlWidth = 280;   ///< the width a row's control wants next to its label
constexpr int kWordmarkWidth = 96;
constexpr int kCoverageWidth = 300;
constexpr int kPageTitleWidth = 170;

/// A filter page's pole/zero plane: the height it wants, and the height a page that carries one must
/// therefore have room for.
constexpr int kPlaneHeight = 320;

constexpr int kMinWidth = 920;
constexpr int kMinHeight = 560;
constexpr int kDefaultWidth = 1240;
constexpr int kDefaultHeight = 660;
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
      plan_ (ui::planInterface (processor.bridge().specs(), kColumns))
{
    // The interface's own existence, counted. A host opening the interface is the only thing that moves
    // these, and `plugin/tests/interface_test.cpp` asserts it.
    created_.fetch_add (1, std::memory_order_relaxed);
    alive_.fetch_add (1, std::memory_order_relaxed);

    schemaLeaves_ = static_cast<int> (model::Schema::instance().leafCount());

    // One page per group, one control per parameter, both straight out of the plan. Nothing below names a
    // parameter: that is the point, and it is what `controlCount()`/`controlFor()` let a test check
    // without reading this file.
    for (int index = 0; index < static_cast<int> (plan_.panels.size()); ++index)
    {
        const auto& panelPlan = plan_.panels[static_cast<std::size_t> (index)];
        auto* panel = panels_.add (new ui::ParameterPanel (panelPlan.title));

        for (const auto* spec : panelPlan.specs)
        {
            auto* parameter = processor_.bridge().parameters()[static_cast<std::size_t> (spec->paramIndex)];
            panel->addControl (std::make_unique<ui::ParameterControl> (*spec, *parameter,
                                                                       processor_.bridge().choices (spec->choiceSet)));
        }

        // The plan's balance, reaching the screen: the page places rows, it does not choose their column.
        panel->setColumns (panelPlan.columns);

        addChildComponent (*panel);
        devices_.push_back ({ panelPlan.title, panelPlan.module, index, -1 });
    }

    // A voice filter's page carries the filter's drawn response as well — the second surface ticket `43`
    // called hard. Two planes, one per voice filter, each on the device page whose module it belongs to.
    for (int slot = 0; slot < dsp::kFilterSlots; ++slot)
    {
        addChildComponent (*planes_.add (new ui::PoleZeroPlane()));

        const auto module = "VoiceFilter" + std::to_string (slot);

        for (auto& device : devices_)
            if (device.module == module)
                device.plane = planes_.size() - 1;
    }

    // The matrix page. **The one device the table does not derive**, and the reason is the bridge's own
    // rule: it binds 75 parameters and no modulation ones, so there is no group for the matrix to be a
    // page *of*. Its data is the loaded document's `ModSlot0`…`ModSlot63`.
    addChildComponent (matrixViewport_);
    matrixViewport_.setViewedComponent (&matrix_, false);
    matrixViewport_.setScrollBarsShown (true, true);
    matrix_.setSize (ui::ModulationMatrixView::preferredWidth(), ui::ModulationMatrixView::preferredHeight());

    {
        // The strip, from the devices in the plan's own order, then the matrix. Titles are the table's own
        // group names — xerum's, not the reference's.
        devices_.push_back ({ "Matrix", "ModSlot", -1, -1 });

        std::vector<juce::String> titles, modules;

        for (const auto& device : devices_)
        {
            titles.push_back (device.title);
            modules.push_back (device.module);
        }

        strip_.setDevices (std::move (titles), std::move (modules));
    }

    strip_.onSelect = [this] (const int index) { showDevice (index); };
    addAndMakeVisible (strip_);

    // The report surface: the drawer first, so it is *under* the bar it opens from and over the page.
    reportViewport_.setScrollBarsShown (true, false);
    reportViewport_.setViewedComponent (&reportView_, false);
    addChildComponent (reportViewport_);

    reportBar_.onToggleRequested = [this] { setReportDrawerOpen (! reportBar_.drawerOpen()); };
    addAndMakeVisible (reportBar_);

    import_.onClick = [this] { importPreset(); };
    import_.setColour (juce::TextButton::buttonColourId, ui::look::track);
    import_.setColour (juce::TextButton::textColourOffId, ui::look::text);
    import_.setColour (juce::TextButton::textColourOnId, ui::look::background);
    addAndMakeVisible (import_);

    status_.setJustificationType (juce::Justification::centredLeft);
    status_.setFont (juce::FontOptions (11.5f));
    status_.setColour (juce::Label::textColourId, ui::look::dimText);
    status_.setText ("presets load through the host; this button imports a Serum 2 preset file",
                     juce::dontSendNotification);
    addAndMakeVisible (status_);

    setResizable (true, true);
    setResizeLimits (kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
    setSize (juce::jmax (kMinWidth, juce::jmax (kDefaultWidth, naturalWidth())),
             juce::jmax (kMinHeight, juce::jmax (kDefaultHeight, naturalHeight())));

    processor_.addChangeListener (this);
    showDevice (0);
    refreshReport();
}

XerumAudioProcessorEditor::~XerumAudioProcessorEditor()
{
    processor_.removeChangeListener (this);
    alive_.fetch_sub (1, std::memory_order_relaxed);
}

int XerumAudioProcessorEditor::naturalHeight() const
{
    int tallest = 200;

    for (int index = 0; index < static_cast<int> (plan_.panels.size()); ++index)
    {
        auto page = plan_.panels[static_cast<std::size_t> (index)].height();

        for (const auto& device : devices_)
            if (device.panel == index && device.plane >= 0)
                page = juce::jmax (page, kPlaneHeight);

        tallest = juce::jmax (tallest, page);
    }

    return kHeaderHeight + kStripHeight + ui::kPageHeaderHeight + tallest + kReportBarHeight + kMargin * 3;
}

int XerumAudioProcessorEditor::naturalWidth() const
{
    // A page's own columns are the width that matters, and they come from the plan rather than from a
    // written-down number: a page with more columns needs a wider window. The matrix is deliberately not
    // part of this — it scrolls rather than forcing the window wider than a device page needs.
    const auto columns = plan_.widestPanel();

    return kMargin * 2 + columns * (ui::kRowLabelWidth + kControlWidth)
           + (columns - 1) * ui::kColumnGap;
}

int XerumAudioProcessorEditor::controlCount() const
{
    int total = 0;

    for (const auto* panel : panels_)
        total += panel->rows();

    return total;
}

ui::ParameterControl* XerumAudioProcessorEditor::controlFor (const std::string_view id) const
{
    for (auto* panel : panels_)
        if (auto* control = panel->controlFor (id))
            return control;

    return nullptr;
}

juce::String XerumAudioProcessorEditor::deviceTitle (const int index) const
{
    return index >= 0 && index < deviceCount() ? juce::String (devices_[static_cast<std::size_t> (index)].title)
                                               : juce::String();
}

juce::String XerumAudioProcessorEditor::deviceModule (const int index) const
{
    return index >= 0 && index < deviceCount() ? juce::String (devices_[static_cast<std::size_t> (index)].module)
                                               : juce::String();
}

bool XerumAudioProcessorEditor::deviceIsMatrix (const int index) const
{
    return index >= 0 && index < deviceCount() && devices_[static_cast<std::size_t> (index)].panel < 0;
}

ui::ParameterPanel* XerumAudioProcessorEditor::panelForDevice (const int index) const
{
    if (index < 0 || index >= deviceCount())
        return nullptr;

    const auto panel = devices_[static_cast<std::size_t> (index)].panel;

    return panel >= 0 ? panels_[panel] : nullptr;
}

const ui::PoleZeroPlane& XerumAudioProcessorEditor::poleZeroPlane (const int filterSlot) const
{
    return *planes_[filterSlot];
}

void XerumAudioProcessorEditor::showDevice (const int index)
{
    if (index < 0 || index >= deviceCount())
        return;

    currentDevice_ = index;
    strip_.setSelected (index);

    const auto& device = devices_[static_cast<std::size_t> (index)];

    // One device at a time, and the strip is what says so.
    for (auto* panel : panels_)
        panel->setVisible (false);

    for (auto* plane : planes_)
        plane->setVisible (false);

    matrixViewport_.setVisible (device.panel < 0);

    if (device.panel >= 0)
    {
        panels_[device.panel]->setVisible (true);

        if (device.plane >= 0)
            planes_[device.plane]->setVisible (true);
    }

    resized();
    repaint();
}

void XerumAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshReport();
}

void XerumAudioProcessorEditor::setReportDrawerOpen (const bool open)
{
    reportBar_.setDrawerOpen (open);
    reportViewport_.setVisible (open);
    resized();
    repaint();
}

void XerumAudioProcessorEditor::refreshReport()
{
    // Read back rather than echo: the report is the processor's, and after a state restore it is the one
    // the preset produced when it was first loaded.
    const auto report = processor_.report();
    const auto document = processor_.document();

    // The width first, because how many lines an entry needs depends on it, then the height the rows
    // turned out to need — never shorter than the drawer, so the panel reads as a panel.
    const auto width = juce::jmax (80, reportViewport_.getMaximumVisibleWidth());

    reportView_.setSize (width, 1);
    reportView_.setReport (report, processor_.lastError(), processor_.contentRoots());
    reportView_.setSize (width, juce::jmax (reportView_.preferredHeight(),
                                            reportViewport_.getMaximumVisibleHeight()));

    // The bar is the surface that is always there, so it is updated even when the drawer is shut.
    reportBar_.setReport (reportView_.summaryText(), juce::String (document.name), report,
                          processor_.lastError());

    // The two drawn surfaces, from the same document the loader produced and the engine was given.
    matrix_.setRows (ui::modulationRowsFromDocument (document));

    for (int slot = 0; slot < planes_.size(); ++slot)
    {
        const auto type = document.parameters.find ("VoiceFilter" + std::to_string (slot)
                                                    + "/plainParams/kParamType");
        planes_[slot]->setFilter (slot, type != document.parameters.end() && type->second.isText()
                                            ? juce::String (type->second.text)
                                            : juce::String());
        planes_[slot]->setSections (ui::poleZeroFromDocument (document, slot));
    }

    // The strip's dots: where the import went wrong, on the device it went wrong in.
    updateDeviceSeverity (report);
}

void XerumAudioProcessorEditor::updateDeviceSeverity (const model::ImportReport& report)
{
    strip_.clearSeverity();

    for (int index = 0; index < deviceCount(); ++index)
    {
        const auto module = deviceModule (index);
        auto has = false;
        auto worst = model::Severity::Info;

        for (const auto& entry : report.entries)
        {
            const juce::String path (entry.modulePath);

            // `ModSlot{N}` is the matrix's module path in a report — the loader reports a row's fields
            // against the template row, because a row's identity is its slot rather than an instance.
            const auto matches = module == "ModSlot" ? path.startsWith ("ModSlot")
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
    graphics.fillAll (ui::look::background);

    auto header = getLocalBounds().removeFromTop (kHeaderHeight).reduced (kMargin, 0);

    graphics.setColour (ui::look::text);
    graphics.setFont (juce::FontOptions (19.0f, juce::Font::bold));
    graphics.drawText (identity::kName, header.removeFromLeft (kWordmarkWidth),
                       juce::Justification::centredLeft, false);

    // **The honest measure of the gap**, on screen: how many of the schema's leaves this build binds. The
    // count is the bridge's own length, so it cannot drift from what is actually on the pages.
    graphics.setColour (ui::look::dimText);
    graphics.setFont (juce::FontOptions (11.0f));
    graphics.drawText (juce::String (controlCount()) + " of " + juce::String (schemaLeaves_)
                           + " schema leaves bound",
                       header.removeFromLeft (kCoverageWidth), juce::Justification::centredLeft, true);

    graphics.setColour (ui::look::accent);
    graphics.fillRect (kMargin, kHeaderHeight - 1, getWidth() - kMargin * 2, 1);

    if (devices_.empty() || pageArea_.isEmpty())
        return;

    // The page's own header: which device, which module, and how much of it there is.
    auto pageHeader = pageArea_.removeFromTop (ui::kPageHeaderHeight);
    const auto& device = devices_[static_cast<std::size_t> (currentDevice_)];

    graphics.setColour (ui::look::text);
    graphics.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    graphics.drawText (juce::String (device.title), pageHeader.removeFromLeft (kPageTitleWidth),
                       juce::Justification::centredLeft, true);

    juce::String subtitle;

    if (device.panel >= 0)
    {
        const auto& panel = plan_.panels[static_cast<std::size_t> (device.panel)];

        subtitle = "device " + juce::String (device.panel + 1) + " of "
                   + juce::String (static_cast<int> (plan_.panels.size())) + "  ·  "
                   + juce::String (device.module) + "  ·  " + juce::String (panel.rows())
                   + (panel.rows() == 1 ? " control" : " controls") + "  ·  " + juce::String (panel.columnsCount())
                   + (panel.columnsCount() == 1 ? " column" : " columns");
    }
    else
    {
        subtitle = "device  ·  " + juce::String (device.module) + "  ·  "
                   + juce::String (ui::kModulationSlots) + " rows × "
                   + juce::String (ui::ModulationMatrixView::columnCount())
                   + " columns, read from the loaded preset";
    }

    graphics.setColour (ui::look::dimText);
    graphics.setFont (juce::FontOptions (11.0f));
    graphics.drawText (subtitle, pageHeader, juce::Justification::centredLeft, true);
}

void XerumAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    auto header = area.removeFromTop (kHeaderHeight).reduced (kMargin, 9);
    header.removeFromLeft (kWordmarkWidth);
    header.removeFromLeft (kCoverageWidth);

    import_.setBounds (header.removeFromRight (juce::jmin (190, juce::jmax (120, header.getWidth() / 3))));
    header.removeFromRight (8);
    status_.setBounds (header);

    strip_.setBounds (area.removeFromTop (kStripHeight));

    reportBar_.setBounds (area.removeFromBottom (kReportBarHeight).reduced (kMargin, 4));

    area = area.reduced (kMargin, 0);
    pageArea_ = area;

    if (devices_.empty())
        return;

    const auto& device = devices_[static_cast<std::size_t> (currentDevice_)];
    auto content = area.withTrimmedTop (ui::kPageHeaderHeight);

    if (device.panel < 0)
    {
        matrixViewport_.setBounds (content);
    }
    else
    {
        if (device.plane >= 0)
        {
            auto planeArea = content.removeFromRight (juce::jmax (300, content.getWidth() * 2 / 5));

            if (planeArea.getWidth() < content.getWidth())
                content.removeFromRight (ui::kColumnGap);

            planes_[device.plane]->setBounds (planeArea.removeFromTop (juce::jmin (kPlaneHeight,
                                                                                   planeArea.getHeight())));
        }

        panels_[device.panel]->setBounds (content.removeFromTop (
            plan_.panels[static_cast<std::size_t> (device.panel)].height()));
    }

    // The drawer: the lower part of the page, over whatever the page is showing and above the bar. It is a
    // component of this window rather than a dialog, which is what "never a modal" means in practice.
    auto drawer = area.removeFromBottom (
        juce::jmin (kDrawerHeight, juce::jmax (kDrawerHeight / 2, area.getHeight() - ui::kPageHeaderHeight - 20)));

    reportViewport_.setBounds (drawer);
    reportViewport_.setVisible (reportBar_.drawerOpen());

    // The drawer re-wraps at its new width, so the rows' height follows the window.
    reportView_.setSize (juce::jmax (80, reportViewport_.getMaximumVisibleWidth()),
                         juce::jmax (reportView_.preferredHeight(), reportViewport_.getMaximumVisibleHeight()));
}

} // namespace xerum::plugin
