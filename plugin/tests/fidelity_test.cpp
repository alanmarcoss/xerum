// Ticket 46 — the fidelity layer: xerum's own render against the captured reference render.
//
// Ticket 15 specified three suites and built two. The third — "xerum's output is close enough to a golden
// render" — was blocked on getting a preset into the *reference* instrument; ticket 20 solved that, ticket 32
// drove it end to end, and the blocker then went unnoticed for a while. This file is the comparison itself.
// `docs/test-strategy.md` is the specification.
//
// # The method, and the finding that decided it
//
// The ticket expected the host harness to be the xerum side, with the preset arriving through the state
// block. **That route does not exist, and it fails in the worst way: silently.** Measured, not assumed:
//
//   * The harness's `--load-state` hands the bytes to `AudioPluginInstance::setStateInformation`, which for
//     a JUCE-hosted **VST3** is not a transport of the plugin's own state: JUCE's host-side wrapper requires
//     its *own* envelope (`VC2!` binary XML containing `<VST3PluginState><IComponent>` with the plugin's
//     bytes base64'd inside — `juce_VST3PluginFormatImpl.h`), and drops anything else without a word.
//     `--plugin xerum.vst3 --load-state <216 370-byte block> --dump-state out` writes the identical 318-byte
//     init document that the same command writes with no `--load-state` at all. The plugin was rendering its
//     init patch, and a comparison measured against the wrong patch looks exactly like a comparison
//     measured against the right one — only the numbers differ.
//   * The **AU** transport is a property list rather than raw bytes, so it has its own envelope too.
//
// So this file drives `XerumAudioProcessor` directly. It is the same object the VST3 and AU bundles wrap —
// the wrappers are JUCE shims, not audio code — and the plugin tests already link those translation units,
// so nothing is being reimplemented at the audio level. What *is* reimplemented is the **stimulus**, because
// the goldens were captured by the harness driving the reference: 2 s, note 60, note-on at 0.02 s, note-off
// at 0.5 s, `--pre-roll` silent blocks before the note, the last block truncated. Reimplementing a stimulus
// and hoping is how a comparison quietly becomes meaningless, so the reproduction is **proved**: the
// `[fidelity-stimulus]` case renders xerum's init patch both ways — in process, and through the harness —
// and requires the two to be **byte-identical**. Every metric in this file is therefore taken under a
// stimulus that has been shown to be the harness's, without needing a JUCE-internal transport envelope.
//
// The capture configuration is read from the manifest rather than written here twice: seconds, note, rate,
// block size, pre-roll and settle are `tests/golden/manifest.json`'s `capture` block. Ticket 20's finding is
// that those values *are* the protocol — an unsettled render of a loaded preset is not reproducible at all,
// and pre-roll 128/256/512 give stable but *different* renders.
//
// # The three metrics ticket 15 names, per fixture, never averaged
//
//   * **null depth** — `20 log10(rms(golden - xerum) / rms(golden))`: how much of xerum's output the
//     reference's cancels. Both signals start at the same sample by construction, so zero lag is the
//     headline; a best-lag figure sits beside it because a *latency* difference and a *content* difference
//     both wreck a null and they mean different things.
//   * **RMS difference** — `rms(golden - xerum)`, absolute, in dBFS: the same residual without the
//     normalisation, so a level difference is visible next to a content one.
//   * **spectral distance** — log-spectral distance: mean |delta magnitude| in dB over 2048-point Hann
//     frames at 512-sample hop. Independent of the other two, and the one a *synth* gets wrong: a patch can
//     carry the right energy and the wrong timbre.
//
// # Rates and blocks are swept
//
// The golden is one 48 kHz capture, so a render at any other rate is brought onto the golden's own grid by a
// windowed-sinc resampler and compared there: one target grid, one metric definition. The resampler's floor
// is *measured* by `[fidelity-resampler]` rather than assumed.
//
// # Thresholds come from the measurement
//
// Ticket 15: *"the first run records what the engine actually achieves, and the tolerance is derived from
// that. A tolerance chosen before the first measurement is a guess wearing a number."* The gate table below
// is therefore the **measured baseline plus a stated regression margin**, and both are recorded. xerum is not
// expected to match the reference sound for sound — tickets 25, 26, 29 and 37 each recorded that their curves
// implement documented behaviour rather than measure the reference — and closing that distance is a different
// effort. The first numbers are poor by design; this file's job is to make them visible, reproducible, and
// hard to worsen without noticing.
//
// # The report
//
// Every measurement is written to `~/.xerum-fixtures/fidelity/fidelity-report.tsv` (outside the repository,
// per ticket 10) and printed to stderr. `XERUM_FIDELITY_DUMP=<dir>` additionally writes the exact pair of
// signals each metric was computed on, so a reading can be listened to instead of only read.

#include "PresetState.h"
#include "XerumAudioProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
    #include <sys/wait.h>
#endif

using namespace xerum::plugin;

namespace fs = std::filesystem;

namespace
{

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------------------
// Where things are
// ---------------------------------------------------------------------------------------------------

std::string envOr (const char* name, const std::string& fallback)
{
    if (const char* value = std::getenv (name); value != nullptr && *value != '\0')
        return value;

    return fallback;
}

constexpr const char* kDefaultContentRoot = "/Library/Audio/Presets/Xfer Records/Serum 2 Presets";

/// Is there a loadable binary inside the plugin bundle?
///
/// A VST3 bundle directory exists as soon as CMake has configured the plugin, but `juce_add_plugin` puts the
/// binary in `Contents/MacOS/` only once the **wrapper** target has been built. So a tree where only the
/// shared-code target was built has a bundle that `exists()` says is there and that no host can load — a
/// missing prerequisite rather than a fidelity result, and it must skip loudly instead of failing hundreds of
/// renders for a reason that is not the engine's. (Found by doing exactly that.)
bool pluginBundleHasBinary (const fs::path& bundle)
{
    std::error_code ec;
    const auto binaries = bundle / "Contents/MacOS";

    if (! fs::is_directory (binaries, ec))
        return false;

    for (const auto& entry : fs::directory_iterator (binaries, ec))
        if (entry.is_regular_file (ec))
            return true;

    return false;
}

fs::path repoRoot() { return fs::path (XERUM_REPO_ROOT); }
fs::path thisBuild() { return fs::path (XERUM_BUILD_DIR); }

/// Everything this layer produces is derived from the reference's material, so it lives outside the
/// repository (ticket 10, `docs/content-policy.md`) and is regenerated rather than committed.
fs::path artefactDir()
{
    const auto dir = fs::path (envOr ("HOME", ".")) / ".xerum-fixtures/fidelity";
    std::error_code ec;
    fs::create_directories (dir, ec);
    return dir;
}

/// Find an artefact of *this* build first, then an explicit override, then any build tree in the repository.
///
/// The preference order is ticket 32's, for the reason recorded there: a scan that prefers a short path picks
/// a peer's tree, so a stale harness sitting elsewhere fails this test even when the one next to the test
/// binary is fine. Whatever is chosen is printed, so a failure names the binary responsible.
fs::path findArtefact (const fs::path& relative, const char* envName)
{
    std::vector<fs::path> candidates;
    candidates.push_back (thisBuild() / relative);

    if (const auto fromEnv = envOr (envName, ""); ! fromEnv.empty())
        candidates.emplace_back (fromEnv);

    std::error_code ec;

    if (fs::is_directory (repoRoot(), ec))
        for (const auto& entry : fs::directory_iterator (repoRoot(), ec))
        {
            const auto name = entry.path().filename().string();

            if (name == "build" || name.rfind ("build-", 0) == 0)
                candidates.push_back (entry.path() / relative);
        }

    for (const auto& candidate : candidates)
        if (fs::exists (candidate, ec))
        {
            std::fprintf (stderr, "[fidelity] %s: %s\n", envName, candidate.c_str());
            return candidate;
        }

    return {};
}

// ---------------------------------------------------------------------------------------------------
// The corpus: the manifest, and this machine's copy of the renders
// ---------------------------------------------------------------------------------------------------

struct Fixture
{
    std::string name;
    std::string slug;
    std::string covers;
    std::string presetPath;
    std::string wavName;
    std::string wavPath;
    std::string sha256;
    std::int64_t bytes { 0 };
    bool presetPresent { false };
    bool wavPresent { false };
};

struct Corpus
{
    fs::path manifestPath;
    fs::path fixtureDir;
    fs::path contentRoot;
    fs::path host;
    fs::path plugin;

    bool manifestOk { false };
    std::string manifestError;

    std::vector<Fixture> fixtures;
    std::vector<std::string> uncoveredTargets;

    // The capture protocol, read from the manifest rather than repeated here (tickets 09 and 20).
    double captureSeconds { 2.0 };
    int captureSampleRate { 48000 };
    int captureBlockSize { 512 };
    int captureNote { 60 };
    int capturePreRoll { 128 };
    int captureSettleMs { 1500 };
    std::string captureReceipt;
    std::string capturePlugin;

    /// What the comparison itself needs: the manifest, the fixture presets, and the golden renders.
    std::string corpusReason;
    /// What only the stimulus cross-check needs: the harness and xerum's built bundle.
    std::string harnessReason;

