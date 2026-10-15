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
#include "ModulationMatrixView.h"
#include "ParameterPanels.h"
#include "PoleZeroPlane.h"
#include "PresetBrowser.h"
#include "SectionStrip.h"
#include "XerumAudioProcessorEditor.h"

#include "factory_bank.h"
#include "modulation.h"
#include "write.h"

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
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
TEST_CASE ("the composition is the design's, and every parameter lands in one panel of one section",
           "[interface]")
{
    XerumAudioProcessor processor;
    const auto& specs = processor.bridge().specs();
    const auto plan = planInterface (specs, designedComposition());

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

        // Ticket `52`: two model slots whose drawn surfaces belong to a **panel** rather than to a page of
        // their own — the oscillator head's wavetable and the filter's response display — and both are
        // derived from the model's own module, not from a list of group names.
        // The wavetable head is the container slots' and only theirs: NOISE and SUB are `Oscillator3` and
        // `Oscillator4` in the model's numbering and are not wavetable slots, which is why the derivation
        // is a range check rather than a prefix test.
        if (panel.title == "Osc A")
        {
            CHECK (panel.wavetableSlot == 0);
            CHECK (panel.positionRow >= 0);   // the head's arrows step this row's parameter
        }

        if (panel.title == "Osc B")
            CHECK (panel.wavetableSlot == 1);

        if (panel.title == "Osc C")
            CHECK (panel.wavetableSlot == 2);

        if (panel.title == "Noise" || panel.title == "Sub Osc")
        {
            CHECK (panel.wavetableSlot == -1);
            CHECK (panel.positionRow < 0);
        }

        // The drawn response display is the voice filters' and only theirs.
        if (panel.title == "Filter 1")
            CHECK (panel.filterSlot == 0);

        if (panel.title == "Filter 2")
            CHECK (panel.filterSlot == 1);

        if (panel.title == "Global")
            CHECK (panel.filterSlot == -1);
    }

    // The pages cover the whole table between them: the plan's own count of what is on screen.
    int rowsOnPages = 0;

    for (const auto& panel : plan.panels)
        rowsOnPages += panel.rows();

    CHECK (rowsOnPages == static_cast<int> (specs.size()));

    //==========================================================================
    // Ticket `52`: the composition, which is data — and the invariant that survives it.

    // **The design's sections, in the design's order**. `docs/design/xerum-direction-warm.html`'s content
    // grid: an oscillator row, a noise-and-sub row, the amplitude envelope's own section (ticket `57`), the
    // filter section, the bottom row (the macro strip beside the group the composition does not name), and
    // the compact matrix **last** — ticket `57` moved it there from between the filters and the bottom row,
    // and the fall-through section is no longer the last one.
    std::vector<std::string> sectionIds;

    for (const auto& section : plan.sections)
        sectionIds.push_back (section.id);

    CHECK (sectionIds == std::vector<std::string> { "osc-row", "noise-sub-row", "env-section",
                                                    "filter-section", "macro-section", "default-section",
                                                    "mod-panel" });

    // **Every panel is in exactly one section**, and every panel's group has a section to ask about: a
    // group that landed twice would be drawn twice, and a group that landed nowhere would vanish from the
    // composition while a host still bound it.
    std::vector<int> placements (plan.panels.size(), 0);

    for (const auto& section : plan.sections)
        for (const auto panel : section.panels)
            ++placements[static_cast<std::size_t> (panel)];

    for (int panel = 0; panel < static_cast<int> (plan.panels.size()); ++panel)
    {
        INFO ("panel " << panel << ": " << plan.panels[static_cast<std::size_t> (panel)].title);
        CHECK (placements[static_cast<std::size_t> (panel)] == 1);
        CHECK (plan.sectionForGroup (plan.panels[static_cast<std::size_t> (panel)].title) >= 0);
    }

    // A section holds what the design puts in it — three oscillators in a row, noise and sub together, the
    // two filters side by side, the eight macros as one strip.
    const auto sectionOf = [&plan] (const std::string& group)
    {
        const auto index = plan.sectionForGroup (group);

        return index < 0 ? std::string() : plan.sections[static_cast<std::size_t> (index)].id;
    };

    CHECK (sectionOf ("Osc A") == "osc-row");
    CHECK (sectionOf ("Osc B") == "osc-row");
    CHECK (sectionOf ("Osc C") == "osc-row");
    CHECK (sectionOf ("Noise") == "noise-sub-row");
    CHECK (sectionOf ("Sub Osc") == "noise-sub-row");
    CHECK (sectionOf ("Filter 1") == "filter-section");
    CHECK (sectionOf ("Filter 2") == "filter-section");
    CHECK (sectionOf ("Macro 1") == "macro-section");
    CHECK (sectionOf ("Macro 8") == "macro-section");

    // **The fall-through, named.** The design's composition has no section for the global section, so it
    // lands in the default one — visible here rather than assumed, which is what `sectionForGroup` exists
    // for. The amplitude envelope used to fall through too; ticket `57` gives it a section of its own (and
    // `Global` used to fall through *beside* it), so this is the one group left, and it is still on the
    // page: a section that gained a home moved no other group.
    CHECK (sectionOf ("Global") == "default-section");
    CHECK (sectionOf ("Amp Envelope") == "env-section");

    // The row a section sits in is data too: the effects cards and the macro strip share the design's
    // `bottom-row`, which is how "side by side" is expressed rather than a section with two columns.
    const auto composition = designedComposition();
    const auto bandOf = [&composition] (const std::string& id)
    {
        for (const auto& spec : composition)
            if (spec.id == id)
                return spec.band;

        return std::string();
    };

    CHECK (bandOf ("fx-section") == bandOf ("macro-section"));
    CHECK (bandOf ("osc-row") != bandOf ("noise-sub-row"));

    // Ticket `57` gave the design's **flexible row** — its `1fr` — to the amplitude envelope, and ticket
    // `53` had given it to the matrix before that. **Ticket `59` removed it from the data entirely**, and
    // this is where that is stated rather than left as an absence: a row that grows when the window does is
    // a section *re-flowing*, and the interface is laid out at one size now and scaled into the window. So
    // the struct has no such field — a compile error is the assertion — and what replaces this block's
    // measurements is the layout's own: `the composition holds one ratio, and resizing it re-flows nothing`
    // requires every section's rectangle to be *equal* at every scale, which is a stronger claim than "the
    // slack goes to the envelope".
    //
    // What is still asserted here is the half that did not move: the composition is one ordered list of
    // sections, and the bands still express side-by-side.
    CHECK (bandOf ("env-section") != bandOf ("mod-panel"));
    CHECK (bandOf ("env-section") != bandOf ("osc-row"));

    // **The FX holder matches nothing in this table**: xerum's bridge exposes no effect parameters, so the
    // section resolves to no panels and is not drawn. It stays in the composition — the bottom row
    // completes itself the day the table has one — and its absence is a fact rather than a silent drop.
    for (const auto& section : plan.sections)
        CHECK (section.id != "fx-section");

    // The model type decides the control, total over the types: a new type in the bridge cannot silently
    // reach the interface without one.
    CHECK (controlKindFor (ParamType::Bool) == ControlKind::Toggle);
    CHECK (controlKindFor (ParamType::Choice) == ControlKind::Choice);
    CHECK (controlKindFor (ParamType::Int) == ControlKind::Slider);
    CHECK (controlKindFor (ParamType::Float) == ControlKind::Slider);

    // And the vocabulary decides how a choice draws: the design's option row where it fits, the picker
    // where it cannot. NOISE's four colours and SUB's six waveforms are rows; the filter's 83 types and the
    // matrix's 61 sources are pickers.
    CHECK (choiceStyleFor (4) == ChoiceStyle::Buttons);
    CHECK (choiceStyleFor (kMaxChoiceButtons) == ChoiceStyle::Buttons);
    CHECK (choiceStyleFor (kMaxChoiceButtons + 1) == ChoiceStyle::Menu);
    CHECK (choiceStyleFor (61) == ChoiceStyle::Menu);
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
                // Ticket `52`: the design draws a choice as a row of option buttons where the vocabulary is
                // short enough to be one, and as the picker where it is not. Both are bound the same way
                // (JUCE's own `ParameterAttachment` over the parameter), so what is asserted here is which
                // of the two this vocabulary got.
                if (control->choiceStyle() == ChoiceStyle::Buttons)
                    CHECK (dynamic_cast<ChoiceButtons*> (&control->widget()) != nullptr);
                else
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

        const auto& choices = processor.bridge().choices (spec.choiceSet);
        auto* control = editor->controlFor (spec.id);
        REQUIRE (control != nullptr);

        if (control->choiceStyle() == ChoiceStyle::Buttons)
        {
            // The design's option row: one pill per choice, in the bridge's own order and words, so a
            // choice the parameter has cannot be missing from the row.
            auto* buttons = dynamic_cast<ChoiceButtons*> (&control->widget());
            REQUIRE (buttons != nullptr);
            CHECK (buttons->choiceCount() == choices.size());

            for (int i = 0; i < choices.size(); ++i)
                CHECK (buttons->choiceText (i) == choices.labels[i]);

            // And a click on a pill reaches the engine, the way a click on the picker's item does.
            buttons->mouseUp (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
                                                buttons->buttonBounds (1).getCentre().toFloat(),
                                                juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, buttons,
                                                buttons, juce::Time::getCurrentTime(),
                                                buttons->buttonBounds (1).getCentre().toFloat(),
                                                juce::Time::getCurrentTime(), 1, false));
            CHECK (processor.parameterValue (spec.modelPath) == Catch::Approx (1.0));
        }
        else
        {
            auto* combo = dynamic_cast<juce::ComboBox*> (&control->widget());
            REQUIRE (combo != nullptr);
            CHECK (combo->getNumItems() == choices.size());

            for (int i = 0; i < choices.size(); ++i)
                CHECK (combo->getItemText (i) == choices.labels[i]);
        }
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

    // Severity is a colour as well as a word, and the ramp is one ramp. Ticket `51`: the ramp is the design
    // system's own three colours — `red`, `orange`, `green` — rather than the old bespoke trio, so this
    // asserts the mapping rather than a named constant that no longer exists.
    CHECK (look::severityColour (xerum::model::Severity::High) != look::severityColour (xerum::model::Severity::Info));
    CHECK (look::severityColour (xerum::model::Severity::High) == look::red);
    CHECK (look::severityColour (xerum::model::Severity::Medium) == look::orange);
    CHECK (look::severityColour (xerum::model::Severity::Info) == look::green);

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
TEST_CASE ("the strip names every section of the composition, and each section holds its own panels",
           "[interface][ui]")
{
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    const auto& plan = editor->plan();
    const auto& strip = editor->sectionStrip();

    // One tab per resolved section, in the design's order — and nothing else: since ticket `52` the matrix
    // is a **section of the composition** rather than a device page beside it.
    CHECK (editor->sectionCount() == static_cast<int> (plan.sections.size()));
    CHECK (strip.sectionCount() == editor->sectionCount());

    for (int index = 0; index < editor->sectionCount(); ++index)
    {
        INFO ("section " << index << ": " << editor->sectionId (index).toStdString());

        CHECK (editor->sectionTitle (index)
               == juce::String (plan.sections[static_cast<std::size_t> (index)].title));
        CHECK (strip.sectionTitle (index) == editor->sectionTitle (index));

        // The strip shows where to click: a tab with a real rectangle.
        CHECK (strip.tabBounds (index).getWidth() > 0);
        CHECK (strip.tabBounds (index).getHeight() > 0);

        // Every section is on the one composed page, with a real rectangle, and they read in the
        // composition's order: a section either starts a new band below the previous one, or shares the
        // previous section's band (the design's `bottom-row`, ticket `53`) to its right.
        const auto bounds = editor->composition().sectionBounds (index);

        CHECK (bounds.getWidth() > 0);
        CHECK (bounds.getHeight() > 0);

        if (index > 0)
        {
            const auto previous = editor->composition().sectionBounds (index - 1);
            const auto below = bounds.getY() >= previous.getBottom();
            const auto beside = bounds.getY() == previous.getY() && bounds.getX() >= previous.getRight();

            INFO ("previous: " << previous.toString().toStdString() << ", this: "
                               << bounds.toString().toStdString());
            CHECK ((below || beside));
        }
    }

    // The sections are the design's, by id — and since ticket `57` in the design's new order:
    // `osc-row` is three oscillators in a row, `noise-sub-row` is noise and sub beneath them,
    // `env-section` is the amplitude envelope's own band at the design's largest knob size,
    // `filter-section` is the two filters, `macro-section` is the bottom row's strip, and `mod-panel` —
    // the one section the table does not derive — is **last**.
    CHECK (editor->sectionId (0) == "osc-row");
    CHECK (editor->sectionId (1) == "noise-sub-row");
    CHECK (editor->sectionId (2) == "env-section");
    CHECK (editor->sectionId (3) == "filter-section");
    CHECK (editor->sectionId (4) == "macro-section");
    CHECK (editor->sectionId (5) == "default-section");
    CHECK (editor->sectionId (6) == "mod-panel");

    CHECK (editor->sectionIsMatrix (6));
    CHECK_FALSE (editor->sectionIsMatrix (0));
    CHECK_FALSE (editor->sectionIsMatrix (2));
    CHECK_FALSE (editor->sectionIsMatrix (3));

    // A section's modules are its panels' — what the strip's severity dot is keyed on — and the matrix's
    // section is `ModSlot`, the module a report names a row by. The amplitude envelope's module is `Env0`
    // (the bridge's own module for it), which is what makes "the envelope's section" a question the strip
    // can answer rather than a name this test wrote down twice.
    CHECK (editor->sectionModules (0)
           == std::vector<juce::String> { "Oscillator0", "Oscillator1", "Oscillator2" });
    CHECK (editor->sectionForModule ("Env0") == 2);
    CHECK (editor->sectionForModule ("VoiceFilter0") == 3);
    CHECK (editor->sectionForModule ("VoiceFilter1") == 3);
    CHECK (editor->sectionForModule ("ModSlot") == 6);
    CHECK (editor->sectionForModule ("NoSuchModule") < 0);

    // A tab locates its section on the one page. **It no longer scrolls** (ticket `59`): the page is laid
    // out at exactly the size of the space it is given and the whole canvas is scaled into the window, so
    // the instrument is on screen whole at every size and there is nothing off-screen to scroll to. What a
    // click still does — and all it ever was, at a window the page fitted — is say where you are.
    //
    // Ticket `53`'s version of this block asserted the *other* case: at 900×600 there was more instrument
    // than window, so a tab scrolled it. That window no longer exists — a fixed ratio has no 900×600 in it —
    // and the size it had has no scroll in it either, which is what the fixed ratio buys. The page's fit is
    // measured at every size in `the composition holds one ratio, and resizing re-flows nothing`.
    {
        const auto last = editor->sectionCount() - 1;

        editor->selectSection (last);
        CHECK (editor->currentSection() == last);
        CHECK (strip.selected() == last);

        editor->selectSection (0);
        CHECK (editor->currentSection() == 0);
        CHECK (strip.selected() == 0);
    }

    // Every group the table has is a panel of the composition, and every panel is one group's — the
    // coverage claim as structure rather than as a count.
    for (const auto& panelPlan : plan.panels)
    {
        INFO ("panel " << panelPlan.title);

        auto* panel = editor->panelForGroup (panelPlan.title);
        REQUIRE (panel != nullptr);
        CHECK (panel->rows() == panelPlan.rows());
        CHECK (juce::String (panel->title()) == juce::String (panelPlan.title));
        CHECK (panel->getWidth() > 0);
        CHECK (panel->getHeight() > 0);

        for (const auto* spec : panelPlan.specs)
            CHECK (panel->controlFor (spec->id) != nullptr);
    }

    CHECK (editor->panelForGroup ("No Such Group") == nullptr);

    // Every control the plan binds is reachable by id — the coverage claim as navigation.
    for (const auto& spec : processor.bridge().specs())
    {
        INFO ("parameter " << spec.id);
        CHECK (editor->controlFor (spec.id) != nullptr);
    }

    // The coverage readout: the interface says how much of the table it binds, and the count is the
    // bridge's own length rather than a number written down twice.
    CHECK (editor->controlCount() == static_cast<int> (processor.bridge().specs().size()));
    CHECK (editor->schemaLeafCount() == static_cast<int> (xerum::model::Schema::instance().leafCount()));
    CHECK (editor->schemaLeafCount() > editor->controlCount());

    // The matrix is a section of the composition and its cells are bound — the 512 the header's readout adds
    // to the panels' controls.
    CHECK (editor->matrixView().isBound());
    CHECK (editor->boundControlCount() == editor->controlCount() + editor->matrixView().boundCellCount());
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
TEST_CASE ("the matrix section is fed by the loaded preset", "[interface][ui]")
{
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (matrixDocument(), {}, 0);

    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    // Nothing loaded yet at construction, so the matrix is 64 empty rows rather than a shorter list.
    REQUIRE (editor->matrixView().rowCount() == 64);

    // The matrix is a **section of the composition** since ticket `52`, not a page beside it, and it is on
    // the one page with the rest of the design's arrangement.
    const auto matrixSection = editor->sectionForModule ("ModSlot");
    REQUIRE (matrixSection >= 0);
    CHECK (editor->sectionIsMatrix (matrixSection));
    CHECK (editor->sectionId (matrixSection) == "mod-panel");
    CHECK (editor->matrixView().isVisible());
    CHECK (editor->matrixView().getWidth() > 0);
    CHECK (editor->matrixView().getHeight() > 0);

    // The grid itself is all 64 rows — the design's section shows eight of them and scrolls (asserted as
    // geometry in `the design's sections are in the design's positions`), so the view is taller than the
    // section that shows it and nothing is hidden from the user.
    CHECK (editor->matrixView().getHeight() == ui::ModulationMatrixView::preferredHeight());
    CHECK (editor->matrixView().getHeight() > editor->composition().sectionBounds (matrixSection).getHeight());

    // The section shows the preset's rows: three of the 64 are stored, and the first one's source is named.
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

    // A device page: the window is the palette's `bg`, the device's panel is `surface` drawn in it, the
    // header's gradient is `surface2` and the interface's own accent is on screen. Ticket `51` renamed
    // every one of these to the design system's role — `background`/`panelFill`/`barFill` no longer exist,
    // and the colours are the design's own.
    editor->selectSection (0);
    const auto pageImage = editor->createComponentSnapshot (editor->getLocalBounds());

    REQUIRE_FALSE (pageImage.isNull());
    CHECK (count (pageImage, ui::look::bg, 8) > 1000);
    CHECK (count (pageImage, ui::look::surface, 8) > 100);    // the panel, the strip and the report bar
    CHECK (count (pageImage, ui::look::surface2, 8) > 20);    // the header gradient, the knob bodies
    CHECK (count (pageImage, ui::look::accent, 8) > 10);      // the wordmark, the value arcs, the module tag

    // The matrix: the column titles' bar, 64 rows of cells drawn, and the amount column in the accent
    // colour — the one cell the view colours specially, because an amount is the row's whole point when it
    // has one. Text is anti-aliased, so it is counted with a tolerance rather than by exact colour.
    editor->selectSection (editor->sectionForModule ("ModSlot"));
    CHECK (editor->sectionIsMatrix (editor->currentSection()));

    const auto matrixImage = editor->matrixView().createComponentSnapshot (
        editor->matrixView().getLocalBounds());
    CHECK (count (matrixImage, ui::look::bg, 8) > 100);        // the un-stored rows, on the window
    CHECK (count (matrixImage, ui::look::surface, 8) > 100);   // the header bar and the stored rows
    CHECK (count (matrixImage, ui::look::fg, 48) > 20);        // the stored rows' own cells
    CHECK (count (matrixImage, ui::look::fgDim, 48) > 200);    // the notes and the em-dashed columns
    CHECK (count (matrixImage, ui::look::accent, 40) > 5);     // the source column

    // **Twelve real columns**, measured positionally rather than by counting text: each column's title is
    // drawn inside that column's own x-range in the header, so a matrix whose columns collapsed into one
    // list would fail here even though it still drew the same words.
    for (int column = 0; column < ModulationMatrixView::columnCount(); ++column)
    {
        auto left = 0;

        for (int previous = 0; previous < column; ++previous)
            left += ModulationMatrixView::columnWidth (previous);

        auto title = 0;
        const auto head = ui::look::fgMuted;   // ticket `51`: the table head is the design's muted role

        for (int y = 4; y < ModulationMatrixView::kHeaderHeight - 4; ++y)
            for (int x = left + 2; x < left + ModulationMatrixView::columnWidth (column) - 2; ++x)
            {
                const auto pixel = matrixImage.getPixelAt (x, y);

                if (std::abs (static_cast<int> (pixel.getRed()) - static_cast<int> (head.getRed())) <= 48
                    && std::abs (static_cast<int> (pixel.getGreen()) - static_cast<int> (head.getGreen())) <= 48
                    && std::abs (static_cast<int> (pixel.getBlue()) - static_cast<int> (head.getBlue())) <= 48)
                    ++title;
            }

        INFO ("column " << column << ": " << ModulationMatrixView::columnTitle (column));
        CHECK (title > 5);
    }

    // The filter section: the panels and the pole/zero plane, which sits **inside** the filter's own panel
    // beside its knobs since ticket `52` (the design's `filter-viz`) rather than on a page of its own.
    // Ticket `51` changed how the plane is drawn — a pole in `accent`, a zero in `accentDim` (the old zero
    // colour was the report's info blue, which the design system does not have) — so the measurement is
    // taken on the **plane's own snapshot** rather than on the page: a plane that painted no sections shows
    // neither colour, which is what the old count of info-blue pixels was standing in for.
    const auto filterSection = editor->sectionForModule ("VoiceFilter0");

    REQUIRE (filterSection >= 0);
    CHECK (editor->sectionId (filterSection) == "filter-section");
    CHECK (editor->sectionForModule ("VoiceFilter1") == filterSection);
    editor->selectSection (filterSection);

    const auto filterImage = editor->createComponentSnapshot (editor->getLocalBounds());
    CHECK (count (filterImage, ui::look::surface, 8) > 100);   // the panels and the plane's own frame
    CHECK (editor->poleZeroPlane (0).sectionCount() == 16);
    CHECK (editor->poleZeroPlane (0).caption().startsWith ("16 of 32"));

    auto& plane = editor->poleZeroPlane (0);
    const auto planeImage = plane.createComponentSnapshot (plane.getLocalBounds());

    REQUIRE (plane.getWidth() > 0);
    CHECK (count (planeImage, ui::look::accent, 8) > 4);      // the poles' crosses
    CHECK (count (planeImage, ui::look::accentDim, 8) > 4);   // the zeros' circles
}

//==============================================================================
TEST_CASE ("a slider is a drawn knob, and its second arc is the preset's own modulation",
           "[interface][ui]")
{
    // Ticket `51`. Three claims, each measured rather than asserted about a paint routine:
    //
    //   1. a slider is a `ui::Knob` — still a `juce::Slider`, so the attachment binds it — at the design's
    //      exact diameter, chosen from the plan (`knobSizeFor`);
    //   2. the arc runs 225° to 315° (the design's 7 o'clock to 5 o'clock), read back through the same
    //      geometry the paint routine uses, and the knob actually paints `accent` and `border` pixels;
    //   3. the **modulation arc** is the loaded preset's: the matrix rows that address a parameter set its
    //      depth, and a parameter no row addresses draws no second arc.
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    // Two rows onto filter 1's cutoff — slot 0 at -42.5 and slot 7 at +50 — so the summed depth is
    // -0.425 + 0.5 = 0.075, and a third row with no destination (slot 9) contributes nothing.
    processor.applyDocument (matrixDocument(), {}, 0);

    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    // Ticket `52` changed what "on screen" means for a measurement like this one: the composition is **one
    // page**, laid out whole, so every panel — including the ones below the fold of a small window — has its
    // real rectangle at all times. Ticket `59` removed the scrolling as well as the fold: the whole page is
    // scaled into the window, so "which section the user is looking at" is a strip selection rather than a
    // scroll position, and the rectangles below are the ones on screen at every size.
    const auto selectModule = [&editor] (const juce::String& module)
    {
        const auto section = editor->sectionForModule (module);

        if (section >= 0)
            editor->selectSection (section);

        return section >= 0;
    };

    REQUIRE (selectModule ("Oscillator0"));

    auto* volume = editor->controlFor ("oscillator0.volume");
    REQUIRE (volume != nullptr);
    auto* knob = volume->knob();
    REQUIRE (knob != nullptr);

    // Still a `juce::Slider`, which is what the binding and the harness require, and now a knob.
    CHECK (dynamic_cast<juce::Slider*> (&volume->widget()) != nullptr);
    CHECK (knob->size() == knobSizeFor (volume->spec()));
    CHECK (knob->size() == KnobSize::Default);
    CHECK (knob->mode() == Knob::Mode::Dial);

    // The design's diameter, on screen rather than in a constant: 44 px for a default knob, 28 for the
    // fixed slots the design puts in its compact band.
    CHECK (knob->getWidth() == ui::look::knobDiameter);
    CHECK (knob->getHeight() == ui::look::knobDiameter);

    auto* noiseLevel = editor->controlFor ("oscillator3.volume");
    REQUIRE (noiseLevel != nullptr);
    REQUIRE (noiseLevel->knob() != nullptr);
    CHECK (noiseLevel->knob()->size() == KnobSize::Small);

    REQUIRE (selectModule ("Oscillator3"));
    CHECK (noiseLevel->knob()->getWidth() == ui::look::knobSmallDiameter);

    REQUIRE (selectModule ("Oscillator0"));   // back to the section the knob snapshot is taken on

    // The arc, through the geometry the painter uses: both ends are below the centre — 7 o'clock and 5
    // o'clock — the start is left of the middle and the end is right of it, and the middle of the sweep is
    // at the top (225° + 135° = 360° = 12 o'clock).
    const auto start = knob->pointAtProportion (0.0);
    const auto middle = knob->pointAtProportion (0.5);
    const auto end = knob->pointAtProportion (1.0);

    CHECK (start.y > middle.y);
    CHECK (end.y > middle.y);
    CHECK (start.x < middle.x);
    CHECK (end.x > middle.x);
    CHECK (std::abs (middle.x - static_cast<float> (knob->getWidth()) * 0.5f) < 1.5f);

    // And it paints: the track in `border` and the value arc in `accent`, counted on the knob's own
    // snapshot so nothing else on the page can supply the pixels.
    const auto count = [] (const juce::Image& image, const juce::Colour colour, const int tolerance)
    {
        int found = 0;

        for (int y = 0; y < image.getHeight(); ++y)
            for (int x = 0; x < image.getWidth(); ++x)
            {
                const auto pixel = image.getPixelAt (x, y);

                if (std::abs (static_cast<int> (pixel.getRed()) - static_cast<int> (colour.getRed())) <= tolerance
                    && std::abs (static_cast<int> (pixel.getGreen()) - static_cast<int> (colour.getGreen())) <= tolerance
                    && std::abs (static_cast<int> (pixel.getBlue()) - static_cast<int> (colour.getBlue())) <= tolerance)
                    ++found;
            }

        return found;
    };

    const auto knobImage = knob->createComponentSnapshot (knob->getLocalBounds());
    CHECK (count (knobImage, ui::look::border, 6) > 20);    // the track's whole sweep
    CHECK (count (knobImage, ui::look::accent, 6) > 5);     // the value arc
    CHECK (count (knobImage, ui::look::surface2, 6) > 100); // the body

    // The second arc is the preset's, not decoration.
    auto* cutoff = editor->controlFor ("voicefilter0.freq");
    REQUIRE (cutoff != nullptr);
    REQUIRE (cutoff->knob() != nullptr);

    CHECK (cutoff->knob()->isModulated());
    CHECK (cutoff->knob()->modulationDepth() == Catch::Approx (0.075));

    // A parameter no matrix row addresses: no arc, and zero depth — the state the design calls "a control
    // with no modulated state is not finished" and this build answers with a fact rather than a stub.
    CHECK_FALSE (volume->knob()->isModulated());
    CHECK (volume->knob()->modulationDepth() == Catch::Approx (0.0));

    // With no preset loaded there is no modulation at all, so every knob is unmodulated.
    {
        XerumAudioProcessor fresh;
        fresh.prepareToPlay (48000.0, 512);

        auto otherEditor = openEditor (fresh);
        REQUIRE (otherEditor != nullptr);

        CHECK_FALSE (otherEditor->controlFor ("voicefilter0.freq")->knob()->isModulated());
    }
}

//==============================================================================
namespace
{
/// The evidence table ticket `53`'s acceptance is read from — extended by ticket `59` with the two numbers
/// the fixed ratio is made of. Every section's rectangle **in the page's own coordinates** (which is the
/// same at every window, and is the whole point), its rectangle **on screen** (the same rectangle times the
/// scale), and every control's rectangle in the order the plan holds them. Printed rather than only
/// asserted, the way the fidelity suite prints its measurements, because "it fits" and "it does not move"
/// are numbers and the reader should be able to check them.
void printComposition (const char* what, XerumAudioProcessorEditor& editor)
{
    const auto nominal = editor.nominalSize();
    const auto page = editor.pageBoundsOnScreen();

    std::fprintf (stderr,
                  "\n[interface] %s: window %dx%d (aspect %.4f), nominal %dx%d (aspect %.4f), scale %.4f, "
                  "page %d tall; on screen the page is %.0fx%.0f at %.0f,%.0f\n",
                  what, editor.getWidth(), editor.getHeight(),
                  static_cast<double> (editor.getWidth()) / static_cast<double> (editor.getHeight()),
                  nominal.x, nominal.y, editor.aspectRatio(), editor.interfaceScale(),
                  editor.compositionHeight(), page.getWidth(), page.getHeight(), page.getX(), page.getY());

    for (int index = 0; index < editor.sectionCount(); ++index)
    {
        const auto bounds = editor.composition().sectionBounds (index);
        const auto onScreen = editor.sectionBoundsOnScreen (index);
        const auto* plan = editor.sectionPlan (index);

        std::fprintf (stderr, "[interface]   section %-16s %4dx%4d @%4d,%-4d panels=%d | on screen %6.1fx%6.1f @%6.1f,%-6.1f\n",
                      editor.sectionId (index).toRawUTF8(), bounds.getWidth(), bounds.getHeight(),
                      bounds.getX(), bounds.getY(),
                      plan == nullptr ? 0 : static_cast<int> (plan->panels.size()),
                      onScreen.getWidth(), onScreen.getHeight(), onScreen.getX(), onScreen.getY());

        if (plan == nullptr)
            continue;

        if (editor.sectionIsMatrix (index))
        {
            std::fprintf (stderr, "[interface]     %d of %d rows shown, %d tall\n",
                          editor.composition().matrixVisibleRows(), ui::kModulationSlots,
                          editor.matrixView().getHeight());
            continue;
        }

        for (const auto panelIndex : plan->panels)
        {
            const auto& panelPlan = editor.plan().panels[static_cast<std::size_t> (panelIndex)];
            auto* panel = editor.panelForGroup (panelPlan.title);

            if (panel == nullptr)
                continue;

            const auto panelBounds = panel->getBounds();

            std::fprintf (stderr, "[interface]     panel %-16s %4dx%4d @%4d,%-4d rows=%d\n",
                          panelPlan.title.c_str(), panelBounds.getWidth(), panelBounds.getHeight(),
                          panelBounds.getX(), panelBounds.getY(), panel->rows());

            for (int row = 0; row < panel->rows(); ++row)
            {
                auto* control = panel->controlAtRow (row);
                const auto rect = control->getBounds();

                std::fprintf (stderr, "[interface]       %-32s %4dx%4d @%4d,%-4d\n",
                              control->label().toRawUTF8(), rect.getWidth(), rect.getHeight(),
                              rect.getX(), rect.getY());
            }
        }
    }
}
/// The first factory preset this machine has, or a file that does not exist. The reference's bank is not
/// part of this repository and the suite has to pass without it, so an absent bank is a **stated skip**
/// rather than a failure — the rule the fidelity suite follows for the whole corpus.
juce::File firstFactoryPreset (XerumAudioProcessor& processor)
{
    for (const auto& root : processor.contentRoots())
    {
        juce::Array<juce::File> found;

        if (juce::File (root).findChildFiles (found, juce::File::findFiles, true, "*.SerumPreset") <= 0)
            continue;

        std::sort (found.begin(), found.end(), [] (const juce::File& a, const juce::File& b)
                   { return a.getFullPathName() < b.getFullPathName(); });

        return found.getFirst();
    }

    return {};
}
} // namespace


//==============================================================================
namespace
{
/// The colour ids a stock widget of this type asks its look-and-feel for when it draws — the list is the
/// point of the test below, because an id that is not here is one nothing on screen consults.
std::vector<int> colourIdsOf (const juce::Component& component)
{
    if (dynamic_cast<const juce::ComboBox*> (&component) != nullptr)
        return { juce::ComboBox::backgroundColourId, juce::ComboBox::textColourId,
                 juce::ComboBox::outlineColourId, juce::ComboBox::buttonColourId,
                 juce::ComboBox::arrowColourId, juce::ComboBox::focusedOutlineColourId };

    if (dynamic_cast<const juce::Slider*> (&component) != nullptr)
        return { juce::Slider::backgroundColourId, juce::Slider::thumbColourId, juce::Slider::trackColourId,
                 juce::Slider::rotarySliderFillColourId, juce::Slider::rotarySliderOutlineColourId,
                 juce::Slider::textBoxTextColourId, juce::Slider::textBoxBackgroundColourId,
                 juce::Slider::textBoxOutlineColourId, juce::Slider::textBoxHighlightColourId };

    if (dynamic_cast<const juce::ToggleButton*> (&component) != nullptr)
        return { juce::ToggleButton::textColourId, juce::ToggleButton::tickColourId,
                 juce::ToggleButton::tickDisabledColourId };

    if (dynamic_cast<const juce::TextButton*> (&component) != nullptr)
        return { juce::TextButton::buttonColourId, juce::TextButton::buttonOnColourId,
                 juce::TextButton::textColourOffId, juce::TextButton::textColourOnId };

    if (dynamic_cast<const juce::Label*> (&component) != nullptr)
        return { juce::Label::textColourId, juce::Label::backgroundColourId, juce::Label::outlineColourId };

    if (dynamic_cast<const juce::ScrollBar*> (&component) != nullptr)
        return { juce::ScrollBar::backgroundColourId, juce::ScrollBar::thumbColourId,
                 juce::ScrollBar::trackColourId };

    return {};
}

/// Every component of a tree, the editor included — what a drawn surface can be.
void collectComponents (juce::Component& component, std::vector<juce::Component*>& out)
{
    out.push_back (&component);

    for (int i = 0; i < component.getNumChildComponents(); ++i)
        collectComponents (*component.getChildComponent (i), out);
}
} // namespace

TEST_CASE ("every colour a drawn surface resolves is a token", "[interface][ui]")
{
    // Ticket `53`'s correction. The window capture found JUCE's own dark-scheme `defaultFill` (#42a2c8) on
    // screen — 0.416 % of the window, the matrix viewport's scroll bars — because the interface had never
    // given its remaining stock widgets a look-and-feel of its own. `ui::look::XerumLookAndFeel` is that
    // look-and-feel; this case is the measurement of "no non-token colour can reappear in a drawn surface",
    // id by id, over the editor's **own** component tree: every component of every stock class it contains
    // is asked for the colour ids its class draws with, and each answer has to be one of
    // `ui::look::palette()` or a colour that draws nothing.
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (matrixDocument(), {}, 0);

    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    // The editor's own look-and-feel is the design system's, and it is the one the tree inherits.
    CHECK (dynamic_cast<ui::look::XerumLookAndFeel*> (&editor->getLookAndFeel()) != nullptr);

    std::vector<juce::Component*> components;
    collectComponents (*editor, components);

    const auto palette = ui::look::palette();
    const auto isPaletteColour = [&palette] (const juce::Colour colour)
    {
        if (colour.getAlpha() == 0)
            return true;   // draws nothing, which is not a colour at all

        return std::find (palette.begin(), palette.end(), colour) != palette.end();
    };

    auto checked = 0;
    auto nonPalette = 0;

    for (auto* component : components)
    {
        for (const auto id : colourIdsOf (*component))
        {
            // `inheritFromParent` false: this is the colour the look-and-feel would hand the widget, which
            // is what it paints with.
            const auto colour = component->findColour (id, false);
            ++checked;

            if (isPaletteColour (colour))
                continue;

            ++nonPalette;

            INFO ("component " << juce::String (typeid (*component).name()) << ", colour id " << id << " -> "
                               << colour.toDisplayString (false).toStdString());

            CHECK (colour.getAlpha() == 0);
        }
    }

    // The walk has to have seen something: the tree is the composed page, and a colourless tree would make
    // every assertion above vacuous.
    std::fprintf (stderr, "[interface] %d colour ids resolved over %d components; %d outside the palette\n",
                  checked, static_cast<int> (components.size()), nonPalette);
    CHECK (checked > 40);
    CHECK (nonPalette == 0);

    // And the blue itself: no id this interface can reach resolves to it any more.
    CHECK (juce::Colour (0xff42a2c8).getARGB() != ui::look::XerumLookAndFeel().findColour (juce::ScrollBar::thumbColourId).getARGB());
    CHECK (ui::look::XerumLookAndFeel().findColour (juce::ScrollBar::thumbColourId) == ui::look::borderHi);

    // **And it is not drawn either.** The ids above are what a widget would ask for; this is what the
    // editor's own render contains — JUCE's paint path at the size the editor **opened** at, the same
    // measurement ticket `52` made. The scroll bars are inside this image (the matrix's grid is taller and
    // wider than its viewport), so the colour that used to be here would be here.
    //
    // The size is not written down here: ticket `53` derives the default height from the composed page, so
    // pinning a number would measure a window the plugin no longer opens at — which is exactly what ticket
    // `57` did to the old hardcoded 1080×1108. The render's size is printed with the shares below.
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
    std::map<juce::uint32, int> counts;
    auto blue = 0;

    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
        {
            const auto pixel = image.getPixelAt (x, y);
            const auto key = pixel.getARGB() & 0x00ffffffu;
            ++counts[key];

            if (std::abs (static_cast<int> (pixel.getRed()) - 0x42) <= 2
                && std::abs (static_cast<int> (pixel.getGreen()) - 0xa2) <= 2
                && std::abs (static_cast<int> (pixel.getBlue()) - 0xc8) <= 2)
                ++blue;
        }

    std::vector<std::pair<juce::uint32, int>> ranked (counts.begin(), counts.end());
    std::sort (ranked.begin(), ranked.end(),
               [] (const auto& a, const auto& b) { return a.second > b.second; });

    const auto total = static_cast<double> (image.getWidth()) * static_cast<double> (image.getHeight());
    std::fprintf (stderr, "[interface] the editor's own render, %dx%d:\n", image.getWidth(), image.getHeight());

    for (const auto& [key, count] : ranked)
    {
        if (count < 400)
            break;

        std::fprintf (stderr, "[interface]   rgb(%3u,%3u,%3u)  %6.3f %%  %s\n", (key >> 16) & 0xffu,
                      (key >> 8) & 0xffu, key & 0xffu, 100.0 * count / total,
                      count == static_cast<int> (juce::Colour (0xff42a2c8).getARGB() & 0x00ffffffu)
                          ? "JUCE's dark-scheme fill blue" : "");
    }

    std::fprintf (stderr, "[interface]   #42a2c8 anywhere in the render: %d px\n", blue);
    CHECK (blue == 0);
}

TEST_CASE ("the composed page fits every window it is given, and stays legible at the minimum",
           "[interface][ui]")
{
    // Ticket `53`'s acceptance, re-scoped by ticket `59` from *the window it opens at* to *every window it
    // is given*:
    //
    //   1. **the whole instrument is visible at once, at every size.** Ticket `53` measured the page's
    //      height against a viewport's and required that no scroll bar be shown. There is no scroll bar to
    //      require any more — the page is not inside a viewport at all, and it is laid out at exactly the
    //      size of the space it is given — so the fit is asserted where it can be *seen*: every section's
    //      rectangle **on screen** is inside the editor's own, at the nominal size, at 1.25×, at the
    //      minimum, and at a size a host has forced off the ratio (where the canvas is letterboxed);
    //   2. **and it stays legible at the minimum.** Ticket `51`'s assertion is kept and extended rather than
    //      replaced: at the minimum scale every control of every panel is non-empty and inside its panel,
    //      every panel inside its section, every section inside the composed page — a layout that clipped or
    //      dropped a control fails here, at the size that clips soonest. What that scale does to the
    //      *type* is printed rather than asserted, because "legible" is the dev's call and "fits" is not a
    //      claim a type size makes.
    //
    // Ticket `52`'s version of this case asserted the same chain at two fixed sizes and said nothing about
    // whether the page fitted either of them, which is the defect ticket `53` is about. Ticket `59`'s
    // version keeps every one of those assertions and makes the *sizes* scales of one nominal size rather
    // than two windows that happen to be supported — 900×600 is not a size this editor can be, because a
    // ratio has no 900×600 in it.
    //
    // This session has no eyes and the standalone could not be resized by hand — driving another app's
    // window needs Assistive access this machine does not grant `osascript` — so the measurement is made
    // where it can be, on the editor itself, and printed.
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    const auto compositionBounds = [&editor] { return editor->composition().getLocalBounds(); };
    const auto editorBounds = [&editor] { return editor->getLocalBounds(); };

    const auto nominal = juce::Point<int> (editor->getWidth(), editor->getHeight());

    // The sizes this case measures, all derived from the editor's own nominal size and its own resize
    // limits rather than written down here: the nominal one, 1.25×, the **minimum the editor allows**, and
    // a window a host has forced off the ratio (wider than the composition's, and short).
    const auto* constrainer = editor->getConstrainer();

    REQUIRE (constrainer != nullptr);

    const auto minimum = juce::Point<int> (constrainer->getMinimumWidth(), constrainer->getMinimumHeight());
    const auto maximum = juce::Point<int> (constrainer->getMaximumWidth(), constrainer->getMaximumHeight());

    std::fprintf (stderr,
                  "\n[interface] the limits the editor's own resize is held to: %dx%d … %dx%d, aspect %.6f "
                  "(the editor's is %.6f)\n",
                  minimum.x, minimum.y, maximum.x, maximum.y, constrainer->getFixedAspectRatio(),
                  editor->aspectRatio());

    // The floor is the **design's own baseline width** (ticket `51`'s 900), and the height that goes with
    // it is the ratio's rather than a second decision.
    CHECK (minimum.x == 900);
    CHECK (minimum.y == juce::roundToInt (static_cast<double> (minimum.x) * nominal.y / nominal.x));
    CHECK (constrainer->getFixedAspectRatio() == Catch::Approx (editor->aspectRatio()));

    // **1. The window it opens at**, which is the nominal size at scale 1.
    {
        printComposition ("nominal window", *editor);

        CHECK (nominal == editor->nominalSize());
        CHECK (editor->interfaceScale() == Catch::Approx (1.0f));
        CHECK (editor->interfaceOrigin() == juce::Point<int> (0, 0));
        CHECK (editor->getWidth() == 1080);
        CHECK (editor->aspectRatio()
               == Catch::Approx (static_cast<double> (nominal.x) / static_cast<double> (nominal.y)));

        // The page is exactly the space it was laid out for, which is how its height *is* the nominal
        // height less the chrome rather than a number that has to fit.
        CHECK (editor->compositionHeight() == editor->compositionArea().getHeight());
        CHECK (editor->sectionCount() > 0);

        for (int index = 0; index < editor->sectionCount(); ++index)
        {
            const auto bounds = editor->composition().sectionBounds (index);

            INFO ("nominal window, section " << index << ": " << editor->sectionId (index).toStdString());

            CHECK (bounds.getWidth() > 0);
            CHECK (bounds.getHeight() > 0);
            CHECK (compositionBounds().contains (bounds));
        }
    }

    // **2. Every size it can be given.** A size at the ratio is filled exactly by the scaled canvas; a size
    // off it is letterboxed. Both are inside the window, and at each size `51`/`53`'s control chain is
    // walked again: every panel inside its section, every control non-empty and inside its panel.
    for (const auto size : { nominal,
                             juce::Point<int> (juce::roundToInt (static_cast<float> (nominal.x) * 1.25f),
                                               juce::roundToInt (static_cast<float> (nominal.y) * 1.25f)),
                             minimum,
                             juce::Point<int> (1400, 600) })
    {
        editor->setSize (size.x, size.y);
        CHECK (editor->getWidth() == size.x);
        CHECK (editor->getHeight() == size.y);

        const auto expectedScale = juce::jmin (static_cast<float> (size.x) / static_cast<float> (nominal.x),
                                               static_cast<float> (size.y) / static_cast<float> (nominal.y));

        printComposition ("measured size", *editor);

        REQUIRE (editor->sectionCount() > 0);
        CHECK (editor->compositionHeight() > 0);
        CHECK (editor->interfaceScale() == Catch::Approx (expectedScale).epsilon (0.0001));

        // **The fit, on screen**: every section is inside the window it is being drawn in. This is the
        // assertion ticket `53`'s "the page fits the viewport" became — the page cannot be taller than the
        // space it is given (it is laid out *at* that size), so what has to be true is that the scaled page
        // is inside the window, and that is what is measured here.
        for (int index = 0; index < editor->sectionCount(); ++index)
        {
            const auto onScreen = editor->sectionBoundsOnScreen (index);

            INFO ("size " << size.x << "x" << size.y << ", section " << index << ": "
                          << editor->sectionId (index).toStdString());

            CHECK (editorBounds().toFloat().contains (onScreen));
        }

        CHECK (editorBounds().toFloat().contains (editor->pageBoundsOnScreen()));

        for (int index = 0; index < editor->sectionCount(); ++index)
        {
            INFO ("size " << size.x << "x" << size.y << ", section " << index << ": "
                          << editor->sectionId (index).toStdString());

            const auto bounds = editor->composition().sectionBounds (index);

            CHECK (bounds.getWidth() > 0);
            CHECK (bounds.getHeight() > 0);
            CHECK (compositionBounds().contains (bounds));

            const auto* plan = editor->sectionPlan (index);
            REQUIRE (plan != nullptr);

            if (editor->sectionIsMatrix (index))
            {
                // The matrix's own grid is bigger than the section that shows it, and scrolls inside it.
                CHECK (editor->matrixView().getWidth() > 0);
                CHECK (editor->matrixView().getHeight() > 0);
                CHECK (editor->matrixView().getHeight() > bounds.getHeight());

                // Ticket `53`'s assertion here was "at least" the constant, because the matrix took the
                // window's slack and a taller window showed more rows. Ticket `57` gave that slack to the
                // envelope and ticket `59` removed it altogether, so the compact matrix shows **exactly**
                // its own four rows at every size — which is the whole of "the matrix never absorbs the
                // window", asserted at every size this case measures.
                CHECK (editor->composition().matrixVisibleRows()
                       == (int) ui::CompositionView::kMatrixVisibleRows);
                continue;
            }

            REQUIRE_FALSE (plan->panels.empty());

            for (const auto panelIndex : plan->panels)
            {
                const auto& panelPlan = editor->plan().panels[static_cast<std::size_t> (panelIndex)];
                auto* panel = editor->panelForGroup (panelPlan.title);

                REQUIRE (panel != nullptr);
                INFO ("panel " << panelPlan.title);

                CHECK (panel->getWidth() > 0);
                CHECK (panel->getHeight() > 0);
                CHECK (bounds.contains (panel->getBounds()));
                CHECK (compositionBounds().contains (panel->getBounds()));

                // Every control of the panel is on it, laid out, and inside it.
                for (int row = 0; row < panel->rows(); ++row)
                {
                    auto* control = panel->controlAtRow (row);
                    REQUIRE (control != nullptr);

                    INFO ("row " << row << ": " << control->label().toStdString());

                    CHECK (control->getWidth() > 0);
                    CHECK (control->getHeight() > 0);
                    CHECK (panel->getLocalBounds().contains (control->getBounds()));
                    CHECK (control->isBound());

                    // A pill is one line tall even on a knob row — ticket `53`'s compact band puts a pill on
                    // the same line as a `knob-sm`, and a pill stretched to that line would be three times
                    // the design's own toggle.
                    if (ui::controlKindFor (control->spec().type) == ui::ControlKind::Toggle)
                        CHECK (control->widget().getHeight() <= ui::kRowHeight);
                }
            }
        }
    }

    // **What the minimum does to the design's own type.** Reported, not asserted: at the minimum scale a
    // design size and what is actually drawn are two different numbers, and the second is the one a reader
    // needs. Ticket `51`'s "the design's sizes" bind at the nominal size — which is what the 44 px knob
    // assertions elsewhere still measure, because every rectangle in the editor's tree is a nominal one.
    {
        editor->setSize (minimum.x, minimum.y);

        std::fprintf (stderr,
                      "[interface] at the minimum %dx%d (scale %.4f): the design's 9px section titles draw at "
                      "%.2f px, its 8px labels at %.2f px, its 7px tags at %.2f px, and a 44px knob at %.2f px\n",
                      minimum.x, minimum.y, editor->interfaceScale(),
                      ui::look::type::section * editor->interfaceScale(),
                      ui::look::type::label * editor->interfaceScale(),
                      ui::look::type::tag * editor->interfaceScale(),
                      ui::look::knobDiameter * editor->interfaceScale());

        CHECK (editor->interfaceScale() < 1.0f);
    }

    // And it draws at a smaller size: the palette is present in the editor's own render at the minimum, so
    // this is \"the composition was painted\" as well as \"it was laid out\".
    editor->setSize (minimum.x, minimum.y);
    editor->selectSection (0);

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds());

    auto background = 0;

    for (int y = 0; y < image.getHeight(); y += 4)
        for (int x = 0; x < image.getWidth(); x += 4)
        {
            const auto pixel = image.getPixelAt (x, y);

            if (std::abs (static_cast<int> (pixel.getRed()) - static_cast<int> (ui::look::bg.getRed())) <= 8
                && std::abs (static_cast<int> (pixel.getGreen()) - static_cast<int> (ui::look::bg.getGreen())) <= 8
                && std::abs (static_cast<int> (pixel.getBlue()) - static_cast<int> (ui::look::bg.getBlue())) <= 8)
                ++background;
        }

    CHECK (background > 500);

    // **A real preset, when this machine has the reference's bank.** The page's geometry is a function of
    // the parameter table and not of what is loaded — that is the binding rule — but the *contents* are the
    // preset's, so the same window is measured again with a factory preset loaded: the page's height has to
    // be the same number, and the wavetable heads and the modulation arcs have to be the preset's own.
    {
        editor->setSize (nominal.x, nominal.y);

        const auto pageWithoutPreset = editor->compositionHeight();
        const auto preset = firstFactoryPreset (processor);

        if (! preset.existsAsFile())
        {
            std::fprintf (stderr, "[interface] no factory bank on this machine — the preset measurement is "
                                  "skipped (the suite passes without one)\n");
        }
        else
        {
            REQUIRE (processor.loadPresetFromFile (preset).wasOk());
            processor.sendSynchronousChangeMessage();

            std::fprintf (stderr, "[interface] with %s loaded: window %dx%d, page %d (empty: %d)\n",
                          preset.getFileName().toRawUTF8(), editor->getWidth(), editor->getHeight(),
                          editor->compositionHeight(), pageWithoutPreset);

            CHECK (editor->compositionHeight() == pageWithoutPreset);
            CHECK (editor->compositionHeight() == editor->compositionArea().getHeight());
            CHECK (editor->interfaceScale() == Catch::Approx (1.0f));

            for (const auto& panelPlan : editor->plan().panels)
            {
                auto* panel = editor->panelForGroup (panelPlan.title);

                if (auto* slot = panelPlan.wavetableSlot >= 0 && panel != nullptr ? panel->wavetableSlot()
                                                                                 : nullptr)
                    std::fprintf (stderr, "[interface]   head %-12s %s\n", panelPlan.title.c_str(),
                                  slot->tableName().toRawUTF8());

                for (int row = 0; panel != nullptr && row < panel->rows(); ++row)
                    if (auto* knob = panel->controlAtRow (row)->knob())
                        if (knob->isModulated())
                            std::fprintf (stderr, "[interface]   modulated %-32s depth %+.3f on a %dpx knob\n",
                                          panel->controlAtRow (row)->label().toRawUTF8(),
                                          knob->modulationDepth(), knob->getWidth());
            }
        }
    }
}

