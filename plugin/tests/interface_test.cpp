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
#include "DeviceStrip.h"
#include "ImportReportView.h"
#include "InterfaceLook.h"
#include "InterfacePlan.h"
#include "ModulationMatrixView.h"
#include "ParameterPanels.h"
#include "PoleZeroPlane.h"
#include "XerumAudioProcessorEditor.h"

#include "modulation.h"
#include "write.h"

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace xerum::plugin;
using namespace xerum::plugin::tests;
using namespace xerum::plugin::ui;

// The two namespaces this suite names a lot but does not import wholesale: an alias, so `model::Value`
// and `dsp::modSourceName` read the way they do in the project's own sources without dragging every
// model and DSP name into a test's global scope.
namespace model = xerum::model;
namespace dsp = xerum::dsp;

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
TEST_CASE ("the layout is built from the parameter table: every parameter once, in its own page",
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

    // Pages are the model's groups, nothing hand-written: as many pages as distinct groups, each page
    // named after its group, and every spec in the page named after *its* group.
    std::set<std::string> groups;

    for (const auto& spec : specs)
        groups.insert (spec.group);

    CHECK (plan.panels.size() == groups.size());

    for (const auto& panel : plan.panels)
    {
        CHECK (groups.count (panel.title) == 1);
        REQUIRE_FALSE (panel.specs.empty());

        for (const auto* spec : panel.specs)
            CHECK (spec->group == panel.title);

        // The page's module is the model's own and the whole page lives in it. The strip's report dots and
        // the page's subtitle are keyed on it, so a page spanning two modules would be a quiet lie.
        for (const auto* spec : panel.specs)
            CHECK (spec->modelPath.rfind (panel.module + "/", 0) == 0);

        // Ticket `44`: the balance now places a page's *rows* — and it still places every one of them, in
        // exactly one column. A row the balance forgot would be a bound control laid out at zero size: a
        // parameter that is still reachable in a host and no longer reachable on screen.
        std::vector<int> placedRows;

        for (const auto& column : panel.columns)
            for (const auto row : column)
                placedRows.push_back (row);

        std::sort (placedRows.begin(), placedRows.end());

        CHECK (placedRows.size() == static_cast<std::size_t> (panel.rows()));

        for (int row = 0; row < panel.rows(); ++row)
            CHECK (placedRows[static_cast<std::size_t> (row)] == row);

        // The design's column rule, derived rather than chosen per page.
        CHECK (panel.columnsCount() == columnsForRows (panel.rows(), 3));
        CHECK (panel.columnsCount() >= 1);
        CHECK (panel.columnsCount() <= 3);

        // And balanced: with one row added at a time to the shortest column, no column can be two rows
        // taller than another.
        std::size_t shortest = std::numeric_limits<std::size_t>::max();
        std::size_t tallest = 0;

        for (const auto& column : panel.columns)
        {
            shortest = std::min (shortest, column.size());
            tallest = std::max (tallest, column.size());
        }

        CHECK (tallest - shortest <= 1);
    }

    // The pages cover the whole table between them: the plan's own count of what is on screen.
    int rowsOnPages = 0;

    for (const auto& panel : plan.panels)
        rowsOnPages += panel.rows();

    CHECK (rowsOnPages == static_cast<int> (specs.size()));

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

//==============================================================================
// Ticket `44` — the design direction of ticket `43`, measured.
//
// Variant 3 ("Devices") is one device per page behind a strip that always shows the others; the two hard
// surfaces are the 64-row matrix and the drawn filter's pole/zero plane; the report is a bar plus a drawer
// and never a modal. These cases assert each of those, and they read the *same* data the interface shows
// — the processor's own report and document — so a page that stopped being fed would fail here.
//==============================================================================

namespace
{
model::Value integerValue (const std::int64_t value)
{
    model::Value out;
    out.type = model::Value::Type::Int;
    out.integer = value;
    return out;
}

/// A document carrying three of the reference's 64 matrix rows, one of each kind that matters: a complete
/// row, a row whose source v1 does not evaluate, and a row that names a source and no destination.
model::PresetDocument matrixDocument()
{
    model::PresetDocument document;
    document.name = "matrix fixture";
    document.schemaVersion = "4.0";

    // Slot 0 — complete: mod wheel into filter 1's cutoff, negative and bipolar, with both curves set.
    document.parameters["ModSlot0/source[0]"] = number (1.0);
    document.parameters["ModSlot0/source[1]"] = number (30.0);
    document.parameters["ModSlot0/plainParams/kParamAmount"] = number (-42.5);
    document.parameters["ModSlot0/plainParams/kParamBipolar"] = number (1.0);
    document.parameters["ModSlot0/plainParams/kParamBypass"] = number (0.0);
    document.parameters["ModSlot0/plainParams/kParamCurveIn"] = number (25.0);
    document.parameters["ModSlot0/plainParams/kParamCurveOut"] = number (-10.0);
    document.parameters["ModSlot0/destModuleTypeString"] = text ("VoiceFilter");
    document.parameters["ModSlot0/destModuleID"] = number (0.0);
    document.parameters["ModSlot0/destModuleParamID"] = number (3.0);
    document.parameters["ModSlot0/destModuleParamName"] = text ("kParamFreq");

    // Slot 7 — source `NoteOn Rand2` (22), which ticket `27` named and did not ship.
    document.parameters["ModSlot7/source[0]"] = number (22.0);
    document.parameters["ModSlot7/plainParams/kParamAmount"] = number (50.0);
    document.parameters["ModSlot7/destModuleTypeString"] = text ("VoiceFilter");
    document.parameters["ModSlot7/destModuleID"] = number (0.0);
    document.parameters["ModSlot7/destModuleParamName"] = text ("kParamFreq");

    // Slot 9 — a source and an amount and no destination: `SY - Tetris`'s six rows, in miniature.
    document.parameters["ModSlot9/source[0]"] = number (1.0);
    document.parameters["ModSlot9/plainParams/kParamAmount"] = number (12.0);

    return document;
}

/// A document carrying a **drawn** response: `objects` pole/zero objects of eight sections each, up to the
/// engine's own four objects / 32 sections.
model::PresetDocument drawnFilterDocument (const int objects)
{
    model::PresetDocument document;
    document.name = "drawn filter fixture";
    document.schemaVersion = "4.0";
    document.parameters["VoiceFilter0/plainParams/kParamType"] = text ("PZ_SVF");

    for (int object = 0; object < objects; ++object)
    {
        const auto prefix = "VoiceFilter0/PZs[" + std::to_string (object) + "]";

        for (int i = 0; i < 8; ++i)
        {
            const auto leaf = [&prefix, i] (const char* field)
            { return prefix + "/" + field + "[" + std::to_string (i) + "]"; };

            document.parameters[leaf ("fp")] = number (0.05 + 0.02 * i + 0.05 * object);
            document.parameters[leaf ("fz")] = number (0.10 + 0.03 * i + 0.05 * object);
            document.parameters[leaf ("qp")] = number (0.5);
            document.parameters[leaf ("qz")] = number (0.25);
            document.parameters[leaf ("gain")] = number (1.0);
            document.parameters[leaf ("types")] = integerValue (i % 7);
        }
    }

    return document;
}
} // namespace

//==============================================================================
TEST_CASE ("the device strip shows every device, and each page is one device's", "[interface][ui]")
{
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    const auto& plan = editor->plan();
    const auto& strip = editor->deviceStrip();

    // One device per group — the plan's pages, in the table's order — plus the matrix, which is the one
    // device the table does not derive (the bridge binds no modulation parameters).
    CHECK (editor->deviceCount() == static_cast<int> (plan.panels.size()) + 1);
    CHECK (strip.deviceCount() == editor->deviceCount());

    for (int index = 0; index < static_cast<int> (plan.panels.size()); ++index)
    {
        INFO ("device " << index << ": " << editor->deviceTitle (index));

        CHECK (editor->deviceTitle (index) == juce::String (plan.panels[static_cast<std::size_t> (index)].title));
        CHECK (editor->deviceModule (index) == juce::String (plan.panels[static_cast<std::size_t> (index)].module));
        CHECK_FALSE (editor->deviceIsMatrix (index));

        // The strip shows it, and shows where to click: a tab with a real rectangle in every device's row.
        CHECK (strip.deviceTitle (index) == editor->deviceTitle (index));
        CHECK (strip.tabBounds (index).getWidth() > 0);
        CHECK (strip.tabBounds (index).getHeight() > 0);
    }

    const auto matrixDevice = editor->deviceCount() - 1;
    CHECK (editor->deviceIsMatrix (matrixDevice));
    CHECK (editor->deviceTitle (matrixDevice) == "Matrix");
    CHECK (editor->deviceModule (matrixDevice) == "ModSlot");
    CHECK (strip.deviceTitle (matrixDevice) == "Matrix");

    // One device at a time: selecting a page shows that device's panel and nothing else's, and selecting
    // the matrix shows the matrix. A page that showed two devices would not be a device page.
    for (int index = 0; index < editor->deviceCount(); ++index)
    {
        editor->showDevice (index);

        CHECK (editor->currentDevice() == index);
        CHECK (strip.selected() == index);
        CHECK (editor->matrixPageVisible() == editor->deviceIsMatrix (index));

        for (int other = 0; other < editor->deviceCount(); ++other)
        {
            auto* panel = editor->panelForDevice (other);

            if (other == index)
            {
                if (panel != nullptr)
                    CHECK (panel->isVisible());
            }
            else if (panel != nullptr)
            {
                CHECK_FALSE (panel->isVisible());
            }
        }

        // The page shows what it says it shows: the panel's own title is the device's, and the plan's
        // balance reached it — the panel lays out the columns the plan derived, not a column count of its
        // own.
        if (auto* panel = editor->shownPanel())
        {
            CHECK (juce::String (panel->title()) == editor->deviceTitle (index));
            CHECK (panel->isVisible());

            for (const auto& page : plan.panels)
                if (page.title == panel->title())
                {
                    CHECK (panel->rows() == page.rows());
                    CHECK (panel->columnCount() == page.columnsCount());
                }
        }
    }

    // Every control the plan binds is on a page — the coverage claim as navigation rather than as a count:
    // pick any parameter, and the device page that carries it carries a control bound to it.
    for (const auto& spec : processor.bridge().specs())
    {
        auto found = false;

        for (int index = 0; index < editor->deviceCount() && ! found; ++index)
            if (auto* panel = editor->panelForDevice (index))
                found = panel->controlFor (spec.id) != nullptr;

        INFO ("parameter " << spec.id);
        CHECK (found);
    }

    // The coverage readout: the interface says how much of the table it binds, and the count is the
    // bridge's own length rather than a number written down twice.
    CHECK (editor->controlCount() == static_cast<int> (processor.bridge().specs().size()));
    CHECK (editor->schemaLeafCount() == static_cast<int> (xerum::model::Schema::instance().leafCount()));
    CHECK (editor->schemaLeafCount() > editor->controlCount());
}

//==============================================================================
TEST_CASE ("the modulation matrix shows all 64 rows with their real columns", "[interface]")
{
    // The data first, read out of a document the way the interface reads it: 64 rows whatever the preset
    // stores, because the reference writes all 64 in every preset and an empty row is a row.
    const auto document = matrixDocument();
    const auto rows = modulationRowsFromDocument (document);

    REQUIRE (rows.size() == 64);
    CHECK (kModulationSlots == 64);

    CHECK (rows[0].stored);
    CHECK (rows[7].stored);
    CHECK (rows[9].stored);
    CHECK_FALSE (rows[1].stored);

    // The columns are the row's own fields, in the engine's vocabulary.
    CHECK (ModulationMatrixView::columnCount() == 12);
    CHECK (ModulationMatrixView::columnTitle (0) == "#");
    CHECK (ModulationMatrixView::columnTitle (1) == "Source");
    CHECK (ModulationMatrixView::columnTitle (2) == "Aux source");
    CHECK (ModulationMatrixView::columnTitle (8) == "Destination");
    CHECK (ModulationMatrixView::columnTitle (10) == "Parameter");

    ModulationMatrixView view;
    view.setRows (rows);

    CHECK (view.rowCount() == 64);
    CHECK (view.storedRowCount() == 3);
    CHECK (view.row (0).stored);
    CHECK (view.row (0).slot.amount == Catch::Approx (-42.5));
    CHECK (view.row (7).slot.source == 22);
    CHECK_FALSE (view.row (1).stored);

    // Row 0, cell by cell: the source pair is **named** from the engine's own table rather than shown as
    // an index — the correction ticket `43` recorded against the prototype's conclusion.
    CHECK (view.cellText (0, 0) == "0");
    CHECK (view.cellText (0, 1) == juce::String (std::string (dsp::modSourceName (1))));
    CHECK (view.cellText (0, 1) == "Mod Wheel");
    CHECK (view.cellText (0, 2) == juce::String (std::string (dsp::modSourceName (30))));
    CHECK (view.cellText (0, 3) == "-42.5%");      // the amount, signed: the sign is the polarity
    CHECK (view.cellText (0, 4) == "bipolar");
    CHECK (view.cellText (0, 5) == "");            // `kParamBypass` 0 — not bypassed
    CHECK (view.cellText (0, 6) == "25.0%");
    CHECK (view.cellText (0, 7) == "-10.0%");
    CHECK (view.cellText (0, 8) == "VoiceFilter");
    CHECK (view.cellText (0, 9) == "0");
    CHECK (view.cellText (0, 10) == "kParamFreq");
    CHECK (view.cellText (0, 11) == "");           // a row that resolves has nothing to say

    // A row the preset does not store is a row of em-dashes, not a row of zeros: "the preset stores
    // nothing here" and "the preset stores zero" are different facts and must not look alike.
    CHECK (view.cellText (1, 0) == "1");
    CHECK (view.cellText (1, 1) != "0");
    CHECK (view.cellText (1, 11) == "");

    for (int column = 1; column < ModulationMatrixView::columnCount() - 1; ++column)
        for (const auto row : { 1, 63 })
            CHECK (view.cellText (row, column) == view.cellText (1, 1));   // one em-dash, everywhere

    CHECK (view.cellText (1, 11).isEmpty());    // and no verdict on a row that does not exist

    // The engine's own verdict, where the user is looking: a source v1 does not evaluate, and a row that
    // addresses nothing.
    const auto unimplemented = view.noteForRow (7);
    CHECK_FALSE (dsp::modSourceIsImplemented (22));
    CHECK (unimplemented.contains (juce::String (std::string (dsp::modSourceName (22)))));
    CHECK (unimplemented.contains ("not evaluated"));

    CHECK (view.noteForRow (9) == juce::String (dsp::modDestinationUnresolvedReason (rows[9].slot)));
    CHECK (view.noteForRow (9).contains ("no destination"));
    CHECK (view.noteForRow (0).isEmpty());
    CHECK (view.noteForRow (1).isEmpty());

    // The full table is laid out, and it is taller than any window: a matrix that fits on one screen is
    // not showing 64 rows with twelve columns.
    CHECK (ModulationMatrixView::preferredHeight() > 64 * ModulationMatrixView::kRowHeight);
    CHECK (ModulationMatrixView::preferredWidth() >= 1000);

    view.setSize (ModulationMatrixView::preferredWidth(), ModulationMatrixView::preferredHeight());
    CHECK (view.getHeight() == ModulationMatrixView::preferredHeight());
}

//==============================================================================
TEST_CASE ("the modulation matrix page is fed by the loaded preset", "[interface][ui]")
{
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (matrixDocument(), {}, 0);

    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    // Nothing loaded yet at construction, so the matrix is 64 empty rows rather than a shorter list.
    REQUIRE (editor->matrixView().rowCount() == 64);

    editor->showDevice (editor->deviceCount() - 1);
    CHECK (editor->matrixPageVisible());
    CHECK (editor->shownPanel() == nullptr);

    // The page shows the preset's rows: three of the 64 are stored, and the first one's source is named.
    CHECK (editor->matrixView().storedRowCount() == 3);
    CHECK (editor->matrixView().cellText (0, 1) == "Mod Wheel");
    CHECK (editor->matrixView().cellText (0, 3) == "-42.5%");
    CHECK (editor->matrixView().cellText (0, 8) == "VoiceFilter");
}

//==============================================================================
TEST_CASE ("the drawn filter shows its sections, up to the engine's 32", "[interface]")
{
    // Two objects, 16 sections: what a preset with a drawn response actually carries.
    const auto description = poleZeroFromDocument (drawnFilterDocument (2), 0);

    CHECK (description.count == 16);
    CHECK (description.count % dsp::PoleZeroDescription::kSectionsPerObject == 0);

    PoleZeroPlane plane;
    plane.setFilter (0, "PZ_SVF");
    plane.setSections (description);
    plane.setSize (420, 320);

    CHECK (plane.sectionCount() == 16);
    CHECK (PoleZeroPlane::sectionCapacity() == 32);
    CHECK (plane.caption() == "16 of 32 sections drawn");
    CHECK (plane.filterSlot() == 0);

    // The plane draws what it holds: a pole above the axis and a zero below it, at their own frequencies,
    // and a section further along the frequency axis is drawn further right.
    const auto pole0 = plane.polePoint (0);
    const auto zero0 = plane.zeroPoint (0);

    CHECK (pole0.y < zero0.y);
    CHECK (plane.polePoint (1).x > pole0.x);
    CHECK (plane.zeroPoint (1).x > zero0.x);
    CHECK (pole0.x >= 0.0f);
    CHECK (plane.polePoint (15).x <= static_cast<float> (plane.getWidth()));

    // Marker size is the section's Q, so a section with more Q is drawn larger: the third number a
    // section carries is not thrown away.
    const auto& first = description.sections[0];
    CHECK (first.poleQ > 0.0);

    // The plane holds the document's own numbers — it plots what the preset drew, not a redrawn shape.
    CHECK (plane.sections().count == description.count);
    CHECK (plane.sections().sections[1].poleFrequency == Catch::Approx (description.sections[1].poleFrequency));
    CHECK (plane.sections().sections[0].zeroFrequency
           == Catch::Approx (drawnFilterDocument (2).parameters.at ("VoiceFilter0/PZs[0]/fz[0]").real));

    // A filter the preset drew nothing for says so rather than drawing an empty plane as though it were a
    // response, and a preset that carries more than four objects is refused at the engine's own cap.
    PoleZeroPlane empty;
    empty.setFilter (1, "L12");
    CHECK (empty.sectionCount() == 0);
    CHECK (empty.caption().contains ("no pole/zero sections"));

    const auto everything = poleZeroFromDocument (drawnFilterDocument (5), 0);
    CHECK (everything.count == dsp::PoleZeroDescription::kMaxSections);
    CHECK (everything.count == 32);
}

//==============================================================================
TEST_CASE ("the interface draws xerum's own identity, and the pages draw", "[interface][ui]")
{
    // The strongest observation of "the design is on screen" that does not need a person to look: render
    // the editor into an image and count the palette's own colours in it. A page that failed to lay out —
    // a panel at zero size, a matrix that never painted, a plane with no markers — is a snapshot that is
    // all background, and the counts below would be zero.
    //
    // The same measurement was made of the running standalone; `plugin/README.md` records the capture, the
    // region it covered and the proportions found there.
    const juce::ScopedJuceInitialiser_GUI gui;

    // One document carrying both hard surfaces at once: three matrix rows and a 16-section drawn response.
    auto document = drawnFilterDocument (2);

    for (const auto& [path, value] : matrixDocument().parameters)
        document.parameters[path] = value;

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (document, {}, 0);

    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    const auto count = [] (const juce::Image& image, const juce::Colour colour, const int tolerance)
    {
        int found = 0;
        const auto r = static_cast<int> (colour.getRed());
        const auto g = static_cast<int> (colour.getGreen());
        const auto b = static_cast<int> (colour.getBlue());

        for (int y = 0; y < image.getHeight(); y += 3)
            for (int x = 0; x < image.getWidth(); x += 3)
            {
                const auto pixel = image.getPixelAt (x, y);

                if (std::abs (static_cast<int> (pixel.getRed()) - r) <= tolerance
                    && std::abs (static_cast<int> (pixel.getGreen()) - g) <= tolerance
                    && std::abs (static_cast<int> (pixel.getBlue()) - b) <= tolerance)
                    ++found;
            }

        return found;
    };

    // A device page: the window is the palette's background, the device's panel is drawn in it, and the
    // interface's own accent and the strip's fill are on screen.
    editor->showDevice (0);
    const auto pageImage = editor->createComponentSnapshot (editor->getLocalBounds());

    REQUIRE_FALSE (pageImage.isNull());
    CHECK (count (pageImage, ui::look::background, 8) > 1000);
    CHECK (count (pageImage, ui::look::panelFill, 8) > 100);
    CHECK (count (pageImage, ui::look::accent, 8) > 10);
    CHECK (count (pageImage, ui::look::barFill, 8) > 100);   // the strip and the report bar

    // The matrix: the column titles' bar, 64 rows of cells drawn, and the amount column in the accent
    // colour — the one cell the view colours specially, because an amount is the row's whole point when it
    // has one. Text is anti-aliased, so it is counted with a tolerance rather than by exact colour.
    const auto matrixDevice = editor->deviceCount() - 1;
    editor->showDevice (matrixDevice);
    CHECK (editor->matrixPageVisible());

    const auto matrixImage = editor->matrixView().createComponentSnapshot (
        editor->matrixView().getLocalBounds());
    CHECK (count (matrixImage, ui::look::barFill, 8) > 100);
    CHECK (count (matrixImage, ui::look::panelFill, 8) > 100);
    CHECK (count (matrixImage, ui::look::text, 48) > 20);
    CHECK (count (matrixImage, ui::look::dimText, 48) > 200);
    CHECK (count (matrixImage, ui::look::accent, 40) > 5);

    // **Twelve real columns**, measured positionally rather than by counting text: each column's title is
    // drawn inside that column's own x-range in the header, so a matrix whose columns collapsed into one
    // list would fail here even though it still drew the same words.
    for (int column = 0; column < ModulationMatrixView::columnCount(); ++column)
    {
        auto left = 0;

        for (int previous = 0; previous < column; ++previous)
            left += ModulationMatrixView::columnWidth (previous);

        auto title = 0;

        for (int y = 4; y < ModulationMatrixView::kHeaderHeight - 4; ++y)
            for (int x = left + 2; x < left + ModulationMatrixView::columnWidth (column) - 2; ++x)
            {
                const auto pixel = matrixImage.getPixelAt (x, y);

                if (std::abs (static_cast<int> (pixel.getRed()) - 141) <= 48
                    && std::abs (static_cast<int> (pixel.getGreen()) - 151) <= 48
                    && std::abs (static_cast<int> (pixel.getBlue()) - 166) <= 48)
                    ++title;
            }

        INFO ("column " << column << ": " << ModulationMatrixView::columnTitle (column));
        CHECK (title > 5);
    }

    // A filter page: the panel and the pole/zero plane, whose zero markers are drawn in the report's own
    // info colour — so a plane that did not paint its sections shows no such pixels.
    auto filterDevice = -1;

    for (int index = 0; index < editor->deviceCount(); ++index)
        if (editor->deviceModule (index) == "VoiceFilter0")
            filterDevice = index;

    REQUIRE (filterDevice >= 0);
    editor->showDevice (filterDevice);

    const auto filterImage = editor->createComponentSnapshot (editor->getLocalBounds());
    CHECK (editor->poleZeroPlane (0).sectionCount() == 16);
    CHECK (editor->poleZeroPlane (0).caption().startsWith ("16 of 32"));
    CHECK (count (filterImage, ui::look::severityInfo, 8) > 2);
}

//==============================================================================
TEST_CASE ("the import report is a bar plus a drawer, never a modal", "[interface][ui]")
{
    const juce::ScopedJuceInitialiser_GUI gui;

    const auto file = writeFixture (halfLoadingDocument());

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    const auto& bar = editor->reportBar();

    // The bar is the surface that is always there, and the drawer starts shut. Both are **components of
    // this window** — the measurable form of "never a modal" — and neither is a dialog in front of it.
    CHECK (editor->reportView().getTopLevelComponent() == editor.get());
    CHECK (bar.getTopLevelComponent() == editor.get());
    CHECK (bar.getParentComponent() == editor.get());
    CHECK (editor->reportView().getParentComponent() != nullptr);
    CHECK_FALSE (editor->reportDrawerOpen());

    // Before anything is loaded: the contract's words, on the bar, with the drawer shut. *This* is the
    // acceptance: a clean import says `Nothing to report`, and empty and absent do not look alike.
    CHECK (bar.summaryText() == "Nothing to report");
    CHECK (bar.clean());
    CHECK_FALSE (bar.failed());
    CHECK (editor->reportView().rowCount() == 0);
    CHECK (editor->reportView().summaryText() == "Nothing to report");

    // The drawer is the same report, expanded: opening it shows the rows, and it is this window's own
    // component rather than a modal to dismiss.
    editor->setReportDrawerOpen (true);
    CHECK (editor->reportDrawerOpen());
    editor->setReportDrawerOpen (false);
    CHECK_FALSE (editor->reportDrawerOpen());

    REQUIRE (processor.loadPresetFromFile (file).wasOk());
    processor.sendSynchronousChangeMessage();

    const auto report = processor.report();
    REQUIRE_FALSE (report.clean());

    const auto& view = editor->reportView();

    // The bar carries the summary and the worst severity; the drawer carries the rows, unchanged from
    // ticket `31` — severity, module path, item and reason, in the loader's own strings.
    CHECK (bar.summaryText() == view.summaryText());
    CHECK_FALSE (bar.clean());
    CHECK (bar.worst() == report.worst());
    CHECK (bar.presetText() == "interface fixture");   // the bar says which preset it is about
    CHECK (bar.summaryText().contains (juce::String (static_cast<int> (report.entries.size())) + " item"));
    CHECK (view.rowCount() == static_cast<int> (report.entries.size()));

    for (int i = 0; i < view.rowCount(); ++i)
    {
        const auto& entry = report.entries[static_cast<std::size_t> (i)];
        const auto row = view.rowText (i);

        INFO ("report row " << i << ": " << row);

        CHECK (row.contains (juce::String (entry.modulePath)));
        CHECK (row.contains (juce::String (entry.reason)));
    }

    // The strip locates the problem: the device whose module the report named carries the dot, and a
    // device the report said nothing about does not. That is what makes the strip a map of the import
    // rather than a second copy of it.
    const auto& strip = editor->deviceStrip();
    auto dottedDevices = 0;

    for (int index = 0; index < editor->deviceCount(); ++index)
    {
        const juce::String module (editor->deviceModule (index));
        auto expected = false;
        auto expectedWorst = xerum::model::Severity::Info;

        for (const auto& entry : report.entries)
        {
            const juce::String path (entry.modulePath);
            const auto matches = module == "ModSlot" ? path.startsWith ("ModSlot")
                                                     : (path == module || path.startsWith (module + "/"));

            if (! matches)
                continue;

            expected = true;

            if (entry.severity > expectedWorst)
                expectedWorst = entry.severity;
        }

        INFO ("device " << index << " (" << module << ")");
        CHECK (strip.hasDeviceSeverity (index) == expected);

        if (expected)
        {
            ++dottedDevices;
            CHECK (strip.deviceSeverity (index) == expectedWorst);
        }
    }

    CHECK (dottedDevices >= 1);

    // A load failure is on the bar too, in the failure's own colour — the surface says what happened
    // without anyone opening anything.
    {
        XerumAudioProcessor other;
        auto otherEditor = openEditor (other);
        REQUIRE (otherEditor != nullptr);

        juce::MemoryBlock state;
        other.getStateInformation (state);
        other.setStateInformation ("not a state block", 17);
        otherEditor->refreshReport();

        CHECK (otherEditor->reportBar().failed());
        CHECK (otherEditor->reportBar().summaryText().startsWith ("Load failed"));
        CHECK_FALSE (otherEditor->reportBar().clean());
    }
}

//==============================================================================
// Ticket `45` — the matrix and the plane are **editors**, and their edits are heard.
//
// Ticket `44` built both as views and said why: the bridge bound no modulation parameter and nothing
// reached `VoiceFilter::setPoleZero`, so a control would have written a value nothing read. These cases
// assert the path that closed that gap, and they assert it the way the acceptance demands — by a **render
// difference**, never by a parameter value.
//==============================================================================

namespace
{
/// A document whose **one** modulation row opens filter 1's cutoff from `fixed` (source 38, the constant
/// 1.0 source), with the filter otherwise shut. Amount 100 therefore opens it to 20 kHz and amount 0
/// leaves it at 20 Hz: whether the row applies is the difference between a sound and near-silence.
model::PresetDocument modulationDocument (const double amount, const int source = 38)
{
    model::PresetDocument document;
    document.name = "modulation fixture";
    document.schemaVersion = "4.0";

    document.parameters["Oscillator0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Oscillator0/plainParams/kParamVolume"] = number (1.0);
    document.parameters["VoiceFilter0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["VoiceFilter0/plainParams/kParamType"] = text ("L12");
    document.parameters["VoiceFilter0/plainParams/kParamFreq"] = number (0.0);   // 20 Hz: shut
    document.parameters["VoiceFilter0/plainParams/kParamReso"] = number (0.0);

    document.parameters["ModSlot0/source[0]"] = number (source);
    document.parameters["ModSlot0/plainParams/kParamAmount"] = number (amount);
    document.parameters["ModSlot0/plainParams/kParamBipolar"] = number (0.0);
    document.parameters["ModSlot0/plainParams/kParamBypass"] = number (0.0);
    document.parameters["ModSlot0/plainParams/kParamCurveIn"] = number (1.0);
    document.parameters["ModSlot0/plainParams/kParamCurveOut"] = number (1.0);
    document.parameters["ModSlot0/destModuleTypeString"] = text ("VoiceFilter");
    document.parameters["ModSlot0/destModuleID"] = number (0.0);
    document.parameters["ModSlot0/destModuleParamID"] = number (3.0);            // `kParamFreq`
    document.parameters["ModSlot0/destModuleParamName"] = text ("kParamFreq");

    return document;
}

void playNote (juce::MidiBuffer& events, const int block)
{
    if (block == 0)
        events.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
}

/// Render a document on a **fresh** processor, so the comparison between two variants is the variant and
/// not carried-over filter state. The same note, block size and block count every time.
std::vector<float> renderDocument (const model::PresetDocument& document)
{
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (document, {}, 0);

    return render (processor, 12, 512, playNote);
}

/// A document carrying a **drawn** response: `VoiceFilter0` is `PZ_SVF`, and one object of eight identical
/// sections — a pole at 0.2 of Nyquist, a zero at 0.5 — so the filter is a real response rather than a
/// pass-through.
model::PresetDocument poleZeroDocument()
{
    model::PresetDocument document;
    document.name = "drawn filter fixture";
    document.schemaVersion = "4.0";

    document.parameters["Oscillator0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Oscillator0/plainParams/kParamVolume"] = number (1.0);
    document.parameters["VoiceFilter0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["VoiceFilter0/plainParams/kParamType"] = text ("PZ_SVF");

    for (int i = 0; i < 8; ++i)
    {
        const std::string prefix = "VoiceFilter0/PZs[0]";
        const auto leaf = [&prefix, i] (const char* field)
        { return prefix + "/" + field + "[" + std::to_string (i) + "]"; };

        document.parameters[leaf ("fp")] = number (0.2);
        document.parameters[leaf ("fz")] = number (0.5);
        document.parameters[leaf ("qp")] = number (0.7);
        document.parameters[leaf ("qz")] = number (0.5);
        document.parameters[leaf ("gain")] = number (1.0);
        document.parameters[leaf ("types")] = integerValue (i % 7);
    }

    return document;
}
} // namespace

TEST_CASE ("the matrix's eight editable columns are bound to the bridge's own parameters",
           "[interface][ui][modulation]")
{
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    auto& matrix = editor->matrixView();
    REQUIRE (matrix.isBound());

    // Every editable column of every row is a real control, bound to the bridge's own parameter object —
    // the same claim ticket `31` makes for a page, made here for a matrix cell.
    for (int row = 0; row < ui::kModulationSlots; ++row)
        for (int column = 0; column < ui::ModulationMatrixView::columnCount(); ++column)
        {
            auto* control = matrix.controlForCell (row, column);

            if (! ui::ModulationMatrixView::columnIsEditable (column))
            {
                CHECK (control == nullptr);   // the slot, the curves and the note stay painted
                continue;
            }

            INFO ("cell " << row << "," << column);
            REQUIRE (control != nullptr);
            CHECK (control->isBound());

            const auto* spec = processor.bridge().modulationSpec (
                row, ui::ModulationMatrixView::fieldForColumn (column));
            REQUIRE (spec != nullptr);
            CHECK (&control->parameter()
                   == processor.bridge().parameters()[static_cast<std::size_t> (spec->paramIndex)]);
        }

    // The header's coverage number counts them: eight fields of 64 rows, on top of the plan's controls.
    CHECK (matrix.boundCellCount() == 8 * ui::kModulationSlots);
    CHECK (editor->boundControlCount() == editor->controlCount() + 8 * ui::kModulationSlots);

    // The palette of the two choice columns is the engine's own vocabulary, not a hand-written list: a
    // source is named through `dsp::modSourceName` and a destination class is one the engine resolves.
    const auto* sourceSpec = processor.bridge().modulationSpec (0, ModRowField::Source);
    REQUIRE (sourceSpec != nullptr);

    auto* combo = dynamic_cast<juce::ComboBox*> (
        &matrix.controlForCell (0, 1)->widget());
    REQUIRE (combo != nullptr);
    CHECK (combo->getNumItems() == 61);   // "none" plus the reference's 60

    for (int i = 1; i <= 60; ++i)
        CHECK (juce::String (combo->getItemText (i)) == juce::String (std::string (dsp::modSourceName (i))));
}

TEST_CASE ("a matrix row reaches the engine, and an edit to it is heard", "[interface][modulation]")
{
    const juce::ScopedJuceInitialiser_GUI gui;

    // A row at full depth opens the filter, so the render is loud; every way of neutralising it — zero
    // amount, no source, bypass, a zero aux source, a destination the voice does not read — makes it
    // quiet. The measurement is the **render**, never the parameter.
    const auto loud = rmsOf (renderDocument (modulationDocument (100.0)));
    REQUIRE (loud > 0.0);

    const auto quiet = [&loud] (const model::PresetDocument& document, const char* what)
    {
        const auto energy = rmsOf (renderDocument (document));
        INFO (std::string (what) + ": " + std::to_string (energy) + " against " + std::to_string (loud));
        CHECK (energy * 2.0 < loud);
    };

    quiet (modulationDocument (0.0), "amount 0");

    {
        auto document = modulationDocument (100.0);
        document.parameters["ModSlot0/source[0]"] = number (0.0);   // no source
        quiet (document, "no source");
    }

    {
        auto document = modulationDocument (100.0);
        document.parameters["ModSlot0/plainParams/kParamBypass"] = number (1.0);
        quiet (document, "bypassed");
    }

    {
        auto document = modulationDocument (100.0);
        // Mod Wheel is a source this build evaluates, and the plugin sets no wheel, so the aux source is
        // zero and the row (whose contribution the aux *scales*) contributes nothing.
        document.parameters["ModSlot0/source[1]"] = number (1.0);
        quiet (document, "aux source at zero");
    }

    {
        auto document = modulationDocument (100.0);
        document.parameters["ModSlot0/destModuleID"] = number (1.0);   // VoiceFilter1: not enabled
        quiet (document, "destination instance 1");
    }

    // The **live edit**: one processor, rendered, then a cell moved, then rendered again — which is what
    // a user does and what the acceptance calls being heard.
    {
        XerumAudioProcessor processor;
        processor.prepareToPlay (48000.0, 512);
        processor.applyDocument (modulationDocument (100.0), {}, 0);

        const auto before = render (processor, 12, 512, playNote);

        processor.setParameterValue ("ModSlot0/plainParams/kParamAmount", 0.0);
        processor.flushModulation();   // the flag the listener raised, applied now rather than by the timer
        const auto after = render (processor, 12, 512, playNote);

        CHECK (channelDifference (before, after) > 0.01);
        CHECK (rmsOf (after) * 2.0 < rmsOf (before));
    }

    // Polarity, with a source whose value is not 1: velocity. Unipolar maps it to itself, bipolar
    // re-centres it, so the two produce different cutoffs and different renders.
    {
        const auto unipolar = renderDocument (modulationDocument (80.0, /* velocity */ 16));

        auto bipolarDocument = modulationDocument (80.0, /* velocity */ 16);
        bipolarDocument.parameters["ModSlot0/plainParams/kParamBipolar"] = number (1.0);
        const auto bipolar = renderDocument (bipolarDocument);

        CHECK (channelDifference (unipolar, bipolar) > 0.005);
    }
}

TEST_CASE ("a matrix edit is a preset edit, and it survives a save", "[interface][modulation]")
{
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (modulationDocument (100.0), {}, 0);

    // The document is where the row lives, and an edit lands there — this is what "a preset edit rather
    // than a parallel state" means concretely.
    processor.setParameterValue ("ModSlot0/plainParams/kParamAmount", -25.0);
    processor.setParameterValue ("ModSlot0/plainParams/kParamBypass", 1.0);

    // A change only raises a flag; the document write happens when it is drained. In a host the timer does
    // that within 50 ms; a test asks for it.
    processor.flushModulation();

    CHECK (processor.document().parameters.at ("ModSlot0/plainParams/kParamAmount").real
           == Catch::Approx (-25.0));
    CHECK (processor.document().parameters.at ("ModSlot0/plainParams/kParamBypass").real
           == Catch::Approx (1.0));

    // A row the preset did not carry, edited, is written into the document as a complete row — every field
    // the bridge declares, so the row is a row rather than a stray key. The **destination name** is the
    // one field it does not get: the name is not a declared parameter (the reference's own name is
    // preserved rather than invented), so such a row is reported as naming no destination until a preset
    // supplies one — reported, not approximated.
    processor.setParameterValue ("ModSlot5/source[0]", 16.0);
    processor.flushModulation();
    REQUIRE (processor.document().parameters.count ("ModSlot5/source[0]") == 1);
    CHECK (processor.document().parameters.at ("ModSlot5/plainParams/kParamAmount").real == Catch::Approx (0.0));
    CHECK (processor.document().parameters.at ("ModSlot5/destModuleParamID").integer == 0);
    CHECK (processor.document().parameters.count ("ModSlot5/destModuleParamName") == 0);

    // And the save carries it: a state block written now, restored into a fresh processor, has the edit.
    juce::MemoryBlock state;
    processor.getStateInformation (state);

    XerumAudioProcessor restored;
    restored.prepareToPlay (48000.0, 512);
    restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

    CHECK (restored.parameterValue ("ModSlot0/plainParams/kParamAmount") == Catch::Approx (-25.0));
    CHECK (restored.parameterValue ("ModSlot0/plainParams/kParamBypass") == Catch::Approx (1.0));
    CHECK (restored.parameterValue ("ModSlot5/source[0]") == Catch::Approx (16.0));
    CHECK (restored.document().parameters.at ("ModSlot0/plainParams/kParamAmount").real
           == Catch::Approx (-25.0));
}

TEST_CASE ("the pole/zero plane's edits reach the engine, and a drawn response is no longer reported",
           "[interface][ui][modulation]")
{
    const juce::ScopedJuceInitialiser_GUI gui;

    // The drawn response **is applied**: a `PZ_SVF` filter with eight sections filters the oscillator, so
    // the render is not the dry saw. Measured by comparing against the same document with the filter off.
    const auto filtered = renderDocument (poleZeroDocument());

    auto bypassed = poleZeroDocument();
    bypassed.parameters["VoiceFilter0/plainParams/kParamEnable"] = number (0.0);
    const auto dry = renderDocument (bypassed);

    CHECK (rmsOf (filtered) > 0.0);
    CHECK (channelDifference (filtered, dry) > 0.01);

    // The retraction, asserted: the import report no longer names a drawn response as unapplied. It used
    // to — at `high` — and a report true before and false after is worse than no report (ticket 41).
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (poleZeroDocument(), {}, 0);

    for (const auto& entry : processor.report().entries)
        CHECK (juce::String (entry.reason).contains ("drawn in the preset") == false);

    // The plane edits through its own gesture, and the edit reaches the document and the engine. The drag
    // is synthesised exactly as the mouse delivers it: down on the pole marker, then drag right.
    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    // The plane has a real rectangle only on its own page.
    auto filterDevice = -1;

    for (int index = 0; index < editor->deviceCount(); ++index)
        if (editor->deviceModule (index) == "VoiceFilter0")
            filterDevice = index;

    REQUIRE (filterDevice >= 0);
    editor->showDevice (filterDevice);

    auto& plane = editor->poleZeroPlane (0);
    REQUIRE (plane.sectionCount() == 8);
    REQUIRE (plane.getWidth() > 0);

    const auto before = render (processor, 12, 512, playNote);
    const auto pole = plane.polePoint (0);
    REQUIRE (plane.markerAt (pole) == 0);   // section 0's pole

    const auto dragTo = juce::Point<float> (pole.x + plane.getWidth() * 0.3f, pole.y - 20.0f);
    const juce::MouseEvent down (juce::Desktop::getInstance().getMainMouseSource(), pole,
                                 juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &plane, &plane,
                                 juce::Time::getCurrentTime(), pole, juce::Time::getCurrentTime(), 1, false);
    const juce::MouseEvent drag (juce::Desktop::getInstance().getMainMouseSource(), dragTo,
                                 juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &plane, &plane,
                                 juce::Time::getCurrentTime(), pole, juce::Time::getCurrentTime(), 1, true);

    plane.mouseDown (down);
    plane.mouseDrag (drag);

    CHECK (plane.selectedSection() == 0);
    CHECK (plane.sections().sections[0].poleFrequency > 0.2);   // moved right
    CHECK (plane.sections().sections[0].poleQ > 0.7);           // dragged up

    // The document's own object moved with it — the plane is an editor, not an overlay. (`document()`
    // returns a copy, so the value is taken by value rather than by a reference into the temporary.)
    const auto moved = processor.document().parameters.at ("VoiceFilter0/PZs[0]/fp[0]");
    CHECK (moved.real == Catch::Approx (plane.sections().sections[0].poleFrequency));

    const auto after = render (processor, 12, 512, playNote);
    CHECK (channelDifference (before, after) > 0.01);
}
