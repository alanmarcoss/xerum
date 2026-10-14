// Preset loading: through ticket 23's document, with the report reaching the interface.
//
// The fixture is a preset **xerum writes itself** (`writeContainer`), so this suite is meaningful on a
// fresh clone. The factory corpus is resolved by path and skipped loudly when it is not installed, per
// docs/test-strategy.md.
//
// Run alone with:  ctest --test-dir build-30 -R xerum-plugin-tests  (or the binary with "[preset]")

#include "TestSupport.h"

#include "cbor.h"
#include "container.h"
#include "decompress.h"
#include "write.h"

#include <cstring>

#include <catch2/catch_approx.hpp>

using namespace xerum::plugin;
using namespace xerum::plugin::tests;

namespace
{

std::vector<std::byte> bytesOf (const juce::File& file)
{
    juce::MemoryBlock block;

    if (! file.loadFileAsData (block))
        return {};

    const auto* data = static_cast<const std::byte*> (block.getData());
    return { data, data + block.getSize() };
}

juce::File writeFixture (const xerum::model::PresetDocument& document)
{
    const auto written = xerum::model::writeContainer (document, {});
    REQUIRE (written.ok);

    auto file = juce::File::createTempFile (".SerumPreset");
    REQUIRE (file.replaceWithData (written.bytes.data(), written.bytes.size()));

    return file;
}

xerum::model::PresetDocument engineDocument()
{
    xerum::model::PresetDocument document;
    document.name = "plugin fixture";
    document.schemaVersion = "4.0";
    document.parameters["Oscillator0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Oscillator0/plainParams/kParamVolume"] = number (0.375);
    document.parameters["Oscillator0/plainParams/kParamPitch"] = number (-3.0);
    document.parameters["Oscillator0/plainParams/kParamUnison"] = number (4.0);
    document.parameters["Global0/plainParams/kParamPolyCount"] = number (6.0);
    document.parameters["Env0/plainParams/kParamAttack"] = number (0.02);
    document.parameters["Env0/plainParams/kParamRelease"] = number (0.05);
    document.parameters["VoiceFilter0/plainParams/kParamEnable"] = number (1.0);
    document.parameters["VoiceFilter0/plainParams/kParamFreq"] = number (0.4);
    return document;
}

} // namespace

