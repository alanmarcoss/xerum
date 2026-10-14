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
#include "spectral_morph.h"
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
    // implement (the model's half), a warp *variant* whose source routing it does not have (the plugin's
    // half), and a parameter from a newer reference. The stereo unison spread used to be a fourth — a
    // value the engine held and did not act on — and ticket 41 put it in the signal path, so it is now a
    // value the preset reaches the host with instead.
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
    // The plugin's half: the variant's source routing.
    CHECK (hasEntry (xerum::model::Severity::Info, "kPD_OSC"));
    // The stereo spread is no longer reported — ticket 41 wired it — and the preset's value reached the
    // parameter the host automates instead.
    CHECK_FALSE (hasEntry (xerum::model::Severity::Info, "kParamUnisonStereo"));
    CHECK (processor.parameterValue ("Oscillator0/plainParams/kParamUnisonStereo") == Catch::Approx (75.0));
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
    CHECK (text.contains ("kPD_OSC"));
    CHECK (text.contains ("high 1"));

    // The unimplemented warp did not silently become a neighbour: the parameter shows the sentinel, and
    // the document still holds the name the preset used.
    const auto& warpChoices = processor.bridge().choices (ChoiceSet::WarpMode);
    CHECK (processor.parameterValue ("Oscillator0/WTOsc0/plainParams/kParamWarpMenu")
           == static_cast<double> (warpChoices.sentinel));
    CHECK (processor.document().parameters.at ("Oscillator0/WTOsc0/plainParams/kParamWarpMenu").text == "kRemap_4");

    file.deleteFile();
}

TEST_CASE ("NOISE and SUB reach the engine from a preset, and what they cannot honour is named",
           "[plugin][preset][noise][sub]")
{
    // Ticket 40, at the plugin boundary: the two fixed slots' parameters are on the host surface, a
    // preset's values reach them, and the slot makes a sound — which is the whole acceptance, since a
    // DSP engine nothing maps onto is indistinguishable from no engine at all.
    auto document = engineDocument();
    document.parameters["Oscillator0/plainParams/kParamEnable"] = number (0.0);   // the two slots alone
    document.parameters["VoiceFilter0/plainParams/kParamEnable"] = number (0.0);
    document.parameters["Oscillator3/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Oscillator3/plainParams/kParamVolume"] = number (1.0);
    document.parameters["Oscillator3/NoiseOsc3/plainParams/kParamNoiseType"] = text ("Brown");
    document.parameters["Oscillator3/NoiseOsc3/plainParams/kParamRandomPhase"] = number (40.0);
    document.parameters["Oscillator4/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Oscillator4/plainParams/kParamVolume"] = number (1.0);
    document.parameters["Oscillator4/plainParams/kParamOctave"] = number (-1.0);
    document.parameters["Oscillator4/SubOsc4/plainParams/kParamShape"] = text ("kSaw");
    document.parameters["Oscillator4/SubOsc4/plainParams/kParamInitialPhase"] = number (90.0);
    document.parameters["Oscillator4/SubOsc4/plainParams/kParamContiguousPhase"] = number (1.0);

    const auto file = writeFixture (document);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (file).wasOk());

    // The values reached the parameters the host automates, including the two choices.
    CHECK (processor.parameterValue ("Oscillator3/plainParams/kParamEnable") == 1.0);
    CHECK (processor.parameterValue ("Oscillator3/NoiseOsc3/plainParams/kParamNoiseType")
           == static_cast<double> (choiceIndexForText (processor.bridge().choices (ChoiceSet::NoiseType),
                                                       ChoiceSet::NoiseType, "Brown")));
    CHECK (processor.parameterValue ("Oscillator4/SubOsc4/plainParams/kParamShape")
           == static_cast<double> (choiceIndexForText (processor.bridge().choices (ChoiceSet::SubShape),
                                                       ChoiceSet::SubShape, "kSaw")));
    CHECK (processor.parameterValue ("Oscillator4/plainParams/kParamOctave") == Catch::Approx (-1.0));

    // And they reached the engine: the two slots alone are audible.
    CHECK (peakOf (render (processor, 24, 512, noteThenRelease (48, 12))) > 0.01);

    // Nothing about a colour or a waveform is unimplemented, so the plugin's half of the report is empty
    // of complaints about them.
    CHECK (processor.report().count (xerum::model::Severity::High) == 0);

    file.deleteFile();
}

