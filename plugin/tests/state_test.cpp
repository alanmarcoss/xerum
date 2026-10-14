// State: what `getStateInformation` contains, what it therefore cannot round-trip, and the acceptance
// criterion that a save → reload → restore renders byte-identically.
//
// Run alone with:  ctest --test-dir build-30 -R xerum-plugin-tests  (or the binary with "[state]")

#include "TestSupport.h"

#include "PluginIdentity.h"
#include "cbor.h"
#include "container.h"

#include <catch2/catch_approx.hpp>

#include <cstring>

using namespace xerum::plugin;
using namespace xerum::plugin::tests;

namespace
{

xerum::model::PresetDocument fixtureDocument()
{
    xerum::model::PresetDocument document;
    document.name = "fixture";
    document.schemaVersion = "4.0";

    // Engine parameters, including one discrete that has to survive as the token the preset wrote.
    document.parameters["Oscillator0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Oscillator0/plainParams/kParamVolume"] = number (0.375);
    document.parameters["Oscillator0/plainParams/kParamPitch"] = number (-3.0);
    document.parameters["Global0/plainParams/kParamPolyCount"] = number (8.0);
    document.parameters["Env0/plainParams/kParamAttack"] = number (0.02);

    // `Phase12P` and `P12` are two names the engine builds identically, so the bridge collapses them
    // onto one choice. The token the preset used must still be what a save writes.
    document.parameters["VoiceFilter0/plainParams/kParamType"] = text ("Phase12P");
    document.parameters["VoiceFilter0/plainParams/kParamEnable"] = number (1.0);

    // A warp the engine does not implement: the document keeps the name, the report names it, and the
    // state must not quietly replace it with a neighbour.
    document.parameters["Oscillator0/WTOsc0/plainParams/kParamWarpMenu"] = text ("kRemap_4");

    // A parameter the schema does not describe at all — ticket 23 keeps it, and so must the state.
    document.parameters["Oscillator0/WTOsc0/plainParams/kParamFromTheFuture"] = number (7.0);

    document.assetReferences.push_back ("/Analog/Basic Shapes.wav");

    return document;
}

xerum::model::ImportReport reportWithOneModelEntry()
{
    xerum::model::ImportReport report;
    addReportEntry (report, xerum::model::Severity::High, "Oscillator0/WTOsc0", "kRemap_4",
                    "warp family is not shipped in v1");
    return report;
}

} // namespace

TEST_CASE ("state round-trips: save, reload into a fresh instance, restore, render byte-identically",
           "[plugin][state]")
{
    const auto document = fixtureDocument();

    XerumAudioProcessor first;
    first.prepareToPlay (48000.0, 512);
    first.applyDocument (document, reportWithOneModelEntry(), 1);

    // A user moves one control, which is the case the merge has to represent: everything else must keep
    // the token the preset wrote, and this one must be the new value.
    first.setParameterValue ("Global0/plainParams/kParamMasterVolume", 0.5);

    juce::MemoryBlock block;
    first.getStateInformation (block);

    REQUIRE (block.getSize() > 0);

    // It is xerum's own block, and it is **not** a preset container: ticket 32 measured that a
    // document-only write of `XferJson` aborts the reference on 172 of 626 presets, so xerum's state does
    // not wear the reference's magic.
    CHECK (std::memcmp (block.getData(), identity::kStateMagic, identity::kStateMagicSize) == 0);
    CHECK (std::memcmp (block.getData(), "XferJson", std::strlen ("XferJson")) != 0);

    XerumAudioProcessor second;
    second.prepareToPlay (48000.0, 512);
    second.setStateInformation (block.getData(), static_cast<int> (block.getSize()));

    CHECK (second.lastError().isEmpty());

    const auto restored = second.document();

    // The document survived: values, the unknown parameter, and the asset reference.
    for (const auto& [path, value] : first.document().parameters)
    {
        const auto found = restored.parameters.find (path);
        REQUIRE (found != restored.parameters.end());

        if (value.isText())
            CHECK (found->second.text == value.text);
        else
        {
            double expected = 0.0;
            double actual = 0.0;
            REQUIRE (value.numberAsDouble (expected));
            REQUIRE (found->second.numberAsDouble (actual));
            CHECK (actual == Catch::Approx (expected));
        }
    }

    CHECK (restored.assetReferences == first.document().assetReferences);
    CHECK (restored.name == document.name);
    CHECK (restored.schemaVersion == document.schemaVersion);

    // The one moved control is in the state; every untouched one kept the preset's own token.
    CHECK (restored.parameters.at ("Global0/plainParams/kParamMasterVolume").real == Catch::Approx (0.5));
    CHECK (restored.parameters.at ("VoiceFilter0/plainParams/kParamType").text == "Phase12P");
    CHECK (restored.parameters.at ("Oscillator0/WTOsc0/plainParams/kParamWarpMenu").text == "kRemap_4");
    CHECK (restored.parameters.at ("Oscillator0/WTOsc0/plainParams/kParamFromTheFuture").real == Catch::Approx (7.0));

    // Every host-facing parameter reads the same on both instances.
    for (const auto& spec : first.bridge().specs())
        CHECK (first.parameterValue (spec.modelPath) == Catch::Approx (second.parameterValue (spec.modelPath)));

    // The report the user saw came back, including the plugin's own part.
    CHECK (second.reportText().contains ("kRemap_4"));
    CHECK (second.reportText().contains ("warp family is not shipped in v1"));

    // And the acceptance criterion: the same note renders to the same samples, bit for bit.
    const auto before = render (first, 48, 512, noteThenRelease (60, 24));
    const auto after = render (second, 48, 512, noteThenRelease (60, 24));

    REQUIRE (before.size() == after.size());
    CHECK (before == after);
    CHECK (peakOf (before) > 0.01);   // a control: the comparison is of something audible
}