    bool corpusAvailable() const { return corpusReason.empty(); }
    bool harnessAvailable() const { return harnessReason.empty(); }
};

Corpus loadCorpus()
{
    Corpus out;
    out.manifestPath = repoRoot() / "tests/golden/manifest.json";
    out.fixtureDir = envOr ("XERUM_FIXTURE_DIR", (fs::path (envOr ("HOME", ".")) / ".xerum-fixtures").string());
    out.contentRoot = envOr ("XERUM_SERUM_CONTENT_ROOT", kDefaultContentRoot);
    out.host = findArtefact ("host/xerum-host_artefacts/Release/xerum-host", "XERUM_HOST_BIN");
    out.plugin = findArtefact ("plugin/xerum_artefacts/Release/VST3/xerum.vst3", "XERUM_XERUM_PLUGIN");

    std::error_code ec;

    if (! fs::exists (out.manifestPath, ec))
    {
        out.manifestError = "the golden manifest is not in this checkout (" + out.manifestPath.string() + ")";
        out.corpusReason = out.manifestError;
        out.harnessReason = out.manifestError;
        return out;
    }

    const auto manifest = juce::JSON::parse (juce::File (juce::String (out.manifestPath.string())));

    if (! manifest.isObject())
    {
        out.manifestError = "the golden manifest at " + out.manifestPath.string() + " does not parse";
        out.corpusReason = out.manifestError;
        out.harnessReason = out.manifestError;
        return out;
    }

    out.manifestOk = true;

    if (const auto* capture = manifest["capture"].getDynamicObject())
    {
        const auto read = [capture] (const char* key, const double fallback)
        { return capture->hasProperty (key) ? (double) capture->getProperty (key) : fallback; };

        out.captureSeconds = read ("seconds", 2.0);
        out.captureSampleRate = (int) read ("sampleRate", 48000.0);
        out.captureBlockSize = (int) read ("blockSize", 512.0);
        out.captureNote = (int) read ("note", 60.0);
        out.capturePreRoll = (int) read ("preRoll", 128.0);
        out.captureSettleMs = (int) read ("settleMs", 1500.0);
        out.captureReceipt = capture->getProperty ("receipt").toString().toStdString();
        out.capturePlugin = capture->getProperty ("plugin").toString().toStdString();
    }

    if (const auto* targets = manifest["uncoveredTargets"].getArray())
        for (const auto& target : *targets)
            out.uncoveredTargets.push_back (target.toString().toStdString());

    if (const auto* fixtures = manifest["fixtures"].getArray())
        for (const auto& item : *fixtures)
        {
            Fixture fixture;
            fixture.name = item["name"].toString().toStdString();
            fixture.slug = item["slug"].toString().toStdString();
            fixture.wavName = item["wav"].toString().toStdString();
            fixture.sha256 = item["sha256"].toString().toStdString();
            fixture.bytes = (std::int64_t) (double) item["bytes"];

            if (const auto* covers = item["covers"].getArray())
            {
                juce::StringArray joined;

                for (const auto& cover : *covers)
                    joined.add (cover.toString());

                fixture.covers = joined.joinIntoString (", ").toStdString();
            }

            // The manifest records an absolute path under the content root the corpus was captured with.
            // Resolve by path, and fall back to this machine's root if they differ: it is the same preset.
            const auto declared = item["preset"].toString().toStdString();
            fixture.presetPath = declared;

            if (! fs::exists (declared, ec))
                if (const auto marker = declared.find ("/Presets/"); marker != std::string::npos)
                    fixture.presetPath = (fs::path (out.contentRoot) / declared.substr (marker + 9)).string();

            fixture.wavPath = (fs::path (out.fixtureDir) / fixture.wavName).string();
            fixture.presetPresent = fs::exists (fixture.presetPath, ec);
            fixture.wavPresent = fs::exists (fixture.wavPath, ec);

            out.fixtures.push_back (std::move (fixture));
        }

    const auto countWhere = [&out] (const bool Fixture::* field)
    {
        return (std::ptrdiff_t) std::count_if (out.fixtures.begin(), out.fixtures.end(),
                                               [field] (const Fixture& f) { return f.*field; });
    };

    const auto total = (std::ptrdiff_t) out.fixtures.size();
    const auto presets = countWhere (&Fixture::presetPresent);
    const auto wavs = countWhere (&Fixture::wavPresent);

    std::vector<std::string> missing;

    if (out.fixtures.empty())
        missing.push_back ("the golden manifest lists no fixtures");

    if (presets == 0)
        missing.push_back ("none of the " + std::to_string (total) + " fixture presets are installed under "
                           + out.contentRoot.string() + " (the corpus is drawn from the reference's factory bank, "
                             "which is not in this repository — ticket 10)");
    else if (presets < total)
        std::fprintf (stderr, "[fidelity] %td of %td fixture presets are missing from %s — those fixtures are "
                              "excluded\n", total - presets, total, out.contentRoot.c_str());

    if (wavs == 0)
        missing.push_back ("the golden renders are not captured at " + out.fixtureDir.string()
                           + " (the WAVs stay out of the repository — ticket 10; re-capture with "
                             "tools/capture-golden.py)");
    else if (wavs < total)
        std::fprintf (stderr, "[fidelity] %td of %td golden renders are missing from %s — those fixtures are "
                              "excluded\n", total - wavs, total, out.fixtureDir.c_str());

    for (std::size_t i = 0; i < missing.size(); ++i)
        out.corpusReason += (i == 0 ? "" : "; and ") + missing[i];

    std::vector<std::string> harness;

    if (out.host.empty())
        harness.push_back ("the headless harness has not been built (see host/README.md); set XERUM_HOST_BIN");

    if (out.plugin.empty())
        harness.push_back ("xerum's own plugin has not been built in " + thisBuild().string()
                           + " (plugin/xerum_artefacts/Release/VST3/xerum.vst3)");
    else if (! pluginBundleHasBinary (out.plugin))
        harness.push_back ("xerum's plugin bundle at " + out.plugin.string() + " has no binary in it — only the "
                           "shared-code target has been built; build the wrapper with "
                           "\"cmake --build <dir> --target xerum_VST3\"");

    for (std::size_t i = 0; i < harness.size(); ++i)
        out.harnessReason += (i == 0 ? "" : "; and ") + harness[i];

    return out;
}

const Corpus& corpus()
{
    static const Corpus loaded = loadCorpus();
    return loaded;
}

/// A development aid, not a test input: run the measurement over a subset of slugs while iterating.
/// `XERUM_FIDELITY_ONLY=arp-aardvark,808-straight`
std::set<std::string> slugFilter()
{
    std::set<std::string> wanted;
    const auto spec = envOr ("XERUM_FIDELITY_ONLY", "");

    for (std::size_t start = 0; start < spec.size();)
    {
        const auto comma = spec.find (',', start);
        const auto piece = spec.substr (start, comma == std::string::npos ? std::string::npos : comma - start);

        if (! piece.empty())
            wanted.insert (piece);

        if (comma == std::string::npos)
            break;

        start = comma + 1;
    }

    return wanted;
}

/// The fixtures this run measures: those whose preset and render are both here, optionally filtered.
std::vector<Fixture> measurableFixtures (const Corpus& c)
{
    std::vector<Fixture> out;
    const auto filter = slugFilter();

    for (const auto& fixture : c.fixtures)
    {
        if (! fixture.presetPresent || ! fixture.wavPresent)
            continue;

        if (! filter.empty() && filter.count (fixture.slug) == 0)
            continue;

        out.push_back (fixture);
    }

    return out;
}

/// The corpus's own statement of which fixtures are negative: ticket 09 included the **deferred engines**
/// deliberately, "for the import report" — xerum must *report* them rather than substitute something
/// silently, and a comparison against a reference render of one is therefore expected to diverge. They are
/// measured and reported like everything else, and excluded from the gate with this as the stated reason.
const std::set<std::string>& deferredEngineCovers()
{
    static const std::set<std::string> covers {
        "engine:GranularOsc", "engine:SampleOsc", "engine:SpectralOsc", "engine:MultiSampleOsc"
    };

    return covers;
}

bool isNegativeFixture (const Fixture& fixture)
{
    for (const auto& cover : deferredEngineCovers())
        if (fixture.covers.find (cover) != std::string::npos)
            return true;

    return false;
}

// ---------------------------------------------------------------------------------------------------
// The harness, for the one thing this layer still asks of it
// ---------------------------------------------------------------------------------------------------

std::string shellQuote (const std::string& text)
{
    std::string quoted = "'";

    for (const auto character : text)
        quoted += (character == '\'') ? std::string ("'\\''") : std::string (1, character);

    return quoted + "'";
}

std::string runCommand (const std::string& command, int* exitStatus = nullptr)
{
    if (exitStatus != nullptr)
        *exitStatus = -1;

    std::FILE* pipe = ::popen (command.c_str(), "r");

    if (pipe == nullptr)
        return {};

    std::string output;
    char buffer[4096];

    while (std::fgets (buffer, sizeof buffer, pipe) != nullptr)
        output += buffer;

    const int status = ::pclose (pipe);

    if (exitStatus != nullptr && status != -1 && WIFEXITED (status))
        *exitStatus = WEXITSTATUS (status);

    return output;
}

std::string firstLines (const fs::path& path, const std::size_t lines)
{
    std::ifstream file (path);
    std::string out;
    std::string line;

    for (std::size_t i = 0; i < lines && std::getline (file, line); ++i)
        out += line + " | ";

    return out;
}

/// The SHA-256 of a file, computed by the system's own tool, so the corpus on this disk is *checked* against
/// the manifest's record rather than assumed to be it. Returns the empty string and a reason if neither tool
/// exists — a machine without one skips this check rather than failing on it.
std::string fileSha256 (const fs::path& path, std::string& error)
{
    static const char* const tools[] { "shasum -a 256", "sha256sum" };

    for (const auto* tool : tools)
    {
        const auto output = runCommand (std::string (tool) + " " + shellQuote (path.string()) + " 2>/dev/null");
        const auto hash = juce::String (output).trim().upToFirstOccurrenceOf (" ", false, false).toLowerCase();

        if (hash.length() == 64)
            return hash.toStdString();
    }

    error = "neither shasum nor sha256sum is available";
    return {};
}

/// The harness's own environment failures, as opposed to a render that went wrong: a plugin it cannot find or
/// instantiate is a missing prerequisite for this layer and must **skip**, while a plugin that crashes on a
/// preset is a fidelity finding and must **fail**.
std::string harnessEnvironmentProblem (const fs::path& log)
{
    static const char* const patterns[] {
        "no plugin description found", "could not create instance", "plugin not found", "unrecognised argument"
    };

    std::ifstream file (log);
    std::string line;

    while (std::getline (file, line))
        for (const auto* pattern : patterns)
            if (line.find (pattern) != std::string::npos)
                return line;

    return {};
}

/// Render xerum's *init* patch through the harness — no preset, no state. This is the reference side of the
/// stimulus cross-check and nothing else; nothing about a fixture's fidelity comes through here.
std::string harnessRender (const Corpus& c, const int sampleRate, const int blockSize, const fs::path& wav)
{
    const auto log = wav.string() + ".harness.log";

    const auto command = shellQuote (c.host.string())
                       + " --plugin " + shellQuote (c.plugin.string())
                       + " --out " + shellQuote (wav.string())
                       + " --seconds " + std::to_string (c.captureSeconds)
                       + " --sr " + std::to_string (sampleRate)
                       + " --block " + std::to_string (blockSize)
                       + " --note " + std::to_string (c.captureNote)
                       + " --pre-roll " + std::to_string (c.capturePreRoll)
                       + " --settle-ms " + std::to_string (c.captureSettleMs)
                       + " --quiet > " + shellQuote (log) + " 2>&1";

    int status = -1;
    runCommand (command, &status);

    if (status == 0 && fs::exists (wav))
        return {};

    if (const auto environment = harnessEnvironmentProblem (log); ! environment.empty())
        return environment;

    return "the harness exited " + std::to_string (status) + ": " + firstLines (log, 3);
}

// ---------------------------------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------------------------------

struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;
    double sampleRate { 0.0 };

    std::size_t size() const { return left.size(); }
};

bool readWav (const fs::path& path, Stereo& out, std::string& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (
        formats.createReaderFor (juce::File (juce::String (path.string()))));

    if (reader == nullptr)
    {
        error = "cannot read " + path.string();
        return false;
    }

    const auto length = (int) reader->lengthInSamples;
    const auto channels = (int) juce::jmin (2, (int) reader->numChannels);

    if (length <= 0 || channels <= 0)
    {
        error = "no audio in " + path.string();
        return false;
    }

    juce::AudioBuffer<float> buffer (channels, length);

    if (! reader->read (&buffer, 0, length, 0, true, channels > 1))
    {
        error = "read failed: " + path.string();
        return false;
    }

    const auto* second = channels > 1 ? buffer.getReadPointer (1) : buffer.getReadPointer (0);

    out.sampleRate = reader->sampleRate;
    out.left.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + length);
    out.right.assign (second, second + length);
    return true;
}

bool writeWav (const fs::path& path, const Stereo& audio)
{
    std::error_code ec;
    fs::create_directories (path.parent_path(), ec);

    juce::File file (juce::String (path.string()));
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return false;

    const auto options = juce::AudioFormatWriterOptions()
                             .withSampleRate (audio.sampleRate)
                             .withNumChannels (2)
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    auto writer = wav.createWriterFor (stream, options);

    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer (2, (int) audio.size());
    std::copy (audio.left.begin(), audio.left.end(), buffer.getWritePointer (0));
    std::copy (audio.right.begin(), audio.right.end(), buffer.getWritePointer (1));

    const auto ok = writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    writer.reset();   // closes the stream and fixes the header up
    return ok;
}

