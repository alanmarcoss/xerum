// The factory bank, rendered (ticket 54).
//
// ## What rendering can and cannot prove
//
// Nobody here has ears, so this test does not claim any preset *sounds good* — that is the dev's call and
// nothing in a binary can make it. What it proves is the failure mode that actually kills a bank of a
// hundred: **that they are all the same sound.** A hundred presets that differ only in name is the thing
// to catch, and it is catchable, because two presets that render the same render the same.
//
// So every preset is loaded through the product's own loader, rendered, and summarised twice:
//
//   * a **spectrum** — 16 log-spaced bands of the average magnitude spectrum over the frames the sound is
//     actually in, normalised to the loudest band and floored at -60 dB, so the measure is a *shape* and
//     not a level;
//   * an **envelope** — 16 RMS segments over the whole render, the same normalisation and floor, so a
//     percussive preset and a sustained one do not collapse because one is shorter than the other.
//
// Two presets **collapse** when both summaries are within their floors: 1.0 dB mean absolute difference
// across the bands, and 2.0 dB worst-segment difference. The test reports the closest pair in the bank and
// how far apart it is, so the margin is visible rather than asserted.
//
// ## The other half: the bank is honest
//
// A preset whose token is outside xerum's vocabulary resolves to the "not in xerum's set" sentinel and its
// module silently does nothing — the one failure that leaves a preset looking fine and playing wrong. So
// each preset is also checked for: a clean report (nothing asked for that the product does not do), every
// parameter being one the bridge actually declares (no dead parameters), and every choice token resolving.
//
// Run alone with:  ctest --test-dir build-54 -R xerum-plugin-tests   (or the binary with "[bank]")

#include "TestSupport.h"

#include "factory_bank.h"
#include "json.h"
#include "table_library.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{

using xerum::plugin::XerumAudioProcessor;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr int kNoteBlocks = 141;      ///< ~1.5 s held
constexpr int kTailBlocks = 140;      ///< ~1.5 s of tail
constexpr int kSegments = 16;
constexpr int kBands = 16;
constexpr int kFftSize = 2048;
constexpr int kHop = 512;

/// The floors below which two presets are the same sound. Stated here rather than tuned per preset: the
/// margin the bank achieves is then a measurement against a fixed rule.
constexpr double kSpectralFloorDb = 1.0;    ///< mean |Δ| across the bands
constexpr double kEnvelopeFloorDb = 2.0;    ///< worst segment difference
constexpr double kFloorDb = -60.0;          ///< where a normalised band or segment stops being a difference

struct Signature
{
    std::array<double, kBands> spectrum {};
    std::array<double, kSegments> envelope {};
    double rms { 0.0 };
    double peak { 0.0 };
    bool finite { true };
};

std::string toString (const std::filesystem::path& path)
{
    return path.string();
}

/// The content root the *processor* would resolve content from that holds the bank. Nothing here hard-codes
/// a path: the point is that the bank is found the way the product finds content, in the order
/// `docs/preset-import-contract.md` §5 states.
struct Bank
{
    std::filesystem::path root;
    std::vector<std::filesystem::path> files;
};

Bank findBank (XerumAudioProcessor& processor)
{
    Bank bank;

    for (const auto& root : processor.contentRoots())
    {
        const std::filesystem::path candidate (root.toStdString());

        if (! std::filesystem::exists (candidate / xerum::presets::kBankDirectory
                                       / xerum::presets::kBankManifestName))
            continue;

        bank.root = candidate;

        std::error_code error;

        for (const auto& entry : std::filesystem::recursive_directory_iterator (
                 candidate / xerum::presets::kBankDirectory, error))
            if (entry.is_regular_file()
                && entry.path().extension() == xerum::presets::kPresetExtension)
                bank.files.push_back (entry.path());

        std::sort (bank.files.begin(), bank.files.end());
        return bank;
    }

    return bank;
}

std::string readText (const std::filesystem::path& path)
{
    std::ifstream in (path, std::ios::binary);

    if (! in)
        return {};

    return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
}

/// What the browser (ticket 55) reads, and therefore what this test reads too.
struct ManifestEntry
{
    std::string name;
    std::string category;
    std::vector<std::string> tables;
};