//==============================================================================
TEST_CASE ("the composition holds one ratio, and resizing it re-flows nothing",
           "[interface][ui]")
{
    // Ticket `59`'s acceptance, as one measurement: **the composition is the same composition at every
    // size.** Ticket `57`'s version of this case asked who absorbed the window's slack — the envelope, was
    // the answer — and that question no longer has a subject: the layout is given a width and nothing else,
    // the page is laid out once, and the whole canvas is scaled into whatever the window is.
    //
    // What replaces it, measured on the editor itself:
    //
    //   * **the rectangles never move.** The nominal rectangles are captured once, and at 1.25×, at the
    //     minimum the editor's own constrainer allows, and at its ceiling they are **equal** — the same
    //     integers in the page's own coordinates. That is "it re-flows nothing", stated as an equality;
    //   * **what changes is one number.** Every on-screen rectangle is the nominal one times
    //     `interfaceScale()`, translated by where the page is drawn — so no section's position relative to
    //     another's can change, because both are the same constant times the same factor;
    //   * **ticket `57`'s slack measurement, re-scoped rather than dropped.** At a fixed ratio the envelope
    //     does not grow *by the slack*; it grows **proportionally**, like every other section, and its share
    //     of the page is a constant at every size. Both readings are asserted below — the re-scoped one here,
    //     and (in `the composed page fits every window it is given`) the sizes themselves;
    //   * **the design mock's own ratio, costed.** `docs/design/xerum-direction-warm.html` is 1082×668 and
    //     the dev may prefer that shape; measured here rather than argued, that window leaves most of its
    //     width empty, because this composition's content is nearly square.
    //
    // The numbers are printed rather than only asserted, because "it does not move" is a pair of
    // measurements. This session has no eyes; it has these rectangles.
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    const auto sectionFor = [&editor] (const juce::String& id)
    {
        for (int index = 0; index < editor->sectionCount(); ++index)
            if (editor->sectionId (index) == id)
                return index;

        return -1;
    };

    const auto envSection = sectionFor ("env-section");
    const auto matrixSection = sectionFor ("mod-panel");

    REQUIRE (envSection >= 0);
    REQUIRE (matrixSection >= 0);

    // **The composition in order, as measured** — the titles and the rectangles off the resolved
    // composition rather than the data it came from, so a section that landed somewhere else fails here.
    {
        std::vector<juce::String> titles;

        for (int index = 0; index < editor->sectionCount(); ++index)
            titles.push_back (editor->sectionTitle (index));

        std::fprintf (stderr, "\n[interface] the composition, in order (nominal coordinates):\n");

        for (int index = 0; index < editor->sectionCount(); ++index)
        {
            const auto bounds = editor->composition().sectionBounds (index);

            std::fprintf (stderr, "[interface]   %d %-16s %-14s %4dx%4d @%4d,%-4d\n", index,
                          editor->sectionId (index).toRawUTF8(), editor->sectionTitle (index).toRawUTF8(),
                          bounds.getWidth(), bounds.getHeight(), bounds.getX(), bounds.getY());
        }

        CHECK (titles == std::vector<juce::String> { "Oscillators", "Noise + Sub", "Amp Envelope", "Filter",
                                                     "Macros", "Other", "Mod Matrix" });

        // Early, and early *in the page*: the envelope is in the first half of the composition, i.e. above
        // the filters and the whole bottom half of the instrument — the position the ticket is about.
        CHECK (titles[static_cast<std::size_t> (envSection)] == "Amp Envelope");
        CHECK (envSection < editor->sectionCount() / 2);
        CHECK (matrixSection == editor->sectionCount() - 1);
    }

    // **The controls of the two sections, in px**, read off the widgets rather than the constants: the
    // envelope's four knobs at the design's largest diameter, and a matrix cell — which is not a dial at
    // all, but the compact numeric field inside a 22px row. This is the "knob sizes in each of the two
    // sections" half of the acceptance.
    //
    // **Ticket `51`'s rule, re-scoped by ticket `59` rather than deleted.** `51` asserted "no control got
    // smaller" against the design system's sizes, and at a fixed ratio that sentence needs its scope said
    // out loud: **the design's sizes bind at the nominal size**, and the ratio scales everything from there.
    // So these assertions still read 44 px — because every rectangle in this tree is a *nominal* one — and
    // what they measure now is the design's own size at the size it was designed at. What the user sees at a
    // window smaller than nominal is 44 px times `interfaceScale()`, which the fit case prints alongside the
    // type sizes rather than asserting, because how small is too small is the dev's call.
    {
        const auto* envPlan = editor->sectionPlan (envSection);
        REQUIRE (envPlan != nullptr);
        REQUIRE (envPlan->panels.size() == 1);

        auto* envelope = editor->panelForGroup (editor->plan().panels[
            static_cast<std::size_t> (envPlan->panels[0])].title);
        REQUIRE (envelope != nullptr);

        std::fprintf (stderr, "[interface] %s: panel %dx%d, %d controls\n", envelope->title().c_str(),
                      envelope->getWidth(), envelope->getHeight(), envelope->rows());

        CHECK (envelope->rows() == 4);   // attack, decay, sustain, release — the whole group

        for (int row = 0; row < envelope->rows(); ++row)
        {
            auto* control = envelope->controlAtRow (row);

            REQUIRE (control != nullptr);
            REQUIRE (control->knob() != nullptr);

            const auto* knob = control->knob();

            std::fprintf (stderr, "[interface]   %-14s knob %2dx%-2d, %s\n",
                          control->label().toRawUTF8(), knob->getWidth(), knob->getHeight(),
                          knob->size() == KnobSize::Default ? "design's largest" : "smaller");

            CHECK (knob->size() == KnobSize::Default);
            CHECK (knob->mode() == Knob::Mode::Dial);
            CHECK (knob->getWidth() == ui::look::knobDiameter);
            CHECK (knob->getHeight() == ui::look::knobDiameter);
        }

        // The matrix's cells are bound controls like every other (`ticket 45`), and their size is the
        // grid's: a row of the compact matrix is 22px and a cell is one row less its border.
        auto* amount = editor->matrixView().controlForCell (0, 3);
        REQUIRE (amount != nullptr);
        REQUIRE (amount->knob() != nullptr);

        std::fprintf (stderr, "[interface] Mod Matrix: cell (0,3) %dx%d, mode %s, row height %d\n",
                      amount->knob()->getWidth(), amount->knob()->getHeight(),
                      amount->knob()->mode() == Knob::Mode::Cell ? "Cell" : "Dial",
                      ModulationMatrixView::kRowHeight);

        CHECK (amount->knob()->mode() == Knob::Mode::Cell);
        CHECK (amount->knob()->getHeight() <= ModulationMatrixView::kRowHeight);
    }

    // The nominal rectangles, captured **once**, before anything is resized: every size below is compared
    // against these, and the claim is that they never change.
    const auto rectsAtNominal = [&editor]
    {
        std::vector<juce::Rectangle<int>> rects;

        for (int index = 0; index < editor->sectionCount(); ++index)
            rects.push_back (editor->composition().sectionBounds (index));

        return rects;
    }();

    const auto nominal = juce::Point<int> (editor->getWidth(), editor->getHeight());
    const auto area = editor->compositionArea();

    std::fprintf (stderr,
                  "\n[interface] the ratio: nominal %dx%d, aspect %.6f; the page is %d of those %d pixels "
                  "(aspect %.6f), the design's chrome the rest\n",
                  nominal.x, nominal.y, editor->aspectRatio(), editor->compositionHeight(), nominal.y,
                  static_cast<double> (area.getWidth()) / static_cast<double> (editor->compositionHeight()));

    CHECK (nominal == editor->nominalSize());
    CHECK (editor->interfaceScale() == Catch::Approx (1.0f));
    CHECK (editor->compositionHeight() == area.getHeight());

    // The sizes this measurement is made at, all **scales of the one nominal size**: 1×, 1.25×, the floor the
    // editor's own constrainer holds a drag to, and its ceiling. Read from the constrainer rather than
    // written down, so this measures the ratio the editor actually enforces.
    const auto* constrainer = editor->getConstrainer();
    REQUIRE (constrainer != nullptr);

    const auto minScale = static_cast<float> (constrainer->getMinimumWidth()) / static_cast<float> (nominal.x);
    const auto maxScale = static_cast<float> (constrainer->getMaximumWidth()) / static_cast<float> (nominal.x);

    for (const auto scale : { 1.0f, 1.25f, minScale, maxScale })
    {
        const auto size = juce::Point<int> (juce::roundToInt (static_cast<float> (nominal.x) * scale),
                                            juce::roundToInt (static_cast<float> (nominal.y) * scale));
        editor->setSize (size.x, size.y);

        printComposition ("scaled", *editor);

        // **The window's aspect is the ratio**, which is what `setFixedAspectRatio` buys: the size a user
        // drags to is a scale of the nominal size and nothing else.
        CHECK (editor->getWidth() == size.x);
        CHECK (editor->getHeight() == size.y);
        CHECK (static_cast<double> (editor->getWidth()) / static_cast<double> (editor->getHeight())
               == Catch::Approx (editor->aspectRatio()).epsilon (0.002));
        CHECK (editor->interfaceScale() == Catch::Approx (scale).epsilon (0.002));

        // **(a) The rectangles are the nominal ones**, in the page's own coordinates. This is "it re-flows
        // nothing", and it is an equality rather than a proportion: the layout is not given a window.
        for (int index = 0; index < editor->sectionCount(); ++index)
        {
            INFO ("scale " << scale << ", section " << index << ": "
                           << editor->sectionId (index).toStdString());

            CHECK (editor->composition().sectionBounds (index)
                   == rectsAtNominal[static_cast<std::size_t> (index)]);
        }

        // **(b) And on screen they are those rectangles times one number** — `interfaceScale()` — translated
        // by where the page is drawn. Nothing else enters: no section's position relative to another's can
        // change, because both are the same constant times the same factor.
        const auto page = editor->pageBoundsOnScreen();

        CHECK (page.getWidth() == Catch::Approx (area.getWidth() * editor->interfaceScale()).epsilon (0.01));
        CHECK (page.getHeight() == Catch::Approx (area.getHeight() * editor->interfaceScale()).epsilon (0.01));

        for (int index = 0; index < editor->sectionCount(); ++index)
        {
            const auto nominalRect = rectsAtNominal[static_cast<std::size_t> (index)];
            const auto onScreen = editor->sectionBoundsOnScreen (index);

            INFO ("scale " << scale << ", section " << index << ": "
                           << editor->sectionId (index).toStdString());

            CHECK (onScreen.getWidth()
                   == Catch::Approx (nominalRect.getWidth() * editor->interfaceScale()).epsilon (0.01));
            CHECK (onScreen.getHeight()
                   == Catch::Approx (nominalRect.getHeight() * editor->interfaceScale()).epsilon (0.01));
            CHECK (onScreen.getX()
                   == Catch::Approx (page.getX() + nominalRect.getX() * editor->interfaceScale()).epsilon (0.01));
            CHECK (onScreen.getY()
                   == Catch::Approx (page.getY() + nominalRect.getY() * editor->interfaceScale()).epsilon (0.01));

            // And it is inside the window: the fit, at every one of these sizes.
            CHECK (editor->getLocalBounds().toFloat().contains (onScreen));
        }

        // **(c) The envelope and the matrix, the pair ticket `57` measured against each other.** Ticket `57`
        // asked who absorbs the window's slack and answered, correctly for a layout that re-flows, "the
        // envelope, by exactly the slack, and the matrix by none". At a fixed ratio there is no slack and the
        // question changes shape: both sections grow **proportionally**, by the same factor, and the
        // envelope's share of the page is a constant. Stated this way rather than dropped, because the old
        // measurement is still true of a *layout*, and no longer true of this one.
        {
            const auto env = editor->sectionBoundsOnScreen (envSection);
            const auto matrix = editor->sectionBoundsOnScreen (matrixSection);

            CHECK (env.getHeight()
                   == Catch::Approx (rectsAtNominal[static_cast<std::size_t> (envSection)].getHeight()
                                         * editor->interfaceScale())
                          .epsilon (0.01));
            CHECK (matrix.getHeight()
                   == Catch::Approx (rectsAtNominal[static_cast<std::size_t> (matrixSection)].getHeight()
                                         * editor->interfaceScale())
                          .epsilon (0.01));
            CHECK (env.getHeight() / page.getHeight()
                   == Catch::Approx (static_cast<float> (rectsAtNominal[static_cast<std::size_t> (envSection)].getHeight())
                                         / static_cast<float> (area.getHeight()))
                          .epsilon (0.01));

            // The compact matrix still shows exactly its four rows: `57`'s other half, unchanged.
            CHECK (editor->composition().matrixVisibleRows()
                   == static_cast<int> (CompositionView::kMatrixVisibleRows));
        }
    }

    // **What the design mock's own ratio would cost.** `docs/design/xerum-direction-warm.html` is 1082×668 —
    // 1.62 wide per tall, against this composition's 0.96 — and the dev may prefer it. Measured rather than
    // argued: at that window the canvas is scaled by its *height* (the width has room to spare), so the
    // composition occupies part of the window and the rest is background, and the same composition one
    // millimetre taller would be clipped if it were scaled to the width instead.
    {
        const auto mock = juce::Point<int> (1082, 668);
        editor->setSize (mock.x, mock.y);

        const auto page = editor->pageBoundsOnScreen();
        const auto used = page.getWidth() / static_cast<float> (mock.x);
        const auto byWidth = static_cast<float> (mock.x) / static_cast<float> (nominal.x);

        std::fprintf (stderr,
                      "[interface] the mock's ratio %dx%d (%.4f): scale %.4f, the composition %.0fx%.0f, "
                      "%.1f %% of the window's width used (%.1f %% empty); scaled to the width instead "
                      "(%.4f) it would need %d px of height, %d more than the mock has\n",
                      mock.x, mock.y, static_cast<double> (mock.x) / mock.y, editor->interfaceScale(),
                      page.getWidth(), page.getHeight(), 100.0f * used, 100.0f * (1.0f - used), byWidth,
                      juce::roundToInt (static_cast<float> (nominal.y) * byWidth),
                      juce::roundToInt (static_cast<float> (nominal.y) * byWidth) - mock.y);

        CHECK (used < 0.75f);   // the width is substantially unused at that ratio
        CHECK (editor->getLocalBounds().toFloat().contains (page));
    }

    // **The captures.** The editor's own render at three sizes — `createComponentSnapshot` over the
    // editor's whole window, which is the method tickets `52`, `53` and `57` measure with and the only
    // capture this machine allows (`screencapture` is refused by Screen Recording here). At each size the
    // composition is the same composition drawn bigger or smaller, and the palette's own shares of the
    // window are measured as well as written out: a layout that re-flowed would move those shares by tens
    // of points; a layout that scales moves them by tenths, which is the anti-aliasing of its own edges.
    {
        auto directory = juce::File (juce::String (XERUM_BUILD_DIR)).getChildFile ("interface-evidence");
        directory.createDirectory();

        juce::PNGImageFormat format;
        std::vector<std::pair<juce::String, std::map<juce::String, double>>> measured;

        for (const auto scale : { 1.0f, 1.25f, minScale })
        {
            const auto size = juce::Point<int> (juce::roundToInt (static_cast<float> (nominal.x) * scale),
                                                juce::roundToInt (static_cast<float> (nominal.y) * scale));
            editor->setSize (size.x, size.y);
            editor->selectSection (0);

            const auto image = editor->createComponentSnapshot (editor->getLocalBounds());

            REQUIRE_FALSE (image.isNull());

            const auto label = "ticket-59-scale-" + juce::String (juce::roundToInt (scale * 100.0f)) + ".png";
            const auto png = directory.getChildFile (label);

            if (auto stream = std::unique_ptr<juce::FileOutputStream> (png.createOutputStream()))
                format.writeImageToStream (image, *stream);

            // The palette's own share of this render. Exact colours only, the way the token walk counts
            // them, so this is "the surfaces are where they were" rather than a blend of two tokens.
            std::map<juce::String, double> shares;
            const auto total = static_cast<double> (image.getWidth()) * static_cast<double> (image.getHeight());

            for (const auto& [name, colour] : std::vector<std::pair<const char*, juce::Colour>> {
                     { "bg", ui::look::bg },       { "surface", ui::look::surface },
                     { "surface2", ui::look::surface2 }, { "border", ui::look::border },
                     { "accent", ui::look::accent } })
            {
                auto count = 0;

                for (int y = 0; y < image.getHeight(); ++y)
                    for (int x = 0; x < image.getWidth(); ++x)
                        if (image.getPixelAt (x, y) == colour)
                            ++count;

                shares[name] = 100.0 * count / total;
            }

            std::fprintf (stderr, "[interface] %s: %dx%d — bg %.3f %%, surface %.3f %%, surface2 %.3f %%, "
                                  "border %.3f %%, accent %.3f %%\n",
                          label.toRawUTF8(), image.getWidth(), image.getHeight(), shares["bg"], shares["surface"],
                          shares["surface2"], shares["border"], shares["accent"]);

            CHECK (image.getWidth() == editor->getWidth());
            CHECK (image.getHeight() == editor->getHeight());
            CHECK (png.getSize() > 0);

            measured.emplace_back (label, std::move (shares));
        }

        REQUIRE (measured.size() == 3);

        // **The same composition, at three scales.** The **filled areas**' shares of the window agree with
        // the nominal render's to well inside a point: the panels are the same fraction of the page, the page
        // the same fraction of the window, at every size — which is what "scaled, not re-flowed" looks like as
        // a number. Two points is a wide bound deliberately: it is a claim about *no re-flow*, and a re-flow
        // moves these by tens.
        //
        // `border` is printed and deliberately **not** compared, which is a fact about pixels rather than
        // about the layout: a 1 px hairline drawn at 0.83× is anti-aliased into its neighbours, so its
        // exact-colour share falls with the scale (3.17 % at 1×, 0.44 % at 0.83×) while the panels it frames
        // do not move at all. The shares that would move if the composition re-flowed are the big ones.
        for (const auto& [label, shares] : measured)
        {
            INFO ("render " << label.toStdString());

            for (const auto* name : { "bg", "surface", "surface2", "accent" })
                CHECK (std::abs (shares.at (name) - measured.front().second.at (name)) < 2.0);
        }
    }
}