TEST_CASE ("a NOISE slot that names a sample is silent and says why, rather than substituting a colour",
           "[plugin][preset][noise]")
{
    // NOISE is a sample player in the reference. xerum synthesises the four generated colours, so this is
    // exactly the case where a substitution would be tempting — White noise in place of the file — and
    // where the contract forbids it.
    auto document = engineDocument();
    document.parameters["Oscillator0/plainParams/kParamEnable"] = number (0.0);
    document.parameters["VoiceFilter0/plainParams/kParamEnable"] = number (0.0);
    document.parameters["Oscillator3/plainParams/kParamEnable"] = number (1.0);
    document.parameters["Oscillator3/plainParams/kParamVolume"] = number (1.0);
    document.parameters["Oscillator3/NoiseOsc3/relativePathToNoiseSample"] = text ("Organics/AC hum1.wav");

    // The playback controls a *sample* has and a generated colour has not, plus the field whose meaning is
    // not established: each is named rather than dropped.
    document.parameters["Oscillator3/NoiseOsc3/plainParams/kParamOneShot"] = number (1.0);
    document.parameters["Oscillator3/NoiseOsc3/plainParams/kParamInitialPhase"] = number (50.0);
    document.parameters["Oscillator3/NoiseOsc3/plainParams/kParamColor"] = number (0.8);
    document.assetReferences.push_back ("Organics/AC hum1.wav");

    const auto file = writeFixture (document);

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (file).wasOk());

    const auto report = processor.report();

    const auto entryFor = [&report] (const std::string& item)
    {
        return std::find_if (report.entries.begin(), report.entries.end(),
                             [&item] (const xerum::model::ReportEntry& entry) { return entry.item == item; });
    };

    const auto missing = entryFor ("Organics/AC hum1.wav");
    REQUIRE (missing != report.entries.end());
    CHECK (missing->severity == xerum::model::Severity::High);

    for (const auto* leaf : { "kParamOneShot", "kParamInitialPhase", "kParamColor" })
    {
        const auto entry = entryFor (leaf);
        REQUIRE (entry != report.entries.end());
        CHECK (entry->severity == xerum::model::Severity::Medium);
        CHECK (entry->modulePath == "Oscillator3/NoiseOsc3");
    }

    // The slot renders silence — no substituted colour — and the noise type parameter shows the sentinel,
    // not a colour the preset never asked for.
    const auto rendered = render (processor, 24, 512, noteThenRelease (48, 12));
    CHECK (peakOf (rendered) == 0.0);

    const auto& choices = processor.bridge().choices (ChoiceSet::NoiseType);
    CHECK (processor.parameterValue ("Oscillator3/NoiseOsc3/plainParams/kParamNoiseType")
           == static_cast<double> (choices.sentinel));

    file.deleteFile();

    // And the other way the reference feeds this oscillator: the host's audio input. xerum is an
    // instrument with **no input bus at all** (`isBusesLayoutSupported` refuses any layout with an
    // enabled input), so this is impossible rather than unimplemented, and it is named all the same.
    {
        auto audioInput = document;
        audioInput.parameters.erase ("Oscillator3/NoiseOsc3/relativePathToNoiseSample");

        xerum::model::Value fromInput;
        fromInput.type = xerum::model::Value::Type::Bool;   // a real bool leaf, not a 0/1 float
        fromInput.boolean = true;
        audioInput.parameters["Oscillator3/NoiseOsc3/sampleFromAudioInput"] = fromInput;

        const auto inputFile = writeFixture (audioInput);

        XerumAudioProcessor inputProcessor;
        inputProcessor.prepareToPlay (48000.0, 512);
        REQUIRE (inputProcessor.loadPresetFromFile (inputFile).wasOk());

        const auto entries = inputProcessor.report().entries;
        const auto named = std::find_if (entries.begin(), entries.end(),
                                        [] (const xerum::model::ReportEntry& entry)
                                        { return entry.item == "sampleFromAudioInput"; });

        REQUIRE (named != entries.end());
        CHECK (named->severity == xerum::model::Severity::High);
        CHECK (named->modulePath == "Oscillator3/NoiseOsc3");
        CHECK (named->reason.find ("no input bus") != std::string::npos);

        inputFile.deleteFile();
    }
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

