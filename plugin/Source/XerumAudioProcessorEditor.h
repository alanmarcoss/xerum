// xerum::plugin — the editor.
//
// The interface's job for ticket 30 is not to be pretty: it is to make the **import report reachable**.
// The import contract (§3) says the report MUST be available "programmatically, as text in the harness,
// and in the interface", and the reason is that a preset which half-loads must say so to the person
// using it — not in a log nobody reads. So the editor shows the report, the counts per severity, and the
// content roots a missing asset was looked for under.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace xerum::plugin
{

class XerumAudioProcessor;

class XerumAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                        private juce::ChangeListener
{
public:
    explicit XerumAudioProcessorEditor (XerumAudioProcessor& processor);
    ~XerumAudioProcessorEditor() override;

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void loadPreset();
    void refreshReport();

    XerumAudioProcessor& processor_;
    juce::GenericAudioProcessorEditor parameters_;

    juce::Label reportHeading_;
    juce::TextEditor report_;
    juce::TextButton load_ { "Load preset…" };
    juce::Label status_;
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XerumAudioProcessorEditor)
};

} // namespace xerum::plugin