//==============================================================================
TEST_CASE ("the amplitude envelope is drawn from its own four parameters, and shows the matrix's rows",
           "[interface][ui]")
{
    // Ticket `59`'s second half. Ticket `57` was told not to invent an envelope graphic and to report if
    // prominence needed one; the dev has now asked for the picture directly, so the deliverable is a drawing
    // **in the design system's language** that is **derived from the parameters** rather than decorative —
    // and the way to assert "derived" without eyes is the counterfactual the last three tickets used: change
    // a parameter and require the picture to change, in the segment that parameter owns.
    //
    // Everything here is read from `ui::EnvelopeCurve::geometry()` — the five corners the paint routine
    // draws — rather than from pixels, except in the two places where pixels *are* the claim: that it draws
    // at all, and that it draws nothing where a parameter is not bound.
    const juce::ScopedJuceInitialiser_GUI gui;

    // A document whose matrix addresses **the envelope's own attack** — the row the graph has to show when
    // the matrix has one, through the same `modelPath` the knob's second arc is keyed on.
    model::PresetDocument document;
    document.name = "envelope fixture";
    document.schemaVersion = "4.0";
    document.parameters["ModSlot0/source[0]"] = number (1.0);
    document.parameters["ModSlot0/plainParams/kParamAmount"] = number (40.0);
    document.parameters["ModSlot0/plainParams/kParamBypass"] = number (0.0);
    document.parameters["ModSlot0/destModuleTypeString"] = text ("Env");
    document.parameters["ModSlot0/destModuleID"] = number (0.0);
    document.parameters["ModSlot0/destModuleParamName"] = text ("kParamAttack");

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument (document, {}, 0);

    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    auto& graph = editor->envelopeCurve();

    // The panel it is in: the composition's envelope section, whose group is the amplitude envelope's.
    const auto envSection = editor->sectionForModule ("Env0");
    REQUIRE (envSection >= 0);

    const auto* envPlan = editor->sectionPlan (envSection);
    REQUIRE (envPlan != nullptr);
    REQUIRE (envPlan->panels.size() == 1);

    auto* panel = editor->panelForGroup (
        editor->plan().panels[static_cast<std::size_t> (envPlan->panels[0])].title);
    REQUIRE (panel != nullptr);
    REQUIRE (panel->title() == "Amp Envelope");

    // **The graph is the envelope panel's inline display** — the design's `filter-viz` column, the place the
    // filter's response display sits — and not a page, a window or a panel of its own.
    CHECK (graph.getParentComponent() == static_cast<const juce::Component*> (panel));
    CHECK (panel->inlineDisplay() == &graph);
    CHECK (panel->getLocalBounds().contains (graph.getBounds()));
    CHECK (graph.getWidth() > 0);
    CHECK (graph.getHeight() > 0);
    REQUIRE (graph.isBound());

    std::fprintf (stderr, "\n[interface] the envelope graph: %dx%d inside a %dx%d panel\n", graph.getWidth(),
                  graph.getHeight(), panel->getWidth(), panel->getHeight());

    // **The four stages are the panel's four controls**, found by the `modelPath` the controls were built
    // from — not a second list of parameter ids, and not a value read out of the document.
    const auto knobFor = [&] (const EnvelopeStage stage) -> Knob*
    {
        for (const auto* spec : editor->plan().panels[static_cast<std::size_t> (envPlan->panels[0])].specs)
            if (envelopeStageForModelPath (spec->modelPath) == stage)
                if (auto* control = editor->controlFor (spec->id))
                    return control->knob();

        return nullptr;
    };

    for (const auto stage : { EnvelopeStage::Attack, EnvelopeStage::Decay, EnvelopeStage::Sustain,
                              EnvelopeStage::Release })
    {
        REQUIRE (knobFor (stage) != nullptr);
        CHECK (graph.stageKnob (stage) == knobFor (stage));

        // **The graph and the arcs are the same number.** The curve's shape is a function of
        // `valueToProportionOfLength` — the proportion the knob's value arc is drawn from — and the legend
        // prints `getTextFromValue`, the string the knob's own readout prints. Neither is a second opinion.
        auto* knob = knobFor (stage);

        CHECK (graph.stageValue (stage) == knob->getValue());
        CHECK (graph.stageProportion (stage)
               == Catch::Approx (knob->valueToProportionOfLength (knob->getValue())));
        CHECK (graph.stageText (stage) == knob->getTextFromValue (knob->getValue()));
    }

    // **The counterfactual, part 1: attack owns the rise.** At the parameter's own default the rise is a real
    // segment; at the top of its range it is wider still; at nought it is nought wide, which is an instant
    // attack drawn as the vertical edge it is.
    auto* attack = knobFor (EnvelopeStage::Attack);
    REQUIRE (attack != nullptr);

    const auto rise = [&graph] { return graph.geometry().attackPeak.x - graph.geometry().start.x; };

    const auto riseAtDefault = rise();

    CHECK (riseAtDefault > 0.0f);

    attack->setValue (attack->getMaximum(), juce::sendNotificationSync);
    const auto riseAtMaximum = rise();

    attack->setValue (0.0, juce::sendNotificationSync);
    const auto riseAtZero = rise();

    std::fprintf (stderr, "[interface] the rise: %.2f px at the default, %.2f at the maximum, %.2f at nought "
                          "(plot %.0f wide)\n",
                  riseAtDefault, riseAtMaximum, riseAtZero, graph.geometry().plot.getWidth());

    CHECK (riseAtMaximum > riseAtDefault);
    CHECK (riseAtZero == 0.0f);

    // And the shape is an envelope's whatever the times are: the rise peaks at the top of the plot, the
    // release ends on the baseline at the plot's right edge.
    CHECK (graph.geometry().attackPeak.y == graph.geometry().plot.getY());
    CHECK (graph.geometry().releaseEnd.y == graph.geometry().plot.getBottom());
    CHECK (graph.geometry().releaseEnd.x == Catch::Approx (graph.geometry().plot.getRight()).epsilon (0.01));

    // **The counterfactual, part 2: sustain is a level**, and it is the plateau's height and nothing else.
    auto* sustain = knobFor (EnvelopeStage::Sustain);
    REQUIRE (sustain != nullptr);

    sustain->setValue (0.0, juce::sendNotificationSync);
    CHECK (graph.geometry().decayEnd.y == graph.geometry().plot.getBottom());
    CHECK (graph.geometry().sustainEnd.y == graph.geometry().plot.getBottom());

    sustain->setValue (1.0, juce::sendNotificationSync);
    CHECK (graph.geometry().decayEnd.y == graph.geometry().plot.getY());
    CHECK (graph.geometry().sustainEnd.y == graph.geometry().plot.getY());

    // **The second curve is the matrix's row**, through the path the knob's arc is drawn from: the fixture's
    // 40 % on the attack reaches both, as the same number.
    CHECK (graph.stageModulation (EnvelopeStage::Attack) == Catch::Approx (0.4));
    CHECK (attack->modulationDepth() == Catch::Approx (0.4));
    CHECK (graph.stageModulation (EnvelopeStage::Attack) == Catch::Approx (attack->modulationDepth()));
    CHECK (graph.isModulated());

    // With a value that leaves the attack room to move, the modulated rise is the longer one — drawn from
    // `proportion + depth`, which is exactly what the arc sweeps.
    attack->setValue (1.0, juce::sendNotificationSync);

    const auto plain = graph.geometry (false);
    const auto modulated = graph.geometry (true);

    std::fprintf (stderr, "[interface] with +0.4 on the attack: the rise is %.2f px unmodulated and %.2f px "
                          "modulated\n",
                  plain.attackPeak.x - plain.start.x, modulated.attackPeak.x - modulated.start.x);

    CHECK (modulated.attackPeak.x > plain.attackPeak.x);

    // And no row, no second curve: the state the design calls "a control with no modulated state is not
    // finished", answered with a fact rather than a stub.
    graph.setStageModulation (EnvelopeStage::Attack, 0.0);
    CHECK_FALSE (graph.isModulated());
    CHECK (graph.geometry (true).attackPeak.x == graph.geometry (false).attackPeak.x);

    // **It draws, in the design's own colours.** The graph's own snapshot, counted: the display's `bg` with
    // its `border`, the accent on the curve, the accent's 10 % wash under it, and ink where the monospaced
    // legend is — the four stages' letters and the four values.
    {
        attack->setValue (0.5, juce::sendNotificationSync);
        knobFor (EnvelopeStage::Decay)->setValue (1.0, juce::sendNotificationSync);
        sustain->setValue (0.5, juce::sendNotificationSync);
        knobFor (EnvelopeStage::Release)->setValue (1.0, juce::sendNotificationSync);

        const auto image = graph.createComponentSnapshot (graph.getLocalBounds());

        REQUIRE_FALSE (image.isNull());

        const auto countNear = [&image] (const juce::Colour colour, const int tolerance)
        {
            auto found = 0;

            for (int y = 0; y < image.getHeight(); ++y)
                for (int x = 0; x < image.getWidth(); ++x)
                {
                    const auto pixel = image.getPixelAt (x, y);

                    if (std::abs (static_cast<int> (pixel.getRed()) - static_cast<int> (colour.getRed())) <= tolerance
                        && std::abs (static_cast<int> (pixel.getGreen()) - static_cast<int> (colour.getGreen())) <= tolerance
                        && std::abs (static_cast<int> (pixel.getBlue()) - static_cast<int> (colour.getBlue())) <= tolerance)
                        ++found;
                }

            return found;
        };

        // The legend's row: ink is anything brighter than the display's own background, which is what a
        // thread of 8px monospaced type is without counting anti-aliased pixels as a colour.
        auto legendInk = 0;
        const auto legendTop = image.getHeight() - 11;

        for (int y = legendTop; y < image.getHeight(); ++y)
            for (int x = 0; x < image.getWidth(); ++x)
                if (image.getPixelAt (x, y).getBrightness() > ui::look::bg.getBrightness() + 0.1f)
                    ++legendInk;

        // **The wash under the curve.** `accentBg` is the accent at **10 % alpha**, so what reaches the
        // framebuffer is that colour *over* the display's own background — a dim brownish blend whose red
        // channel leads — and counting the source colour exactly would be counting a colour that was never
        // composited. The band is the fill's area; the curve itself is above it (red 232, and excluded by the
        // bound), and every neutral token has red ≈ green so it cannot be counted.
        auto wash = 0;

        for (int y = 0; y < image.getHeight(); ++y)
            for (int x = 0; x < image.getWidth(); ++x)
            {
                const auto pixel = image.getPixelAt (x, y);

                if (pixel.getRed() > pixel.getGreen() + 5 && pixel.getRed() < 140)
                    ++wash;
            }

        std::fprintf (stderr,
                      "[interface] the graph's own render %dx%d: bg %d, border %d, accent %d, accent-bg %d "
                      "exact, the wash under the curve %d px, legend ink %d px\n",
                      image.getWidth(), image.getHeight(), countNear (ui::look::bg, 0),
                      countNear (ui::look::border, 8), countNear (ui::look::accent, 40),
                      countNear (ui::look::accentBg, 0), wash, legendInk);

        CHECK (countNear (ui::look::bg, 0) > 50);         // the display's own frame
        CHECK (countNear (ui::look::border, 8) > 20);     // its radius
        CHECK (countNear (ui::look::accent, 40) > 10);    // the curve itself
        CHECK (wash > 1000);                              // the accent's wash, under it
        CHECK (legendInk > 5);                            // the four letters and the four times
    }

    // **Nothing was lost by adding it.** The four controls are still the panel's controls, still bound, still
    // a `juce::Slider` a host can automate, at the design's diameter, and the graph's column does not overlap
    // any of them — the arrangement the filter's response display has.
    for (int row = 0; row < panel->rows(); ++row)
    {
        auto* control = panel->controlAtRow (row);

        REQUIRE (control != nullptr);
        CHECK (control->isBound());
        CHECK (dynamic_cast<juce::Slider*> (&control->widget()) != nullptr);
        CHECK (control->knob() != nullptr);
        CHECK (control->knob()->getWidth() == ui::look::knobDiameter);
        CHECK (control->getRight() <= graph.getX());
    }

    // And it is a **read-out**: a click at its centre reaches the panel behind it rather than being swallowed
    // by a component that does nothing with it. Dragging the curve is out of this ticket's scope, and a
    // component that took the click would look draggable without being it — so it takes none, and this is
    // where that is measured rather than implied.
    CHECK_FALSE (graph.hitTest (graph.getWidth() / 2, graph.getHeight() / 2));
}