/// The largest sample-to-sample step of a signal: a cheap discontinuity detector.
///
/// It exists because of a failure shape the finiteness assertion cannot see. `VoiceFilter::processPoleZero`
/// guards an overflow that a `float` cannot hold by emitting silence for **one sample** and clearing the
/// cascade state (`src/dsp/filter.cpp`), which trades a NaN for a one-sample dropout — and a dropout leaves
/// every sample finite. One sample in 96 000 moves an RMS difference by about -40 dB, so the three metrics
/// would not notice it either; the step does.
///
/// It is **reported, never gated**. A transient sharper than the reference's is not a defect, so a threshold
/// here would be invented, and the golden's own step is reported beside xerum's to give the reader the scale.
/// `[fidelity-diagnostics]` shows the sensitivity rather than asserting it: one zeroed sample in a real
/// render raises this figure by an order of magnitude.
double maxStepOf (const std::vector<float>& samples)
{
    double worst = 0.0;

    for (std::size_t i = 1; i < samples.size(); ++i)
        worst = std::max (worst, std::abs ((double) samples[i] - (double) samples[i - 1]));

    return worst;
}

double besselI0 (const double x)
{
    double sum = 1.0;
    double term = 1.0;
    const auto half = x / 2.0;

    for (int k = 1; k < 64; ++k)
    {
        term *= (half / k) * (half / k);
        sum += term;

        if (term < 1e-16 * sum)
            break;
    }

    return sum;
}

/// A windowed-sinc resampler, so a render taken at another rate can be compared on the golden's own grid.
///
/// Polyphase: the fractional position of output sample `n` is `(n * inRate mod outRate) / g` where
/// `g = gcd (inRate, outRate)`, which repeats with a period of `outRate / g`, so the kernel is tabulated once
/// per (rate pair, phase) and each output sample is a 128-tap dot product — 64 taps either side, a Kaiser
/// β = 8.6 window and every phase's taps normalised to unity DC gain.
///
/// **128 taps, not 32, and the measurement is why.** A 32-tap kernel's response was measured (independently,
/// in numpy) as flat only to 19 kHz: -0.61 dB at 20 kHz, -5.8 dB at 22 kHz. That is a broken anti-alias filter
/// for a decimation to 48 kHz — the stopband must be deep *by* 24 kHz or content at 24-48 kHz folds into the
/// 0-24 kHz band the comparison lives in — and it also thinned xerum's own 19-22 kHz content against the
/// golden's, which showed up as a -32 dB floor on the round-trip case. At 128 taps the same measurement reads
/// flat to 22 kHz with -75 dB at 26 kHz for 96 -> 48, and -18 dB at 22 kHz for 44.1 -> 48.
struct Resampler
{
    int taps { 128 };
    int phases { 0 };
    int period { 0 };   ///< gcd(inRate, outRate): the step that `n * inRate mod outRate` always lands on
    std::vector<std::vector<double>> table;
};

const Resampler& resamplerFor (const int inRate, const int outRate)
{
    static std::map<std::pair<int, int>, Resampler> cache;
    const auto key = std::make_pair (inRate, outRate);

    if (const auto found = cache.find (key); found != cache.end())
        return found->second;

    Resampler resampler;
    resampler.period = std::gcd (inRate, outRate);
    resampler.phases = outRate / resampler.period;

    const auto half = resampler.taps / 2;
    const auto cutoff = 0.5 * std::min (1.0, (double) outRate / (double) inRate);
    const auto beta = 8.6;
    const auto i0beta = besselI0 (beta);

    resampler.table.assign ((std::size_t) resampler.phases,
                            std::vector<double> ((std::size_t) resampler.taps, 0.0));

    for (int phase = 0; phase < resampler.phases; ++phase)
    {
        const auto fraction = (double) phase / (double) resampler.phases;
        auto& row = resampler.table[(std::size_t) phase];
        double sum = 0.0;

        for (int j = 0; j < resampler.taps; ++j)
        {
            const auto x = (double) (j - (half - 1)) - fraction;

            if (std::abs (x) <= (double) half)
            {
                const auto z = 2.0 * kPi * cutoff * x;
                const auto sinc = std::abs (x) < 1e-12 ? 1.0 : std::sin (z) / z;
                const auto r = x / (double) half;
                const auto window = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0beta;
                row[(std::size_t) j] = 2.0 * cutoff * sinc * window;
            }

            sum += row[(std::size_t) j];
        }

        if (sum != 0.0)
            for (auto& tap : row)
                tap /= sum;
    }

    return cache.emplace (key, std::move (resampler)).first->second;
}

std::vector<float> resample (const std::vector<float>& in, const int inRate, const int outRate)
{
    if (in.empty() || inRate == outRate)
        return in;

    const auto& resampler = resamplerFor (inRate, outRate);
    const auto half = resampler.taps / 2;
    const auto inLength = (std::int64_t) in.size();
    const auto outLength = inLength * (std::int64_t) outRate / (std::int64_t) inRate;

    std::vector<float> out ((std::size_t) outLength, 0.0f);

    for (std::int64_t n = 0; n < outLength; ++n)
    {
        const auto numerator = n * (std::int64_t) inRate;
        const auto first = numerator / (std::int64_t) outRate;
        const auto phase = (int) ((numerator % (std::int64_t) outRate) / resampler.period);
        const auto& row = resampler.table[(std::size_t) phase];
        double accumulated = 0.0;

        for (int j = 0; j < resampler.taps; ++j)
        {
            const auto index = first + j - (half - 1);

            if (index < 0 || index >= inLength)
                continue;

            accumulated += (double) in[(std::size_t) index] * row[(std::size_t) j];
        }

        out[(std::size_t) n] = (float) accumulated;
    }

    return out;
}

// ---------------------------------------------------------------------------------------------------
// The three metrics
// ---------------------------------------------------------------------------------------------------

double db (const double linear)
{
    return 20.0 * std::log10 (std::max (linear, 1e-30));
}

double rmsOf (const std::vector<float>& samples)
{
    if (samples.empty())
        return 0.0;

    double sum = 0.0;

    for (const auto sample : samples)
    {
        const auto value = (double) sample;
        sum += value * value;
    }

    return std::sqrt (sum / (double) samples.size());
}

double peakOf (const std::vector<float>& samples)
{
    double peak = 0.0;

    for (const auto sample : samples)
        peak = std::max (peak, std::abs ((double) sample));

    return peak;
}

bool allFinite (const std::vector<float>& samples)
{
    for (const auto sample : samples)
        if (! std::isfinite (sample))
            return false;

    return true;
}

/// The log-spectral distance between two equal-length signals, in dB.
///
/// 2048-point Hann frames at 512-sample hop. The mean is taken over the bins where the **reference** has
/// content — within 60 dB of the golden's own peak magnitude — because a mean over *every* bin is dominated
/// by bins where neither signal has anything, and averaging a noise floor against a noise floor measures the
/// floor rather than the timbre. Both spectra are floored at the same window floor, so the metric is bounded
/// by those 60 dB: near 0 when the two timbres agree, tens of dB when xerum has nothing where the reference
/// has content, which is the shape of the failure this layer exists to show.
double logSpectralDistance (const std::vector<float>& golden, const std::vector<float>& xerum)
{
    constexpr int fftSize = 2048;
    constexpr int hop = 512;
    constexpr double windowDb = 60.0;

    if (golden.size() < (std::size_t) fftSize || golden.size() != xerum.size())
        return 0.0;

    juce::dsp::FFT fft (11);
    std::vector<float> window ((std::size_t) fftSize);

    for (int i = 0; i < fftSize; ++i)
        window[(std::size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / (fftSize - 1)));

    std::vector<float> bufferGolden ((std::size_t) fftSize * 2, 0.0f);
    std::vector<float> bufferXerum ((std::size_t) fftSize * 2, 0.0f);

    const auto frames = (std::size_t) ((golden.size() - (std::size_t) fftSize) / (std::size_t) hop + 1);
    const auto bins = (std::size_t) (fftSize / 2 - 1);   // 1..N/2-1; DC and Nyquist are not modes
    std::vector<double> goldenMagnitudes (frames * bins, 0.0);
    std::vector<double> xerumMagnitudes (frames * bins, 0.0);
    double peak = 0.0;

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto offset = frame * (std::size_t) hop;

        for (int i = 0; i < fftSize; ++i)
        {
            bufferGolden[(std::size_t) i] = golden[offset + (std::size_t) i] * window[(std::size_t) i];
            bufferXerum[(std::size_t) i] = xerum[offset + (std::size_t) i] * window[(std::size_t) i];
        }

        fft.performFrequencyOnlyForwardTransform (bufferGolden.data(), true);
        fft.performFrequencyOnlyForwardTransform (bufferXerum.data(), true);

        for (std::size_t bin = 0; bin < bins; ++bin)
        {
            const auto goldenMagnitude = (double) bufferGolden[bin + 1];
            goldenMagnitudes[frame * bins + bin] = goldenMagnitude;
            xerumMagnitudes[frame * bins + bin] = (double) bufferXerum[bin + 1];
            peak = std::max (peak, goldenMagnitude);
        }
    }

    if (peak <= 0.0)
        return 0.0;

    const auto floorDb = 20.0 * std::log10 (peak) - windowDb;
    double total = 0.0;
    std::size_t counted = 0;

    for (std::size_t i = 0; i < goldenMagnitudes.size(); ++i)
    {
        const auto goldenDb = 20.0 * std::log10 (std::max (goldenMagnitudes[i], 1e-12));

        if (goldenDb < floorDb)
            continue;

        const auto xerumDb = 20.0 * std::log10 (std::max (xerumMagnitudes[i], 1e-12));
        total += std::abs (std::max (goldenDb, floorDb) - std::max (xerumDb, floorDb));
        ++counted;
    }

    return counted == 0 ? 0.0 : total / (double) counted;
}

struct Metrics
{
    bool comparable { false };
    double nullDb { 0.0 };          ///< 20 log10(rms(residual) / rms(golden)) — how much the reference cancels
    double bestLagNullDb { 0.0 };   ///< the same at the lag that nulls best, so a latency shows as a lag
    int bestLagSamples { 0 };
    double rmsDiff { 0.0 };         ///< rms(residual), absolute
    double rmsDiffDb { 0.0 };       ///< 20 log10 of the above, dBFS
    double rmsGolden { 0.0 };       ///< worst channel
    double rmsXerum { 0.0 };
    double peakGolden { 0.0 };
    double peakXerum { 0.0 };
    double stepGolden { 0.0 };      ///< largest sample-to-sample step: a one-sample dropout is visible here
    double stepXerum { 0.0 };
    double spectralDb { 0.0 };      ///< log-spectral distance, mean |delta magnitude| in dB
    bool finite { true };
};

/// One channel's contribution, so the aggregate is explicit about averaging and about the lag.
struct ChannelMetrics
{
    bool measured { false };
    double nullDb { 0.0 };
    double bestLagNullDb { 0.0 };
    int bestLagSamples { 0 };
    double residualRms { 0.0 };
    double stepGolden { 0.0 };
    double stepXerum { 0.0 };
    double spectralDb { 0.0 };
};

