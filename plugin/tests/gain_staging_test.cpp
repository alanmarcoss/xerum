// Ticket 61 — is a full-scale voice faithful, or a bug?
//
// Ticket 58 measured xerum's own gain staging and refused to act on it: **a single voice renders 0.9999996** —
// full scale to the last bit — with two voices at 1.999630 and sixteen at 7.740814, so the limiter is not
// taming a hot mix, it is the only thing between polyphony and clipping. That left one question, and it is not
// answerable by reasoning: **does the reference do the same?**
//
//   * **Faithful** — the reference's voices are likewise at full scale, polyphony clips there too, and our
//     limiter is simply the divergence ticket 58 declared. Then the engine needs no change at all.
//   * **A bug** — our voices are hotter than the reference's, our fidelity is wrong at every dynamic level,
//     and the limiter has been masking it.
//
// This file is the measurement. It renders **the same preset on both sides**, under one stimulus, with the
// ceiling off on ours, and reports peak and RMS at one voice and at sixteen — the two measures ticket 54 used
// for the bank, so the vocabulary and the units do not change from ticket to ticket.
//
// # Both sides, one stimulus
//
// * **xerum** is driven in process, exactly as the product's other tests drive it: `loadPresetFile` reads the
//   reference's container into a document, `encodeStateBlock` writes xerum's own block, and
//   `setStateInformation` restores it — the same path ticket 46's fidelity layer uses, and the state is
//   verified landed rather than assumed.
// * **the reference** is driven by `xerum-host` through `tools/render-preset.sh`, which is ticket 20's route:
//   the preset is installed as `<content root>/Presets/User/Default.SerumPreset` (the path the plugin loads as
//   its init patch when it exists), the plugin is instantiated headlessly, and the script **restores the root
//   on every exit path**. `--verify` renders twice and requires the two files byte-identical, because an
//   unsettled render of a loaded preset is not reproducible at all (ticket 20) and an unreproducible number is
//   not evidence.
//
// The stimulus is ticket 58's own: `voices` notes struck together — `36 + 3 * i`, the chord its report used —
// at velocity 127, 48 kHz, block 512, the note struck 0.02 s in and **held** (the harness's note-off falls at
// 0.5 s, past the end of this render). The harness's pre-roll of 128 silent blocks is the reference's
// reproducibility protocol and is given to xerum as well, so the two sides are on the same clock.
//
// # Why this preset, and why it has no FX
//
// Ticket 60 is wiring the FX rack into xerum's processor, and a preset with effects would make this
// measurement depend on an agent's work in flight. So the comparison uses
// **`Bass/Retro Analog/BA - Analog Classics 2.SerumPreset`**, and the reason it has no FX is decoded from the
// preset's own container rather than assumed: all three of the reference's `FXRack` modules carry an **empty
// `FX` array** (`{'FXRack0': {'FX': []}, 'FXRack1': {'FX': []}, 'FXRack2': {'FX': []}}`). The factory bank was
// scanned for that property; **five of its 626 presets have it**, and of those five this is the only
// polyphonic one. The other four set `Global0/plainParams/kParamMonoToggle = 1.0`, and a mono preset renders
// one voice whatever the chord: measured, `BA - Analog Classics 1` peaks at 0.209333 with one note and
// 0.188192 with sixteen — a *lower* peak, because only the last note sounds. A mono preset cannot answer a
// polyphony question, so the scan had to check it.
//
// # What the numbers mean
//
// `peak` is the largest sample magnitude over both channels; `rms` is over both channels. The voice bus is
// stereo-linked, so a one-channel figure would not establish anything about the other.
//
// # The ceiling is off, and its latency is compensated
//
// Ticket 58's ceiling is the thing under discussion, not the measurement. It is switched off on xerum's side
// and its declared latency — which the delay line imposes whether the ceiling is enforced or not — is dropped
// from the front of xerum's render, so ours and the reference's start at the same sample.

#include "PresetState.h"
#include "XerumAudioProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
    #include <sys/wait.h>
#endif

using namespace xerum::plugin;

namespace fs = std::filesystem;

