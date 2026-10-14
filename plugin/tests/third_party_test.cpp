// Third-party pack robustness through the whole import path — ticket 42.
//
// The model layer's half is `tests/model/third_party_pack_test.cpp`. This is the half that cannot live
// there: an asset reference meets the filesystem, a table that does not resolve is substituted with
// xerum's own, and both outcomes reach the report the interface shows — and the slot renders, which is
// the claim the map's acceptance actually makes ("import survives third-party packs").
//
// Every fixture is a preset this suite **writes itself** (`writeContainer`), so the suite is meaningful
// on a fresh clone; the only file it reads is xerum's own content (`content/Tables/xerum/`, our authored
// tables — ticket 34), which ships with the build. No reference content is used here, and where a case
// would need it, it is absent from the test entirely (ticket 10).
//
// Run alone:  build-42/plugin/tests/xerum-plugin-tests "[third-party]"

#include "TestSupport.h"

#include "write.h"

#include <cstdlib>

using namespace xerum::plugin;
using namespace xerum::plugin::tests;

namespace
{

/// A content root holding exactly one file — a copy of xerum's own Sine table — with
/// `XERUM_CONTENT_ROOT` pointed at it for the object's lifetime.
///
/// The plugin's content roots are read once, in its constructor, in the contract's order (a
/// user-configured root first). A test that depended on which root happened to supply a table would be
/// testing the machine, not the code, so the user root is pinned here and restored on the way out.
struct PinnedContentRoot
{
    juce::File root;

    PinnedContentRoot()
    {
        root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getNonexistentChildFile ("xerum-third-party-content", {}, false);

        const auto tables = root.getChildFile ("Tables").getChildFile ("xerum");
        REQUIRE (tables.createDirectory().wasOk());

        const juce::File shipped (juce::String (XERUM_CONTENT_DIR) + "/Tables/xerum/Sine.wav");
        REQUIRE (shipped.existsAsFile());     // xerum's authored table ships with the build (ticket 34)
        REQUIRE (shipped.copyFileTo (tables.getChildFile ("Sine.wav")));

        setenv ("XERUM_CONTENT_ROOT", root.getFullPathName().toRawUTF8(), 1);
    }

    ~PinnedContentRoot()
    {
        unsetenv ("XERUM_CONTENT_ROOT");
        root.deleteRecursively();
    }
};

juce::File writeFixture (const xerum::model::PresetDocument& document)
{
    const auto written = xerum::model::writeContainer (document, {});
    REQUIRE (written.ok);

    auto file = juce::File::createTempFile (".SerumPreset");
    REQUIRE (file.replaceWithData (written.bytes.data(), written.bytes.size()));

    return file;
}

/// A small preset that makes a sound: one enabled wavetable slot, an amplitude envelope, a filter.
xerum::model::PresetDocument engineDocument()
{
    xerum::model::PresetDocument document;
    document.name = "third-party fixture";
    document.schemaVersion = "4.0";
    document.parameters["Oscillator0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Oscillator0/plainParams/kParamVolume"] = number (0.5);
    document.parameters["Global0/plainParams/kParamPolyCount"] = number (6.0);
    document.parameters["Env0/plainParams/kParamAttack"] = number (0.02);
    document.parameters["Env0/plainParams/kParamRelease"] = number (0.05);
    document.parameters["VoiceFilter0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["VoiceFilter0/plainParams/kParamFreq"] = number (0.4);
    return document;
}

const xerum::model::ReportEntry* entryFor (const xerum::model::ImportReport& report,
                                           const xerum::model::Severity severity,
                                           const std::string& item)
{
    for (const auto& entry : report.entries)
        if (entry.severity == severity && entry.item == item)
            return &entry;

    return nullptr;
}

/// Any entry at all whose item or module path mentions the table slot, regardless of severity.
bool reportMentionsTable (const xerum::model::ImportReport& report, const std::string& reference)
{
    for (const auto& entry : report.entries)
        if (entry.item == reference || entry.modulePath == reference)
            return true;

    return false;
}

} // namespace

TEST_CASE ("a case-mismatched asset path resolves, says so in the report, and renders",
           "[third-party]")
{
    // The reference names a file that differs only in case from one xerum ships (`content/Tables/xerum/
    // Sine.wav`). Ticket 24's finding is the reason this test exists at all: on the case-insensitive
    // filesystem every macOS machine has, a mis-cased path used to look like an *exact* hit, so the
    // case-insensitive branch was dead code and the report stayed quiet. The behaviour under test is not
    // the mechanism — it is: does it resolve, does the report say it resolved inexactly, and does the
    // slot still sound.
    auto document = engineDocument();
    document.parameters["Oscillator0/WTOsc0/relativePathToWT"] = text ("/xerum/SINE.wav");
    document.assetReferences.push_back ("/xerum/SINE.wav");

    const auto file = writeFixture (document);

    const PinnedContentRoot pinned;

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (file).wasOk());

    const auto report = processor.report();

    // Resolved, and the report is honest that it was not an exact spelling: info, by the reference's own
    // name, with the path it actually landed on.
    const auto* resolved = entryFor (report, xerum::model::Severity::Info, "/xerum/SINE.wav");
    REQUIRE (resolved != nullptr);
    CHECK (resolved->modulePath == "Oscillator0/WTOsc0/relativePathToWT");
    CHECK (resolved->reason.find ("case-insensitively") != std::string::npos);

