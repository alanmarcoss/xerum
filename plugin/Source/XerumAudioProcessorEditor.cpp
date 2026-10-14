// xerum::plugin — the interface, in code. See XerumAudioProcessorEditor.h.

#include "XerumAudioProcessorEditor.h"

#include "InterfaceLook.h"
#include "PluginIdentity.h"
#include "XerumAudioProcessor.h"

namespace xerum::plugin
{

namespace
{
constexpr int kHeaderHeight = 40;
constexpr int kMargin = 8;
constexpr int kReportWidth = 340;
constexpr int kColumns = 3;
constexpr int kDefaultWidth = 1360;
constexpr int kDefaultHeight = 720;

/// The window's own limits. The minimum is the layout's natural size — below it a parameter would be
/// clipped, and a clipped control is a parameter that is not really reachable.
constexpr int kMinWidth = 1220;
constexpr int kMinHeight = 700;
constexpr int kMaxWidth = 2400;
constexpr int kMaxHeight = 1600;

int naturalHeight (const ui::InterfacePlan& plan)
{
    // The tallest column, since the columns are laid out side by side.
    int tallest = 0;

    for (const auto& column : plan.columns)
    {
        int height = 0;

        for (const auto panel : column)
            height += plan.panels[static_cast<std::size_t> (panel)].height() + ui::kPanelGap;

        tallest = juce::jmax (tallest, height);
    }

    return tallest + kHeaderHeight + kMargin * 2;
}
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

    // One panel per group, one control per parameter, both straight out of the plan. Nothing below names
    // a parameter: that is the point, and it is what `controlCount()`/`controlFor()` let a test check
    // without reading this file.
    for (const auto& panelPlan : plan_.panels)
    {
        auto* panel = panels_.add (new ui::ParameterPanel (panelPlan.title));

        for (const auto* spec : panelPlan.specs)
        {
            auto* parameter = processor_.bridge().parameters()[static_cast<std::size_t> (spec->paramIndex)];
            panel->addControl (std::make_unique<ui::ParameterControl> (*spec, *parameter,
                                                                       processor_.bridge().choices (spec->choiceSet)));
        }

        addAndMakeVisible (*panel);
    }

    reportViewport_.setScrollBarsShown (true, false);
    reportViewport_.setViewedComponent (&reportView_, false);
    addAndMakeVisible (reportViewport_);

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
    setSize (juce::jmax (kMinWidth, kDefaultWidth), juce::jmax (kMinHeight, juce::jmax (kDefaultHeight, naturalHeight (plan_))));

    processor_.addChangeListener (this);
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

ui::ParameterControl* XerumAudioProcessorEditor::controlFor (const std::string_view id) const
{
    for (auto* panel : panels_)
        if (auto* control = panel->controlFor (id))
            return control;

    return nullptr;
}

void XerumAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshReport();
}

void XerumAudioProcessorEditor::refreshReport()
{
    // Read back rather than echo: the report is the processor's, and after a state restore it is the one
    // the preset produced when it was first loaded.
    const auto report = processor_.report();
    const auto document = processor_.document();

    // The width first, because how many lines an entry needs depends on it, then the height the rows
    // turned out to need — never shorter than the column, so the panel reads as a panel.
    const auto width = juce::jmax (80, reportViewport_.getMaximumVisibleWidth());
    const auto height = juce::jmax (reportViewport_.getMaximumVisibleHeight(), 1);

    reportView_.setSize (width, juce::jmax (height, reportView_.getHeight()));
    reportView_.setReport (report, juce::String (document.name), processor_.lastError(), processor_.contentRoots());
    reportView_.setSize (width, juce::jmax (height, reportView_.preferredHeight()));
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

void XerumAudioProcessorEditor::layoutParameterColumns (const juce::Rectangle<int> area)
{
    if (plan_.columns.empty())
        return;

    const auto columns = static_cast<int> (plan_.columns.size());
    const auto columnWidth = (area.getWidth() - ui::kPanelGap * (columns - 1)) / columns;

    auto remaining = area;

    for (const auto& column : plan_.columns)
    {
        auto slot = remaining.removeFromLeft (columnWidth);
        remaining.removeFromLeft (ui::kPanelGap);

        for (const auto panel : column)
        {
            panels_[panel]->setBounds (slot.removeFromTop (plan_.panels[static_cast<std::size_t> (panel)].height()));
            slot.removeFromTop (ui::kPanelGap);
        }
    }
}

void XerumAudioProcessorEditor::paint (juce::Graphics& graphics)
{
    graphics.fillAll (ui::look::background);

    auto header = getLocalBounds().removeFromTop (kHeaderHeight);

    graphics.setColour (ui::look::text);
    graphics.setFont (juce::FontOptions (19.0f, juce::Font::bold));
    graphics.drawText (identity::kName, header.reduced (kMargin, 0).removeFromLeft (200),
                       juce::Justification::centredLeft, false);

    graphics.setColour (ui::look::accent);
    graphics.fillRect (kMargin, kHeaderHeight - 1, getWidth() - kMargin * 2, 1);
}

void XerumAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    auto header = area.removeFromTop (kHeaderHeight).reduced (kMargin, 6);
    header.removeFromLeft (200);   // the wordmark, painted

    import_.setBounds (header.removeFromRight (juce::jmin (180, header.getWidth() / 2)));
    header.removeFromRight (6);
    status_.setBounds (header);

    area = area.reduced (kMargin);

    auto report = area.removeFromRight (kReportWidth);
    area.removeFromRight (ui::kPanelGap);

    reportViewport_.setBounds (report);
    reportView_.setSize (juce::jmax (80, reportViewport_.getMaximumVisibleWidth()),
                         juce::jmax (reportViewport_.getMaximumVisibleHeight(), reportView_.preferredHeight()));

    layoutParameterColumns (area);
}

} // namespace xerum::plugin