ChannelMetrics measureChannel (const std::vector<float>& golden, const std::vector<float>& xerum,
                               const bool withLagSearch)
{
    ChannelMetrics out;
    const auto count = std::min (golden.size(), xerum.size());

    if (count == 0)
        return out;

    std::vector<float> residual (count);

    for (std::size_t i = 0; i < count; ++i)
        residual[i] = xerum[i] - golden[i];

    const auto rmsGolden = rmsOf (golden);
    const auto rmsResidual = rmsOf (residual);

    out.measured = true;
    out.residualRms = rmsResidual;
    out.nullDb = rmsGolden > 0.0 ? db (rmsResidual / rmsGolden) : 0.0;
    out.spectralDb = logSpectralDistance (golden, xerum);
    out.stepGolden = maxStepOf (golden);
    out.stepXerum = maxStepOf (xerum);
    out.bestLagNullDb = out.nullDb;

    if (! withLagSearch)
        return out;

    // A pure latency difference and a content difference both wreck a null test, and they mean different
    // things: one is a delay to compensate, the other is a synth that is simply wrong. So the best lag is
    // searched and reported beside the zero-lag figure rather than replacing it.
    auto best = std::numeric_limits<double>::infinity();
    int bestLag = 0;

    for (int lag = -128; lag <= 128; ++lag)
    {
        const auto from = (std::size_t) std::max (0, -lag);
        const auto to = (std::size_t) std::min ((std::ptrdiff_t) count, (std::ptrdiff_t) count - lag);

        if (to <= from)
            continue;

        double sum = 0.0;

        for (std::size_t i = from; i < to; ++i)
        {
            const auto difference = (double) xerum[i + (std::size_t) lag] - (double) golden[i];
            sum += difference * difference;
        }

        const auto value = std::sqrt (sum / (double) (to - from));

        if (value < best)
        {
            best = value;
            bestLag = lag;
        }
    }

    if (rmsGolden > 0.0 && std::isfinite (best))
    {
        out.bestLagNullDb = db (best / rmsGolden);
        out.bestLagSamples = bestLag;
    }

    return out;
}

Metrics measure (const Stereo& golden, const Stereo& xerum, const bool withLagSearch)
{
    Metrics metrics;
    const auto left = measureChannel (golden.left, xerum.left, withLagSearch);
    const auto right = measureChannel (golden.right, xerum.right, withLagSearch);

    if (! left.measured && ! right.measured)
        return metrics;

    const auto channels = (left.measured ? 1 : 0) + (right.measured ? 1 : 0);

    metrics.comparable = true;
    metrics.finite = allFinite (golden.left) && allFinite (golden.right)
                  && allFinite (xerum.left) && allFinite (xerum.right);
    metrics.nullDb = (left.nullDb + right.nullDb) / channels;
    metrics.spectralDb = (left.spectralDb + right.spectralDb) / channels;
    metrics.rmsDiff = std::max (left.residualRms, right.residualRms);
    metrics.rmsDiffDb = db (metrics.rmsDiff);
    metrics.rmsGolden = std::max (rmsOf (golden.left), rmsOf (golden.right));
    metrics.rmsXerum = std::max (rmsOf (xerum.left), rmsOf (xerum.right));
    metrics.peakGolden = std::max (peakOf (golden.left), peakOf (golden.right));
    metrics.peakXerum = std::max (peakOf (xerum.left), peakOf (xerum.right));
    metrics.stepGolden = std::max (left.stepGolden, right.stepGolden);
    metrics.stepXerum = std::max (left.stepXerum, right.stepXerum);

    const auto better = left.bestLagNullDb <= right.bestLagNullDb ? left : right;
    metrics.bestLagNullDb = better.bestLagNullDb;
    metrics.bestLagSamples = better.bestLagSamples;

    return metrics;
}

// ---------------------------------------------------------------------------------------------------
// Rendering xerum
// ---------------------------------------------------------------------------------------------------

/// A fixture prepared for rendering: the reference's container read into a document (`loadPresetFile`), then
/// written as xerum's own state block (`encodeStateBlock`). That is the whole load path — a state restore and
/// a preset load meet in `applyDocument` (`PresetState.h`) — and ticket 32 proved the block survives the round
/// trip byte-identically.
struct PreparedFixture
{
    bool ok { false };
    std::string error;
    std::vector<std::byte> stateBlock;
    std::size_t parameterCount { 0 };
    std::string documentName;
    int reportEntries { 0 };
    std::string reportNote;
};

PreparedFixture prepareFixture (const Fixture& fixture)
{
    PreparedFixture out;

    const auto loaded = loadPresetFile (juce::File (juce::String (fixture.presetPath)));

    if (! loaded.ok)
    {
        out.error = "xerum could not read the preset: " + loaded.error;
        return out;
    }

    out.stateBlock = encodeStateBlock (loaded.document, loaded.report, loaded.reportEntriesFromModel);
    out.parameterCount = loaded.document.parameters.size();
    out.documentName = loaded.document.name;
    out.reportEntries = (int) loaded.report.entries.size();

    if (! loaded.report.entries.empty())
    {
        const auto& entry = loaded.report.entries.front();
        out.reportNote = entry.modulePath + " " + entry.item + ": " + entry.reason;
    }

    out.ok = true;
    return out;
}

/// Apply the prepared state and **require that it landed**.
///
/// This is the check the harness could not make: a state that is silently dropped turns every metric below
/// into a measurement of the init patch, and the numbers look like numbers. So the document the plugin holds
/// afterwards is compared with the one that was loaded, and a mismatch is a failure rather than a column.
bool applyAndVerify (XerumAudioProcessor& processor, const PreparedFixture& prepared, std::string& error)
{
    processor.setStateInformation (prepared.stateBlock.data(), (int) prepared.stateBlock.size());

    if (! processor.lastError().isEmpty())
    {
        error = "setStateInformation reported: " + processor.lastError().toStdString();
        return false;
    }

    const auto document = processor.document();

    if (document.parameters.size() != prepared.parameterCount || document.name != prepared.documentName)
    {
        error = "the state was not applied (the plugin holds " + std::to_string (document.parameters.size())
              + " parameters named \"" + document.name + "\", expected " + std::to_string (prepared.parameterCount)
              + " named \"" + prepared.documentName + "\")";
        return false;
    }

    return true;
}

/// The host harness's render loop, reproduced exactly (`host/main.cpp`), because the goldens were captured by
/// that loop driving the *reference*: 2 s, a note on at 0.02 s and off at 0.5 s, `preRoll` silent blocks
/// first, buffers cleared every block, the last block truncated.
///
/// Reproduced rather than approximated — and then **proved** reproduced, by `[fidelity-stimulus]`, which renders
/// xerum's init patch through both this and the harness and requires byte-identity. That is what makes these
/// numbers a comparison against the golden rather than a comparison against a second protocol.
Stereo renderStimulus (XerumAudioProcessor& processor, const int sampleRate, const int blockSize,
                       const double seconds, const int note, const int preRoll)
{
    const auto totalSamples = (juce::int64) (seconds * sampleRate);
    const auto noteOnSamples = (juce::int64) (0.02 * sampleRate);
    const auto noteOffSamples = (juce::int64) (0.5 * sampleRate);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;

    for (int block = 0; block < preRoll; ++block)
    {
        buffer.clear();
        midi.clear();
        processor.processBlock (buffer, midi);
    }

    Stereo out;
    out.sampleRate = (double) sampleRate;
    out.left.reserve ((std::size_t) totalSamples);
    out.right.reserve ((std::size_t) totalSamples);

    for (juce::int64 written = 0; written < totalSamples;)
    {
        const auto thisBlock = (int) juce::jmin ((juce::int64) blockSize, totalSamples - written);

        buffer.clear();
        midi.clear();

        if (written <= noteOnSamples && noteOnSamples < written + thisBlock)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100),
                           (int) (noteOnSamples - written));

        if (written <= noteOffSamples && noteOffSamples < written + thisBlock)
            midi.addEvent (juce::MidiMessage::noteOff (1, note), (int) (noteOffSamples - written));

        processor.processBlock (buffer, midi);

        for (int i = 0; i < thisBlock; ++i)
        {
            out.left.push_back (buffer.getSample (0, i));
            out.right.push_back (buffer.getSample (1, i));
        }

        written += thisBlock;
    }

    return out;
}

/// Drop the first `samples` from each channel.
///
/// Ticket `58`: xerum's master ceiling is a lookahead limiter, so the plugin delays its output by exactly
/// `getLatencySamples()` — which a host compensates for automatically (that is what declaring it buys) and
/// a render outside a host has to compensate for explicitly, by the same amount. The extra samples are
/// rendered at the end so that dropping the front leaves a signal of the capture's own length, on the
/// capture's own clock.
void dropLeading (Stereo& audio, const int samples)
{
    if (samples <= 0)
        return;

    const auto count = static_cast<std::size_t> (samples);

    if (audio.left.size() > count)
        audio.left.erase (audio.left.begin(), audio.left.begin() + static_cast<std::ptrdiff_t> (count));

    if (audio.right.size() > count)
        audio.right.erase (audio.right.begin(), audio.right.begin() + static_cast<std::ptrdiff_t> (count));
}

/// One render of one fixture at one configuration, in a fresh processor — the way the harness uses a fresh
/// process per render, so nothing carries between two rows of the sweep.
struct RenderResult
{
    bool ok { false };
    std::string error;
    Stereo audio;
};

RenderResult renderFixture (const Corpus& c, const PreparedFixture& prepared,
                            const int sampleRate, const int blockSize)
{
    RenderResult result;

    if (! prepared.ok)
    {
        result.error = prepared.error;
        return result;
    }

    XerumAudioProcessor processor;

    // Prepared first, then the preset applied: that is the plugin's own runtime path (`loadPresetFromFile`
    // is called from the interface, always on a prepared processor), and the path `applyDocument`'s
    // polyphony handshake is written for. `enableAllBuses` matches what the harness does before playing.
    processor.enableAllBuses();
    processor.prepareToPlay (sampleRate, blockSize);

    if (! applyAndVerify (processor, prepared, result.error))
        return result;

    // **Ticket `58`: this layer bypasses the master ceiling, and compensates its latency.**
    //
    // *Bypassed*, because the golden is a capture of the **reference**, which has no such stage — the
    // reference's schema has no limiter leaf at all. Comparing a limited render against an unlimited
    // golden would measure the limiter rather than the engine, which is the one thing this layer must not
    // do. The alternative the ticket offers — regenerating the corpus with the ceiling on — would bake
    // xerum's divergence into the reference's side of the comparison, and the divergence would stop being
    // measurable. So: off, stated here and in `docs/test-strategy.md`.
    //
    // The ceiling is set **after** the document is applied, because `applyDocument` writes every
    // parameter — and a reference preset carries no `Master0` leaf, so it resolves the ceiling back to
    // its default, which is on.
    //
    // *Compensated*, because the stage delays by its lookahead whether the ceiling is enforced or not:
    // the delay line is what makes the declared latency constant, which is what a host's delay
    // compensation can rely on. So the render is taken `latency` samples long and its front is dropped,
    // putting xerum's output on the stimulus's own clock.
    processor.setParameterValue ("Master0/plainParams/kParamLimitOn", 0.0);

    const auto latency = processor.getLatencySamples();
    const auto captureLength = static_cast<juce::int64> (c.captureSeconds * sampleRate);

    // A block of slack on the end, so the front-drop cannot leave the render a sample short of the golden.
    const auto extended = static_cast<double> (captureLength + latency + blockSize) / static_cast<double> (sampleRate);

    result.audio = renderStimulus (processor, sampleRate, blockSize, extended, c.captureNote, c.capturePreRoll);
    dropLeading (result.audio, latency);

    // Exactly the capture's own length. The extra block above is there so the front-drop cannot leave the
    // render short; it is trimmed off here, because the comparison requires the two signals to be the same
    // length *and* on the same clock, and a render a few samples long reads as a length mismatch rather
    // than as the alignment it is.
    result.audio.left.resize (static_cast<std::size_t> (captureLength));
    result.audio.right.resize (static_cast<std::size_t> (captureLength));

    result.ok = true;
    return result;
}
/// One (fixture, rate, block) measurement.
struct Measurement
{
    std::string slug;
    std::string covers;
    bool negative { false };
    int sampleRate { 0 };
    int blockSize { 0 };
    bool rendered { false };
    std::string failure;      ///< non-empty: the comparison could not be made, and why
    int reportEntries { 0 };
    std::string reportNote;
    Metrics metrics;
};