namespace
{

// ---------------------------------------------------------------------------------------------------
// The protocol (ticket 58's stimulus, at the lengths this comparison needs)
// ---------------------------------------------------------------------------------------------------

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 512;
constexpr double kSeconds = 0.256;      // 12 288 samples: 24 blocks, ticket 58's render length
constexpr int kPreRoll = 128;           // the harness's settling pre-roll (ticket 20)
constexpr int kSettleMs = 1500;
constexpr int kVelocity = 127;          // ticket 58 measured at full velocity
constexpr int kNoteOnMs = 20;           // the harness's note-on, 0.02 s in
constexpr int kHarnessNoteOffMs = 500;  // past the end of this render: the chord is held throughout

/// `36 + 3 * i` — ticket 58's chord, so its published numbers are continued rather than restated in a second
/// protocol.
std::vector<int> chordNotes (const int voices)
{
    std::vector<int> notes;

    for (int i = 0; i < voices; ++i)
        notes.push_back (36 + 3 * i);

    return notes;
}

/// The reference preset this comparison uses. See the header: FX-free and polyphonic, both measured.
constexpr const char* kFxFreePreset = "Bass/Retro Analog/BA - Analog Classics 2.SerumPreset";

constexpr const char* kDefaultContentRoot = "/Library/Audio/Presets/Xfer Records/Serum 2 Presets";

std::string envOr (const char* name, const std::string& fallback)
{
    if (const char* value = std::getenv (name); value != nullptr && *value != '\0')
        return value;

    return fallback;
}

std::string quoted (const std::string& text)
{
    std::string out = "'";

    for (const auto character : text)
        out += (character == '\'') ? std::string ("'\\''") : std::string (1, character);

    return out + "'";
}

fs::path repoRoot() { return fs::path (XERUM_REPO_ROOT); }
fs::path thisBuild() { return fs::path (XERUM_BUILD_DIR); }

fs::path artefactDir()
{
    const auto dir = fs::path (envOr ("HOME", ".")) / ".xerum-fixtures/fidelity/gain-staging";
    std::error_code ec;
    fs::create_directories (dir, ec);
    return dir;
}

/// This build's harness first, then an explicit override, then any build tree in the repository — ticket 32's
/// preference order, so a scan cannot pick a peer's stale binary ahead of the one next to this test.
fs::path findHost()
{
    std::vector<fs::path> candidates;
    candidates.push_back (thisBuild() / "host/xerum-host_artefacts/Release/xerum-host");

    if (const auto fromEnv = envOr ("XERUM_HOST_BIN", ""); ! fromEnv.empty())
        candidates.emplace_back (fromEnv);

    std::error_code ec;

    if (fs::is_directory (repoRoot(), ec))
        for (const auto& entry : fs::directory_iterator (repoRoot(), ec))
        {
            const auto name = entry.path().filename().string();

            if (name == "build" || name.rfind ("build-", 0) == 0)
                candidates.push_back (entry.path() / "host/xerum-host_artefacts/Release/xerum-host");
        }

    for (const auto& candidate : candidates)
        if (fs::exists (candidate, ec))
            return candidate;

    return {};
}

/// The reference plugin, read from the golden manifest's `capture.plugin` rather than written here twice, with
/// an override for a machine that has it elsewhere.
std::string referencePlugin()
{
    if (const auto fromEnv = envOr ("XERUM_REFERENCE_PLUGIN", ""); ! fromEnv.empty())
        return fromEnv;

    const auto manifest = repoRoot() / "tests/golden/manifest.json";
    std::error_code ec;

    if (fs::exists (manifest, ec))
    {
        const auto parsed = juce::JSON::parse (juce::File (juce::String (manifest.string())));

        if (const auto* capture = parsed["capture"].getDynamicObject())
        {
            const auto plugin = capture->getProperty ("plugin").toString();

            if (plugin.isNotEmpty())
                return plugin.toStdString();
        }
    }

    return "/Library/Audio/Plug-Ins/VST3/Serum2.vst3";
}

// ---------------------------------------------------------------------------------------------------
// Audio, and the two measures ticket 54 agreed on
// ---------------------------------------------------------------------------------------------------

struct Stereo
{
    std::vector<float> left, right;
    double sampleRate { 0.0 };

    std::size_t size() const { return left.size(); }
};

double peakOf (const Stereo& audio)
{
    double peak = 0.0;

    for (const auto sample : audio.left)
        peak = std::max (peak, (double) std::abs (sample));

    for (const auto sample : audio.right)
        peak = std::max (peak, (double) std::abs (sample));

    return peak;
}

/// RMS over both channels, the unit ticket 54's bank figures are in.
double rmsOf (const Stereo& audio)
{
    double sum = 0.0;

    for (const auto sample : audio.left)
        sum += (double) sample * (double) sample;

    for (const auto sample : audio.right)
        sum += (double) sample * (double) sample;

    const auto count = 2 * (double) audio.size();
    return count > 0.0 ? std::sqrt (sum / count) : 0.0;
}

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
    writer.reset();
    return ok;
}

// ---------------------------------------------------------------------------------------------------
// The xerum side: in process, through the product's own load path
// ---------------------------------------------------------------------------------------------------

