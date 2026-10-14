// xerum::plugin — ticket 31: the interface's binding, and the three decisions the ticket names.
//
// The interface is designed; the binding is model-driven (ticket 13). So these tests are about the
// *binding* and about the claims ticket 31 requires be assertable rather than a matter of opinion:
//
//   * every parameter the bridge exposes has exactly one control, in its own group's panel, bound to the
//     bridge's own parameter object and labelled with xerum's own name;
//   * a half-loading preset's report is visible in the interface, with severity, module path, item and
//     reason — not a modal that says "some items were skipped";
//   * the plugin runs headless with **no editor created**, and the counter that says so would fail if
//     anyone made the interface eager;
//   * with the interface open, the audio path still allocates nothing;
//   * the import dialog's starting directory is a decision, not an accident.
//
// Run alone:
//   build-31/plugin/tests/xerum-plugin-tests "[interface]"
//   build-31/plugin/tests/xerum-plugin-tests "[interface][headless]"   # the no-editor assertion
//   build-31/plugin/tests/xerum-plugin-tests "[ui]"                    # the cases that open an editor

#include "TestSupport.h"

#include "AllocationCounter.h"
#include "ImportReportView.h"
#include "InterfaceLook.h"
#include "InterfacePlan.h"
#include "ParameterPanels.h"
#include "XerumAudioProcessorEditor.h"

#include "write.h"

#include <catch2/catch_approx.hpp>

#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace xerum::plugin;
using namespace xerum::plugin::tests;
using namespace xerum::plugin::ui;

namespace
{

/// Open the interface the way a host does — `createEditor()` and nothing else. Deliberately not a
/// helper on the processor: a test that could open the editor by accident would weaken the headless case.
std::unique_ptr<XerumAudioProcessorEditor> openEditor (XerumAudioProcessor& processor)
{
    return std::unique_ptr<XerumAudioProcessorEditor> (
        dynamic_cast<XerumAudioProcessorEditor*> (processor.createEditor()));
}

/// A preset xerum writes itself, carrying one of each kind of thing the report names. Same technique as
/// the preset suite, so this is meaningful on a fresh clone with no reference installed.
juce::File writeFixture (const xerum::model::PresetDocument& document)
{
    const auto written = xerum::model::writeContainer (document, {});
    REQUIRE (written.ok);

    auto file = juce::File::createTempFile (".SerumPreset");
    REQUIRE (file.replaceWithData (written.bytes.data(), written.bytes.size()));

    return file;
}

xerum::model::PresetDocument halfLoadingDocument()
{
    xerum::model::PresetDocument document;
    document.name = "interface fixture";
    document.schemaVersion = "4.0";
    document.parameters["Oscillator0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Oscillator0/plainParams/kParamVolume"] = number (0.5);

    // An asset that cannot resolve: the plugin's own half of the report, at high.
    document.parameters["Oscillator0/WTOsc0/relativePathToWT"] = text ("No/Such/Table.wav");

    // A warp mode the engine does not implement (the model's half, at high), and a parameter the bridge
    // carries and does act on since ticket `41` (the unison spread), so it is *not* a report entry — it is
    // here to prove the row count follows the report rather than a fixed list.
    document.parameters["Oscillator0/WTOsc0/plainParams/kParamWarpMenu"] = text ("kRemap_4");
    document.parameters["Oscillator0/plainParams/kParamUnisonStereo"] = number (75.0);

    // A parameter from a newer reference: retained by the document, uninterpreted, reported at medium.
    document.parameters["Oscillator0/WTOsc0/plainParams/kParamFromTheFuture"] = number (3.0);

    return document;
}

} // namespace