std::map<std::string, ManifestEntry> readManifest (const std::filesystem::path& root, std::string& error)
{
    std::map<std::string, ManifestEntry> entries;

    const auto text = readText (root / xerum::presets::kBankDirectory / xerum::presets::kBankManifestName);

    if (text.empty())
    {
        error = "the bank manifest is missing at "
                + toString (root / xerum::presets::kBankDirectory / xerum::presets::kBankManifestName);
        return entries;
    }

    const auto manifest = xerum::model::decodeJson (text);

    if (! manifest.ok)
    {
        error = "the bank manifest is not readable JSON: " + manifest.error;
        return entries;
    }

    const auto* presets = manifest.value.find ("presets");

    if (presets == nullptr || presets->type != xerum::model::JsonValue::Type::Array)
    {
        error = "the bank manifest has no presets list";
        return entries;
    }

    for (const auto& entry : presets->items)
    {
        ManifestEntry parsed;
        parsed.name = entry.stringOr ("name");
        parsed.category = entry.stringOr ("category");

        if (const auto* tables = entry.find ("tables"); tables != nullptr)
            for (const auto& table : tables->items)
                parsed.tables.push_back (table.text);

        entries[entry.stringOr ("path")] = std::move (parsed);
    }

    return entries;
}

/// The spectral summary: the average magnitude spectrum over the frames the sound is in, in 16 log-spaced
/// bands, normalised to the loudest band and floored.
std::array<double, kBands> spectralSummary (const std::vector<float>& samples)
{
    std::array<double, kBands> summary {};

    juce::dsp::FFT fft (11);   // 2048
    std::array<int, kBands + 1> edges {};

    for (int band = 0; band <= kBands; ++band)
        edges[static_cast<std::size_t> (band)] =
            static_cast<int> (std::lround (std::pow (static_cast<double> (kFftSize / 2), band / double (kBands))));

    std::vector<float> window (kFftSize);

    for (int i = 0; i < kFftSize; ++i)
        window[static_cast<std::size_t> (i)] =
            static_cast<float> (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (kFftSize - 1)));

    if (samples.size() < static_cast<std::size_t> (kFftSize))
        return summary;

    // Where the sound is. A preset with a short decay would otherwise be measured mostly on the silence
    // after it, and two percussive presets would then agree about nothing in particular.
    const auto frames = (samples.size() - static_cast<std::size_t> (kFftSize)) / kHop + 1;
    std::vector<double> frameRms (frames, 0.0);
    double peakRms = 0.0;

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        double sum = 0.0;

        for (int i = 0; i < kFftSize; ++i)
        {
            const auto sample = static_cast<double> (samples[frame * kHop + static_cast<std::size_t> (i)]);
            sum += sample * sample;
        }

        frameRms[frame] = std::sqrt (sum / kFftSize);
        peakRms = std::max (peakRms, frameRms[frame]);
    }

    if (peakRms <= 0.0)
        return summary;

    std::vector<float> scratch (static_cast<std::size_t> (kFftSize) * 2, 0.0f);
    std::array<double, kBands> accumulated {};
    int used = 0;

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        if (frameRms[frame] < peakRms * 0.02)
            continue;   // silence after the tail: not part of the sound's shape

        std::fill (scratch.begin(), scratch.end(), 0.0f);

        for (int i = 0; i < kFftSize; ++i)
            scratch[static_cast<std::size_t> (i)] =
                samples[frame * kHop + static_cast<std::size_t> (i)] * window[static_cast<std::size_t> (i)];

        fft.performFrequencyOnlyForwardTransform (scratch.data(), true);

        for (int band = 0; band < kBands; ++band)
        {
            double largest = 0.0;

            for (int bin = std::max (1, edges[static_cast<std::size_t> (band)]);
                 bin < std::min (kFftSize / 2 - 1, edges[static_cast<std::size_t> (band + 1)]); ++bin)
                largest = std::max (largest, static_cast<double> (scratch[static_cast<std::size_t> (bin)]));

            accumulated[static_cast<std::size_t> (band)] += largest;
        }

        ++used;
    }

    if (used == 0)
        return summary;

    double largest = 0.0;

    for (auto& band : accumulated)
    {
        band /= used;
        largest = std::max (largest, band);
    }

    if (largest <= 0.0)
        return summary;

    for (int band = 0; band < kBands; ++band)
        summary[static_cast<std::size_t> (band)] =
            std::max (kFloorDb, 20.0 * std::log10 (accumulated[static_cast<std::size_t> (band)] / largest));

    return summary;
}