/// Render every fixture at every configuration and measure each against its golden counterpart.
///
/// `lagSearchConfig` is the one configuration whose rows get the best-lag search: it is the capture
/// configuration, the primary comparison, and the search is 257 correlations per channel — worth it there and
/// not on all the sweep rows.
std::vector<Measurement> measureGrid (const Corpus& c, const std::vector<Fixture>& fixtures,
                                      const std::vector<std::pair<int, int>>& configurations,
                                      const std::pair<int, int>& lagSearchConfig)
{
    std::vector<Measurement> measurements;
    std::map<std::string, PreparedFixture> prepared;
    std::map<std::string, Stereo> goldens;
    std::map<std::string, std::string> goldenErrors;

    for (const auto& fixture : fixtures)
        prepared[fixture.slug] = prepareFixture (fixture);

    for (const auto& fixture : fixtures)
    {
        Stereo golden;
        std::string error;

        if (readWav (fixture.wavPath, golden, error))
            goldens[fixture.slug] = std::move (golden);
        else
            goldenErrors[fixture.slug] = error;
    }

    for (const auto& fixture : fixtures)
    {
        const auto& state = prepared.at (fixture.slug);
        const auto& golden = goldens.count (fixture.slug) > 0 ? goldens.at (fixture.slug) : Stereo();
        const auto goldenRate = (int) std::lround (golden.sampleRate);

        for (const auto& [sampleRate, blockSize] : configurations)
        {
            Measurement measurement;
            measurement.slug = fixture.slug;
            measurement.covers = fixture.covers;
            measurement.negative = isNegativeFixture (fixture);
            measurement.sampleRate = sampleRate;
            measurement.blockSize = blockSize;
            measurement.reportEntries = state.reportEntries;
            measurement.reportNote = state.reportNote;

            if (goldenErrors.count (fixture.slug) > 0)
            {
                measurement.failure = goldenErrors.at (fixture.slug);
                measurements.push_back (std::move (measurement));
                continue;
            }

            const auto rendered = renderFixture (c, state, sampleRate, blockSize);

            if (! rendered.ok)
            {
                measurement.failure = rendered.error;
                measurements.push_back (std::move (measurement));
                continue;
            }

            const auto xerumRate = (int) std::lround (rendered.audio.sampleRate);

            // The golden is the capture's grid; anything else is brought onto it, so all three metrics keep
            // one definition and one target. The resampler's own floor is measured by `[fidelity-resampler]`.
            Stereo aligned = rendered.audio;
            aligned.sampleRate = golden.sampleRate;

            if (xerumRate != goldenRate)
            {
                aligned.left = resample (rendered.audio.left, xerumRate, goldenRate);
                aligned.right = resample (rendered.audio.right, xerumRate, goldenRate);
            }

            const auto length = std::min (golden.size(), aligned.size());

            if (length == 0 || std::abs ((std::ptrdiff_t) golden.size() - (std::ptrdiff_t) aligned.size()) > 2)
            {
                measurement.failure = "length mismatch: golden " + std::to_string (golden.size()) + " samples at "
                                    + std::to_string (goldenRate) + " Hz, xerum " + std::to_string (aligned.size())
                                    + " at " + std::to_string (goldenRate) + " Hz";
                measurements.push_back (std::move (measurement));
                continue;
            }

            Stereo trimmedGolden { std::vector<float> (golden.left.begin(), golden.left.begin() + (std::ptrdiff_t) length),
                                   std::vector<float> (golden.right.begin(), golden.right.begin() + (std::ptrdiff_t) length),
                                   golden.sampleRate };
            Stereo trimmedXerum { std::vector<float> (aligned.left.begin(), aligned.left.begin() + (std::ptrdiff_t) length),
                                  std::vector<float> (aligned.right.begin(), aligned.right.begin() + (std::ptrdiff_t) length),
                                  aligned.sampleRate };

            measurement.metrics = measure (trimmedGolden, trimmedXerum,
                                           measurement.sampleRate == lagSearchConfig.first
                                               && measurement.blockSize == lagSearchConfig.second);
            measurement.rendered = true;

            // Numbers are one thing and hearing the pair is another: `XERUM_FIDELITY_DUMP=<dir>` writes the
            // two signals the metrics were computed on, at the capture configuration only, so a reading can
            // be listened to and re-analysed without this file having to be re-run.
            if (const auto dump = envOr ("XERUM_FIDELITY_DUMP", ""); ! dump.empty()
                  && measurement.sampleRate == lagSearchConfig.first
                  && measurement.blockSize == lagSearchConfig.second)
            {
                writeWav (fs::path (dump) / (measurement.slug + "-golden.wav"), trimmedGolden);
                writeWav (fs::path (dump) / (measurement.slug + "-xerum.wav"), trimmedXerum);
            }

            measurements.push_back (std::move (measurement));
        }
    }

    return measurements;
}

// ---------------------------------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------------------------------

/// The gate set: fixtures whose regression blocks a release. Everything else is diagnostic.
///
/// **The rule that picks them is completeness, not leniency.** A preset whose import is *not* clean — whose
/// report names an engine, a warp family, a filter or an effect this build does not ship, or a modulation row
/// that resolves to nothing — is measuring that recorded gap, not the engine, so a threshold on it would be a
/// threshold on a document. A fixture whose import report is empty is `applyDocument` receiving everything the
/// preset asked for, and its numbers are the engine's own. Every gate row below is therefore a fixture with
/// **no report entries at all**, and the gate asserts that property, so a gate fixture that stops importing
/// cleanly fails rather than quietly becoming a different measurement.
///
/// The thresholds are **the measured baseline plus `kRegressionMarginDb`**, which is ticket 15's rule: the
/// first run records what the engine actually achieves and the tolerance is derived from it. Both numbers are
/// recorded — the baseline is the comment on each row (the first measurement, 48 kHz / block 512, transcribed
/// from `~/.xerum-fixtures/fidelity/fidelity-report.tsv`) and the margin is below.
///
/// The margin is a **regression budget, not a noise allowance**: the xerum side is deterministic to the sample
/// (asserted by the determinism case), so a passing gate cannot fail by accident on the same machine — it
/// fails when a DSP change makes the engine measurably worse. 3 dB is roughly a doubling of the error the
/// metric sees: far larger than any floating-point or build-configuration wobble, far smaller than the step a
/// real regression takes.
constexpr double kRegressionMarginDb = 3.0;

/// The same budget for the sweep. A render at another rate is compared after being resampled onto the golden's
/// grid, so this one also has to absorb the resampler's floor — measured by `[fidelity-resampler]` at better
/// than -40 dB of null, i.e. orders of magnitude below the residuals a cross-synth comparison produces.
///
/// The measured spread across the fifteen configurations, first run, over the gate fixtures: **at most 1.1 dB**
/// on RMS difference (ba-basist, 48 kHz/block 512 to 44.1 kHz/block 512) and **at most 0.66 dB** on spectral
/// distance (808-decomposed, 48 kHz/block 512 to 96 kHz/block 1024). Part of that is the rate axis and part is
/// real block-size dependence in the engine — ba-basist moves 2.1 dB across block sizes at a fixed rate, which
/// is a *finding* this layer reports rather than a defect it can fix. 3 dB is over twice the largest observed
/// move, so it leaves the axis where a synth actually breaks (silence, a wrong rate, a broken resampler path)
/// plainly outside the gate.
constexpr double kSweepMarginDb = 3.0;

struct GateThreshold
{
    const char* slug;
    double baselineRmsDiffDb;
    double baselineSpectralDb;
};

/// Empty on purpose until the first measurement has run: a threshold invented before it is a guess wearing a
/// number (ticket 15). The first run records the numbers into the report, and they are transcribed here.
///
/// The gate uses **RMS difference and spectral distance**, not null depth. Null depth and RMS difference are
/// the same residual in two scalings — `rmsDiff = rms(golden) * 10^(null/20)` — and for a fixed golden the one
/// determines the other, so a second row would be the same test written twice. The null is reported because
/// ticket 15 names it and because it is the interpretable form once the two converge; the absolute error is
/// the better regression signal while they have not.
const std::vector<GateThreshold>& gateThresholds()
{
    static const std::vector<GateThreshold> thresholds {
        // Transcribed from ~/.xerum-fixtures/fidelity/fidelity-report.tsv, first full run, 2026-10-14, from a
        // **clean** build, at 48 kHz / block 512, rounded to 0.01 dB. The seven rows are every fixture in the
        // corpus whose import report is empty and which is not one of ticket 09's negative fixtures;
        // `arp-bell-arperium` also imports cleanly but is a negative fixture, so the gate follows the corpus
        // and leaves it out. Thresholds are these plus kRegressionMarginDb.
        //
        // A clean rebuild reproduced these to the digit, three separate processes produced byte-identical
        // renders for all 32 fixtures, and an independent numpy implementation of the three metrics agreed
        // with this one to within 0.004 dB (all measured; see the ticket's report). An earlier reading — from
        // a binary built while the tree was still moving, another session was editing the DSP at the time —
        // differed for four fixtures. That is why the report names the executable that produced it, and why
        // these numbers describe the tree they were measured on rather than "xerum".
        // slug                          rmsDiff dB   spectral dB
        { "arp-aardvark",               -25.44,      10.85 },   // engine:Wavetable
        { "arp-acid101",                -21.72,       9.89 },   // engine:SubOsc
        { "ba-funk-me-electric-2",      -20.79,      14.76 },   // warp:kBendPos
        { "ba-basist",                  -19.14,      15.00 },   // warp:kPWM
        { "ba-deep-throat-bass",        -19.52,      18.85 },   // warp:kRM_OSC
        { "808-decomposed",             -21.06,      14.60 },   // filter:MgL24
        { "808-diesel",                 -18.33,      14.95 },   // fx:FXFilter
    };

    return thresholds;
}

// ---------------------------------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------------------------------

std::string tableLine (const Measurement& m)
{
    char buffer[512];

    if (! m.failure.empty())
    {
        std::snprintf (buffer, sizeof buffer, "%-30s %-26s %6d/%5d  %s",
                       m.slug.c_str(), m.covers.c_str(), m.sampleRate, m.blockSize, m.failure.c_str());
        return buffer;
    }

    std::snprintf (buffer, sizeof buffer,
                   "%-30s %-26s %6d/%5d  %8.2f  %8.2f %5d  %8.2f  %8.2f  %8.2f  %8.2f %9.4f %9.4f%s%s",
                   m.slug.c_str(), m.covers.c_str(), m.sampleRate, m.blockSize,
                   m.metrics.nullDb, m.metrics.bestLagNullDb, m.metrics.bestLagSamples,
                   m.metrics.rmsDiffDb, m.metrics.rmsXerum, m.metrics.rmsGolden, m.metrics.spectralDb,
                   m.metrics.stepXerum, m.metrics.stepGolden,
                   m.metrics.finite ? "" : "  NON-FINITE",
                   m.metrics.rmsXerum <= 1e-9 ? "  SILENT" : "");

    return buffer;
}

const char* kTableHeader =
    "fixture                        covers                     rate/block   null(dB)  nullBest  lag  rmsDiff  rmsXerum  rmsGolden  spectral(dB)  stepXerum stepGolden";

void printTable (const std::vector<Measurement>& rows, const std::string& title)
{
    std::fprintf (stderr, "\n[fidelity] %s\n%s\n", title.c_str(), kTableHeader);

    for (const auto& row : rows)
        std::fprintf (stderr, "%s\n", tableLine (row).c_str());
}