//==============================================================================
TEST_CASE ("the layout is built from the parameter table: every parameter once, in its own group",
           "[interface]")
{
    XerumAudioProcessor processor;
    const auto& specs = processor.bridge().specs();
    const auto plan = planInterface (specs, 3);

    REQUIRE_FALSE (specs.empty());

    // Every parameter the bridge exposes is in the layout, exactly once.
    const auto ids = plan.ids();
    const std::set<std::string> planned (ids.begin(), ids.end());

    CHECK (ids.size() == specs.size());
    CHECK (planned.size() == ids.size());

    for (const auto& spec : specs)
        CHECK (planned.count (spec.id) == 1);

    // Panels are the model's groups, nothing hand-written: as many panels as distinct groups, each panel
    // named after its group, and every spec in the panel named after *its* group.
    std::set<std::string> groups;

    for (const auto& spec : specs)
        groups.insert (spec.group);

    CHECK (plan.panels.size() == groups.size());

    for (const auto& panel : plan.panels)
    {
        CHECK (groups.count (panel.title) == 1);

        for (const auto* spec : panel.specs)
            CHECK (spec->group == panel.title);
    }

    // And every panel is placed in exactly one column.
    std::set<int> placed;

    for (const auto& column : plan.columns)
        for (const auto panel : column)
            placed.insert (panel);

    CHECK (plan.columns.size() == 3);
    CHECK (placed.size() == plan.panels.size());

    // The model type decides the control, total over the types: a new type in the bridge cannot silently
    // reach the interface without one.
    CHECK (controlKindFor (ParamType::Bool) == ControlKind::Toggle);
    CHECK (controlKindFor (ParamType::Choice) == ControlKind::Choice);
    CHECK (controlKindFor (ParamType::Int) == ControlKind::Slider);
    CHECK (controlKindFor (ParamType::Float) == ControlKind::Slider);
}

//==============================================================================
TEST_CASE ("the plugin runs headless with no editor created", "[interface][headless]")
{
    const auto createdBefore = XerumAudioProcessorEditor::instancesCreated();

    {
        // Everything a host does without a UI: construct, prepare, play, save and restore state, and hand
        // it a state block that is not ours. The interface must not come into existence for any of it.
        XerumAudioProcessor processor;
        processor.prepareToPlay (48000.0, 512);
        processor.setParameterValue ("Oscillator0/plainParams/kParamEnable", 1.0);

        const auto rendered = render (processor, 8, 512, noteThenRelease (60, 4));
        CHECK (peakOf (rendered) > 0.0);

        juce::MemoryBlock state;
        processor.getStateInformation (state);
        processor.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        render (processor, 8, 512, noteThenRelease (64, 4));
        processor.setStateInformation ("not a state block", 17);

        // `hasEditor()` is true — the *host* decides whether the interface exists — and nothing above
        // decided it should.
        CHECK (processor.hasEditor());
        CHECK (XerumAudioProcessorEditor::instancesCreated() == createdBefore);
        CHECK (XerumAudioProcessorEditor::instancesAlive() == 0);
    }

    // The positive control, and the thing that makes the check above a measurement instead of a constant:
    // asking for the interface creates exactly one, and it exists only while its owner keeps it. If anyone
    // made the editor eager — in the processor's constructor, in `prepareToPlay`, in `processBlock` — the
    // count above would have moved and this test would fail.
    {
        const juce::ScopedJuceInitialiser_GUI gui;
        XerumAudioProcessor processor;

        auto editor = openEditor (processor);
        REQUIRE (editor != nullptr);
        CHECK (XerumAudioProcessorEditor::instancesCreated() == createdBefore + 1);
        CHECK (XerumAudioProcessorEditor::instancesAlive() == 1);

        editor.reset();
        CHECK (XerumAudioProcessorEditor::instancesAlive() == 0);
    }
}