/// The envelope summary: 16 RMS segments across the whole render, normalised to the loudest and floored.
std::array<double, kSegments> envelopeSummary (const std::vector<float>& samples)
{
    std::array<double, kSegments> summary {};

    if (samples.empty())
        return summary;

    const auto perSegment = std::max<std::size_t> (1, samples.size() / kSegments);
    double largest = 0.0;

    for (int segment = 0; segment < kSegments; ++segment)
    {
        const auto begin = static_cast<std::size_t> (segment) * perSegment;
        const auto end = std::min (samples.size(), begin + perSegment);

        if (begin >= end)
            continue;

        double sum = 0.0;

        for (auto i = begin; i < end; ++i)
            sum += static_cast<double> (samples[i]) * static_cast<double> (samples[i]);

        summary[static_cast<std::size_t> (segment)] = std::sqrt (sum / double (end - begin));
        largest = std::max (largest, summary[static_cast<std::size_t> (segment)]);
    }

    if (largest <= 0.0)
        return summary;

    for (auto& segment : summary)
        segment = std::max (kFloorDb, 20.0 * std::log10 (segment / largest));

    return summary;
}

Signature summarise (const std::vector<float>& samples)
{
    Signature signature;

    signature.spectrum = spectralSummary (samples);
    signature.envelope = envelopeSummary (samples);
    signature.rms = xerum::plugin::tests::rmsOf (samples);

    for (const auto sample : samples)
    {
        signature.peak = std::max (signature.peak, std::abs (static_cast<double> (sample)));

        if (! std::isfinite (sample))
            signature.finite = false;
    }

    return signature;
}

double spectrumDistance (const Signature& left, const Signature& right)
{
    double sum = 0.0;

    for (std::size_t band = 0; band < left.spectrum.size(); ++band)
        sum += std::abs (left.spectrum[band] - right.spectrum[band]);

    return sum / double (left.spectrum.size());
}

double envelopeDistance (const Signature& left, const Signature& right)
{
    double worst = 0.0;

    for (std::size_t segment = 0; segment < left.envelope.size(); ++segment)
        worst = std::max (worst, std::abs (left.envelope[segment] - right.envelope[segment]));

    return worst;
}

/// How far apart two presets are, in units of the floors: above 1.0 they do not collapse.
double separation (const Signature& left, const Signature& right)
{
    return std::max (spectrumDistance (left, right) / kSpectralFloorDb,
                     envelopeDistance (left, right) / kEnvelopeFloorDb);
}

struct Rendered
{
    std::string name;
    std::string category;
    std::filesystem::path file;
    Signature signature;
};

} // namespace