TEST_CASE ("the one factory preset that asks for a spectral morph after load now gets it",
           "[plugin][preset][morph-wired]")
{
    // Ticket 41's real-world case. `KY - Delicacy` is the only preset in the whole factory bank that sets
    // `WTOsc{N}/interpolateAfterLoad == 2` (walked: 23 of 626 presets carry the field at all; one sets it
    // to 2, three times to 1 and the rest to 0), and its oscillator A carries the table inline — so it is
    // the one place in the corpus where the morph can apply at all. The two report entries that used to
    // cover this slot must be gone: the model's "xerum crossfades the table instead", and the plugin's
    // "xerum reads wavetable files, so this slot renders silence".
    const juce::File preset (juce::String (corpusRoot())
                             + "/Presets/Factory/Keyboard/KY - Delicacy.SerumPreset");

    if (! preset.existsAsFile())
        SKIP ("the reference's corpus is not installed at " << corpusRoot().toStdString()
                                                           << " (docs/test-strategy.md)");

    XerumAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadPresetFromFile (preset).wasOk());

    const auto document = processor.document();
    const auto afterLoad = document.parameters.find ("Oscillator0/WTOsc0/interpolateAfterLoad");

    REQUIRE (afterLoad != document.parameters.end());

    double value = 0.0;
    REQUIRE (afterLoad->second.numberAsDouble (value));
    CHECK (value == 2.0);

    // The preset carries four 2048-sample cycles inline (measured: 8192 `embeddedWTData` entries), and the
    // morph expands them to the 256 WT POS slots the reference describes `numFrames` as.
    CHECK (document.parameters.count ("Oscillator0/WTOsc0/embeddedWTData[0]") == 1);
    CHECK (document.parameters.count ("Oscillator0/WTOsc0/embeddedWTData[8191]") == 1);

    const auto* table = processor.oscillatorTable (0);
    REQUIRE (table != nullptr);
    CHECK (table->frameCount == xerum::dsp::kExpandedFrameCount);
    CHECK (table->frameSize == xerum::dsp::kDefaultFrameSize);

    for (const auto& entry : processor.report().entries)
    {
        CHECK (entry.reason.find ("spectral morphing after load") == std::string::npos);
        CHECK (entry.item != "embeddedWTData");
    }

    // The slot is in the signal path. A bare render of this preset is silent *downstream of the
    // oscillator*: `VoiceFilter0` sits at `kParamFreq` 0 and `VoiceFilter1` is an `H24` at the top of the
    // band whose `kParamLevelOut` is 0 — and xerum runs the two voice filters **in series** and has no
    // parameter for the output level (both checkable in `Voice::render` and `ParameterBridge`), so filter 1
    // attenuates everything filter 0 passed. That is a separate, pre-existing gap and not what this test is
    // about, so both are switched off and the oscillator's own output is what is measured.
    processor.setParameterValue ("VoiceFilter0/plainParams/kParamEnable", 0.0);
    processor.setParameterValue ("VoiceFilter1/plainParams/kParamEnable", 0.0);
    CHECK (peakOf (render (processor, 16, 512, noteThenRelease (48, 8))) > 0.001);
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