    // The resolved table is what plays, and the slot renders.
    REQUIRE (processor.oscillatorTable (0) != nullptr);
    CHECK (processor.oscillatorTable (0)->frameCount == 1);   // xerum's Sine

    CHECK (peakOf (render (processor, 16, 512, noteThenRelease (60, 8))) > 0.01);

    // A case-insensitive hit is not a substitution: no high-severity complaint about this path.
    CHECK (entryFor (report, xerum::model::Severity::High, "/xerum/SINE.wav") == nullptr);

    file.deleteFile();
}

TEST_CASE ("a table reference that resolves to nothing is reported and substituted, visibly",
           "[third-party]")
{
    // "Resolves to a missing file": the preset names a table, and no content root has it. The contract
    // forbids a silent substitution, so this must be two facts in the report — the miss, and what played
    // instead.
    auto document = engineDocument();
    document.parameters["Oscillator0/WTOsc0/relativePathToWT"] = text ("/third-party/Nowhere.wav");
    document.assetReferences.push_back ("/third-party/Nowhere.wav");

    const auto file = writeFixture (document);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (file).wasOk());

    const auto report = processor.report();

    const auto* missing = entryFor (report, xerum::model::Severity::High, "/third-party/Nowhere.wav");
    REQUIRE (missing != nullptr);
    CHECK (missing->modulePath == "Oscillator0/WTOsc0/relativePathToWT");
    CHECK (missing->reason.find ("not found in") != std::string::npos);

    // The substitution is documented (info, naming our own table), not silent.
    const auto* substituted = entryFor (report, xerum::model::Severity::Info, "/third-party/Nowhere.wav");
    REQUIRE (substituted != nullptr);
    CHECK (substituted->reason.find ("substituted xerum's own") != std::string::npos);

    // And the documented default is what actually plays: a table, and a rendered note.
    REQUIRE (processor.oscillatorTable (0) != nullptr);
    CHECK (peakOf (render (processor, 16, 512, noteThenRelease (60, 8))) > 0.01);

    file.deleteFile();
}

TEST_CASE ("an absent table and a missing file are the same substitution and different reports",
           "[third-party]")
{
    // "Names no file at all": the slot carries no `relativePathToWT`. The substitution is identical —
    // xerum's own table — but there is nothing for the report to complain about, because the preset
    // asked for nothing. That is the observable difference from the missing-file case above, and it is
    // the difference a user needs to see: a complaint means the pack lost something it meant to use.
    auto document = engineDocument();   // no relativePathToWT anywhere

    const auto file = writeFixture (document);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (file).wasOk());

    const auto report = processor.report();

    // No complaint about a table: nothing was named, so nothing was dropped.
    CHECK_FALSE (reportMentionsTable (report, "Oscillator0/WTOsc0/relativePathToWT"));
    for (const auto& entry : report.entries)
        CHECK (entry.severity != xerum::model::Severity::High);

    // Yet the slot sounds — the same documented default as the missing-file case.
    REQUIRE (processor.oscillatorTable (0) != nullptr);
    CHECK (peakOf (render (processor, 16, 512, noteThenRelease (60, 8))) > 0.01);

    file.deleteFile();
}

TEST_CASE ("a schema version above 6.0 imports with a report and still renders", "[third-party]")
{
    // A version newer than anything the catalogue accepts. The container's metadata carries it as a JSON
    // number; the loader parses the payload anyway and reports the version rather than refusing.
    auto document = engineDocument();
    document.schemaVersion = "9.9";

    const auto file = writeFixture (document);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (file).wasOk());

    const auto report = processor.report();

    const auto* version = entryFor (report, xerum::model::Severity::Medium, "9.9");
    REQUIRE (version != nullptr);
    CHECK (version->modulePath == "preset");
    CHECK (version->reason.find ("rather than refused") != std::string::npos);

    // The payload was read despite the version, and the preset still makes a sound.
    CHECK (processor.document().parameterCount() > 0);
    CHECK (peakOf (render (processor, 16, 512, noteThenRelease (60, 8))) > 0.01);

    file.deleteFile();
}

TEST_CASE ("a parameter the schema does not describe imports, is reported, is retained, and renders",
           "[third-party]")
{
    auto document = engineDocument();
    document.parameters["Oscillator0/WTOsc0/plainParams/kParamFromTheFuture"] = number (3.0);

    const auto file = writeFixture (document);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (file).wasOk());

    const auto report = processor.report();

    const bool reported = std::any_of (report.entries.begin(), report.entries.end(),
                                       [] (const xerum::model::ReportEntry& entry)
                                       {
                                           return entry.severity == xerum::model::Severity::Medium
                                                  && entry.item.find ("FromTheFuture") != std::string::npos;
                                       });
    CHECK (reported);

    // Retained, so a write cannot lose the field the reference put there.
    const auto loaded = processor.document();
    const auto found = loaded.parameters.find ("Oscillator0/WTOsc0/plainParams/kParamFromTheFuture");
    REQUIRE (found != loaded.parameters.end());
    CHECK (found->second.real == 3.0);

    CHECK (peakOf (render (processor, 16, 512, noteThenRelease (60, 8))) > 0.01);

    file.deleteFile();
}