TEST_CASE ("the factory bank loads through the product and is not a hundred presets of the same sound",
           "[plugin][bank]")
{
    XerumAudioProcessor probe;
    probe.prepareToPlay (kSampleRate, kBlockSize);

    const auto bank = findBank (probe);

    INFO ("content roots tried: " << [&probe]
          {
              std::string joined;

              for (const auto& root : probe.contentRoots())
                  joined += root.toStdString() + " | ";

              return joined;
          }());

    REQUIRE_FALSE (bank.files.empty());

    std::string manifestError;
    const auto manifest = readManifest (bank.root, manifestError);

    INFO (manifestError);
    REQUIRE (manifest.size() == bank.files.size());

    std::vector<Rendered> rendered;
    rendered.reserve (bank.files.size());

    for (const auto& file : bank.files)
    {
        const auto relative = std::filesystem::relative (file, bank.root).generic_string();

        INFO ("preset file: " << relative);
        REQUIRE (manifest.count (relative) == 1);

        const auto& entry = manifest.at (relative);

        // ---- load, through the product's own loader -------------------------------------------------
        juce::MemoryBlock bytes;

        REQUIRE (juce::File (juce::String (file.string())).loadFileAsData (bytes));
        REQUIRE (bytes.getSize() > 0);

        XerumAudioProcessor processor;
        processor.prepareToPlay (kSampleRate, kBlockSize);
        processor.setStateInformation (bytes.getData(), static_cast<int> (bytes.getSize()));

        INFO ("load reported: " << processor.lastError().toStdString());
        REQUIRE (processor.lastError().isEmpty());

        // A clean report: every module, parameter and asset this preset asks for is one xerum implements.
        // A preset that tripped this would be asking for something the engine does not do, which is the
        // "dead preset" ticket 12 forbids.
        const auto report = processor.report();

        if (! report.entries.empty())
        {
            INFO ("first report entry: [" << xerum::model::toString (report.entries.front().severity) << "] "
                  << report.entries.front().modulePath << " · " << report.entries.front().item << " — "
                  << report.entries.front().reason);
        }

        CHECK (report.entries.empty());

        // The name travelled with the file, and it is the manifest's name. `document()` returns a copy,
        // so this is the one the rest of this iteration reads — an iterator into a temporary would dangle.
        const auto document = processor.document();

        CHECK (document.name == entry.name);

        // ---- every parameter is one the product declares --------------------------------------------
        //
        // This is the check that catches a typo in a path: a document key that no parameter stands for is
        // written into the state and read by nobody — a dead parameter in a preset that looks fine.
        std::set<std::string> declared;
        std::map<std::string, const xerum::plugin::ParamSpec*> choices;
        std::map<std::string, const xerum::plugin::ParamSpec*> sources;

        const auto& parameterSpecs = processor.bridge().specs();
        const auto& modulationSpecs = processor.bridge().modulationSpecs();

        for (const auto* list : { &parameterSpecs, &modulationSpecs })
            for (const auto& spec : *list)
            {
                declared.insert (spec.modelPath);

                if (spec.type == xerum::plugin::ParamType::Choice)
                    choices[spec.modelPath] = &spec;
            }

        for (const auto& [path, value] : document.parameters)
        {
            INFO ("parameter: " << path);

            // Two document-only keys the bank writes, and both are read by something other than the
            // parameter surface: a slot's **table reference** (`resolveContent` resolves it, and the
            // parameter surface has no string type) and a modulation row's **destination name**
            // (`ModulationMatrix::setSlots` drops a row that has none). Everything else must be a
            // parameter the bridge declares, or it is a key nothing reads.
            if (path.ends_with ("/relativePathToWT") || path.ends_with ("/destModuleParamName"))
            {
                CHECK (value.isText());
                continue;
            }

            CHECK (declared.count (path) == 1);
        }

        // ---- every choice token resolves ------------------------------------------------------------
        for (const auto& [path, spec] : choices)
        {
            const auto found = document.parameters.find (path);

            if (found == document.parameters.end())
                continue;

            INFO ("choice: " << path << " = " << found->second.text);

            // A row's source is stored as the reference's own number, not as a token.
            if (xerum::plugin::ParameterBridge::choiceStoredAsNumber (spec->choiceSet))
            {
                double source = 0.0;
                REQUIRE (found->second.numberAsDouble (source));
                CHECK (source >= 0.0);
                CHECK (source <= double (xerum::dsp::kReferenceSourceCount));

                // 0 is the reference's "no source", which is what an aux slot holds when a row reads
                // one signal. Every other value must be one v1 actually evaluates.
                if (static_cast<int> (source) != 0)
                    CHECK (xerum::dsp::modSourceIsImplemented (static_cast<int> (source)));

                continue;
            }

            const auto& list = processor.bridge().choices (spec->choiceSet);
            const auto index = xerum::plugin::choiceIndexForText (list, spec->choiceSet, found->second.text);

            // The sentinel is the "not in xerum's set" entry: a token that lands there is a module that
            // does nothing while the preset claims otherwise.
            CHECK (index != list.sentinel);
        }

        // ---- every table it plays is in the same content root as the bank ----------------------------
        //
        // The self-containment check, at the level the product resolves at: the bank's tables come from
        // xerum's own root (the directory that also holds `Factory/`), never from the reference's tree,
        // and never as a rooted reference (the spelling a reference preset uses).
        for (const auto& table : entry.tables)
        {
            INFO ("table: " << table);
            CHECK_FALSE (table.empty());
            CHECK (table.front() != '/');
            CHECK (std::filesystem::exists ((bank.root / "Tables" / table).lexically_normal()));
        }

        // ---- render ---------------------------------------------------------------------------------
        const auto samples = xerum::plugin::tests::render (
            processor, kNoteBlocks + kTailBlocks, kBlockSize,
            xerum::plugin::tests::noteThenRelease (60, kNoteBlocks));

        const auto signature = summarise (samples);

        INFO ("preset: " << entry.name << "  rms " << signature.rms << "  peak " << signature.peak);
        CHECK (signature.finite);
        CHECK (signature.rms > 1e-3);
        CHECK (signature.peak > 1e-3);

        rendered.push_back ({ entry.name, entry.category, file, signature });
    }

    INFO ("presets rendered: " << rendered.size());
    CHECK (rendered.size() >= xerum::presets::kMinimumBankSize);

    {
        double quietest = std::numeric_limits<double>::max();
        double loudest = 0.0;
        double quietestPeak = std::numeric_limits<double>::max();
        std::string quietestName;

        for (const auto& preset : rendered)
        {
            if (preset.signature.rms < quietest)
            {
                quietest = preset.signature.rms;
                quietestName = preset.name;
            }

            loudest = std::max (loudest, preset.signature.rms);
            quietestPeak = std::min (quietestPeak, preset.signature.peak);
        }

        std::fprintf (stderr, "[bank] rendered level: quietest %s at rms %.5f, loudest at rms %.5f "
                              "(floor 0.001); quietest peak %.5f\n",
                      quietestName.c_str(), quietest, loudest, quietestPeak);
    }

    // ---- no two presets collapse ---------------------------------------------------------------------
    double closest = std::numeric_limits<double>::max();
    double closestSpectral = 0.0;
    double closestEnvelope = 0.0;
    std::size_t closestLeft = 0;
    std::size_t closestRight = 0;
    double minSpectral = std::numeric_limits<double>::max();
    double minEnvelope = std::numeric_limits<double>::max();

    for (std::size_t left = 0; left < rendered.size(); ++left)
    {
        for (std::size_t right = left + 1; right < rendered.size(); ++right)
        {
            const auto spectral = spectrumDistance (rendered[left].signature, rendered[right].signature);
            const auto envelope = envelopeDistance (rendered[left].signature, rendered[right].signature);

            minSpectral = std::min (minSpectral, spectral);
            minEnvelope = std::min (minEnvelope, envelope);

            const auto distance = separation (rendered[left].signature, rendered[right].signature);

            if (distance < closest)
            {
                closest = distance;
                closestSpectral = spectral;
                closestEnvelope = envelope;
                closestLeft = left;
                closestRight = right;
            }
        }
    }

    // Printed rather than only asserted: "the closest pair is X dB apart" is the margin, and a margin
    // nobody can see is a claim.
    std::fprintf (stderr,
                  "[bank] %zu presets  closest pair: %s / %s (spectral %.2f dB, envelope %.2f dB, "
                  "separation %.2fx the floor)\n",
                  rendered.size(), rendered[closestLeft].name.c_str(), rendered[closestRight].name.c_str(),
                  closestSpectral, closestEnvelope, closest);
    std::fprintf (stderr, "[bank] floors: spectral %.1f dB mean, envelope %.1f dB worst segment; "
                          "the bank's own minima are %.2f dB and %.2f dB\n",
                  kSpectralFloorDb, kEnvelopeFloorDb, minSpectral, minEnvelope);

    // Every pair that is closer than this is worth seeing: the point of the measure is to show the
    // margin, and one line naming the closest pair hides whether the bank is comfortable or lucky.
    {
        std::vector<std::pair<double, std::pair<std::size_t, std::size_t>>> near;

        for (std::size_t left = 0; left < rendered.size(); ++left)
            for (std::size_t right = left + 1; right < rendered.size(); ++right)
            {
                const auto distance = separation (rendered[left].signature, rendered[right].signature);

                if (distance < 1.5)
                    near.emplace_back (distance, std::make_pair (left, right));
            }

        // Every pair, sorted, so the ten closest are always visible: a listing that is empty on a green
        // run would leave the margin to be taken on trust.
        near.clear();

        for (std::size_t left = 0; left < rendered.size(); ++left)
            for (std::size_t right = left + 1; right < rendered.size(); ++right)
                near.emplace_back (separation (rendered[left].signature, rendered[right].signature),
                                   std::make_pair (left, right));

        std::sort (near.begin(), near.end());

        std::fprintf (stderr, "[bank] the %zu closest pair(s) of %zu:\n", std::min<std::size_t> (10, near.size()),
                      near.size());

        for (std::size_t i = 0; i < near.size() && i < 10; ++i)
        {
            const auto& [distance, pair] = near[i];

            std::fprintf (stderr, "[bank]   %6.2fx  %-22s %-22s  spectral %5.2f dB  envelope %5.2f dB\n",
                          distance,
                          rendered[pair.first].name.c_str(), rendered[pair.second].name.c_str(),
                          spectrumDistance (rendered[pair.first].signature, rendered[pair.second].signature),
                          envelopeDistance (rendered[pair.first].signature, rendered[pair.second].signature));
        }
    }

    INFO ("closest pair: " << rendered[closestLeft].name << " (" << rendered[closestLeft].category
                           << ") / " << rendered[closestRight].name << " (" << rendered[closestRight].category
                           << "): spectral " << closestSpectral << " dB, envelope " << closestEnvelope
                           << " dB");
    CHECK (closest > 1.0);

    // The rule stated as a count, so a failure names the pairs rather than a minimum: two summaries are
    // allowed to be near each other as long as the other one is not — that is what "different sound"
    // means here, and requiring both to differ would be a stricter claim than the measure supports.
    int collapsing = 0;

    for (std::size_t left = 0; left < rendered.size(); ++left)
        for (std::size_t right = left + 1; right < rendered.size(); ++right)
        {
            if (spectrumDistance (rendered[left].signature, rendered[right].signature) < kSpectralFloorDb
                && envelopeDistance (rendered[left].signature, rendered[right].signature) < kEnvelopeFloorDb)
            {
                ++collapsing;
                UNSCOPED_INFO ("collapses: " << rendered[left].name << " / " << rendered[right].name);
            }
        }

    CHECK (collapsing == 0);
}