std::string reportHeader (const Corpus& c, const std::vector<std::pair<int, int>>& configurations)
{
    std::ostringstream out;
    out << "# xerum fidelity report — ticket 46 (docs/test-strategy.md, layer \"Fidelity\")\n"
        << "#   measured by " << juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName()
        << "\n"
        << "#               (an earlier reading, taken from a binary built while the tree was still moving,\n"
        << "#                differed for four fixtures: these numbers describe the tree that was built, so the\n"
        << "#                binary that produced them is part of the evidence)\n"
        << "#   manifest   " << c.manifestPath.string() << "\n"
        << "#   renders    " << c.fixtureDir.string() << "\n"
        << "#   capture    " << c.capturePlugin << " at " << c.captureSampleRate << " Hz, block "
        << c.captureBlockSize << ", " << c.captureSeconds << " s, note " << c.captureNote
        << ", pre-roll " << c.capturePreRoll << ", settle " << c.captureSettleMs << " ms\n"
        << "#   receipt    " << c.captureReceipt << "\n"
        << "#   xerum side xerum's own processor, driven in process by the harness's render loop\n"
        << "#              (host/main.cpp): " << c.captureSeconds << " s at <rate>, block <size>, note "
        << c.captureNote << ", " << c.capturePreRoll << " silent pre-roll blocks, note on at 0.02 s and off at\n"
        << "#              0.5 s, last block truncated. [fidelity-stimulus] proves that loop is the harness's by\n"
        << "#              rendering the same init patch both ways and requiring byte-identity.\n"
        << "#   preset     loadPresetFile -> encodeStateBlock -> setStateInformation, then the applied document\n"
        << "#              is compared with the loaded one, so a dropped state fails instead of measuring the init patch\n"
        << "#   harness    " << (c.host.empty() ? std::string ("(absent)") : c.host.string()) << "\n"
        << "#   plugin     " << (c.plugin.empty() ? std::string ("(absent)") : c.plugin.string()) << "\n"
        << "#   sweep      ";

    for (std::size_t i = 0; i < configurations.size(); ++i)
        out << (i == 0 ? "" : ", ") << configurations[i].first << "/" << configurations[i].second;

    out << "\n#\n"
        << "# Why the harness is not the xerum side: `--load-state` cannot carry a plugin's own state block\n"
        << "# through a JUCE-hosted VST3 — the host-side wrapper transports its own envelope\n"
        << "# (VC2! binary XML with the plugin's bytes base64'd inside a <VST3PluginState><IComponent>) and drops\n"
        << "# anything else silently. Measured: --load-state with a 216 370-byte block followed by --dump-state\n"
        << "# writes the identical 318-byte init document that the same command writes with no --load-state.\n"
        << "#\n"
        << "# Metrics, per fixture and never averaged (docs/test-strategy.md):\n"
        << "#   nullDb     20 log10(rms(golden - xerum) / rms(golden)) — how much of xerum's output the\n"
        << "#              reference's cancels. Negative is good; 0 dB means xerum is uncorrelated with the golden.\n"
        << "#   nullBest   the same at the best lag in +/-128 samples, with that lag in the next column: a\n"
        << "#              latency difference and a content difference both wreck a null and mean different things.\n"
        << "#   rmsDiff    20 log10(rms(golden - xerum)) in dBFS — the same residual, absolute.\n"
        << "#   rmsXerum   20 log10 of xerum's own RMS, so a level difference is visible next to a content one.\n"
        << "#   rmsGolden  the golden's RMS, measured on the same grid.\n"
        << "#   spectral   log-spectral distance: mean |delta magnitude| in dB over 2048-point Hann frames at 512\n"
        << "#              hop, averaged over the bins within 60 dB of the reference's own peak — a mean over every\n"
        << "#              bin would be dominated by bins where neither signal has anything. Bounded by 60 dB.\n"
        << "#   stepXerum  the largest sample-to-sample step in xerum's render, and stepGolden the same for the\n"
        << "#              reference: a discontinuity detector, reported because a guard that answers an overflow\n"
        << "#              with \"silence for one sample\" (src/dsp/filter.cpp) leaves every sample finite, so\n"
        << "#              neither finiteness nor an RMS difference sees the dropout it makes. Never gated — a\n"
        << "#              sharper transient than the reference's is not a defect.\n"
        << "#   rate/block a render at anything but the capture's own rate is resampled onto the golden's grid\n"
        << "#              before measurement; the resampler's own floor is measured by [fidelity-resampler].\n"
        << "#\n"
        << "# Limitations (recorded, not implicit):\n"
        << "#   uncoveredTargets  ";

    for (std::size_t i = 0; i < c.uncoveredTargets.size(); ++i)
        out << (i == 0 ? "" : ", ") << c.uncoveredTargets[i];

    out << "\n#                     the 626-preset factory bank contains no preset using these, so no fixture can\n"
        << "#                     cover them: three of the sixteen FX classes go unexercised by this layer.\n"
        << "#   negative fixtures the four deferred engines (granular, spectral, multisample, sample), included by\n"
        << "#                     ticket 09 as negative fixtures for the import report. xerum reports them rather\n"
        << "#                     than substituting something, so their renders are expected to diverge and they are\n"
        << "#                     excluded from the gate below. They are measured, not dropped.\n"
        << "#\n"
        << "# columns: slug\tcovers\tnegative\trate\tblock\trendered\tfailure\treportEntries\tnullDb\tnullBestDb\tbestLag\trmsDiffDb\trmsXerumDb\trmsGoldenDb\tpeakXerum\tpeakGolden\tspectralDb\tstepXerum\tstepGolden\tfinite\treportNote\n";

    return out.str();
}

/// One TSV row, assembled from fields rather than from a literal string of tabs: the metric block grew once
/// already and a missing tab silently shifts every column after it, which is exactly the kind of mistake a
/// reader of this file would not catch.
std::string reportRow (const Measurement& m)
{
    const auto number = [] (const double value) { return juce::String (value, 6).toStdString(); };

    std::vector<std::string> fields {
        m.slug,
        m.covers,
        m.negative ? "yes" : "no",
        std::to_string (m.sampleRate),
        std::to_string (m.blockSize),
        m.rendered ? "yes" : "no",
        m.failure,
        std::to_string (m.reportEntries)
    };

    if (m.rendered)
        fields.insert (fields.end(), { number (m.metrics.nullDb), number (m.metrics.bestLagNullDb),
                                       std::to_string (m.metrics.bestLagSamples), number (m.metrics.rmsDiffDb),
                                       number (m.metrics.rmsXerum), number (m.metrics.rmsGolden),
                                       number (m.metrics.peakXerum), number (m.metrics.peakGolden),
                                       number (m.metrics.spectralDb), number (m.metrics.stepXerum),
                                       number (m.metrics.stepGolden), m.metrics.finite ? "yes" : "no" });
    else
        fields.resize (fields.size() + 12);

    fields.push_back (m.reportNote);

    std::string out;

    for (std::size_t i = 0; i < fields.size(); ++i)
        out += (i == 0 ? "" : "\t") + fields[i];

    return out + "\n";
}

// ---------------------------------------------------------------------------------------------------
// The instrument's own floor
// ---------------------------------------------------------------------------------------------------

/// The resampler's floor, measured — the analysis tool's requirement, derived exactly the way the gate's
/// thresholds are (ticket 15's rule: from the measurement, never invented).
///
/// Measured over all 32 fixtures with the 128-tap kernel, first run:
///
///   44.1 kHz — worst null **-33.27 dB**, worst spectral distance **0.486 dB**. That floor is *inherent*, not
///              a filter defect: the golden carries 0.005-0.040 % of its energy above the 44.1 kHz Nyquist
///              (measured per fixture), and content that a 44.1 kHz render cannot represent cannot be
///              cancelled by any resampler. The 128-tap kernel already reaches it — the 32-tap one it
///              replaced measured -31.89 dB, 1.4 dB worse.
///   96 kHz  — worst null **-49.58 dB**, worst spectral distance **0.066 dB**.
///
/// The requirement is the measurement plus `kResamplerMarginDb`. What it protects is the sweep: a cross-rate
/// row's numbers must stay measurements of the *engine*, not of this tool. The 44.1 kHz floor sits 8 dB below
/// the smallest residual the comparison produces (arp-aardvark's -25.44 dB), which bounds its inflation of a
/// cross-rate row to well under a decibel — inside the 3 dB sweep margin.
struct RateFloor
{
    int rate;
    double nullDb;
    double spectralDb;
};

constexpr double kResamplerMarginDb = 3.0;