//==============================================================================
TEST_CASE ("every parameter is reachable through a control labelled with xerum's own name",
           "[interface][ui]")
{
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    const auto& specs = processor.bridge().specs();

    // The interface's own coverage: one control per parameter the bridge exposes, no more and no fewer.
    CHECK (editor->controlCount() == static_cast<int> (specs.size()));

    for (const auto& spec : specs)
    {
        INFO ("parameter " << spec.id);

        auto* control = editor->controlFor (spec.id);
        REQUIRE (control != nullptr);

        // Bound to the bridge's own parameter object — not to a copy, and not merely displayed next to it.
        CHECK (control->isBound());
        CHECK (&control->parameter() == processor.bridge().parameters()[static_cast<std::size_t> (spec.paramIndex)]);

        // Labelled with xerum's own name: the spec's label, which is not the reference's model name and
        // not the reference plugin's display string (ticket 13 D6).
        CHECK (control->label().isNotEmpty());
        CHECK (control->label() == juce::String (spec.label));
        CHECK_FALSE (control->label().containsIgnoreCase ("kParam"));
        CHECK_FALSE (control->label().containsIgnoreCase ("kOsc"));

        // And the widget is the one this model type maps to.
        switch (controlKindFor (spec.type))
        {
            case ControlKind::Toggle:
                CHECK (dynamic_cast<juce::ToggleButton*> (&control->widget()) != nullptr);
                break;
            case ControlKind::Choice:
                CHECK (dynamic_cast<juce::ComboBox*> (&control->widget()) != nullptr);
                break;
            case ControlKind::Slider:
                CHECK (dynamic_cast<juce::Slider*> (&control->widget()) != nullptr);
                break;
        }
    }

    // A choice control offers exactly the model's vocabulary: a value the engine has that the combo does
    // not offer is a value a user cannot reach.
    for (const auto& spec : specs)
    {
        if (spec.type != ParamType::Choice)
            continue;

        INFO ("choice " << spec.id);

        auto* combo = dynamic_cast<juce::ComboBox*> (&editor->controlFor (spec.id)->widget());
        REQUIRE (combo != nullptr);

        const auto& choices = processor.bridge().choices (spec.choiceSet);
        CHECK (combo->getNumItems() == choices.size());

        for (int i = 0; i < choices.size(); ++i)
            CHECK (combo->getItemText (i) == choices.labels[i]);
    }

    // Reachable means reaching the engine, in both directions. A widget that moved but changed nothing
    // would satisfy every check above and be useless.
    {
        auto* slider = dynamic_cast<juce::Slider*> (&editor->controlFor ("oscillator0.volume")->widget());
        REQUIRE (slider != nullptr);

        slider->setValue (0.25, juce::sendNotificationSync);
        CHECK (processor.parameterValue ("Oscillator0/plainParams/kParamVolume") == Catch::Approx (0.25));

        processor.setParameterValue ("Oscillator0/plainParams/kParamVolume", 0.75);
        CHECK (slider->getValue() == Catch::Approx (0.75));
    }

    {
        auto* combo = dynamic_cast<juce::ComboBox*> (&editor->controlFor ("voicefilter0.type")->widget());
        REQUIRE (combo != nullptr);
        REQUIRE (combo->getNumItems() > 1);

        combo->setSelectedItemIndex (1, juce::sendNotificationSync);
        CHECK (processor.parameterValue ("VoiceFilter0/plainParams/kParamType") == Catch::Approx (1.0));
    }
}

//==============================================================================
TEST_CASE ("a half-loading preset's report is visible in the interface", "[interface][ui]")
{
    const juce::ScopedJuceInitialiser_GUI gui;

    const auto file = writeFixture (halfLoadingDocument());

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    // Before anything is loaded: the contract's words, because the report is empty and not absent.
    CHECK (editor->reportView().rowCount() == 0);
    CHECK (editor->reportView().summaryText() == "Nothing to report");

    REQUIRE (processor.loadPresetFromFile (file).wasOk());

    // A host's message loop delivers the load's change message; JUCE compiles modal loops out of this
    // build (`JUCE_MODAL_LOOPS_PERMITTED` is off), so the test delivers it the way JUCE does — through the
    // broadcaster's synchronous path — which also proves the editor is registered as a listener.
    processor.sendSynchronousChangeMessage();

    const auto report = processor.report();
    REQUIRE_FALSE (report.clean());

    const auto& view = editor->reportView();

    // Every entry the processor reports is a row on screen, with its severity, module path, item and
    // reason — the four things the contract requires the user be able to read.
    CHECK (view.rowCount() == static_cast<int> (report.entries.size()));
    CHECK (view.severityCount (xerum::model::Severity::High) == report.count (xerum::model::Severity::High));
    CHECK (view.severityCount (xerum::model::Severity::Medium) == report.count (xerum::model::Severity::Medium));
    CHECK (view.severityCount (xerum::model::Severity::Info) == report.count (xerum::model::Severity::Info));

    for (int i = 0; i < static_cast<int> (report.entries.size()); ++i)
    {
        const auto& entry = report.entries[static_cast<std::size_t> (i)];
        const auto row = view.rowText (i);

        INFO ("report row " << i << ": " << row);

        CHECK (view.rowSeverity (i) == entry.severity);
        CHECK (row.contains (juce::String (entry.modulePath)));
        CHECK (row.contains (juce::String (entry.item)));
        CHECK (row.contains (juce::String (entry.reason)));
        CHECK (row.contains (look::severityName (entry.severity).toUpperCase()));
    }

    // Severity is a colour as well as a word, and the ramp is one ramp.
    CHECK (look::severityColour (xerum::model::Severity::High) != look::severityColour (xerum::model::Severity::Info));
    CHECK (look::severityColour (xerum::model::Severity::High) == look::severityHigh);

    // The summary names the counts and the worst severity, which is the one word a user needs first.
    CHECK (view.summaryText().contains ("high"));
    CHECK (view.summaryText().contains ("medium"));
    CHECK (view.summaryText().contains ("info"));
    CHECK (view.summaryText().contains (juce::String (static_cast<int> (report.entries.size())) + " item"));

    // The missing asset's own path is on screen, so the user can find what was not honoured.
    auto mentionsAsset = false;

    for (int i = 0; i < view.rowCount(); ++i)
        mentionsAsset = mentionsAsset || view.rowText (i).contains ("No/Such/Table.wav");

    CHECK (mentionsAsset);
}