//==============================================================================
TEST_CASE ("the design's sections are in the design's positions", "[interface][ui]")
{
    // Ticket `52`'s acceptance, as geometry: three oscillators **in a row** across the top, noise and sub
    // beneath them, the amplitude envelope's own band (ticket `57`), the filters with their response
    // displays **inside** their own panels, — ticket `53` — the design's bottom row, the macro strip beside
    // the section the composition does not name its groups in, and — ticket `57` — the compact matrix
    // **last**. These are the placements the dev said were missing: the elements were the design's and the
    // arrangement was a panel-per-group grid.
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    // The geometry below is in the page's own coordinates, which are the same at every window (ticket `59`),
    // so this is the size the editor opened at — its nominal size — rather than a second size chosen here.
    editor->setSize (editor->nominalSize().x, editor->nominalSize().y);

    const auto& composition = editor->composition();

    const auto sectionFor = [&editor] (const juce::String& id)
    {
        for (int index = 0; index < editor->sectionCount(); ++index)
            if (editor->sectionId (index) == id)
                return index;

        return -1;
    };

    const auto panelOf = [&editor] (const int panelIndex)
    {
        return editor->panelForGroup (editor->plan().panels[static_cast<std::size_t> (panelIndex)].title);
    };

    // **The oscillator row: three panels in a row.** One line, three of them, left to right, not
    // overlapping, in the table's own order.
    const auto oscRow = sectionFor ("osc-row");
    REQUIRE (oscRow >= 0);

    const auto* oscPlan = editor->sectionPlan (oscRow);
    REQUIRE (oscPlan != nullptr);
    REQUIRE (oscPlan->panels.size() == 3);

    auto previous = juce::Rectangle<int>();

    for (const auto panelIndex : oscPlan->panels)
    {
        auto* panel = panelOf (panelIndex);
        REQUIRE (panel != nullptr);

        const auto bounds = panel->getBounds();

        CHECK (bounds.getHeight() > 0);

        if (! previous.isEmpty())
        {
            CHECK (bounds.getY() == previous.getY());         // one row
            CHECK (bounds.getX() >= previous.getRight());     // left to right, not overlapping
        }

        previous = bounds;
    }

    // Each oscillator's head carries the design's wavetable slot — a monospaced name field with two arrows —
    // and it sits **above** the panel's controls.
    for (const auto panelIndex : oscPlan->panels)
    {
        auto* panel = panelOf (panelIndex);
        REQUIRE (panel != nullptr);

        auto* slot = panel->wavetableSlot();
        REQUIRE (slot != nullptr);

        INFO ("oscillator head: " << panel->title());

        CHECK (slot->getWidth() > 0);
        CHECK (slot->getHeight() > 0);
        CHECK (panel->getLocalBounds().contains (slot->getBounds()));
        CHECK (slot->getBottom() <= panel->controlAtRow (0)->getY());

        // The arrows are real rectangles inside the head, and they are live: the slot's frame position is a
        // parameter, so `onStep` is wired and the arrows are drawn.
        CHECK (slot->isSteppable());
        CHECK (slot->arrowBounds (-1).getWidth() > 0);
        CHECK (slot->arrowBounds (1).getRight() <= slot->getWidth());
        CHECK (slot->arrowBounds (-1).getRight() <= slot->arrowBounds (1).getX());
    }

    // **Noise and sub beneath them**: one row of two panels, below the oscillator row.
    const auto noiseRow = sectionFor ("noise-sub-row");
    REQUIRE (noiseRow >= 0);
    CHECK (composition.sectionBounds (noiseRow).getY() >= composition.sectionBounds (oscRow).getBottom());

    const auto* noisePlan = editor->sectionPlan (noiseRow);
    REQUIRE (noisePlan != nullptr);
    REQUIRE (noisePlan->panels.size() == 2);
    CHECK (panelOf (noisePlan->panels[0])->getBounds().getY()
           == panelOf (noisePlan->panels[1])->getBounds().getY());

    // **The filters, with their response displays inside them**, below noise and sub.
    const auto filterSection = sectionFor ("filter-section");
    REQUIRE (filterSection >= 0);
    CHECK (composition.sectionBounds (filterSection).getY() > composition.sectionBounds (noiseRow).getY());

    const auto* filterPlan = editor->sectionPlan (filterSection);
    REQUIRE (filterPlan != nullptr);
    REQUIRE (filterPlan->panels.size() == 2);

    for (const auto slot : { 0, 1 })
    {
        auto& plane = editor->poleZeroPlane (slot);
        auto* panel = panelOf (filterPlan->panels[static_cast<std::size_t> (slot)]);

        REQUIRE (panel != nullptr);
        INFO ("filter " << slot << ": " << panel->title());

        // The display is a **child of the filter's own panel**, beside its knobs — the design's `filter-viz`
        // — rather than a page of its own, which is what ticket `44` had.
        CHECK (plane.getParentComponent() == static_cast<const juce::Component*> (panel));
        CHECK (plane.getWidth() > 0);
        CHECK (plane.getHeight() > 0);
        CHECK (panel->getLocalBounds().contains (plane.getBounds()));

        // Beside the knobs, not under them: the display's column is to the right of every control.
        for (int row = 0; row < panel->rows(); ++row)
            CHECK (panel->controlAtRow (row)->getRight() <= plane.getX());
    }

    // **The matrix is the last band of the page**, after the bottom row, and it is the composition's
    // **compact** section since ticket `57`: four of the 64 rows, its `N / 64` tag still the geometry's own
    // number, and no slack — the section it used to take the window's slack with is the envelope's now
    // (asserted below), so the number of rows it shows *is* the constant rather than at least it.
    const auto matrixSection = sectionFor ("mod-panel");
    REQUIRE (matrixSection >= 0);
    CHECK (editor->sectionIsMatrix (matrixSection));
    CHECK (matrixSection == editor->sectionCount() - 1);

    const auto matrixBounds = composition.sectionBounds (matrixSection);
    const auto visibleRows = (matrixBounds.getHeight() - ui::kSectionTitleHeight
                              - ui::ModulationMatrixView::kHeaderHeight)
                             / ui::ModulationMatrixView::kRowHeight;

    CHECK (visibleRows == editor->composition().matrixVisibleRows());
    CHECK (visibleRows == ui::CompositionView::kMatrixVisibleRows);
    CHECK (editor->matrixView().getHeight() > matrixBounds.getHeight());

    // **The envelope's own section**, ticket `57`'s first half: directly after the noise/sub row and before
    // the filters, holding the amplitude envelope's panel and nothing else, and the panel's knobs are the
    // design's **largest** — `KnobSize::Default`, the same 44px the oscillators and the filters draw, and
    // larger than the compact band's small knob and the macros'. Position is asserted here, the size
    // measured separately, and the band the section owns is checked against the page's width: an envelope
    // that landed in a half-width panel would have the position without the room.
    const auto envSection = sectionFor ("env-section");
    REQUIRE (envSection >= 0);
    CHECK (composition.sectionBounds (envSection).getY() >= composition.sectionBounds (noiseRow).getBottom());
    CHECK (composition.sectionBounds (envSection).getBottom()
           <= composition.sectionBounds (filterSection).getY());

    const auto* envPlan = editor->sectionPlan (envSection);
    REQUIRE (envPlan != nullptr);
    REQUIRE (envPlan->panels.size() == 1);

    auto* envelope = panelOf (envPlan->panels[0]);
    REQUIRE (envelope != nullptr);
    CHECK (envelope->title() == "Amp Envelope");
    CHECK (envelope->getBounds().getWidth() == composition.sectionBounds (envSection).getWidth());

    for (int row = 0; row < envelope->rows(); ++row)
    {
        auto* control = envelope->controlAtRow (row);
        REQUIRE (control != nullptr);

        INFO ("envelope control " << control->label().toStdString());

        REQUIRE (control->knob() != nullptr);
        CHECK (control->knob()->size() == KnobSize::Default);
        CHECK (control->knob()->getWidth() == ui::look::knobDiameter);
        CHECK (control->knob()->getHeight() == ui::look::knobDiameter);
    }

    // **The bottom row: the macros beside the section the composition does not name its groups in.** The
    // design pairs its effects cards with the macro strip there, and xerum has no effects group — so the
    // half of that row the absent FX section would have held is where the fall-through section sits
    // (ticket `53`), and the two are one row rather than two lines. Since ticket `57` this row is the last
    // one but for the matrix, so the assertions that named the matrix as its neighbour name the envelope's
    // section instead.
    const auto macroSection = sectionFor ("macro-section");
    REQUIRE (macroSection >= 0);
    CHECK (composition.sectionBounds (macroSection).getY()
           >= composition.sectionBounds (filterSection).getBottom());
    CHECK (composition.sectionBounds (matrixSection).getY()
           >= composition.sectionBounds (macroSection).getBottom());

    const auto* macroPlan = editor->sectionPlan (macroSection);
    REQUIRE (macroPlan != nullptr);
    REQUIRE (macroPlan->panels.size() == 8);

    for (const auto panelIndex : macroPlan->panels)
    {
        auto* panel = panelOf (panelIndex);

        REQUIRE (panel != nullptr);
        CHECK (panel->isCompact());   // a strip item: a bare knob and its label, no panel chrome
        CHECK (panel->getBounds().getY() == panelOf (macroPlan->panels[0])->getBounds().getY());
        CHECK (panel->getBounds().getX() >= panelOf (macroPlan->panels[0])->getBounds().getX());
    }

    // **The fall-through, visible**: the one group the design's composition has no place for is the global
    // section, and it shares the design's bottom row with the macro strip rather than taking a line of its
    // own. It is no longer the *last* section — ticket `57` put the compact matrix after the bottom row —
    // so the assertion that said so is replaced by the reason: this row is the page's last but one, and
    // what ends the page is the matrix.
    const auto defaultSection = sectionFor ("default-section");
    REQUIRE (defaultSection >= 0);
    CHECK (defaultSection == matrixSection - 1);

    const auto* defaultPlan = editor->sectionPlan (defaultSection);
    REQUIRE (defaultPlan != nullptr);

    std::set<std::string> fellThrough;

    for (const auto panelIndex : defaultPlan->panels)
        fellThrough.insert (editor->plan().panels[static_cast<std::size_t> (panelIndex)].title);

    // `Amp Envelope` used to be in this set; ticket `57` gave it the section above, and this is the check
    // that says the fall-through lost exactly that group and kept the other. Ticket `58` is the second
    // member: the master ceiling's group is `Master`, the design's composition has no section for it, and
    // the fall-through is **exactly where a group nobody named is supposed to land** — the alternative
    // would be a group that vanished from the page, which is the invariant ticket `51` protected and ticket
    // `52` built the fall-through to keep. Naming it a section is a composition decision and belongs to
    // whoever next opens the composition, not to the parameter that arrived.
    CHECK (fellThrough == std::set<std::string> { "Global", "Master" });

    const auto macroBounds = composition.sectionBounds (macroSection);
    const auto defaultBounds = composition.sectionBounds (defaultSection);

    CHECK (defaultBounds.getY() == macroBounds.getY());            // one row
    CHECK (defaultBounds.getX() >= macroBounds.getRight());        // the strip's half, then the section's
    CHECK (defaultBounds.getRight() <= composition.getWidth());    // and the row ends with the page
    CHECK (defaultBounds.getHeight() == macroBounds.getHeight());

    // **The option rows**: the design draws a short vocabulary as a row of buttons, which is exactly where
    // its own `btn-opt` rows are — NOISE's colours and SUB's waveforms. The filter's 83-value vocabulary
    // cannot be a row of pills and is the themed picker.
    auto buttonRows = 0;

    for (const auto& spec : processor.bridge().specs())
    {
        if (controlKindFor (spec.type) != ControlKind::Choice)
            continue;

        if (spec.group != "Noise" && spec.group != "Sub Osc")
            continue;

        auto* control = editor->controlFor (spec.id);
        REQUIRE (control != nullptr);
        CHECK (control->choiceStyle() == ChoiceStyle::Buttons);
        CHECK (dynamic_cast<ChoiceButtons*> (&control->widget()) != nullptr);
        ++buttonRows;
    }

    CHECK (buttonRows == 2);   // one in each of the design's two compact panels

    // And the row is a control rather than a picture: a key reaches the parameter the same way a click
    // does, so the option row is not a surface only a mouse can use.
    auto* waveform = editor->controlFor ("oscillator4.subosc4.shape");
    REQUIRE (waveform != nullptr);
    REQUIRE (waveform->choiceStyle() == ChoiceStyle::Buttons);

    auto* pills = dynamic_cast<ChoiceButtons*> (&waveform->widget());
    REQUIRE (pills != nullptr);

    const auto shapeBefore = processor.parameterValue ("Oscillator4/SubOsc4/plainParams/kParamShape");
    CHECK (pills->keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    CHECK (processor.parameterValue ("Oscillator4/SubOsc4/plainParams/kParamShape") == shapeBefore + 1);

    CHECK (pills->keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));
    CHECK (processor.parameterValue ("Oscillator4/SubOsc4/plainParams/kParamShape") == shapeBefore);

    auto* filterType = editor->controlFor ("voicefilter0.type");
    REQUIRE (filterType != nullptr);
    CHECK (filterType->choiceStyle() == ChoiceStyle::Menu);
    CHECK (dynamic_cast<juce::ComboBox*> (&filterType->widget()) != nullptr);
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
    //
    // Since ticket `59` the interface's components hang from the editor's **canvas** (the one component that
    // is scaled into the window) rather than directly from the editor, so the direct parent is the canvas
    // and the **top-level** is still the editor. The top-level is the claim that matters — it is the one a
    // modal cannot make — and the canvas is asserted alongside it so the chain is stated rather than assumed.
    CHECK (editor->reportView().getTopLevelComponent() == editor.get());
    CHECK (bar.getTopLevelComponent() == editor.get());
    CHECK (bar.getParentComponent() == &editor->interfaceCanvas());
    CHECK (editor->interfaceCanvas().isParentOf (&editor->reportView()));
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

    // The strip locates the problem: the section whose module the report named carries the dot, and a
    // section the report said nothing about does not. That is what makes the strip a map of the import
    // rather than a second copy of it — and since a section holds several modules (ticket `52`) it is the
    // section's own module list that is asked.
    const auto& strip = editor->sectionStrip();
    auto dottedSections = 0;

    for (int index = 0; index < editor->sectionCount(); ++index)
    {
        auto expected = false;
        auto expectedWorst = xerum::model::Severity::Info;

        for (const auto& module : editor->sectionModules (index))
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

        INFO ("section " << index << " (" << editor->sectionId (index).toStdString() << ")");
        CHECK (strip.hasSectionSeverity (index) == expected);

        if (expected)
        {
            ++dottedSections;
            CHECK (strip.sectionSeverity (index) == expectedWorst);
        }
    }

    CHECK (dottedSections >= 1);

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

    // **Non-finite output is the failure mode a cascade of drawn sections can have** — and the corpus's
    // drawn-filter fixture is the only member that ever produced a non-finite reading (ticket `46`'s
    // report; attributed there to the tree moving, and worth guarding now that the response is applied).
    // 32 sections at the extremes — a pole radius of `0.9999` (Q 1, pole at 0 of Nyquist) against a zero
    // at Nyquist — is where it would show, so it is measured rather than assumed.
    {
        auto extremes = poleZeroDocument();

        for (int object = 0; object < 4; ++object)
            for (int i = 0; i < 8; ++i)
            {
                const std::string prefix = "VoiceFilter0/PZs[" + std::to_string (object) + "]";
                const auto leaf = [&prefix, i] (const char* field)
                { return prefix + "/" + field + "[" + std::to_string (i) + "]"; };

                extremes.parameters[leaf ("fp")] = number (object % 2 == 0 ? 0.0 : 1.0);
                extremes.parameters[leaf ("fz")] = number (object % 2 == 0 ? 1.0 : 0.0);
                extremes.parameters[leaf ("qp")] = number (1.0);
                extremes.parameters[leaf ("qz")] = number (1.0);
                extremes.parameters[leaf ("gain")] = number (1.0);
                extremes.parameters[leaf ("types")] = integerValue (i % 7);
            }

        const auto samples = renderDocument (extremes);
        auto finite = true;

        for (const auto sample : samples)
            finite = finite && std::isfinite (sample);

        CHECK (finite);
        CHECK (rmsOf (samples) >= 0.0);
    }

    // The plane edits through its own gesture, and the edit reaches the document and the engine. The drag
    // is synthesised exactly as the mouse delivers it: down on the pole marker, then drag right.
    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    // The plane lives inside its filter's panel on the composed page (ticket `52`), so it has its rectangle
    // as soon as the editor is laid out; the selection is what puts the section the user is working in on
    // the strip (ticket `59`: the whole page is on screen at every size, so there is no scroll to it).
    const auto filterSection = editor->sectionForModule ("VoiceFilter0");
    REQUIRE (filterSection >= 0);
    editor->selectSection (filterSection);

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

//==============================================================================
// Ticket `55`: moving through the bank — and, at every step, **what changed when the thing was removed**.
//
// The failure this case is built to catch is a browser that is *decorative*: a list that highlights a row
// without loading anything, a search that filters a copy of the names and not the thing that plays, a
// "previous" that walks a cursor nothing reads. Every assertion below is therefore a **difference** — the
// loaded document before and after, the audio the engine produces before and after, the render of the
// window with the browser open against the same window with it shut — because each of those fails if the
// browser is a picture and passes if it is a mechanism, and no assertion that merely *describes* the panel
// (104 rows, eight categories, a highlighted row) can tell the two apart.

namespace
{

/// The manifest, read here with **JUCE's** parser rather than with the reader the browser itself uses.
/// The point of the check is that the browser's list is measured against the file, not against the
/// browser's own opinion of the file: two readers that disagreed would otherwise agree with each other.
struct ManifestRow
{
    juce::String name;
    juce::String category;
    juce::String path;
};

std::vector<ManifestRow> readManifestRows (const juce::File& root, std::vector<juce::String>& categories)
{
    std::vector<ManifestRow> rows;

    const auto manifest = root.getChildFile (juce::String (std::string (xerum::presets::kBankDirectory)))
                              .getChildFile (juce::String (std::string (xerum::presets::kBankManifestName)));

    if (! manifest.existsAsFile())
        return rows;

    const auto parsed = juce::JSON::parse (manifest.loadFileAsString());
    const auto* object = parsed.getDynamicObject();

    if (object == nullptr)
        return rows;

    if (const auto* list = object->getProperty ("categories").getArray())
        for (const auto& category : *list)
            categories.push_back (category.toString());

    if (const auto* list = object->getProperty ("presets").getArray())
        for (const auto& entry : *list)
            if (const auto* preset = entry.getDynamicObject())
                rows.push_back ({ preset->getProperty ("name").toString(),
                                  preset->getProperty ("category").toString(),
                                  preset->getProperty ("path").toString() });

    return rows;
}

/// What was **loaded**, as one string: every parameter of the document, and **not** its name. Leaving the
/// name out is the point — two presets that differ only in name are two presets that play the same, and
/// the digest is what would notice. So "the browser loaded a different preset" becomes a claim about the
/// document rather than about which row happened to be highlighted.
juce::String documentDigest (const model::PresetDocument& document)
{
    juce::StringArray parts;
    parts.add (juce::String (document.parameters.size()));

    for (const auto& [path, value] : document.parameters)
    {
        double number = 0.0;

        parts.add (juce::String (path) + "="
                   + (value.isText() ? juce::String (value.text)
                                     : juce::String (value.numberAsDouble (number) ? number : 0.0)));
    }

    // A stable fold of the parts. Not a cryptographic hash and not pretending to be one: it is a summary
    // of the document's *contents*, which is all this needs to be.
    std::uint64_t fold = 1469598103934665603ull;

    for (const auto& part : parts)
        for (const auto character : part)
        {
            fold ^= static_cast<std::uint64_t> (static_cast<juce::uint32> (character));
            fold *= 1099511628211ull;
        }

    return juce::String::toHexString (static_cast<juce::int64> (fold)) + "/" + juce::String (parts.size());
}

/// The design system's colours, and how much of a rendered window is exactly one of them. Printed rather
/// than asserted where antialiasing makes an exact match meaningless; asserted where it does not (that the
/// window's own background is its most common colour, and that nothing outside the palette reappears as a
/// *field* of colour).
struct RenderReport
{
    juce::File png;
    int width { 0 };
    int height { 0 };
    int palettePixels { 0 };
    int totalPixels { 0 };
    std::map<juce::uint32, int> ranked;
};

RenderReport captureRender (XerumAudioProcessorEditor& editor, const juce::String& name)
{
    RenderReport report;

    const auto image = editor.createComponentSnapshot (editor.getLocalBounds());
    report.width = image.getWidth();
    report.height = image.getHeight();
    report.totalPixels = report.width * report.height;

    auto directory = juce::File (juce::String (XERUM_BUILD_DIR)).getChildFile ("browser-evidence");
    directory.createDirectory();
    report.png = directory.getChildFile (name + ".png");

    juce::PNGImageFormat png;

    if (auto stream = std::unique_ptr<juce::FileOutputStream> (report.png.createOutputStream()))
        png.writeImageToStream (image, *stream);

    const auto palette = ui::look::palette();

    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
        {
            const auto pixel = image.getPixelAt (x, y);
            ++report.ranked[pixel.getARGB() & 0x00ffffffu];

            if (std::find (palette.begin(), palette.end(), pixel) != palette.end())
                ++report.palettePixels;
        }

    return report;
}

void printRender (const char* what, const RenderReport& report)
{
    std::vector<std::pair<juce::uint32, int>> ranked (report.ranked.begin(), report.ranked.end());
    std::sort (ranked.begin(), ranked.end(),
               [] (const auto& a, const auto& b) { return a.second > b.second; });

    std::fprintf (stderr, "\n[browser] %s: %dx%d -> %s\n", what, report.width, report.height,
                  report.png.getFullPathName().toRawUTF8());
    std::fprintf (stderr, "[browser]   %d distinct colours; exactly-a-palette-token %d px (%.2f %%)\n",
                  static_cast<int> (ranked.size()), report.palettePixels,
                  100.0 * report.palettePixels / juce::jmax (1, report.totalPixels));

    for (std::size_t i = 0; i < ranked.size() && i < 8; ++i)
    {
        const auto key = ranked[i].first;
        const auto token = [&key]
        {
            for (const auto colour : ui::look::palette())
                if ((colour.getARGB() & 0x00ffffffu) == key)
                    return " (a palette token)";

            return "";
        }();

        std::fprintf (stderr, "[browser]   rgb(%3u,%3u,%3u) %6.2f %% of the window%s\n", (key >> 16) & 0xffu,
                      (key >> 8) & 0xffu, key & 0xffu, 100.0 * ranked[i].second / report.totalPixels, token);
    }
}

/// A real click, through the component's own mouse handling — the same construction the pole/zero case
/// uses, so the gesture is the one a mouse makes rather than a call to the callback behind it.
void clickComponent (juce::Component& component, const juce::Point<int> at)
{
    const juce::MouseEvent event (juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(),
                                  juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component,
                                  juce::Time::getCurrentTime(), at.toFloat(),
                                  juce::Time::getCurrentTime(), 1, false);
    component.mouseDown (event);
}

/// The bank this machine resolves, or a null file. Every case below needs it.
juce::File bankRoot (XerumAudioProcessor& processor)
{
    for (const auto& root : processor.contentRoots())
    {
        const auto candidate = juce::File (root);

        if (candidate.getChildFile (juce::String (std::string (xerum::presets::kBankDirectory)))
                .getChildFile (juce::String (std::string (xerum::presets::kBankManifestName)))
                .existsAsFile())
            return candidate;
    }

    return {};
}

void printBrowser (XerumAudioProcessorEditor& editor)
{
    const auto& browser = editor.browser();
    const auto& catalog = editor.bankCatalog();

    const auto filter = browser.categoryIndex() < 0
                            ? std::string ("All")
                            : catalog.categories[static_cast<std::size_t> (browser.categoryIndex())];

    std::fprintf (stderr,
                  "\n[browser] window %dx%d; the browser covers %dx%d at %d,%d; %d presets, %d "
                  "categories; filter %s, %d visible of %d, cursor %d, query \"%s\"\n",
                  editor.getWidth(), editor.getHeight(), browser.getWidth(),
                  browser.getHeight(), browser.getX(), browser.getY(), catalog.size(),
                  static_cast<int> (catalog.categories.size()), filter.c_str(), browser.visibleCount(),
                  catalog.size(), browser.selectedIndex(), browser.search().toRawUTF8());

    const auto rect = [] (const juce::Rectangle<int>& r)
    {
        return juce::String (r.getWidth()) + "x" + juce::String (r.getHeight()) + " @" + juce::String (r.getX())
               + "," + juce::String (r.getY());
    };

    std::fprintf (stderr, "[browser]   heading  %s\n", rect (browser.headingBounds()).toRawUTF8());
    std::fprintf (stderr, "[browser]   search   %s\n", rect (browser.searchBounds()).toRawUTF8());

    for (int index = -1; index < static_cast<int> (catalog.categories.size()); ++index)
        std::fprintf (stderr, "[browser]   pill %-12s %s%s\n",
                      index < 0 ? "All" : catalog.categories[static_cast<std::size_t> (index)].c_str(),
                      rect (browser.categoryBounds (index)).toRawUTF8(),
                      index == browser.categoryIndex() ? "   <- selected" : "");

    std::fprintf (stderr, "[browser]   list     %s, %d rows of %dpx, first visible row %d\n",
                  rect (browser.listBounds()).toRawUTF8(), browser.rowsVisible(), browser.rowHeight(),
                  browser.firstVisibleRow());

    for (int row = 0; row < browser.visibleCount() && row < 3; ++row)
        std::fprintf (stderr, "[browser]     row %d  %-22s  %s\n", row,
                      rect (browser.rowBounds (row)).toRawUTF8(),
                      catalog.presets[static_cast<std::size_t> (browser.visibleRows()[
                          static_cast<std::size_t> (row)])].name.c_str());

    if (browser.selectedIndex() >= 0 && browser.selectedIndex() < catalog.size())
        std::fprintf (stderr, "[browser]   cursor is bank slot %d \"%s\" of %s (%s)\n",
                      browser.selectedIndex(),
                      catalog.presets[static_cast<std::size_t> (browser.selectedIndex())].name.c_str(),
                      filter.c_str(),
                      catalog.presets[static_cast<std::size_t> (browser.selectedIndex())].path.c_str());

    std::fprintf (stderr, "[browser]   footer   %s\n", rect (browser.footerBounds()).toRawUTF8());
}

} // namespace

//==============================================================================
TEST_CASE ("the browser offers the manifest's bank and nothing else, and every entry it offers loads",
           "[interface][ui][browser]")
{
    // Ticket `55`'s first half, as measurements against the **file** and the **loader**:
    //
    //   1. the list is the manifest's list — same order, same names, same categories — read here by a
    //      second parser, so the browser cannot be wrong about the bank in a way that agrees with itself;
    //   2. every entry it can offer is a `.xerumpreset` under `Factory/`, and so the import path's
    //      `.SerumPreset` cannot appear in it: the two doors do not open onto the same list;
    //   3. stepping the whole bank loads the whole bank — 104 loads, every one through
    //      `loadPresetFromFile`, every one a document that names the manifest's own preset, and **no two
    //      the same document** (a hundred presets that load to one sound is the failure the bank's own
    //      test exists to catch, and this is where a browser could reintroduce it by loading the wrong
    //      file);
    //   4. and the render changes — the audition, as far as arithmetic can reach it.
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    const auto& catalog = editor->bankCatalog();

    INFO ("bank: " << catalog.error);
    REQUIRE (catalog.ok);
    REQUIRE (catalog.size() > 0);

    const auto root = bankRoot (processor);
    REQUIRE (root != juce::File());

    std::vector<juce::String> categories;
    const auto rows = readManifestRows (root, categories);

    INFO ("manifest: " << rows.size() << " presets, " << categories.size() << " categories");
    REQUIRE_FALSE (rows.empty());

    // ---- 1. the list is the manifest's list ---------------------------------------------------------
    CHECK (catalog.size() == static_cast<int> (rows.size()));
    CHECK (static_cast<int> (catalog.categories.size()) == static_cast<int> (categories.size()));

    for (std::size_t index = 0; index < categories.size(); ++index)
        CHECK (catalog.categories[index] == categories[index].toStdString());

    for (std::size_t index = 0; index < rows.size() && index < static_cast<std::size_t> (catalog.size()); ++index)
    {
        const auto& entry = catalog.presets[index];

        INFO ("preset " << index << ": " << entry.name);
        CHECK (juce::String (entry.name) == rows[index].name);
        CHECK (juce::String (entry.category) == rows[index].category);
        CHECK (juce::String (entry.path) == rows[index].path);
    }

    // **The import path cannot appear in the browser**, as a property of the list rather than of the code
    // that built it: every entry is the factory bank's own extension below the bank's own directory.
    for (const auto& entry : catalog.presets)
    {
        INFO ("path: " << entry.path);

        CHECK (juce::String (entry.path).startsWith (juce::String (std::string (xerum::presets::kBankDirectory))
                                                      + "/"));
        CHECK (juce::String (entry.path).endsWith (juce::String (std::string (xerum::presets::kPresetExtension))));
        CHECK_FALSE (juce::String (entry.path).endsWith (".SerumPreset"));

        // And the file is really there: a manifest entry with no bytes behind it is a browser row that
        // cannot load, which is worse than a browser with no row.
        CHECK (catalog.fileFor (static_cast<int> (&entry - catalog.presets.data())).existsAsFile());
    }

    // ---- the two doors ------------------------------------------------------------------------------
    //
    // xerum has exactly one way in for a user's **own** preset, and it is not this one. Spelled three
    // ways, all read from the same window: the import button names the **reference's** extension; the
    // browser's list contains none of it (asserted above, over the whole list, as a property of every
    // entry); and — the one a user actually sees — the header field says where a factory preset is filed
    // and says nothing at all for a preset that arrived through the other door. The last is measured by
    // loading a `.SerumPreset` and reading the field back.
    {
        CHECK (editor->importButton().getButtonText().contains (".SerumPreset"));

        REQUIRE (editor->loadBankPreset (3));
        CHECK (editor->presetField().factoryCategory() == juce::String (catalog.presets[3].category));
        CHECK (editor->bankIndex() == 3);

        const auto imported = writeFixture (halfLoadingDocument());
        REQUIRE (processor.loadPresetFromFile (imported).wasOk());
        processor.sendSynchronousChangeMessage();

        std::fprintf (stderr, "\n[browser] after importing %s: the field reads \"%s\", bank slot %d\n",
                      imported.getFileName().toRawUTF8(), editor->presetField().displayText().toRawUTF8(),
                      editor->bankIndex());

        CHECK (editor->bankIndex() == -1);
        CHECK (editor->presetField().factoryCategory().isEmpty());
        CHECK (editor->presetField().displayText() == juce::String ("interface fixture"));
        CHECK_FALSE (editor->presetField().displayText().contains ("\xc2\xb7"));
        CHECK (editor->browser().selectedIndex() == -1);
    }

    // ---- 2. the browser's own list, before any filter ------------------------------------------------
    CHECK (editor->browser().visibleCount() == catalog.size());
    CHECK (editor->browser().categoryIndex() == -1);

    for (int row = 0; row < editor->browser().visibleCount(); ++row)
        CHECK (editor->browser().visibleRows()[static_cast<std::size_t> (row)] == row);

    // ---- 3. stepping the whole bank loads the whole bank --------------------------------------------
    //
    // Every load goes through the product's own loader, and every loaded document is captured: the set of
    // distinct documents is the measurement that says the browser moved the *engine* and not a cursor.
    {
        std::set<juce::String> digests;
        std::set<juce::String> names;
        auto failures = 0;

        for (int index = 0; index < catalog.size(); ++index)
        {
            INFO ("bank slot " << index << ": " << catalog.presets[static_cast<std::size_t> (index)].path);

            if (! editor->loadBankPreset (index))
            {
                ++failures;
                continue;
            }

            const auto document = processor.document();
            digests.insert (documentDigest (document));
            names.insert (juce::String (document.name));

            // The document names the preset the manifest named — the loader read the file it was handed.
            CHECK (juce::String (document.name)
                   == juce::String (catalog.presets[static_cast<std::size_t> (index)].name));

            // And the interface agrees about which slot that was: the header says the category, and the
            // browser's cursor is on the row.
            CHECK (editor->bankIndex() == index);
            CHECK (editor->presetField().factoryCategory()
                   == juce::String (catalog.presets[static_cast<std::size_t> (index)].category));
        }

        CHECK (failures == 0);
        std::fprintf (stderr, "\n[browser] %d of %d bank slots loaded; %zu distinct documents, %zu distinct names\n",
                      catalog.size() - failures, catalog.size(), digests.size(), names.size());
        CHECK (static_cast<int> (digests.size()) == catalog.size());
    }

    // ---- 4. and the render changes ------------------------------------------------------------------
    //
    // Nobody here can hear a preset, and nothing in a binary can say whether one sounds good. What can be
    // said is that **the sound is not the same sound**: two different bank presets render differently
    // through the engine the interface loaded them into.
    {
        REQUIRE (editor->loadBankPreset (0));
        const auto first = render (processor, 16, 512, noteThenRelease (60, 12));

        REQUIRE (editor->loadBankPreset (catalog.size() / 2));
        const auto second = render (processor, 16, 512, noteThenRelease (60, 12));

        const auto difference = channelDifference (first, second);
        std::fprintf (stderr, "[browser] slot 0 against slot %d: channel difference %.4f\n", catalog.size() / 2,
                      difference);

        CHECK (difference > 0.01);
    }
}

//==============================================================================
TEST_CASE ("the bank browses by category, by step and by search, and the keyboard does all of it",
           "[interface][ui][browser]")
{
    // Ticket `55`'s second half. Each assertion below is a **difference**: what moved when the gesture
    // happened, measured on the loaded document rather than on the panel. A browser that highlights rows
    // and loads nothing passes every description of itself and fails every one of these.
    const juce::ScopedJuceInitialiser_GUI gui;

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto editor = openEditor (processor);
    REQUIRE (editor != nullptr);

    const auto& catalog = editor->bankCatalog();
    REQUIRE (catalog.ok);

    // The manifest, read once here by this test's own parser and shared by the blocks below: what "the
    // second category" and "the query" are is taken from the bank rather than written down in this file,
    // so the case keeps meaning something when the bank changes.
    std::vector<juce::String> categories;
    const auto rows = readManifestRows (bankRoot (processor), categories);
    REQUIRE_FALSE (rows.empty());
    REQUIRE (static_cast<int> (rows.size()) == catalog.size());

    /// A query that matches exactly one preset, **found in the bank** rather than written here: the
    /// longest name no other name contains.
    const auto uniqueName = [&rows]
    {
        juce::String best;

        for (const auto& row : rows)
        {
            auto matches = 0;

            for (const auto& other : rows)
                if (other.name.contains (row.name))
                    ++matches;

            if (matches == 1 && row.name.length() > best.length())
                best = row.name;
        }

        return best;
    }();

    INFO ("bank: " << rows.size() << " presets, " << categories.size() << " categories, unique query \""
                   << uniqueName << "\"");
    REQUIRE (uniqueName.isNotEmpty());

    auto& browser = editor->browser();
    const auto slotName = [&catalog] (const int index)
    { return juce::String (catalog.presets[static_cast<std::size_t> (index)].name); };
    const auto loaded = [&processor] { return juce::String (processor.document().name); };

    // The window the host opens: the page fits it, and the browser does not change that.
    CHECK (editor->getWidth() == 1080);
    CHECK (editor->compositionHeight() == editor->compositionArea().getHeight());

    const auto pageClosed = editor->compositionHeight();

    editor->setBrowserOpen (true);
    REQUIRE (editor->browserOpen());
    REQUIRE (browser.getWidth() > 0);

    // **Counterfactual: the fit.** The browser is a surface over the page, so the page is exactly as tall
    // with it open as with it shut — at both sizes, and measured **at each size** rather than across them.
    // Ticket `53` named the minimum window as the one that scrolled; ticket `59` removed scrolling entirely
    // (the page is laid out at the size of the space it is given and scaled with it), so what this block
    // measures now is the page's *height* being untouched by the browser at each size — which is the claim
    // the ticket is about, and the one the overlay's own cost depends on.
    {
        const auto pageAtDefaultOpen = editor->compositionHeight();
        editor->setBrowserOpen (false);
        const auto pageAtDefaultShut = editor->compositionHeight();
        editor->setBrowserOpen (true);

        CHECK (pageAtDefaultOpen == pageAtDefaultShut);
        CHECK (pageAtDefaultOpen == pageClosed);

        const auto size = juce::Point<int> (editor->getWidth(), editor->getHeight());

        // A window a host has forced off the ratio: the canvas is letterboxed, and the page's own height —
        // and the browser's effect on it — is the same fact as at the ratio.
        editor->setSize (900, 600);
        const auto pageAtMinimumOpen = editor->compositionHeight();
        editor->setBrowserOpen (false);
        const auto pageAtMinimumShut = editor->compositionHeight();
        editor->setBrowserOpen (true);

        editor->setSize (size.x, size.y);

        std::fprintf (stderr,
                      "\n[browser] the composed page: %d at %dx%d and %d at 900x600, each measured with "
                      "the browser open and with it shut\n",
                      pageAtDefaultOpen, size.x, size.y, pageAtMinimumOpen);

        CHECK (pageAtMinimumOpen == pageAtMinimumShut);
        CHECK (editor->compositionHeight() == pageClosed);

        // The header: the bank's arrows are **inside** the preset field's own width, so the wordmark, the
        // coverage readout and the two right-hand controls kept ticket `53`'s rectangles.
        const auto& header = editor->headerLayout();

        std::fprintf (stderr,
                      "\n[browser] header at %dx%d: wordmark %d, preset %dx%d, coverage %d, import %d, "
                      "status %d\n",
                      editor->getWidth(), editor->getHeight(), header.wordmark.getWidth(),
                      header.preset.getWidth(), header.preset.getHeight(), header.coverage.getWidth(),
                      header.import.getWidth(), header.status.getWidth());

        CHECK (header.preset.getWidth() == 240);
        CHECK (header.preset.getHeight() == 22);
        CHECK (header.coverage.getWidth() == 278);   // 1060 - 84 - 6 - 240 - (168 + 8 + 260 + 24)
        CHECK (header.import.getWidth() == 168);
        CHECK (header.status.getWidth() == 260);

        // The arrows are within the field, and the name does not run under them.
        const auto& field = editor->presetField();

        CHECK (field.getLocalBounds().contains (field.arrowBounds (-1)));
        CHECK (field.getLocalBounds().contains (field.arrowBounds (1)));
        CHECK (field.textBounds().getRight() <= field.arrowBounds (-1).getX());
    }

    // ---- the field's own arrows, through a real click ------------------------------------------------
    //
    // The header's ▶ is what a mouse uses; `[` and `]` are what the keyboard uses. Both are the same
    // gesture, so a click must move the bank exactly as a key does.
    {
        REQUIRE (editor->loadBankPreset (10));
        REQUIRE (editor->bankIndex() == 10);

        const auto before = loaded();
        clickComponent (editor->presetField(), editor->presetField().arrowBounds (1).getCentre());

        CHECK (editor->bankIndex() == 11);
        CHECK (loaded() == slotName (11));
        CHECK (loaded() != before);
        CHECK (editor->presetField().displayText()
               == juce::String (catalog.presets[11].category) + " \xc2\xb7 " + slotName (11));

        clickComponent (editor->presetField(), editor->presetField().arrowBounds (-1).getCentre());
        CHECK (editor->bankIndex() == 10);
    }

    // ---- previous and next, and the ends of the bank --------------------------------------------------
    //
    // "Hold one key and hear them change" is the whole point, and the ends of the bank are where a
    // cursor-only implementation gives up. The bank is a **ring**: forward from the last sound lands on
    // the first and backward from the first lands on the last, so stepping never stops the audition.
    {
        REQUIRE (editor->loadBankPreset (0));

        CHECK (editor->keyPressed (juce::KeyPress ('[')));        // backwards off the front ...
        CHECK (editor->bankIndex() == catalog.size() - 1);        // ... lands on the last preset
        CHECK (loaded() == slotName (catalog.size() - 1));

        CHECK (editor->keyPressed (juce::KeyPress (']')));        // ... and forwards off the end
        CHECK (editor->bankIndex() == 0);
        CHECK (loaded() == slotName (0));

        // And one step back from the last is the one *before* it — a step, not a wrap. The two are
        // different gestures and the count says which happened.
        REQUIRE (editor->loadBankPreset (catalog.size() - 1));
        editor->stepPreset (1);
        CHECK (editor->bankIndex() == 0);

        REQUIRE (editor->loadBankPreset (catalog.size() - 1));
        editor->stepPreset (-1);
        CHECK (editor->bankIndex() == catalog.size() - 2);
    }

    // ---- the browser's own keys ---------------------------------------------------------------------
    {
        REQUIRE (editor->loadBankPreset (5));
        const auto before = loaded();

        CHECK (browser.keyPressed (juce::KeyPress (juce::KeyPress::downKey)));
        CHECK (loaded() == slotName (6));
        CHECK (loaded() != before);

        CHECK (browser.keyPressed (juce::KeyPress (juce::KeyPress::upKey)));
        CHECK (loaded() == slotName (5));

        // `[` and `]` are handed on rather than treated as search text, so the gesture works with the
        // browser open and focused — which is where a keyboard user is while browsing.
        CHECK_FALSE (browser.keyPressed (juce::KeyPress (']')));
        CHECK (editor->keyPressed (juce::KeyPress (']')));
        CHECK (loaded() == slotName (6));
        CHECK (browser.search().isEmpty());
    }

    // ---- categories, the first cut ------------------------------------------------------------------
    {
        // The manifest's own order, and the manifest's own count: the second category, whatever it is.
        REQUIRE (categories.size() > 2);

        const auto wanted = categories[2];

        auto expected = 0;
        auto firstSlot = -1;
        auto lastSlot = -1;

        for (int index = 0; index < catalog.size(); ++index)
            if (juce::String (catalog.presets[static_cast<std::size_t> (index)].category) == wanted)
            {
                ++expected;

                if (firstSlot < 0)
                    firstSlot = index;

                lastSlot = index;
            }

        REQUIRE (expected > 0);

        // Two steps right from `All` is the second category.
        browser.setCategoryIndex (-1, false);
        CHECK (browser.categoryIndex() == -1);
        CHECK (browser.visibleCount() == catalog.size());

        CHECK (browser.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
        CHECK (browser.categoryIndex() == 0);
        CHECK (browser.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));

        CHECK (browser.categoryIndex() == 1);
        CHECK (juce::String (catalog.categories[1].c_str()) == categories[1]);

        browser.setCategoryIndex (2);
        CHECK (juce::String (catalog.categories[2].c_str()) == wanted);
        CHECK (browser.visibleCount() == expected);

        // The rows are the manifest's own subsequence, and the count is the one this test counted from
        // the file rather than the one the browser reported.
        for (int row = 0; row < browser.visibleCount(); ++row)
        {
            const auto slot = browser.visibleRows()[static_cast<std::size_t> (row)];
            CHECK (juce::String (catalog.presets[static_cast<std::size_t> (slot)].category) == wanted);
        }

        CHECK (browser.visibleRows().front() == firstSlot);
        CHECK (browser.visibleRows().back() == lastSlot);

        // And a selection inside a category is a load: the filter changes what is offered, not what the
        // gesture means.
        CHECK (editor->bankIndex() == firstSlot);
        CHECK (loaded() == slotName (firstSlot));

        // Down to the end of the category and one more: it wraps **within the category**, because the
        // category is the cut and stepping must not leave it.
        for (int step = 1; step < expected; ++step)
            browser.keyPressed (juce::KeyPress (juce::KeyPress::downKey));

        CHECK (editor->bankIndex() == lastSlot);
        CHECK (loaded() == slotName (lastSlot));

        browser.keyPressed (juce::KeyPress (juce::KeyPress::downKey));
        CHECK (editor->bankIndex() == firstSlot);

        // Left from `All` is the last category — the ring again, at the level of the cut.
        browser.setCategoryIndex (-1);
        CHECK (browser.keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));
        CHECK (browser.categoryIndex() == static_cast<int> (catalog.categories.size()) - 1);
    }

    // ---- search -------------------------------------------------------------------------------------
    {
        // A query taken **from the bank** rather than written down: the first word of a name, and the
        // expectation counted from the manifest by this test.
        const auto query = rows[7].name.upToFirstOccurrenceOf (" ", false, false).toLowerCase();
        REQUIRE (query.isNotEmpty());

        auto expected = 0;

        for (const auto& row : rows)
            if (row.name.toLowerCase().contains (query))
                ++expected;

        REQUIRE (expected > 0);

        browser.setCategoryIndex (-1);
        browser.setSearch (query);
        CHECK (browser.visibleCount() == expected);

        for (const auto slot : browser.visibleRows())
            CHECK (juce::String (catalog.presets[static_cast<std::size_t> (slot)].name).toLowerCase().contains (query));

        std::fprintf (stderr, "[browser] query \"%s\": %d of %d rows, cursor on \"%s\"\n",
                      query.toRawUTF8(), browser.visibleCount(), catalog.size(),
                      editor->bankIndex() >= 0 ? loaded().toRawUTF8() : "-");

        // **Typing** is the same path as `setSearch`: the character keys go through `keyPressed`.
        browser.setSearch ({});
        for (const auto character : query.toUpperCase())
        {
            const juce::KeyPress key (static_cast<int> (character), juce::ModifierKeys(), character);
            CHECK (browser.keyPressed (key));
        }

        CHECK (browser.search() == query.toUpperCase());
        CHECK (browser.visibleCount() == expected);
    }

    // ---- a query that matches one thing loads that thing --------------------------------------------
    {
        // The unique query was derived above from the bank itself, so the expectation is the manifest's
        // and not a number written here.
        const auto& name = uniqueName;

        browser.setSearch (name);
        CHECK (browser.visibleCount() == 1);
        CHECK (browser.selectedIndex() == catalog.indexOfName (name));
        CHECK (loaded() == name);
        CHECK (editor->bankIndex() == catalog.indexOfName (name));

        // And the engine really moved: the document loaded is the queried preset's, not the one that was
        // playing when the query was typed.
        CHECK (editor->presetField().displayText()
               == juce::String (catalog.presets[static_cast<std::size_t> (editor->bankIndex())].category)
                      + " \xc2\xb7 " + name);
    }

    // ---- a query that matches nothing shows nothing -------------------------------------------------
    //
    // The counterfactual the failure mode demands: a filter that fails open (everything shown) is worse
    // than no filter, because it silently offers the wrong hundred presets under a query.
    {
        const auto before = documentDigest (processor.document());

        browser.setSearch ("zz-no-such-preset-zz");

        CHECK (browser.visibleCount() == 0);
        CHECK (browser.visibleRows().empty());
        CHECK (browser.rowBounds (0).isEmpty());

        // Nothing was loaded, and the row that was selected is still selected: a query that matches
        // nothing must not silently move the sound.
        CHECK (documentDigest (processor.document()) == before);
        CHECK (browser.selectedIndex() == editor->bankIndex());
    }

    // ---- Escape closes ------------------------------------------------------------------------------
    {
        browser.setSearch ({});
        CHECK (editor->browserOpen());

        CHECK (browser.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
        CHECK_FALSE (editor->browserOpen());
    }

    // ---- the two surfaces over the page do not fight -------------------------------------------------
    //
    // The browser is the topmost child of the editor, so a report drawer opened *under* it would look like
    // nothing had happened — the one defect an overlay introduces that no rectangle check would notice.
    // Asking for either surface is asking for it to be the one on screen.
    {
        editor->setBrowserOpen (true);
        CHECK (editor->browserOpen());

        editor->setReportDrawerOpen (true);
        CHECK (editor->reportDrawerOpen());
        CHECK_FALSE (editor->browserOpen());

        editor->setBrowserOpen (true);
        CHECK (editor->browserOpen());
        CHECK_FALSE (editor->reportDrawerOpen());

        editor->setBrowserOpen (false);
    }

    // ---- the render, at the steps -------------------------------------------------------------------
    //
    // The window as it is drawn, at four moments: the browser at rest, the browser stepped to the end of
    // the bank, a search narrowed to one row, and a search narrowed to none. Written out as PNGs and
    // summarised, because this session has no eyes and the drawing is the part of the acceptance that
    // cannot be asserted in numbers alone.
    {
        editor->setSize (editor->nominalSize().x, editor->nominalSize().y);
        editor->setBrowserOpen (true);
        browser.setCategoryIndex (-1);
        browser.setSearch ({});
        REQUIRE (editor->loadBankPreset (0));

        // **The whole bank, by key, to the end and back** — the audition the ticket is about, driven the
        // way a keyboard user drives it, with every single step checked: a step that highlighted a row
        // without loading would pass a description of the panel and fail right here, at the row it did it
        // on.
        for (int index = 1; index < catalog.size(); ++index)
        {
            INFO ("stepping forward to slot " << index);
            editor->keyPressed (juce::KeyPress (']'));
            CHECK (loaded() == slotName (index));
        }

        for (int index = catalog.size() - 2; index >= 0; --index)
        {
            INFO ("stepping back to slot " << index);
            editor->keyPressed (juce::KeyPress ('['));
            CHECK (loaded() == slotName (index));
        }

        CHECK (editor->bankIndex() == 0);
        std::fprintf (stderr, "\n[browser] drove the bank from slot 0 to slot %d and back by key: %d steps, "
                              "each one a load\n", catalog.size() - 1, 2 * (catalog.size() - 1));

        printBrowser (*editor);
        printRender ("the browser, on the first preset of the bank", captureRender (*editor, "browser-first"));

        // The same window with the browser **shut**. Everything outside the browser's rectangle must be
        // pixel-identical: the browser is a surface over the page, and if it were painting anywhere else —
        // or if opening it moved anything — these two renders would differ.
        const auto open = editor->createComponentSnapshot (editor->getLocalBounds());
        const auto browserBounds = browser.getBounds();

        // **And the drawing went where the rectangles say it went.** This session has no eyes, so "the
        // panel is drawn" is a measurement: a point inside each rectangle — chosen where no glyph is —
        // must carry exactly the fill that part of the panel is made of. The browser's own coordinates are
        // offset by its position in the window, which is itself one of the numbers being checked: if the
        // panel were drawn somewhere other than `browserBounds`, these samples would land on the page.
        {
            const auto accentRow = ui::look::surface.overlaidWith (ui::look::accentBg);
            const auto at = [&open, &browserBounds] (const juce::Rectangle<int>& r, const int dx)
            {
                return open.getPixelAt (browserBounds.getX() + r.getX() + dx,
                                        browserBounds.getY() + r.getCentreY());
            };
            // JUCE's own `PixelARGB::blend` rounds where `Colour::overlaidWith` truncates, so a composited
            // fill is allowed to be one unit off per channel; a plain fill is not allowed to be off at all.
            const auto near = [] (const juce::Colour a, const juce::Colour b, const int tolerance)
            {
                return std::abs (static_cast<int> (a.getRed()) - static_cast<int> (b.getRed())) <= tolerance
                    && std::abs (static_cast<int> (a.getGreen()) - static_cast<int> (b.getGreen())) <= tolerance
                    && std::abs (static_cast<int> (a.getBlue()) - static_cast<int> (b.getBlue())) <= tolerance;
            };

            const auto cursorRow = browser.rowBounds (browser.rowOfIndex (browser.selectedIndex()));
            const auto plainRow = browser.rowBounds (browser.rowOfIndex (browser.selectedIndex()) + 1);

            REQUIRE_FALSE (cursorRow.isEmpty());
            REQUIRE_FALSE (plainRow.isEmpty());

            const auto search = at (browser.searchBounds(), 4);
            const auto cursor = at (cursorRow, 2);
            // The **selected** pill, whatever it is: `-1` is `All`, which is where a fresh panel is.
            const auto pill = at (browser.categoryBounds (browser.categoryIndex()), 4);
            const auto plain = at (plainRow, 2);

            std::fprintf (stderr,
                          "[browser] the render at the rectangles: search field %s, a plain row %s, the "
                          "cursor's row %s, the selected pill %s (accent over the panel is %s)\n",
                          search.toDisplayString (false).toRawUTF8(), plain.toDisplayString (false).toRawUTF8(),
                          cursor.toDisplayString (false).toRawUTF8(), pill.toDisplayString (false).toRawUTF8(),
                          accentRow.toDisplayString (false).toRawUTF8());

            CHECK (near (search, ui::look::bg, 0));         // the field's own fill, exactly
            CHECK (near (plain, ui::look::surface, 0));     // the panel's fill, one row down
            CHECK (near (cursor, accentRow, 1));            // the cursor's row: the accent over the panel
            CHECK (near (pill, accentRow, 1));              // the selected pill: the same tint
            CHECK (cursor != plain);                        // and the row *is* told from the plain one
        }

        editor->setBrowserOpen (false);
        const auto shut = editor->createComponentSnapshot (editor->getLocalBounds());

        auto outside = 0;
        auto differing = 0;

        for (int y = 0; y < open.getHeight() && y < shut.getHeight(); ++y)
            for (int x = 0; x < open.getWidth() && x < shut.getWidth(); ++x)
                if (! browserBounds.contains (x, y))
                {
                    ++outside;

                    if (open.getPixelAt (x, y) != shut.getPixelAt (x, y))
                        ++differing;
                }

        std::fprintf (stderr, "[browser] browser-open against browser-shut: %d of %d pixels outside the "
                              "browser's own rectangle differ\n", differing, outside);

        CHECK (differing == 0);

        editor->setBrowserOpen (true);

        // Stepped to the end of the bank.
        REQUIRE (editor->loadBankPreset (catalog.size() - 1));
        printBrowser (*editor);
        printRender ("the browser, on the last preset of the bank", captureRender (*editor, "browser-last"));

        // A search narrowed to one row.
        browser.setSearch (uniqueName);
        CHECK (browser.visibleCount() == 1);
        printBrowser (*editor);
        printRender ("the browser, searched to a single match", captureRender (*editor, "browser-one"));

        // And to none.
        browser.setSearch ("zz-no-such-preset-zz");
        CHECK (browser.visibleCount() == 0);
        printBrowser (*editor);
        printRender ("the browser, searched to nothing", captureRender (*editor, "browser-none"));
    }
}
