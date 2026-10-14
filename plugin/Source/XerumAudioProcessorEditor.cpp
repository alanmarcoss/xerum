// xerum::plugin — the editor. See XerumAudioProcessorEditor.h.

#include "XerumAudioProcessorEditor.h"

#include "XerumAudioProcessor.h"

namespace xerum::plugin
{

XerumAudioProcessorEditor::XerumAudioProcessorEditor (XerumAudioProcessor& processor)
    : AudioProcessorEditor (&processor),
      processor_ (processor),
      parameters_ (processor)
{
    setSize (1020, 640);

    addAndMakeVisible (parameters_);

    reportHeading_.setText ("Import report", juce::dontSendNotification);
    reportHeading_.setJustificationType (juce::Justification::centredLeft);
    reportHeading_.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    addAndMakeVisible (reportHeading_);

    report_.setMultiLine (true, true);
    report_.setReadOnly (true);
    report_.setCaretVisible (false);
    report_.setScrollbarsShown (true);
    report_.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f,
                                                    juce::Font::plain)));
    addAndMakeVisible (report_);

    load_.onClick = [this] { loadPreset(); };
    addAndMakeVisible (load_);

    status_.setJustificationType (juce::Justification::centredLeft);
    status_.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (status_);

    processor_.addChangeListener (this);
    refreshReport();
}

XerumAudioProcessorEditor::~XerumAudioProcessorEditor()
{
    processor_.removeChangeListener (this);
}

void XerumAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshReport();
}

void XerumAudioProcessorEditor::loadPreset()
{
    chooser_ = std::make_unique<juce::FileChooser> ("Load a preset", juce::File(), "*.SerumPreset");

    chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                           [this] (const juce::FileChooser& chooser)
                           {
                               const auto file = chooser.getResult();

                               if (! file.existsAsFile())
                                   return;

                               const auto result = processor_.loadPresetFromFile (file);

                               status_.setText (result.wasOk() ? "loaded " + file.getFileName()
                                                               : "could not load " + file.getFileName(),
                                                juce::dontSendNotification);
                               refreshReport();
                           });
}

void XerumAudioProcessorEditor::refreshReport()
{
    // Read back rather than echo: the report is the processor's, and after a state restore it is the one
    // the preset produced when it was first loaded.
    report_.setText (processor_.reportText(), false);

    auto footer = juce::String();

    if (const auto error = processor_.lastError(); error.isNotEmpty())
        footer << "last error: " << error << juce::newLine;

    juce::StringArray roots;

    for (const auto& root : processor_.contentRoots())
        roots.add (root);

    if (! roots.isEmpty())
        footer << "content roots, in order: " << roots.joinIntoString (" | ");

    if (footer.isNotEmpty())
        status_.setText (footer, juce::dontSendNotification);
}

void XerumAudioProcessorEditor::paint (juce::Graphics& graphics)
{
    graphics.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void XerumAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (8);

    auto right = area.removeFromRight (520);
    parameters_.setBounds (area.withTrimmedRight (8));

    auto footer = right.removeFromBottom (48);
    load_.setBounds (footer.removeFromTop (26));

    right.removeFromTop (4);
    reportHeading_.setBounds (right.removeFromTop (20));
    report_.setBounds (right.removeFromTop (right.getHeight() - 26));
    status_.setBounds (footer.removeFromTop (22));
}

} // namespace xerum::plugin
