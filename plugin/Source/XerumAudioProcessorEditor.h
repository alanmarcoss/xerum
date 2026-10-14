// xerum::plugin — the interface.
//
// Ticket `31`. The interface's shape is designed here; the **binding is model-driven**, which is ticket
// `13`'s rule: every control is created from a `ParamSpec` and bound to the bridge's own parameter object,
// so adding a parameter to the schema adds a control, and the interface cannot be missing one the engine
// has. `ui::planInterface` places the panels; `ui::ParameterControl` builds and binds one parameter's
// control; `ui::ImportReportView` shows ticket `23`'s report.
//
// Three decisions ticket `31` names, made here and recorded in `plugin/README.md`:
//
//   1. **Coverage** — v1 exposes *every* parameter the bridge exposes: all of them have a control in
//      xerum's own window, in their module's panel. Not a curated subset, and not the 776 schema leaves
//      either: the bridge's surface is exactly what the engine implements (a control that writes a value
//      nothing reads is the placeholder smell this project forbids), and hiding half of it would make the
//      interface a subset of the plugin. The coverage is asserted, not asserted-about: the editor builds
//      one control per spec and `plugin/tests/interface_test.cpp` checks the count and every id.
//   2. **The report** — a permanent panel, one row per entry, with severity, module path, item and reason
//      (`ui::ImportReportView`). Not a modal.
//   3. **Presets** — xerum's presets travel through the **host's own preset mechanism** (state, ticket
//      30), because that path works on a machine with no reference installed and already round-trips
//      byte-identically. The interface does not browse the content root for a preset list; it offers one
//      explicit action, importing a `.SerumPreset`, whose dialog merely *starts* at the first existing
//      content root.
//
// ## The editor must not exist until a host opens it
//
// The headless harness is how the reference is driven and how this plugin is validated, so nothing here
// may be constructed eagerly. This class counts its own constructions (`instancesCreated`) and live
// instances (`instancesAlive`) so that the claim is a measurement rather than a promise: a test drives the
// processor through prepare, render and state restore and requires zero editors, and the count would fail
// if anyone made the editor eager. Nothing in this file is reachable from `processBlock`.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ImportReportView.h"
#include "InterfacePlan.h"
#include "ParameterPanels.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

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

    const ui::ImportReportView& reportView() const { return reportView_; }
    const juce::TextButton& importButton() const { return import_; }

    /// Read the processor's report and show it. Called on a change message (a preset load or a state
    /// restore) and once at construction; public because the tests drive it without a message loop.
    void refreshReport();

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void importPreset();
    void layoutParameterColumns (juce::Rectangle<int> area);

    XerumAudioProcessor& processor_;

    ui::InterfacePlan plan_;
    juce::OwnedArray<ui::ParameterPanel> panels_;
    juce::Viewport reportViewport_;
    ui::ImportReportView reportView_;

    juce::TextButton import_ { "Import .SerumPreset…" };
    juce::Label status_;
    std::unique_ptr<juce::FileChooser> chooser_;

    static std::atomic<int> created_;
    static std::atomic<int> alive_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XerumAudioProcessorEditor)
};

} // namespace xerum::plugin