TEST_CASE ("a state block that is not xerum's is refused, and says so without eating the patch",
           "[plugin][state]")
{
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    processor.setParameterValue ("Oscillator0/plainParams/kParamVolume", 0.25);

    const char rubbish[] = "not a state block at all, not even close to one";
    processor.setStateInformation (rubbish, static_cast<int> (sizeof (rubbish)));

    CHECK (processor.lastError().isNotEmpty());
    CHECK (processor.reportText().contains ("high"));

    // The patch survived: a refused restore must not clear what the user had.
    CHECK (processor.parameterValue ("Oscillator0/plainParams/kParamVolume") == Catch::Approx (0.25));
}

TEST_CASE ("a state block from a future format is refused rather than guessed at", "[plugin][state]")
{
    const auto document = fixtureDocument();
    auto bytes = encodeStateBlock (document, {}, 0);

    // Bump the format version, which is what a later xerum would write.
    bytes[identity::kStateMagicSize] = std::byte { identity::kStateFormat + 1 };

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.setStateInformation (bytes.data(), static_cast<int> (bytes.size()));

    CHECK (processor.lastError().contains ("format version"));
}

TEST_CASE ("the state block is deterministic, and is not a reference container", "[plugin][state]")
{
    const auto document = fixtureDocument();

    const auto first = encodeStateBlock (document, reportWithOneModelEntry(), 1);
    const auto second = encodeStateBlock (document, reportWithOneModelEntry(), 1);

    // Determinism is what makes "render byte-identically" a meaningful acceptance criterion rather than a
    // coincidence, and `encodeCbor`'s sorted keys are what provides it.
    CHECK (first == second);

    // The payload really is CBOR of a document: it decodes, and it says which format it is.
    const auto decoded = decodeStateBlock (first.data(), static_cast<int> (first.size()));
    REQUIRE (decoded.ok);
    CHECK (decoded.reportEntriesFromModel == 1);
    CHECK (decoded.report.entries.size() == 1);
}

TEST_CASE ("an empty report round-trips as empty, and the interface says so in words", "[plugin][state]")
{
    // The contract's third invariant: a clean import still produces a report — **empty, not absent** —
    // and the two must not look the same to a user.
    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    processor.applyDocument ({}, {}, 0);

    CHECK (processor.reportText().contains ("Nothing to report"));

    juce::MemoryBlock block;
    processor.getStateInformation (block);

    XerumAudioProcessor restored;
    restored.prepareToPlay (48000.0, 512);
    restored.setStateInformation (block.getData(), static_cast<int> (block.getSize()));

    CHECK (restored.reportText().contains ("Nothing to report"));
    CHECK (restored.report().entries.empty());
}