TEST_CASE ("a preset reaches the parameters, and a half-loading one says so in the interface",
           "[plugin][preset]")
{
    auto document = engineDocument();

    // Three kinds of thing the plugin's own part of the report has to name: a warp the engine does not
    // implement, a warp *variant* whose source routing it does not have, and a value it holds but does
    // not act on (the stereo unison spread).
    document.parameters["Oscillator0/WTOsc0/plainParams/kParamWarpMenu"] = text ("kRemap_4");
    document.parameters["Oscillator0/WTOsc0/plainParams/kParamWarp2"] = number (0.5);
    document.parameters["Oscillator0/WTOsc0/plainParams/kParamWarpMenu2"] = text ("kPD_OSC");
    document.parameters["Oscillator0/plainParams/kParamUnisonStereo"] = number (75.0);

    // A parameter from a newer reference: retained by the document, uninterpreted, and reported.
    document.parameters["Oscillator0/WTOsc0/plainParams/kParamFromTheFuture"] = number (3.0);

    const auto file = writeFixture (document);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    const auto result = processor.loadPresetFromFile (file);
    REQUIRE (result.wasOk());
    CHECK (processor.lastError().isEmpty());

    // The preset's values reached the parameters the host automates.
    CHECK (processor.parameterValue ("Oscillator0/plainParams/kParamEnable") == 1.0);
    CHECK (processor.parameterValue ("Oscillator0/plainParams/kParamVolume") == Catch::Approx (0.375));
    CHECK (processor.parameterValue ("Oscillator0/plainParams/kParamPitch") == Catch::Approx (-3.0));
    CHECK (processor.parameterValue ("Oscillator0/plainParams/kParamUnison") == 4.0);
    CHECK (processor.parameterValue ("Global0/plainParams/kParamPolyCount") == 6.0);
    CHECK (processor.parameterValue ("VoiceFilter0/plainParams/kParamFreq") == Catch::Approx (0.4));

    // And they reached the engine, which is the part that makes a sound.
    CHECK (peakOf (render (processor, 16, 512, noteThenRelease (60, 8))) > 0.01);

    const auto report = processor.report();
    const auto text = processor.reportText();

    const auto hasEntry = [&report] (const xerum::model::Severity severity, const std::string& item)
    {
        return std::any_of (report.entries.begin(), report.entries.end(),
                            [severity, &item] (const xerum::model::ReportEntry& entry)
                            { return entry.severity == severity && entry.item == item; });
    };

    // The model's half: the unimplemented warp, by name, at high.
    CHECK (hasEntry (xerum::model::Severity::High, "kRemap_4"));
    // The plugin's half: the variant's source routing, and the value it holds but does not apply.
    CHECK (hasEntry (xerum::model::Severity::Info, "kPD_OSC"));
    CHECK (hasEntry (xerum::model::Severity::Info, "kParamUnisonStereo"));
    // And the uninterpreted parameter from a newer reference. The model reports it under its *normalised*
    // path — the module is `{N}`-addressed there — so the match is on the parameter's own name.
    CHECK (std::any_of (report.entries.begin(), report.entries.end(),
                        [] (const xerum::model::ReportEntry& entry)
                        {
                            return entry.severity == xerum::model::Severity::Medium
                                   && entry.item.find ("FromTheFuture") != std::string::npos;
                        }));

    // The text the interface shows carries them, with the path a user can find, and counts per severity.
    CHECK (text.contains ("Oscillator0/WTOsc0"));
    CHECK (text.contains ("kRemap_4"));
    CHECK (text.contains ("kParamUnisonStereo"));
    CHECK (text.contains ("high 1"));

    // The unimplemented warp did not silently become a neighbour: the parameter shows the sentinel, and
    // the document still holds the name the preset used.
    const auto& warpChoices = processor.bridge().choices (ChoiceSet::WarpMode);
    CHECK (processor.parameterValue ("Oscillator0/WTOsc0/plainParams/kParamWarpMenu")
           == static_cast<double> (warpChoices.sentinel));
    CHECK (processor.document().parameters.at ("Oscillator0/WTOsc0/plainParams/kParamWarpMenu").text == "kRemap_4");

    file.deleteFile();
}

TEST_CASE ("a clean import still produces a report — empty, not absent", "[plugin][preset]")
{
    const auto file = writeFixture (engineDocument());

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    REQUIRE (processor.loadPresetFromFile (file).wasOk());

    // Nothing in this preset is unimplemented, so the report has no entries — and the plugin still has a
    // report, and says so in words rather than showing nothing at all.
    CHECK (processor.report().entries.empty());
    CHECK (processor.reportText().contains ("Nothing to report"));
    CHECK (processor.reportText().contains ("high 0  medium 0  info 0"));

    file.deleteFile();
}

TEST_CASE ("a named table that cannot be resolved is reported, and a documented default substituted",
           "[plugin][preset]")
{
    auto document = engineDocument();
    document.parameters["Oscillator0/WTOsc0/relativePathToWT"] = text ("/definitely/not/here.wav");
    document.assetReferences.push_back ("/definitely/not/here.wav");

    const auto file = writeFixture (document);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (file).wasOk());

    const auto report = processor.report();
    const auto missing = std::find_if (report.entries.begin(), report.entries.end(),
                                       [] (const xerum::model::ReportEntry& entry) { return entry.severity == xerum::model::Severity::High; });

    REQUIRE (missing != report.entries.end());
    CHECK (missing->item == "/definitely/not/here.wav");
    CHECK (missing->modulePath == "Oscillator0/WTOsc0/relativePathToWT");

    // The substitution is documented, not silent: xerum's own table is what plays, and it is there.
    REQUIRE (processor.oscillatorTable (0) != nullptr);
    CHECK (processor.oscillatorTable (0)->frameCount == 1);   // xerum's Sine: one frame
    CHECK (processor.reportText().contains ("Sine"));

    // The other slots asked for nothing, and got xerum's default without a report entry — the report is
    // for what the preset asked for.
    REQUIRE (processor.oscillatorTable (1) != nullptr);
    CHECK (report.count (xerum::model::Severity::High) == 1);

    file.deleteFile();
}