TEST_CASE ("every modulation row the bank writes changes the render", "[plugin][bank]")
{
    XerumAudioProcessor probe;
    probe.prepareToPlay (kSampleRate, kBlockSize);

    const auto bank = findBank (probe);
    REQUIRE_FALSE (bank.files.empty());

    int checked = 0;

    for (const auto& file : bank.files)
    {
        juce::MemoryBlock bytes;
        REQUIRE (juce::File (juce::String (file.string())).loadFileAsData (bytes));

        XerumAudioProcessor processor;
        processor.prepareToPlay (kSampleRate, kBlockSize);
        processor.setStateInformation (bytes.getData(), static_cast<int> (bytes.getSize()));

        const auto document = processor.document();

        // Which rows the preset actually wires: a row with no source contributes nothing by definition.
        std::vector<int> rows;

        for (int row = 0; row < xerum::plugin::kMatrixRows; ++row)
        {
            const auto found = document.parameters.find ("ModSlot" + std::to_string (row) + "/source[0]");
            double source = 0.0;

            if (found != document.parameters.end() && found->second.numberAsDouble (source)
                && static_cast<int> (source) != 0)
                rows.push_back (row);
        }

        if (rows.empty())
            continue;

        // The preset as it ships, and the same preset with one row removed. If the two render the same,
        // the row is decoration: a preset that claims a route the engine does not evaluate. This is the
        // failure `ModulationMatrix::setSlots` produces when a row's destination cannot be resolved, and
        // it is invisible to every other check in this file.
        const auto rendered = [&bytes] (const xerum::model::PresetDocument& modified, const bool wheel)
        {
            XerumAudioProcessor playing;
            playing.prepareToPlay (kSampleRate, kBlockSize);
            playing.setStateInformation (bytes.getData(), static_cast<int> (bytes.getSize()));
            playing.applyDocument (modified, {}, 0);

            const auto noteThenRelease = xerum::plugin::tests::noteThenRelease (60, kNoteBlocks);

            return xerum::plugin::tests::render (
                playing, kNoteBlocks + kTailBlocks, kBlockSize,
                [&noteThenRelease, wheel] (juce::MidiBuffer& events, const int block)
                {
                    if (wheel && block == 0)
                        events.addEvent (juce::MidiMessage::controllerEvent (1, 1, 127), 0);

                    noteThenRelease (events, block);
                });
        };

        for (const auto row : rows)
        {
            auto without = document;
            const auto prefix = "ModSlot" + std::to_string (row) + "/";

            for (auto it = without.parameters.begin(); it != without.parameters.end();)
                it = it->first.starts_with (prefix) ? without.parameters.erase (it) : std::next (it);

            // A row that reads the mod wheel needs the wheel up to be measured at all; every other row
            // moves on its own, so it is measured with the wheel down.
            const auto found = document.parameters.find (prefix + "source[0]");
            double source = 0.0;
            const bool wheelRow = found != document.parameters.end()
                                  && found->second.numberAsDouble (source)
                                  && static_cast<int> (source) == 1;

            const auto withRow = summarise (rendered (document, wheelRow));
            const auto withoutRow = summarise (rendered (without, wheelRow));

            ++checked;

            INFO ("preset " << document.name << ", row " << row);
            CHECK (separation (withRow, withoutRow) > 1.0);
        }
    }

    // The bank is not required to modulate at all, but every row it does write has to do something.
    std::fprintf (stderr, "[bank] %d modulation row(s) across the bank verified as live\n", checked);
}