const std::vector<RateFloor>& resamplerFloors()
{
    static const std::vector<RateFloor> floors {
        { 44100, -33.27, 0.486 },
        { 96000, -49.58, 0.066 },
    };

    return floors;
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// The tests
// ---------------------------------------------------------------------------------------------------

TEST_CASE ("the golden manifest is intact, and the targets it cannot cover are recorded",
           "[fidelity][fidelity-manifest]")
{
    // This case needs neither the reference nor the corpus: the manifest is committed, so it runs on a fresh
    // clone and in CI. It re-verifies the claims ticket 09 recorded when it captured the corpus — every
    // fixture identified, every hash distinct — and prints the limitation the corpus carries.
    const auto& c = corpus();
    REQUIRE (c.manifestOk);

    CHECK_FALSE (c.fixtures.empty());
    CHECK_FALSE (c.uncoveredTargets.empty());

    std::set<std::string> slugs;
    std::set<std::string> hashes;

    for (const auto& fixture : c.fixtures)
    {
        INFO (fixture.slug);
        CHECK_FALSE (fixture.slug.empty());
        CHECK_FALSE (fixture.name.empty());
        CHECK_FALSE (fixture.wavName.empty());
        CHECK_FALSE (fixture.covers.empty());
        CHECK (fixture.sha256.size() == 64);
        CHECK (fixture.bytes > 0);

        slugs.insert (fixture.slug);
        hashes.insert (fixture.sha256);
    }

    // Distinct slugs and distinct hashes: two fixtures that are accidentally the same audio would make the
    // "coverage" claim above fiction, which is what ticket 09 checked and this re-checks.
    CHECK (slugs.size() == c.fixtures.size());
    CHECK (hashes.size() == c.fixtures.size());

    // The capture protocol is part of the corpus, not a default (tickets 09 and 20): a corpus whose manifest
    // forgot it would not be reproducible.
    CHECK (c.capturePreRoll > 0);
    CHECK (c.captureSettleMs > 0);
    CHECK (c.captureSampleRate > 0);
    CHECK (c.captureBlockSize > 0);
    CHECK_FALSE (c.captureReceipt.empty());

    std::fprintf (stderr, "\n[fidelity] golden corpus: %zu fixtures, capture %d Hz / block %d, %g s, note %d, "
                          "pre-roll %d, settle %d ms\n[fidelity] uncovered targets (a limitation of this layer, "
                          "not of the engine):\n",
                  c.fixtures.size(), c.captureSampleRate, c.captureBlockSize, c.captureSeconds, c.captureNote,
                  c.capturePreRoll, c.captureSettleMs);

    for (const auto& target : c.uncoveredTargets)
        std::fprintf (stderr, "[fidelity]   %s\n", target.c_str());

    std::fprintf (stderr, "[fidelity] no factory preset uses these, so no fixture can cover them: three of the "
                          "sixteen FX classes go unexercised by the fidelity layer.\n");
}

TEST_CASE ("every fixture renders through xerum and is measured against its golden render across rates and blocks",
           "[fidelity][fidelity-corpus]")
{
    const auto& c = corpus();

    if (! c.corpusAvailable())
        SKIP ("fidelity corpus not available: " + c.corpusReason);

    const auto fixtures = measurableFixtures (c);
    REQUIRE_FALSE (fixtures.empty());

    // The comparison is only meaningful against the corpus the manifest recorded, so the renders on this disk
    // are checked against it — size and SHA-256 — before anything is measured. A stale or re-captured WAV
    // would make every number below a comparison against the wrong reference, and those numbers would look
    // exactly as plausible as the right ones.
    std::string hashToolError;

    for (const auto& fixture : fixtures)
    {
        INFO (fixture.slug + " golden render");

        std::error_code sizeError;
        CHECK ((std::int64_t) fs::file_size (fixture.wavPath, sizeError) == fixture.bytes);

        const auto hash = fileSha256 (fixture.wavPath, hashToolError);

        if (hash.empty())
            WARN ("corpus hashes not verified here: " << hashToolError);
        else
            CHECK (hash == fixture.sha256);
    }

    // The axes pluginval sweeps, for its reason: a synth stable at one rate and broken at another is a common
    // failure. The capture configuration is included, so the primary table is one row of the same grid rather
    // than a separate render.
    std::set<std::pair<int, int>> configSet { { c.captureSampleRate, c.captureBlockSize } };

    for (const int rate : { 44100, 48000, 96000 })
        for (const int block : { 64, 128, 256, 512, 1024 })
            configSet.insert ({ rate, block });

    const std::vector<std::pair<int, int>> configurations (configSet.begin(), configSet.end());
    const std::pair<int, int> capture { c.captureSampleRate, c.captureBlockSize };

    std::fprintf (stderr, "\n[fidelity] measuring %zu fixtures across %zu rate/block configurations (%zu renders)\n",
                  fixtures.size(), configurations.size(), fixtures.size() * configurations.size());

    const auto started = std::chrono::steady_clock::now();
    const auto grid = measureGrid (c, fixtures, configurations, capture);
    const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();

    std::fprintf (stderr, "[fidelity] %zu renders measured in %.1f s\n", grid.size(), seconds);

    // -------------------------------------------------------------------------------------------
    // The primary table: every fixture at the capture configuration, the three metrics per fixture.
    // -------------------------------------------------------------------------------------------

    std::vector<Measurement> base;

    for (const auto& measurement : grid)
        if (measurement.sampleRate == capture.first && measurement.blockSize == capture.second)
            base.push_back (measurement);

    printTable (base, "per-fixture comparison at the capture configuration ("
                          + std::to_string (capture.first) + " Hz / block " + std::to_string (capture.second) + ")");

    // -------------------------------------------------------------------------------------------
    // The rate and block sweep.
    // -------------------------------------------------------------------------------------------

    std::fprintf (stderr, "\n[fidelity] sweep: %s\n", kTableHeader);

    for (const auto& measurement : grid)
        std::fprintf (stderr, "%s\n", tableLine (measurement).c_str());

    std::fprintf (stderr, "\n[fidelity] per-configuration mean over the non-negative fixtures (the sweep's shape "
                          "at a glance; every row is in the report):\n[fidelity]   %10s %6s %10s %10s %10s\n",
                  "rate", "block", "null(dB)", "spectral", "negative");

    for (const auto& [rate, block] : configurations)
    {
        double nullSum = 0.0;
        double spectralSum = 0.0;
        int positives = 0;
        int negatives = 0;

        for (const auto& measurement : grid)
        {
            if (measurement.sampleRate != rate || measurement.blockSize != block)
                continue;

            if (measurement.negative)
            {
                ++negatives;
                continue;
            }

            if (! measurement.rendered)
                continue;

            nullSum += measurement.metrics.nullDb;
            spectralSum += measurement.metrics.spectralDb;
            ++positives;
        }

        std::fprintf (stderr, "[fidelity]   %10d %6d %10.2f %10.2f %10d\n", rate, block,
                      positives > 0 ? nullSum / positives : 0.0,
                      positives > 0 ? spectralSum / positives : 0.0, negatives);
    }

    // -------------------------------------------------------------------------------------------
    // Every render happened.
    // -------------------------------------------------------------------------------------------

    for (const auto& measurement : grid)
    {
        INFO (measurement.slug + " at " + std::to_string (measurement.sampleRate) + "/"
              + std::to_string (measurement.blockSize));

        // A preset that is here and a render that could not be made is a defect worth failing on: it means the
        // plugin refused the preset or produced something unmeasurable, not that the machine lacks a file.
        CHECK (measurement.rendered);
        CHECK (measurement.failure.empty());

        // **Finiteness is a hard invariant at every one of the configurations, and the only thing the sweep
        // asserts rather than reports.** A NaN or an infinity at one rate is exactly the "stable at one rate,
        // broken at another" failure this axis exists to find, and unlike a metric it needs no threshold — a
        // finite render is finite. Silence is deliberately *not* asserted here: a deferred engine or a recorded
        // gap can legitimately render nothing, and that is reported per fixture below.
        //
        // This assertion is here because the layer first met that failure with nothing to fail on: a
        // `VoiceFilter::processPoleZero` cascade that overflowed a `float` from a `double` showed up only as a
        // column reading `nan`. It is fixed and guarded in `src/dsp/filter.cpp`, and this fails if it returns.
        if (measurement.rendered)
            CHECK (measurement.metrics.finite);
    }

    // -------------------------------------------------------------------------------------------
    // The engine's own health, per fixture — reported, never averaged away.
    //
    // A non-finite render and a silent one are findings about the *engine*, not about the comparison, and
    // this ticket measures the comparison rather than fixing the engine. They are named below and written
    // into the report with the fixture's own import report as the explanation; only a **gate** fixture making
    // either blocks a release. Failing the whole layer on a defect it does not fix would leave the layer
    // un-runnable, which is the state it was filed to end.
    // -------------------------------------------------------------------------------------------

    int nonFinite = 0;
    int silent = 0;

    for (const auto& measurement : base)
    {
        if (! measurement.rendered)
            continue;

        if (! measurement.metrics.finite)
            ++nonFinite;
        else if (measurement.metrics.rmsXerum <= 1e-9)
            ++silent;
    }

    std::fprintf (stderr, "\n[fidelity] %zu fixtures measured at the capture configuration: %d non-finite, %d "
                          "silent. Both are engine findings, named here rather than smoothed into the table:\n",
                  base.size(), nonFinite, silent);

    for (const auto& measurement : base)
    {
        if (! measurement.rendered || (measurement.metrics.finite && measurement.metrics.rmsXerum > 1e-9))
            continue;

        std::fprintf (stderr, "[fidelity]   %-28s %-22s %s\n[fidelity]     import report: %s\n",
                      measurement.slug.c_str(), measurement.covers.c_str(),
                      measurement.metrics.finite ? "renders nothing" : "renders non-finite samples",
                      measurement.reportNote.empty() ? "(clean import — no reason recorded)"
                                                     : measurement.reportNote.c_str());
    }

    // The comparison has to mean something: if nothing were audible, finite and non-negative, every number
    // above would be a measurement of silence.
    const auto audible = std::count_if (base.begin(), base.end(), [] (const Measurement& m)
    {
        return m.rendered && m.metrics.finite && m.metrics.rmsXerum > 1e-6 && ! m.negative;
    });

    CHECK (audible > 0);

    // -------------------------------------------------------------------------------------------
    // The gate: a named set of fixtures whose regression blocks a release.
    // -------------------------------------------------------------------------------------------

    const auto findIn = [] (const std::vector<Measurement>& rows, const std::string& slug) -> const Measurement*
    {
        for (const auto& row : rows)
            if (row.slug == slug)
                return &row;

        return nullptr;
    };

    const auto& gate = gateThresholds();

    CHECK_FALSE (gate.empty());

    for (const auto& entry : gate)
    {
        const auto* measured = findIn (base, entry.slug);

        if (measured == nullptr)
        {
            // Only reachable when XERUM_FIDELITY_ONLY filtered the run down, or a preset is missing.
            std::fprintf (stderr, "[fidelity] gate fixture %s was not measured in this run\n", entry.slug);
            continue;
        }

        INFO (std::string ("gate: ") + entry.slug);

        REQUIRE (measured->rendered);
        CHECK (measured->metrics.finite);

        // A gate fixture that renders silence is a release blocker whatever else the numbers say.
        CHECK (measured->metrics.rmsXerum > 1e-6);

        // ... and so is one that stops importing cleanly, because the gate's whole claim is that its numbers
        // are the engine's rather than a recorded gap's.
        CHECK (measured->reportEntries == 0);

        // Ticket 09's negative fixtures are excluded *because they are negative fixtures*, whatever their
        // import report says: the corpus marks them as the deferred engines' evidence, so gating on them would
        // gate on a document. Asserted rather than assumed, so a gate row cannot quietly become one.
        CHECK_FALSE (measured->negative);

        // The threshold is the recorded baseline plus the regression margin, both stated; the measurement is
        // printed whether it passes or not, so a failure shows the number rather than only the verdict.
        std::fprintf (stderr, "[fidelity] gate %-28s rmsDiff %8.2f dB (baseline %8.2f + %.1f)   spectral %8.2f dB "
                              "(baseline %8.2f + %.1f)\n",
                      entry.slug, measured->metrics.rmsDiffDb, entry.baselineRmsDiffDb, kRegressionMarginDb,
                      measured->metrics.spectralDb, entry.baselineSpectralDb, kRegressionMarginDb);

        CHECK (measured->metrics.rmsDiffDb <= entry.baselineRmsDiffDb + kRegressionMarginDb);
        CHECK (measured->metrics.spectralDb <= entry.baselineSpectralDb + kRegressionMarginDb);
    }

    // The sweep gate: the same fixtures must not get worse when the rate or the block size changes.
    for (const auto& entry : gate)
        for (const auto& measurement : grid)
        {
            if (measurement.slug != entry.slug || ! measurement.rendered)
                continue;

            if (measurement.sampleRate == capture.first && measurement.blockSize == capture.second)
                continue;

            INFO (std::string ("sweep gate: ") + entry.slug + " at " + std::to_string (measurement.sampleRate)
                  + "/" + std::to_string (measurement.blockSize));
            CHECK (measurement.metrics.finite);
            CHECK (measurement.metrics.rmsDiffDb <= entry.baselineRmsDiffDb + kSweepMarginDb);
            CHECK (measurement.metrics.spectralDb <= entry.baselineSpectralDb + kSweepMarginDb);
        }

    // -------------------------------------------------------------------------------------------
    // The negative fixtures: measured and reported, excluded from the gate with a stated reason.
    // -------------------------------------------------------------------------------------------

    const auto negatives = std::count_if (base.begin(), base.end(),
                                          [] (const Measurement& m) { return m.negative; });

    CHECK (negatives == (std::ptrdiff_t) deferredEngineCovers().size());

    std::fprintf (stderr, "\n[fidelity] negative fixtures (deferred engines, ticket 09): excluded from the gate "
                          "because xerum reports the engine rather than substituting something, so their render "
                          "is expected to diverge. Measured all the same:\n");

    for (const auto& measurement : base)
    {
        if (! measurement.negative)
            continue;

        std::fprintf (stderr, "[fidelity]   %s\n", tableLine (measurement).c_str());

        // Stated, not silently dropped: none of them is in the gate.
        for (const auto& entry : gate)
            CHECK (measurement.slug != entry.slug);
    }

    // -------------------------------------------------------------------------------------------
    // The report.
    // -------------------------------------------------------------------------------------------

    std::ostringstream report;
    report << reportHeader (c, configurations);

    report << "#\n# gate — thresholds derived from the first measurement (ticket 15), margin "
           << kRegressionMarginDb << " dB at the capture configuration and " << kSweepMarginDb
           << " dB across the sweep. Selected by completeness of the import, not by leniency: every row below\n"
           << "# imports with no report entries at all, and the gate asserts that.\n"
           << "# slug\trmsDiffBaseline\trmsDiffThreshold\tspectralBaseline\tspectralThreshold\n";

    for (const auto& entry : gate)
        report << "# " << entry.slug << '\t' << entry.baselineRmsDiffDb << '\t'
               << entry.baselineRmsDiffDb + kRegressionMarginDb << '\t'
               << entry.baselineSpectralDb << '\t' << entry.baselineSpectralDb + kRegressionMarginDb << '\n';

    report << "#\n";

    for (const auto& measurement : grid)
        report << reportRow (measurement);

    const auto reportPath = artefactDir() / "fidelity-report.tsv";

    {
        std::ofstream file (reportPath, std::ios::trunc);
        file << report.str();
    }

    std::fprintf (stderr, "\n[fidelity] report: %s\n", reportPath.c_str());
}

TEST_CASE ("xerum's render is deterministic, so the comparison is a measurement",
           "[fidelity][fidelity-determinism]")
{
    const auto& c = corpus();

    if (! c.corpusAvailable())
        SKIP ("fidelity corpus not available: " + c.corpusReason);

    const auto fixtures = measurableFixtures (c);
    REQUIRE_FALSE (fixtures.empty());

    // Two renders of the same preset have to agree to the sample, or a comparison against a golden is a
    // comparison against noise. This runs over **every** fixture at the capture configuration — the one the
    // gate's baselines are recorded from — because a single nondeterministic fixture would make one row of the
    // report a coin toss. The deferred engines are included: they have to be *deterministically* wrong, or the
    // import report's evidence would not be reproducible either.
    std::size_t compared = 0;

    for (const auto& fixture : fixtures)
    {
        const auto prepared = prepareFixture (fixture);

        if (! prepared.ok)
        {
            std::fprintf (stderr, "[fidelity] determinism: %s: %s\n", fixture.slug.c_str(), prepared.error.c_str());
            continue;
        }

        // The load path is deterministic too: the same preset must produce the same state block, or the
        // comparison would move under a fixed engine.
        const auto again = prepareFixture (fixture);
        REQUIRE (again.ok);
        CHECK (prepared.stateBlock == again.stateBlock);

        INFO (fixture.slug + " at the capture configuration");

        const auto first = renderFixture (c, prepared, c.captureSampleRate, c.captureBlockSize);
        const auto second = renderFixture (c, prepared, c.captureSampleRate, c.captureBlockSize);

        REQUIRE (first.ok);
        REQUIRE (second.ok);

        CHECK_FALSE (first.audio.left.empty());
        CHECK (first.audio.left == second.audio.left);
        CHECK (first.audio.right == second.audio.right);
        ++compared;
    }

    CHECK (compared == fixtures.size());

    // And across the rate axis, where a different sample rate and block size could hold a per-rate state that
    // is not reset between renders.
    for (const int rate : { 44100, 96000 })
    {
        const auto prepared = prepareFixture (fixtures.front());
        REQUIRE (prepared.ok);

        INFO (fixtures.front().slug + " at " + std::to_string (rate));

        const auto first = renderFixture (c, prepared, rate, 128);
        const auto second = renderFixture (c, prepared, rate, 128);

        REQUIRE (first.ok);
        REQUIRE (second.ok);

        CHECK (first.audio.left == second.audio.left);
        CHECK (first.audio.right == second.audio.right);
        ++compared;
    }

    std::fprintf (stderr, "\n[fidelity] determinism: %zu renders reproduced to the sample, and %zu state blocks "
                          "byte-identical\n", compared, fixtures.size());
}

TEST_CASE ("the in-process render loop is the harness's own loop", "[fidelity][fidelity-stimulus]")
{
    const auto& c = corpus();

    if (! c.corpusAvailable())
        SKIP ("fidelity corpus not available: " + c.corpusReason);

    if (! c.harnessAvailable())
        SKIP ("the harness is not available to reproduce the stimulus: " + c.harnessReason);

    // Renders taken in process are only comparable with the goldens if the *stimulus* is the one that captured
    // them, and the goldens were captured by `xerum-host` driving the reference. So the same init patch is
    // rendered both ways and required to be byte-identical: if this passes, `renderStimulus` is the harness's
    // loop, and every metric in this file is taken under the capture's own conditions.
    //
    // The harness side carries the timing protocol (pre-roll, settle); the in-process side is a plain
    // function, so agreement also shows the settle is a sleep and not a state change for xerum.
    const std::vector<std::pair<int, int>> configurations {
        { c.captureSampleRate, c.captureBlockSize }, { 96000, 64 }
    };

    for (const auto& [rate, block] : configurations)
    {
        INFO ("init patch at " + std::to_string (rate) + "/" + std::to_string (block));

        XerumAudioProcessor processor;
        processor.enableAllBuses();
        processor.prepareToPlay (rate, block);
        const auto inProcess = renderStimulus (processor, rate, block, c.captureSeconds, c.captureNote,
                                               c.capturePreRoll);

        const auto wav = artefactDir() / ("stimulus-" + std::to_string (rate) + "-" + std::to_string (block) + ".wav");
        std::error_code ec;
        fs::remove (wav, ec);

        if (const auto problem = harnessRender (c, rate, block, wav); ! problem.empty())
            SKIP ("the harness cannot render xerum's plugin here: " + problem);

        Stereo harness;
        std::string error;

        if (! readWav (wav, harness, error))
        {
            FAIL (error);
            continue;
        }

        REQUIRE (harness.size() == inProcess.size());

        // Byte identity, not "close": the point of this case is that the two loops are the same loop, and a
        // tolerance here would hide exactly the drift it exists to catch.
        CHECK (harness.left == inProcess.left);
        CHECK (harness.right == inProcess.right);

        const auto metrics = measure (inProcess, harness, false);

        std::fprintf (stderr, "[fidelity] stimulus %d/%d: %zu samples, in-process vs harness: null %.2f dB, "
                              "rms diff %.2f dBFS, spectral %.4f dB\n", rate, block, harness.size(),
                      metrics.nullDb, metrics.rmsDiffDb, metrics.spectralDb);
    }
}

TEST_CASE ("a one-sample dropout is visible to the step the report carries",
           "[fidelity][fidelity-diagnostics]")
{
    const auto& c = corpus();

    if (! c.corpusAvailable())
        SKIP ("fidelity corpus not available: " + c.corpusReason);

    const auto fixtures = measurableFixtures (c);
    REQUIRE_FALSE (fixtures.empty());

    // The step metric exists for the failure shape the finiteness assertion cannot see. A guard that answers an
    // overflow with "silence for one sample and clear the cascade" (`src/dsp/filter.cpp`) leaves every sample
    // finite, and one sample in 96 000 moves an RMS difference by about -40 dB, so neither the assertion nor
    // the metrics would notice the dropout it makes. Its sensitivity is therefore *shown* here rather than
    // asserted in the abstract: a real render has its loudest sample zeroed — the dropout's best case, and the
    // only case a peak bound would stand a chance on — and the figure it produces is printed against the
    // untouched render's and against the peak.
    const auto prepared = prepareFixture (fixtures.front());
    REQUIRE (prepared.ok);

    const auto rendered = renderFixture (c, prepared, c.captureSampleRate, c.captureBlockSize);
    REQUIRE (rendered.ok);
    REQUIRE_FALSE (rendered.audio.left.empty());

    auto dropped = rendered.audio;
    const auto before = maxStepOf (dropped.left);
    const auto peak = peakOf (dropped.left);

    const auto loudest = std::size_t (std::distance (
        dropped.left.begin(),
        std::max_element (dropped.left.begin(), dropped.left.end(),
                          [] (const float a, const float b) { return std::abs (a) < std::abs (b); })));

    dropped.left[loudest] = 0.0f;
    const auto after = maxStepOf (dropped.left);

    std::fprintf (stderr, "\n[fidelity] dropout detector: %s — step %.5f clean -> %.5f after one sample zeroed, "
                          "x%.1f; peak %.5f -> %.5f; the render stays finite either way\n",
                  fixtures.front().slug.c_str(), before, after, before > 0.0 ? after / before : 0.0,
                  peak, peakOf (dropped.left));

    CHECK (before > 0.0);
    CHECK (after > before);

    // Derivable rather than invented: zeroing the sample at the signal's maximum leaves a step to a neighbour
    // that is at least half the peak for any signal that is not already a step function at its maximum.
    CHECK (after >= 0.5 * peak);

    CHECK (allFinite (dropped.left));
    CHECK (allFinite (rendered.audio.left));
    CHECK (allFinite (rendered.audio.right));
}

TEST_CASE ("the resampler that puts a swept render on the golden's grid does not limit the comparison",
           "[fidelity][fidelity-resampler]")
{
    const auto& c = corpus();

    if (! c.corpusAvailable())
        SKIP ("fidelity corpus not available: " + c.corpusReason);

    const auto fixtures = measurableFixtures (c);
    REQUIRE_FALSE (fixtures.empty());

    // The golden corpus is one 48 kHz capture, so a render taken at 44.1 or 96 kHz is moved onto the golden's
    // grid before the same three metrics can be computed on it. That makes the resampler part of the
    // instrument, so its own floor is measured here rather than trusted: a golden resampled away and back must
    // still null against itself, far below any synth's residual, or the cross-rate numbers would be reporting
    // the analysis tool instead of the engine.
    //
    // The requirement is the recorded measurement plus `kResamplerMarginDb`, the same rule the gate follows.
    for (const auto& floor : resamplerFloors())
    {
        const auto rate = floor.rate;
        double worstNull = -std::numeric_limits<double>::infinity();
        double worstSpectral = 0.0;

        for (const auto& fixture : fixtures)
        {
            Stereo golden;
            std::string error;

            if (! readWav (fixture.wavPath, golden, error))
            {
                FAIL (error);
                continue;
            }

            INFO (fixture.slug + " at " + std::to_string (rate) + " Hz");

            Stereo roundTripped;
            roundTripped.sampleRate = golden.sampleRate;
            roundTripped.left = resample (resample (golden.left, (int) golden.sampleRate, rate), rate, (int) golden.sampleRate);
            roundTripped.right = resample (resample (golden.right, (int) golden.sampleRate, rate), rate, (int) golden.sampleRate);

            // Integer rational resampling is exact in length here (96000 -> 88200 -> 96000), but a mismatch
            // would silently shorten the comparison rather than fail it, so it is made a length.
            roundTripped.left.resize (golden.size(), 0.0f);
            roundTripped.right.resize (golden.size(), 0.0f);

            const auto metrics = measure (golden, roundTripped, false);
            worstNull = std::max (worstNull, metrics.nullDb);
            worstSpectral = std::max (worstSpectral, metrics.spectralDb);

            CHECK (metrics.nullDb <= floor.nullDb + kResamplerMarginDb);
            CHECK (metrics.spectralDb <= floor.spectralDb + kResamplerMarginDb);
        }

        std::fprintf (stderr, "[fidelity] resampler floor 48k -> %d Hz -> 48k: worst null %.2f dB (recorded %.2f), "
                              "worst spectral %.3f dB (recorded %.3f) over %zu fixtures\n",
                      rate, worstNull, floor.nullDb, worstSpectral, floor.spectralDb, fixtures.size());
    }
}