/// The harness's render loop (`host/main.cpp`) with a **chord** instead of one note. The pre-roll and the event
/// schedule are the harness's; ticket 46's `[fidelity-stimulus]` is what proves that reproduction is the
/// harness's loop and not a second protocol.
Stereo renderChord (XerumAudioProcessor& processor, const std::vector<int>& notes, const double seconds)
{
    const auto totalSamples = (juce::int64) (seconds * kSampleRate);
    const auto noteOnSamples = (juce::int64) (kSampleRate * kNoteOnMs / 1000);
    const auto noteOffSamples = (juce::int64) (kSampleRate * kHarnessNoteOffMs / 1000);

    juce::AudioBuffer<float> buffer (2, kBlockSize);
    juce::MidiBuffer midi;

    for (int block = 0; block < kPreRoll; ++block)
    {
        buffer.clear();
        midi.clear();
        processor.processBlock (buffer, midi);
    }

    Stereo out;
    out.sampleRate = (double) kSampleRate;
    out.left.reserve ((std::size_t) totalSamples);
    out.right.reserve ((std::size_t) totalSamples);

    for (juce::int64 written = 0; written < totalSamples;)
    {
        const auto thisBlock = (int) juce::jmin ((juce::int64) kBlockSize, totalSamples - written);

        buffer.clear();
        midi.clear();

        if (written <= noteOnSamples && noteOnSamples < written + thisBlock)
            for (const auto note : notes)
                midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) kVelocity),
                               (int) (noteOnSamples - written));

        if (written <= noteOffSamples && noteOffSamples < written + thisBlock)
            for (const auto note : notes)
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

void dropLeading (Stereo& audio, const int samples)
{
    if (samples <= 0 || audio.left.size() <= (std::size_t) samples)
        return;

    audio.left.erase (audio.left.begin(), audio.left.begin() + samples);
    audio.right.erase (audio.right.begin(), audio.right.begin() + samples);
}

struct XerumRender
{
    bool ok { false };
    std::string error;
    Stereo audio;
    int latency { 0 };
    std::string documentName;
    std::size_t parameters { 0 };
    std::size_t fxRackPaths { 0 };
    std::size_t fxSlots { 0 };
    std::string fxPathSample;
    int reportEntries { 0 };
    std::string firstReport;
    std::string oscLevel;
    std::string masterLevel;
};

/// Fixed-point text for the report: the numbers in the table are meant to be compared to the digits ticket 58
/// published, so they are printed at six places rather than at whatever `%g` felt like.
std::string fixed (const double value, const int places)
{
    char buffer[64];
    std::snprintf (buffer, sizeof buffer, "%.*f", places, value);
    return buffer;
}

/// One leaf of the document the plugin actually holds, as text — the level leaves are what this comparison
/// turns on, and reading them out of the engine's own document is how the two sides' *authored* levels can be
/// stated rather than inferred from the audio.
std::string leaf (const xerum::model::PresetDocument& document, const std::string& path)
{
    const auto found = document.parameters.find (path);

    if (found == document.parameters.end())
        return "absent";

    double number = 0.0;

    if (found->second.numberAsDouble (number))
        return fixed (number, 6);

    return found->second.isText() ? found->second.text : "(non-numeric)";
}