TEST_CASE ("a slot that selects an engine xerum cannot synthesise is named, and left silent",
           "[plugin][preset]")
{
    auto document = engineDocument();
    document.parameters["Oscillator1/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Oscillator1/plainParams/kParamType"] = text ("kOsc_Spectral");   // ticket 36's finding

    const auto file = writeFixture (document);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (file).wasOk());

    // Slot 1 renders silence rather than a substituted wavetable, and the report says why.
    CHECK (processor.oscillatorTable (1) == nullptr);
    CHECK (processor.reportText().contains ("does not synthesise"));

    // Slot 0 is untouched and still plays.
    CHECK (processor.oscillatorTable (0) != nullptr);
    CHECK (peakOf (render (processor, 16, 512, noteThenRelease (60, 8))) > 0.01);

    file.deleteFile();
}

TEST_CASE ("a damaged preset file is an error, not an exception at the plugin boundary",
           "[plugin][preset]")
{
    // Ticket 15 names this invariant: a throw into a host process is a crash in someone else's DAW. The
    // contract's first clause is "MUST not fail, whatever the file contains", so every path from a file to
    // the engine is wrapped — and this exercises the wrapping with bytes that are not a container, bytes
    // that are a container with a truncated payload, and an empty file.
    const std::vector<std::vector<std::byte>> damaged = {
        { std::byte { 0x00 }, std::byte { 0xff }, std::byte { 0x42 } },
        [] {
            std::vector<std::byte> header (40, std::byte { 0 });
            const char* magic = "XferJson";
            std::memcpy (header.data(), magic, 8);
            return header;
        }(),
        {},
    };

    for (const auto& bytes : damaged)
    {
        auto file = juce::File::createTempFile (".SerumPreset");
        REQUIRE (file.replaceWithData (bytes.data(), bytes.size()));

        XerumAudioProcessor processor;
        processor.prepareToPlay (48000.0, 512);
        processor.setParameterValue ("Oscillator0/plainParams/kParamVolume", 0.5);

        const auto result = processor.loadPresetFromFile (file);

        CHECK_FALSE (result.wasOk());
        CHECK (processor.lastError().isNotEmpty());
        CHECK (processor.reportText().contains ("could not load"));

        // And the patch survived: a file we could not read must not clear what the user had.
        CHECK (processor.parameterValue ("Oscillator0/plainParams/kParamVolume") == Catch::Approx (0.5));

        file.deleteFile();
    }
}

TEST_CASE ("the factory corpus, if it is installed", "[plugin][preset][corpus]")
{
    // Resolved by path and skipped loudly: the corpus is not committed (ticket 10), and a skip must never
    // be able to look like a pass.
    const juce::File root (corpusRoot());

    if (! root.isDirectory())
        SKIP ("no factory corpus at " << root.getFullPathName().toStdString()
                                     << " — this test runs only where the reference instrument is installed");

    juce::Array<juce::File> presets;
    root.findChildFiles (presets, juce::File::findFiles, true, "*.SerumPreset");

    REQUIRE (! presets.isEmpty());

    presets.sort();

    // The whole bank, one preset at a time: every one must load, produce a document with parameters, and
    // produce a report. That is the contract's first clause — "MUST not fail, whatever the file contains".
    int loaded = 0;
    int withReport = 0;
    int clean = 0;

    for (const auto& preset : presets)
    {
        XerumAudioProcessor processor;
        processor.prepareToPlay (48000.0, 512);

        if (! processor.loadPresetFromFile (preset).wasOk())
        {
            UNSCOPED_INFO ("failed to load " << preset.getFullPathName().toStdString() << ": "
                                            << processor.lastError().toStdString());
            FAIL_CHECK ("every preset in the corpus must load");
            continue;
        }

        ++loaded;

        const auto document = processor.document();

        CHECK (! document.parameters.empty());
        CHECK (! processor.reportText().isEmpty());

        ++withReport;

        if (processor.report().clean())
            ++clean;
    }

    UNSCOPED_INFO ("corpus: " << loaded << " of " << presets.size() << " loaded, " << withReport
                              << " with a report, " << clean << " clean");

    CHECK (loaded == presets.size());
    CHECK (withReport == presets.size());
}