//==============================================================================
TEST_CASE ("the preset-import dialog starts at the first content root that exists", "[interface]")
{
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory);

    const std::string missing = "/nonexistent/xerum/does/not/exist";

    CHECK (importStartDirectory ({}, home) == home);
    CHECK (importStartDirectory ({ juce::String (missing) }, home) == home);
    CHECK (importStartDirectory ({ juce::String (missing), temp.getFullPathName() }, home) == temp);
}

//==============================================================================
TEST_CASE ("with the interface open, a full render still allocates nothing", "[interface][ui][realtime]")
{
    // The constraint ticket 31 repeats: no interface code on the audio path. The strongest available
    // measurement is the one ticket 30 used for the audio path itself — a global allocation counter — with
    // the interface alive: the attachments, the panels and the report view all exist, and the render still
    // allocates nothing.
    //
    // Parameters are deliberately written *before* the measured window. A parameter change repaints the
    // message thread's widgets synchronously, which is not the audio path and is not what this asserts.
    const juce::ScopedJuceInitialiser_GUI gui;

    constexpr int blockSize = 512;
    constexpr int blocks = 200;

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, blockSize);
    processor.setParameterValue ("Oscillator0/plainParams/kParamEnable", 1.0);
    processor.setParameterValue ("VoiceFilter0/plainParams/kParamEnable", 1.0);
    processor.setParameterValue ("Global0/plainParams/kParamPolyCount", 2.0);
    processor.prepareToPlay (48000.0, blockSize);   // polyphony is a prepare() argument (ticket 28)

    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    const auto runSequence = [&processor]
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer events;

        for (int block = 0; block < blocks; ++block)
        {
            buffer.clear();
            events.clear();

            if (block == 0 || block == 20 || block == 40 || block == 60)
                events.addEvent (juce::MidiMessage::noteOn (1, 48 + block / 20, static_cast<juce::uint8> (100)), 0);

            if (block == 30)
                events.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);   // sustain

            if (block == 80)
                events.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);

            if (block == 100)
                events.addEvent (juce::MidiMessage::noteOff (1, 48), 0);

            if (block == 120)
                events.addEvent (juce::MidiMessage::pitchWheel (1, 4096), 0);

            processor.processBlock (buffer, events);
        }
    };

    runSequence();   // warm-up: grow every buffer the measured pass will need

    resetAllocations();
    setTracking (true);

    runSequence();

    setTracking (false);

    UNSCOPED_INFO ("allocations with the interface open: " << allocations());
    CHECK (allocations() == 0);

    // The positive control: the hook is live, so "zero" is a measurement and not a broken instrument.
    CHECK (hookIsLive());
}