/// One xerum render: a fresh processor, the reference preset applied through the product's own path (or none
/// at all, for the init-patch row), the ceiling switched off **after** the document is applied, and the
/// declared latency compensated.
XerumRender renderXerum (const std::string& presetPath, const std::vector<int>& notes)
{
    XerumRender result;

    XerumAudioProcessor processor;
    processor.enableAllBuses();
    processor.prepareToPlay (kSampleRate, kBlockSize);

    if (! presetPath.empty())
    {
        const auto loaded = loadPresetFile (juce::File (juce::String (presetPath)));

        if (! loaded.ok)
        {
            result.error = "xerum could not read the preset: " + loaded.error;
            return result;
        }

        const auto block = encodeStateBlock (loaded.document, loaded.report, loaded.reportEntriesFromModel);
        processor.setStateInformation (block.data(), (int) block.size());

        if (! processor.lastError().isEmpty())
        {
            result.error = "setStateInformation reported: " + processor.lastError().toStdString();
            return result;
        }

        result.documentName = loaded.document.name;
        result.parameters = loaded.document.parameters.size();
        result.reportEntries = (int) loaded.report.entries.size();

        if (! loaded.report.entries.empty())
            result.firstReport = loaded.report.entries.front().modulePath + " "
                               + loaded.report.entries.front().reason;

        // **The FX-free claim, checked in the engine's own document rather than taken on trust.** An
        // `FXRack{N}/FX[]/<Class>` path is what an occupied effect slot decodes to; the rack *module* itself
        // is present in every preset, empty or not, which is why the count is of slots and not of racks. Zero
        // slots is what keeps ticket 60's work in flight out of this measurement.
        for (const auto& entry : loaded.document.parameters)
        {
            if (entry.first.find ("FXRack") == std::string::npos)
                continue;

            ++result.fxRackPaths;

            if (entry.first.find ("/FX[]/") != std::string::npos)
            {
                ++result.fxSlots;

                if (result.fxPathSample.size() < 200)
                    result.fxPathSample += (result.fxPathSample.empty() ? "" : ", ") + entry.first;
            }
        }

        if (result.parameters == 0 || processor.document().parameters.size() != result.parameters)
        {
            result.error = "the state was not applied (the plugin holds "
                         + std::to_string (processor.document().parameters.size())
                         + " parameters, expected " + std::to_string (result.parameters) + ")";
            return result;
        }
    }

    // Ticket 58: off, and set *after* the document, because `applyDocument` writes every parameter and a
    // reference preset carries no `Master0` leaf — which resolves the ceiling back to its default, which is on.
    processor.setParameterValue ("Master0/plainParams/kParamLimitOn", 0.0);
    result.latency = processor.getLatencySamples();

    // The two leaves that decide a voice's level, read out of the document the engine is actually holding —
    // the init patch when nothing was applied, and the imported preset when one was.
    result.oscLevel = leaf (processor.document(), "Oscillator0/plainParams/kParamVolume");
    result.masterLevel = leaf (processor.document(), "Global0/plainParams/kParamMasterVolume");

    // Rendered `latency` samples long and the front dropped, so the two sides start at the same sample
    // (ticket 46's compensation, for the same reason).
    const auto extended = kSeconds + (double) (result.latency + kBlockSize) / (double) kSampleRate;
    result.audio = renderChord (processor, notes, extended);
    dropLeading (result.audio, result.latency);

    const auto wanted = (std::size_t) (kSeconds * kSampleRate);
    result.audio.left.resize (wanted);
    result.audio.right.resize (wanted);

    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------------------------------
// The reference side: the harness, driving the real plugin
// ---------------------------------------------------------------------------------------------------

std::string runCommand (const std::string& command, int& exitStatus)
{
    exitStatus = -1;
    std::FILE* pipe = ::popen (command.c_str(), "r");

    if (pipe == nullptr)
        return {};

    std::string output;
    char buffer[4096];

    while (std::fgets (buffer, sizeof buffer, pipe) != nullptr)
        output += buffer;

    const int status = ::pclose (pipe);

    if (status != -1)
    {
#if defined(__APPLE__) || defined(__linux__)
        if (WIFEXITED (status))
            exitStatus = WEXITSTATUS (status);
#else
        exitStatus = status;
#endif
    }

    return output;
}

struct ReferenceRender
{
    bool ok { false };
    std::string error;
    Stereo audio;
    std::string output;
    /// Two renders were byte-identical. An unreproducible reference render is not evidence (ticket 20), and
    /// the init route cannot borrow `render-preset.sh --verify` to establish it.
    bool verified { false };
};

/// Byte-for-byte identity of two files, which is what the reference's own settling protocol is checked with
/// (`render-preset.sh --verify`).
bool identicalFiles (const fs::path& a, const fs::path& b)
{
    std::ifstream left (a, std::ios::binary);
    std::ifstream right (b, std::ios::binary);

    if (! left || ! right)
        return false;

    char l[4096] = {};
    char r[4096] = {};

    while (left && right)
    {
        left.read (l, sizeof l);
        right.read (r, sizeof r);

        if (left.gcount() != right.gcount()
            || std::memcmp (l, r, (std::size_t) left.gcount()) != 0)
            return false;
    }

    return left.eof() && right.eof();
}

/// The note list as the harness takes it: `--notes 36,39,...`.
std::string noteList (const std::vector<int>& notes)
{
    std::string list;

    for (const auto note : notes)
        list += (list.empty() ? "" : ",") + std::to_string (note);

    return list;
}

/// Everything the harness needs to render this stimulus, except the plugin and the output path, which the two
/// routes below supply differently (the script owns `--plugin`, ticket 20's route; the init route does not).
std::string stimulusArguments (const std::vector<int>& notes)
{
    return " --seconds " + std::to_string (kSeconds)
         + " --sr " + std::to_string (kSampleRate)
         + " --block " + std::to_string (kBlockSize)
         + " --notes " + quoted (noteList (notes))
         + " --velocity " + std::to_string (kVelocity)
         + " --pre-roll " + std::to_string (kPreRoll)
         + " --settle-ms " + std::to_string (kSettleMs);
}

/// The reference with nothing installed: its own init patch, which is what a virgin instance plays and the
/// exact counterpart of xerum's init patch — the patch ticket 58's 0.9999996 was measured on.
///
/// `sets` are the harness's own `--set` flags, appended verbatim: the control experiment below raises the
/// reference's two level knobs with them, which is the only way to ask what its voice does when its own
/// default attenuation is removed.
ReferenceRender renderReferenceInit (const fs::path& host, const fs::path& plugin,
                                     const fs::path& out, const std::vector<int>& notes,
                                     const std::string& sets = {})
{
    ReferenceRender result;
    const auto verify = fs::path (out.string() + ".verify");

    // Twice, and byte-identical, for the same reason `render-preset.sh --verify` renders twice: a reference
    // render that is not reproducible is not evidence of anything (ticket 20).
    for (const auto& target : { out, verify })
    {
        int status = -1;
        result.output += runCommand (quoted (host.string())
                                     + " --plugin " + quoted (plugin.string())
                                     + " --out " + quoted (target.string())
                                     + sets
                                     + stimulusArguments (notes), status);

        if (status != 0)
        {
            result.error = "the harness exited " + std::to_string (status);
            return result;
        }
    }

    if (identicalFiles (out, verify))
    {
        std::error_code ec;
        fs::remove (verify, ec);
        result.verified = true;
    }
    else
    {
        result.error = "two renders of the reference's init patch differ — raise the settle time or the "
                       "pre-roll (ticket 20)";
        return result;
    }

    std::string readError;

    if (! readWav (out, result.audio, readError))
    {
        result.error = readError;
        return result;
    }

    result.ok = true;
    return result;
}

/// The reference with the preset installed the way ticket 20 established: `<content root>/Presets/User/
/// Default.SerumPreset`, through `tools/render-preset.sh`, which restores the root on every exit path and
/// `--verify`s the render byte-identical before returning it.
ReferenceRender renderReferencePreset (const fs::path& host, const fs::path& contentRoot,
                                       const fs::path& preset, const fs::path& out,
                                       const std::vector<int>& notes)
{
    ReferenceRender result;

    const auto script = repoRoot() / "tools/render-preset.sh";
    std::error_code ec;

    if (! fs::exists (script, ec))
    {
        result.error = "tools/render-preset.sh is not in this checkout";
        return result;
    }

    int status = -1;
    result.output = runCommand (quoted (script.string())
                                + " --preset " + quoted (preset.string())
                                + " --out " + quoted (out.string())
                                + " --bin " + quoted (host.string())
                                + " --root " + quoted (contentRoot.string())
                                + " --verify --"
                                + stimulusArguments (notes), status);

    if (status != 0)
    {
        result.error = "render-preset.sh exited " + std::to_string (status) + ": " + result.output;
        return result;
    }

    if (result.output.find ("byte-identical") == std::string::npos)
    {
        result.error = "--verify did not report two byte-identical renders: " + result.output;
        return result;
    }

    result.verified = true;

    std::string readError;

    if (! readWav (out, result.audio, readError))
    {
        result.error = readError;
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// The comparison
// ---------------------------------------------------------------------------------------------------

struct Row
{
    std::string source;
    std::string patch;
    int voices { 0 };
    bool ok { false };
    std::string detail;
    double peak { 0.0 };
    double rms { 0.0 };
    bool verified { false };
};

double toDb (const double linear)
{
    return linear > 0.0 ? 20.0 * std::log10 (linear) : -1000.0;
}

TEST_CASE ("the reference's voice and ours, peak and RMS at one voice and at sixteen, on an FX-free preset",
           "[plugin][gain-staging]")
{
    const auto host = findHost();
    const auto plugin = fs::path (referencePlugin());
    const auto root = fs::path (envOr ("XERUM_SERUM_CONTENT_ROOT", kDefaultContentRoot));
    const auto preset = root / "Presets/Factory" / kFxFreePreset;
    const auto outDir = artefactDir();

    std::error_code ec;

    if (! fs::exists (host, ec))
        SKIP ("the headless harness has not been built (see host/README.md); set XERUM_HOST_BIN");

    if (! fs::exists (plugin, ec))
        SKIP ("the reference plugin is not installed at " + plugin.string()
              + "; this comparison has no reference side without it");

    if (! fs::exists (preset, ec))
        SKIP ("the reference's FX-free preset is not installed at " + preset.string()
              + " (the factory bank lives under the content root; ticket 10)");

    std::vector<Row> rows;

    // --- xerum's init patch: ticket 58's own measurement, re-taken under this comparison's stimulus -----
    for (const auto voices : { 1, 16 })
    {
        const auto render = renderXerum ({}, chordNotes (voices));

        // Twice, and required sample-identical: a render that is not reproducible is not evidence (ticket 20),
        // and the reference's rows are held to the same standard.
        const auto repeat = renderXerum ({}, chordNotes (voices));
        const auto same = render.ok && repeat.ok
                       && render.audio.left == repeat.audio.left
                       && render.audio.right == repeat.audio.right;

        Row row { "xerum", "init", voices, render.ok, render.error, 0.0, 0.0, same };

        if (render.ok)
        {
            row.peak = peakOf (render.audio);
            row.rms = rmsOf (render.audio);
            writeWav (outDir / ("xerum-init-" + std::to_string (voices) + ".wav"), render.audio);

            if (voices == 1)
                std::fprintf (stderr, "[gain-staging] xerum init: limiter latency %d samples, "
                                      "Oscillator0 kParamVolume %s, Global0 kParamMasterVolume %s\n",
                              render.latency, render.oscLevel.c_str(), render.masterLevel.c_str());
        }

        rows.push_back (row);
    }

    // --- xerum, rendering the reference's own FX-free preset through the product's loader ---------------
    std::size_t presetFxRackPaths = 0;
    std::size_t presetFxSlots = 0;
    std::string presetFxSample;
    std::size_t presetParameters = 0;
    int presetReportEntries = 0;
    std::string presetName;
    std::string presetReport;
    std::string presetOscLevel;
    std::string presetMasterLevel;

    for (const auto voices : { 1, 16 })
    {
        const auto render = renderXerum (preset.string(), chordNotes (voices));
        const auto repeat = renderXerum (preset.string(), chordNotes (voices));
        const auto same = render.ok && repeat.ok
                       && render.audio.left == repeat.audio.left
                       && render.audio.right == repeat.audio.right;

        Row row { "xerum", "preset", voices, render.ok, render.error, 0.0, 0.0, same };

        if (render.ok)
        {
            row.peak = peakOf (render.audio);
            row.rms = rmsOf (render.audio);
            writeWav (outDir / ("xerum-preset-" + std::to_string (voices) + ".wav"), render.audio);
            presetFxRackPaths = render.fxRackPaths;
            presetFxSlots = render.fxSlots;
            presetFxSample = render.fxPathSample;
            presetParameters = render.parameters;
            presetReportEntries = render.reportEntries;
            presetName = render.documentName;
            presetReport = render.firstReport;
            presetOscLevel = render.oscLevel;
            presetMasterLevel = render.masterLevel;
        }

        rows.push_back (row);
    }

    std::fprintf (stderr, "[gain-staging] xerum loaded \"%s\": %zu parameters, %zu FXRack paths of which "
                          "%zu effect slots%s, %d import-report entries%s\n",
                  presetName.c_str(), presetParameters, presetFxRackPaths, presetFxSlots,
                  presetFxSample.empty() ? "" : (" (" + presetFxSample + ")").c_str(), presetReportEntries,
                  presetReport.empty() ? "" : (" — first: " + presetReport).c_str());
    std::fprintf (stderr, "[gain-staging] xerum rendered: Oscillator0 kParamVolume %s, "
                          "Global0 kParamMasterVolume %s\n",
                  presetOscLevel.c_str(), presetMasterLevel.c_str());

    // --- the reference's init patch, where nothing is installed at that path ----------------------------
    //
    // Skipped rather than forced: this path *is* the preset slot, so a file sitting in it would make the row
    // measure that file and call it the init patch — which is exactly the failure ticket 20 recorded.
    const auto installedSlot = root / "Presets/User/Default.SerumPreset";

    if (fs::exists (installedSlot, ec))
        std::fprintf (stderr, "[gain-staging] reference init patch NOT measured: %s exists, so an "
                              "un-presetted render would be a render of that file instead\n",
                      installedSlot.string().c_str());
    else
        for (const auto voices : { 1, 16 })
        {
            const auto out = outDir / ("reference-init-" + std::to_string (voices) + ".wav");
            const auto render = renderReferenceInit (host, plugin, out, chordNotes (voices));
            Row row { "reference", "init", voices, render.ok, render.error, 0.0, 0.0, false };

            if (render.ok)
            {
                row.peak = peakOf (render.audio);
                row.rms = rmsOf (render.audio);
                row.verified = render.verified;
            }

            rows.push_back (row);
        }

    // --- the control: the reference's init patch with its own two level knobs raised ---------------------
    //
    // **This is the row that decides whether "a voice at full scale" is a defect at all.** If the reference's
    // single voice were only ever ~-11 dBFS, one could argue that a voice at 0 dBFS is simply a gain-staging
    // mistake. It is not what the reference's voice *is*: at the top of its own level controls the reference's
    // single voice is **above** full scale, and the amount it rises by is exactly the amount its own parameter
    // text says those two knobs had attenuated it — `Main Vol` "50% [-9.0 dB]" and `OSC A / A Level`
    // "75% [-5.0 dB]" at the defaults, "+3.0 dB" and "0.0 dB" at the top (read back with
    // `xerum-host --plugin /Library/Audio/Plug-Ins/VST3/Serum2.vst3 --set "Main Vol=1.0" --set "A Level=1.0"
    // --dump-params /tmp/p.tsv`, ticket 38's dump). Main Vol rises 12 dB and A Level 5 dB, so the text claims
    // +17.0 dB; the audio moves by 7.111x, which is +17.04 dB, and the two agree to 0.04 dB — which is also
    // the cross-check that this file's whole measurement chain is sound.
    if (! fs::exists (installedSlot, ec))
    {
        const auto sets = std::string (" --set \"Main Vol=1.0\" --set \"A Level=1.0\"");
        const auto out = outDir / "reference-init-max-1.wav";
        const auto render = renderReferenceInit (host, plugin, out, chordNotes (1), sets);
        Row row { "reference", "init-max", 1, render.ok, render.error, 0.0, 0.0, false };

        if (render.ok)
        {
            row.peak = peakOf (render.audio);
            row.rms = rmsOf (render.audio);
            row.verified = render.verified;
        }

        rows.push_back (row);
    }

    // --- the reference, rendering the same preset through the same host ---------------------------------
    for (const auto voices : { 1, 16 })
    {
        const auto out = outDir / ("reference-preset-" + std::to_string (voices) + ".wav");
        const auto render = renderReferencePreset (host, root, preset, out, chordNotes (voices));
        Row row { "reference", "preset", voices, render.ok, render.error, 0.0, 0.0, false };

        if (render.ok)
        {
            row.peak = peakOf (render.audio);
            row.rms = rmsOf (render.audio);
            row.verified = render.verified;
        }

        rows.push_back (row);
    }

    // --- the evidence ----------------------------------------------------------------------------------
    const auto report = outDir / "gain-staging-report.tsv";
    std::ofstream tsv (report);
    tsv << "# ticket 61: peak and RMS, one voice and sixteen, same stimulus, xerum's ceiling off\n"
        << "# scheme\tpatch\tvoices\tpeak\trms\tpeakDb\trmsDb\treproducible\n";

    std::fprintf (stderr, "\n[gain-staging] %-10s %-8s %6s %12s %12s %10s %10s  %s\n",
                  "source", "patch", "voices", "peak", "rms", "peak dB", "rms dB", "verified");

    for (const auto& row : rows)
    {
        if (! row.ok)
        {
            std::fprintf (stderr, "[gain-staging] %-10s %-8s %6d   NOT MEASURED: %s\n",
                          row.source.c_str(), row.patch.c_str(), row.voices, row.detail.c_str());
            continue;
        }

        std::fprintf (stderr, "[gain-staging] %-10s %-8s %6d %12s %12s %10s %10s  %s\n",
                      row.source.c_str(), row.patch.c_str(), row.voices,
                      fixed (row.peak, 6).c_str(), fixed (row.rms, 6).c_str(),
                      fixed (toDb (row.peak), 2).c_str(), fixed (toDb (row.rms), 2).c_str(),
                      row.verified ? "two renders byte-identical" : "");

        tsv << row.source << '\t' << row.patch << '\t' << row.voices << '\t'
            << fixed (row.peak, 6) << '\t' << fixed (row.rms, 6) << '\t'
            << fixed (toDb (row.peak), 2) << '\t' << fixed (toDb (row.rms), 2) << '\t'
            << (row.verified ? "yes" : "no") << '\n';
    }

    tsv.close();
    std::fprintf (stderr, "[gain-staging] report: %s (the renders beside it are the same numbers)\n\n",
                  report.string().c_str());

    const auto find = [&rows] (const char* source, const char* patch, const int voices) -> const Row&
    {
        for (const auto& row : rows)
            if (row.source == source && row.patch == patch && row.voices == voices)
                return row;

        FAIL ("no row for " << source << "/" << patch << "/" << voices);
        static const Row empty;
        return empty;
    };

    const auto has = [&rows] (const char* source, const char* patch)
    {
        for (const auto& row : rows)
            if (row.source == source && row.patch == patch)
                return true;

        return false;
    };

    /// A row's peak, or `fallback` when that row was not measured — so a row without a sixteen-voice
    /// counterpart (the control below) is skipped rather than failing a lookup.
    const auto peakFor = [&rows] (const char* source, const char* patch, const int voices, const double fallback)
    {
        for (const auto& row : rows)
            if (row.ok && row.source == source && row.patch == patch && row.voices == voices)
                return row.peak;

        return fallback;
    };

    REQUIRE (has ("xerum", "init"));
    REQUIRE (has ("xerum", "preset"));
    REQUIRE (has ("reference", "preset"));

    // **The claim this comparison rests on, checked rather than trusted**: the preset xerum renders has no
    // effect in its racks, so ticket 60's work in flight cannot reach any number here. A `FXRack{N}/FX[]/...`
    // path is what an occupied slot decodes to; zero of them is the property that matters.
    REQUIRE (presetFxSlots == 0);
    REQUIRE (presetParameters > 0);

    // Every row that ran really rendered. A silent render would make every level comparison below vacuous,
    // and silence is the failure that looks most like a measurement.
    for (const auto& row : rows)
        if (row.ok)
            CHECK (row.peak > 0.01);

    // Every side is reproducible — the reference's own rows through the harness's two-render protocol, ours
    // by rendering twice in process and requiring the two signals equal. An unreproducible number is not
    // evidence (ticket 20), whichever side produced it.
    for (const auto& row : rows)
        if (row.ok)
            CHECK (row.verified);

    // The chord is polyphonic on both sides — a preset that ignored the extra keys would answer the wrong
    // question, which is the trap `BA - Analog Classics 1` fell into (the header records it).
    for (const auto& row : rows)
        if (row.ok && row.voices == 1)
        {
            const auto sixteen = peakFor (row.source.c_str(), row.patch.c_str(), 16, 0.0);

            if (sixteen > 0.0)
                CHECK (sixteen > row.peak);
        }

    // ---------------------------------------------------------------------------------------------
    // The finding, as assertions — each one a number this file measured rather than a reading of it
    // ---------------------------------------------------------------------------------------------

    const auto xerumInit = find ("xerum", "init", 1);
    const auto xerumPreset = find ("xerum", "preset", 1);
    const auto referencePreset = find ("reference", "preset", 1);
    const auto haveReferenceInit = has ("reference", "init");

    // **Ticket 58's measurement, re-taken under this stimulus.** If this does not hold, the xerum side is not
    // the patch that report describes and the two sides are not being compared at all. (It also reproduces
    // the sixteen-voice figure exactly: 7.740814, the number in ticket 58's table.)
    CHECK (xerumInit.peak > 0.95);
    CHECK (find ("xerum", "init", 16).peak == Catch::Approx (7.740814).epsilon (0.01));

    // **The engine is not hotter than the reference's voice path.** On the same preset, under the same
    // stimulus, ours is *quieter* — so the "our voices are hotter than the reference's" reading does not
    // survive the measurement, and no extra gain sits in the voice bus.
    CHECK (toDb (xerumPreset.peak) < toDb (referencePreset.peak) - 5.0);

    // **Polyphony overloads the reference too.** Sixteen keys held at velocity 127 take the FX-free preset over
    // full scale (1.352828). The limiter's premise — that a chord, not a voice, is what clips — is the
    // reference's behaviour as well; ours starts higher, which is why it reaches further.
    CHECK (find ("reference", "preset", 16).peak > 1.0);

    if (haveReferenceInit)
    {
        const auto referenceInit = find ("reference", "init", 1);

        // **A single voice at full scale is not what the reference does.** Its own init patch — the counterpart
        // of ours, and the patch a virgin instance plays — renders ~10.8 dB below full scale under the same
        // stimulus, and the FX-free factory preset ~10.5 dB below. Both are far from the ceiling, let alone
        // over it.
        CHECK (referenceInit.peak < 0.5);
        CHECK (referencePreset.peak < 0.5);

        // And the gap between the two *defaults* is a level, not a rounding: ours is authored at unity.
        CHECK (toDb (referenceInit.peak) < toDb (xerumInit.peak) - 8.0);

        CHECK (find ("reference", "init", 16).peak > 1.0);
        CHECK (find ("xerum", "init", 16).peak > 4.0 * referenceInit.peak);

        // **The control.** Raising the reference's own level knobs puts its single voice *above* full scale
        // (measured 2.049533, +6.23 dBFS), so a voice exceeding full scale is not a gain-staging defect in
        // itself — it is what this instrument's voice does when its own level controls are up. And the rise is
        // the one its own parameter text predicts: +17.0 dB from the knobs, +17.04 dB in the audio.
        if (has ("reference", "init-max"))
        {
            const auto boosted = find ("reference", "init-max", 1);
            CHECK (boosted.peak > 1.0);
            CHECK (toDb (boosted.peak / referenceInit.peak) == Catch::Approx (17.0).margin (0.5));
        }
    }
    else
    {
        WARN ("the reference's init patch was not measured: a file sits at "
              "<content root>/Presets/User/Default.SerumPreset, so an un-presetted render would have been a "
              "render of that file (ticket 20's failure mode). The same-preset rows above still decide the "
              "question; only the init-vs-init row is missing.");
    }
}


