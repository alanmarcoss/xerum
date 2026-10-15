// xerum::presets — the factory bank, as data. See factory_bank.h for where it lives and why.
//
// ## How a preset is written here
//
// A preset is a **recipe**: a name, a category, a sentence, and the handful of choices a synth patch is
// made of — for each oscillator slot a table and a frame position, the warp, the unison spread; whether
// NOISE or SUB is in; the one or two filters; the amplitude envelope; and the modulation rows. The
// builder below turns a recipe into the `PresetDocument` the product speaks, so the *paths* the product
// is keyed by exist in exactly one place in this file and cannot be mistyped per preset.
//
// Every token a recipe carries — a filter type, a warp mode name, a noise colour, a matrix source — is
// resolved through the engine's own vocabulary (`dsp::filterSpecFromName`, `dsp::warpModeFromName`, …)
// while the bank is built, and anything that does not resolve is recorded in `bankProblems()`. The
// failure that would otherwise be invisible is exactly this one: a parameter whose token is outside
// xerum's set resolves to the "not in xerum's set" sentinel and its module quietly does nothing. So the
// bank checks itself, the authoring tool refuses to write a bank that has problems, and both test suites
// fail on a non-empty list.
//
// ## What is deliberately *not* here
//
// No preset references the reference instrument's content: every table is `xerum/<Name>.wav`, generated
// from a formula in `dsp::starterLibrary()` (docs/content-policy.md). No preset uses a parameter outside
// ticket 12's engine cut — the suite asserts that by checking every path against the bridge's own
// parameter table.

#include "factory_bank.h"

#include "filter.h"
#include "json.h"
#include "modulation.h"
#include "noise_osc.h"
#include "preset_block.h"
#include "schema.h"
#include "sub_osc.h"
#include "voice.h"
#include "warp.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <utility>

namespace xerum::presets
{

namespace
{

// ------------------------------------------------------------------------------------ the vocabulary

// The categories, spelled once.
constexpr std::string_view kBass = "Bass";
constexpr std::string_view kLead = "Lead";
constexpr std::string_view kPad = "Pad";
constexpr std::string_view kKeys = "Keys";
constexpr std::string_view kPluck = "Pluck";
constexpr std::string_view kFx = "FX";
constexpr std::string_view kTexture = "Texture";
constexpr std::string_view kPercussion = "Percussion";

// The tables, by the name the manifest and the tool use. The frame count is in the comment because it is
// the range a `position` may take: `Wavetable::interpolated` clamps at `frameCount - 1`, so a valid
// position runs 1..frames-1 (and any position at all for a single-frame table).
constexpr auto kSine = "xerum/Sine.wav";                     //  1
constexpr auto kSaw = "xerum/Saw.wav";                       //  1
constexpr auto kSquare = "xerum/Square.wav";                 //  1
constexpr auto kTriangle = "xerum/Triangle.wav";             //  1
constexpr auto kRectified = "xerum/Rectified.wav";           //  1
constexpr auto kFmMorph = "xerum/FM Morph.wav";               // 32
constexpr auto kSyncSweep = "xerum/Sync Sweep.wav";           // 32
constexpr auto kPwmMorph = "xerum/PWM Morph.wav";             // 32
constexpr auto kFoldDrive = "xerum/Fold Drive.wav";           // 32
constexpr auto kHarmonicSweep = "xerum/Harmonic Sweep.wav";   // 24
constexpr auto kFormantSweep = "xerum/Formant Sweep.wav";     // 24
constexpr auto kFormantHigh = "xerum/Formant High.wav";       // 12
constexpr auto kVocalStack = "xerum/Vocal Stack.wav";         // 12
constexpr auto kOddStack = "xerum/Odd Stack.wav";             // 12
constexpr auto kSawFade = "xerum/Saw Fade.wav";               // 12
constexpr auto kSquareFade = "xerum/Square Fade.wav";         // 12
constexpr auto kBitCrush = "xerum/Bit Crush.wav";             // 12
constexpr auto kBendSweep = "xerum/Bend Sweep.wav";           // 12
constexpr auto kRingSwell = "xerum/Ring Swell.wav";           // 12
constexpr auto kGlassStack = "xerum/Glass Stack.wav";         // 12
constexpr auto kNoiseBlend = "xerum/Noise Blend.wav";         // 12
constexpr auto kDust = "xerum/Dust.wav";                      //  8
constexpr auto kGrowlFold = "xerum/Growl Fold.wav";           // 12
constexpr auto kPulseGrit = "xerum/Pulse Grit.wav";           // 12
constexpr auto kSyncSaw = "xerum/Sync Saw.wav";               // 12
constexpr auto kMetalSync = "xerum/Metal Sync.wav";           // 12
constexpr auto kSparkle = "xerum/Sparkle.wav";                // 12

// The matrix's destination vocabulary, straight from the engine (`voice.h`), so a preset cannot address
// a class, an instance or a parameter the voice does not read.
constexpr auto kOscillatorClass = dsp::voiceDestination::kOscillatorClass;
constexpr auto kVoiceFilterClass = dsp::voiceDestination::kVoiceFilterClass;

// ------------------------------------------------------------------------------------ the recipe

struct OscRecipe
{
    const char* table { nullptr };      ///< nullptr: the slot is off
    double position { 1.0 };            ///< `kParamTablePos`, in frames
    double octave { 0.0 };
    double pitch { 0.0 };
    double fine { 0.0 };
    double level { 1.0 };
    int unison { 1 };
    double detune { 0.0 };              ///< `kParamUnisonRange`, semitones
    double stereo { 0.0 };              ///< `kParamUnisonStereo`, percent
    const char* warp1 { nullptr };      ///< the reference's own mode name, e.g. "kSync"
    double warp1Amount { 0.0 };
    const char* warp2 { nullptr };
    double warp2Amount { 0.0 };
};

struct FilterRecipe
{
    const char* type { nullptr };       ///< nullptr: the filter is off
    double cutoff { 1.0 };
    double resonance { 0.0 };
    double drive { 0.0 };
    double variation { 0.0 };           ///< `kParamVar`: the morph position for the morphing filters
};

struct AmpRecipe
{
    double attack { 0.004 };
    double decay { 0.0 };
    double sustain { 1.0 };
    double release { 0.08 };
};

struct ModRecipe
{
    int row { 0 };
    std::string_view source {};              ///< the reference's own source name, e.g. "LFO 1"
    std::string_view destinationClass {};    ///< the engine's own class name, e.g. "VoiceFilter"
    int instance { 0 };
    std::string_view destination {};         ///< the destination parameter's *name*, e.g. "kParamFreq"
    double amount { 0.0 };                   ///< percent, signed
    bool bipolar { false };
};

struct PresetRecipe
{
    const char* name { nullptr };
    const char* category { nullptr };
    const char* description { nullptr };

    OscRecipe a {};
    OscRecipe b {};
    OscRecipe c {};

    const char* noise { nullptr };      ///< colour name, nullptr: NOISE is off
    double noiseLevel { 0.2 };
    bool noiseRandomPhase { true };

    const char* sub { nullptr };        ///< shape token, nullptr: SUB is off
    double subOctave { -1.0 };
    double subLevel { 0.6 };
    double subFine { 0.0 };

    FilterRecipe filter1 {};
    FilterRecipe filter2 {};

    AmpRecipe amp {};

    int polyphony { 16 };
    bool mono { false };
    bool legato { false };
    double glide { 0.0 };
    double level { 0.9 };

    std::vector<ModRecipe> mod {};
};

// ------------------------------------------------------------------------------------ the builder

/// Whether the reference's own vocabulary for a leaf carries this token. `ParameterBridge::buildChoices`
/// builds xerum's choice lists from exactly these enums, so a token that is not one of them resolves to
/// the "not in xerum's set" sentinel — a module the preset claims to use and the engine does not touch.
bool inReferenceVocabulary (const std::string_view schemaPath, const std::string_view token)
{
    const auto* leaf = model::Schema::instance().describe (schemaPath);

    if (leaf == nullptr)
        return false;

    for (std::size_t i = 0; i < leaf->valueCount; ++i)
        if (leaf->values[i] == token)
            return true;

    return false;
}

/// The id the engine addresses a destination parameter by, from its **name**.
///
/// The name is the only stable identity a row's destination has across schema versions (ticket 03:
/// `destModuleParamID` is version-dependent), and the engine reads the id — so the two are derived from
/// one table here rather than written twice in a recipe. An unknown pair is a problem, not a guess.
int destinationId (const std::string_view klass, const std::string_view name)
{
    if (klass == dsp::voiceDestination::kOscillatorClass)
    {
        if (name == "kParamVolume")    return dsp::voiceDestination::kParamVolume;
        if (name == "kParamOctave")    return dsp::voiceDestination::kParamOctave;
        if (name == "kParamPitch")     return dsp::voiceDestination::kParamPitch;
        if (name == "kParamFine")      return dsp::voiceDestination::kParamFine;
        if (name == "kParamCoarsePit") return dsp::voiceDestination::kParamCoarsePit;
    }

    if (klass == dsp::voiceDestination::kVoiceFilterClass)
    {
        if (name == "kParamFreq") return dsp::voiceDestination::kParamFreq;
        if (name == "kParamReso") return dsp::voiceDestination::kParamReso;
    }

    return -1;
}

model::Value number (const double value)
{
    model::Value out;
    out.type = model::Value::Type::Float;
    out.real = value;
    return out;
}

model::Value integer (const long long value)
{
    model::Value out;
    out.type = model::Value::Type::Int;
    out.integer = value;
    return out;
}

model::Value text (std::string value)
{
    model::Value out;
    out.type = model::Value::Type::String;
    out.text = std::move (value);
    return out;
}

model::Value flag (const bool value)
{
    model::Value out;
    out.type = model::Value::Type::Int;
    out.integer = value ? 1 : 0;
    return out;
}

std::string parameterPath (const char* module, const char* leaf)
{
    return std::string (module) + leaf;
}

class Builder
{
public:
    Builder (model::PresetDocument& document, std::vector<std::string>& problems, const std::string& presetName)
        : document_ (document), problems_ (problems), name_ (presetName)
    {}

    /// A number as a float leaf. Whole numbers of a leaf the schema types as an integer go through
    /// `whole` instead, which is what the model's own reader produces for those.
    void value (const std::string& path, const double amount)
    {
        document_.parameters[path] = number (amount);
    }

    void whole (const std::string& path, const long long amount)
    {
        document_.parameters[path] = integer (amount);
    }

    void on (const std::string& path)
    {
        document_.parameters[path] = flag (true);
    }

    void token (const std::string& path, const std::string_view name)
    {
        document_.parameters[path] = text (std::string (name));
    }

    void problem (std::string message)
    {
        problems_.push_back (name_ + ": " + std::move (message));
    }

private:
    model::PresetDocument& document_;
    std::vector<std::string>& problems_;
    const std::string& name_;
};

} // namespace
} // namespace xerum::presets

// ------------------------------------------------------------------------------------ the recipes
//
// The bank. Grouped by category, and inside a category the order the browser will show. Every entry is
// authored here: the name is ours, the tables are xerum's own generated tables, and the parameters are
// ones the engine implements.

namespace xerum::presets
{

namespace
{

const std::vector<PresetRecipe>& recipes()
{
    static const std::vector<PresetRecipe> recipes =
    {
        // ==========================================================================================
        // Bass
        // ==========================================================================================
        { .name = "Molten Sub",
          .category = "Bass",
          .description = "A saw fading into its own fundamental two octaves down, under a Moog ladder at "
                         "12% resonance, with the SUB slot doubling it — the plain low end.",
          .a = { .table = kSawFade, .position = 2, .octave = -2 },
          .sub = "kSaw",
          .subOctave = -2,
          .filter1 = { .type = "LadderMg", .cutoff = 0.44, .resonance = 6 },
          .amp = { .attack = 0.006, .decay = 0.75, .sustain = 0.92, .release = 0.22 },
          .mono = true },

        { .name = "Iron Filament",
          .category = "Bass",
          .description = "The 32-frame tanh folder at frame 6, driven further by a tube-saturation warp, "
                         "through a 24 dB ladder.",
          .a = { .table = kFoldDrive, .position = 6, .octave = -2, .warp1 = "kDistTube", .warp1Amount = 0.35 },
          .sub = "Sine",
          .filter1 = { .type = "L24", .cutoff = 0.24, .resonance = 26 },
          .amp = { .attack = 0.002, .decay = 0.32, .sustain = 0.9, .release = 0.1 } },

        { .name = "Cinder Block",
          .category = "Bass",
          .description = "A saturated ramp from frame 4 of Growl Fold, an acid ladder driven 25% and "
                         "resonating at 30%.",
          .a = { .table = kGrowlFold, .position = 4, .octave = -2 },
          .filter1 = { .type = "LadderAcid", .cutoff = 0.32, .resonance = 38, .drive = 25 },
          .amp = { .attack = 0.003, .decay = 0.62, .sustain = 0.78, .release = 0.12 } },

        { .name = "Basalt",
          .category = "Bass",
          .description = "Harmonics stacking onto a sine at frame 3, an 18 dB ladder, and a square sub for "
                         "the body.",
          .a = { .table = kHarmonicSweep, .position = 3, .octave = -2 },
          .sub = "kSquare",
          .subOctave = -2,
          .filter1 = { .type = "L18", .cutoff = 0.42, .resonance = 8 },
          .amp = { .attack = 0.004, .decay = 0.7, .sustain = 0.8, .release = 0.18 } },

        { .name = "Night Ferry",
          .category = "Bass",
          .description = "A pure sine two octaves down with a saw sub, gliding 40 ms into every note under "
                         "a 24 dB low pass.",
          .a = { .table = kSine, .octave = -2 },
          .sub = "kSaw",
          .subOctave = -2,
          .filter1 = { .type = "L24", .cutoff = 0.26, .resonance = 10 },
          .amp = { .attack = 0.01, .decay = 0.8, .sustain = 0.9, .release = 0.3 },
          .mono = true,
          .glide = 0.04 },

        { .name = "Copper Wire",
          .category = "Bass",
          .description = "Phase-modulated sine at frame 4, modulated again by its own other side with "
                         "kFM_OSC, through a Moog ladder.",
          .a = { .table = kFmMorph, .position = 4, .octave = -2, .warp1 = "kFM_OSC", .warp1Amount = 0.25 },
          .filter1 = { .type = "MgL24", .cutoff = 0.52, .resonance = 12 },
          .amp = { .attack = 0.004, .decay = 0.9, .sustain = 0.72, .release = 0.3 } },

        { .name = "Deep Anchor",
          .category = "Bass",
          .description = "Triangle two octaves down with a sine sub three octaves down and a 6 dB slope: "
                         "almost no filtering, because the harmonics were not there to begin with.",
          .a = { .table = kTriangle, .octave = -2 },
          .sub = "Sine",
          .subOctave = -3,
          .filter1 = { .type = "L6", .cutoff = 0.5, .resonance = 4 },
          .amp = { .attack = 0.02, .decay = 1.0, .sustain = 1.0, .release = 0.4 },
          .mono = true },

        { .name = "Rust Bloom",
          .category = "Bass",
          .description = "A ramp quantised at frame 7 of Bit Crush, soft-clipped, through a 12 dB ladder "
                         "driven 20%.",
          .a = { .table = kBitCrush, .position = 7, .octave = -2, .warp1 = "kDistSoftClip", .warp1Amount = 0.3 },
          .sub = "kTriangle",
          .filter1 = { .type = "L12", .cutoff = 0.35, .resonance = 20, .drive = 20 },
          .amp = { .attack = 0.003, .decay = 0.6, .sustain = 0.78, .release = 0.2 } },

        { .name = "Tar Pit",
          .category = "Bass",
          .description = "Growl Fold at frame 9 with a sync warp on top, into an EMS ladder at 35% "
                         "resonance.",
          .a = { .table = kGrowlFold, .position = 9, .octave = -2, .warp1 = "kSync", .warp1Amount = 0.3 },
          .filter1 = { .type = "LadderEMS", .cutoff = 0.2, .resonance = 35 },
          .amp = { .attack = 0.004, .decay = 0.5, .sustain = 0.72, .release = 0.14 } },

        { .name = "Low Orbit",
          .category = "Bass",
          .description = "A 5%-wide pulse from frame 3 of Pulse Grit with a square sub, 24 dB below and "
                         "10% resonance.",
          .a = { .table = kPulseGrit, .position = 3, .octave = -2 },
          .sub = "kSquare",
          .subOctave = -2,
          .filter1 = { .type = "L24", .cutoff = 0.45, .resonance = 10 },
          .amp = { .attack = 0.005, .decay = 0.7, .sustain = 0.8, .release = 0.2 } },

        { .name = "Brass Root",
          .category = "Bass",
          .description = "A saw at frame 8 with the filter-as-warp family adding a low pass of its own, "
                         "gliding 30 ms into a Moog ladder at 22%.",
          .a = { .table = kSawFade, .position = 8, .octave = -1,
                 .warp1 = "kFilterLPF", .warp1Amount = 0.3 },
          .filter1 = { .type = "LadderMg", .cutoff = 0.33, .resonance = 22, .drive = 10 },
          .amp = { .attack = 0.03, .decay = 0.5, .sustain = 0.7, .release = 0.25 },
          .mono = true,
          .glide = 0.03 },

        { .name = "Granite Steps",
          .category = "Bass",
          .description = "A saw through a rising sync ratio at frame 5, sync-warped again, 18 dB down.",
          .a = { .table = kSyncSaw, .position = 5, .octave = -2, .warp1 = "kSync", .warp1Amount = 0.4 },
          .filter1 = { .type = "L18", .cutoff = 0.4, .resonance = 12 },
          .amp = { .attack = 0.003, .decay = 0.6, .sustain = 0.8, .release = 0.15 } },

        { .name = "Wet Concrete",
          .category = "Bass",
          .description = "A sine dissolving into noise at frame 2 of Noise Blend, with the NOISE slot's "
                         "own pink colour added quietly for the grit.",
          .a = { .table = kNoiseBlend, .position = 2, .octave = -2 },
          .noise = "Pink",
          .noiseLevel = 0.3,
          .filter1 = { .type = "L12", .cutoff = 0.55, .resonance = 22 },
          .amp = { .attack = 0.008, .decay = 1.1, .sustain = 0.82, .release = 0.35 } },

        { .name = "Submarine Glow",
          .category = "Bass",
          .description = "A resonant peak walked up the harmonics at frame 2, a sine sub two octaves below "
                         "that, and a slow 1.2 s decay.",
          .a = { .table = kFormantSweep, .position = 2, .octave = -2 },
          .sub = "Sine",
          .subOctave = -3,
          .filter1 = { .type = "L12", .cutoff = 0.55, .resonance = 8 },
          .amp = { .attack = 0.05, .decay = 1.2, .sustain = 0.95, .release = 0.5 },
          .mono = true },

        { .name = "Ember Bass",
          .category = "Bass",
          .description = "Fold Drive at frame 12 — the hardest of its 32 frames — distorted again by the "
                         "DirtyMg ladder, driven 30%.",
          .a = { .table = kFoldDrive, .position = 12, .octave = -2,
                 .warp2 = "kDistTube", .warp2Amount = 0.5 },
          .filter1 = { .type = "DirtyMg", .cutoff = 0.18, .resonance = 34, .drive = 30 },
          .amp = { .attack = 0.0015, .decay = 0.22, .sustain = 0.58, .release = 0.1 } },

        { .name = "Diesel Choir",
          .category = "Bass",
          .description = "The half-integer sync table at frame 4, two detuned voices 6 semitones apart, "
                         "soft-clipped and filtered by a Moog ladder.",
          .a = { .table = kMetalSync, .position = 4, .octave = -2, .unison = 2, .detune = 6,
                 .warp1 = "kDistSoftClip", .warp1Amount = 0.2 },
          .filter1 = { .type = "LadderMg", .cutoff = 0.36, .resonance = 18 },
          .amp = { .attack = 0.01, .decay = 0.7, .sustain = 0.85, .release = 0.25 } },

        // ==========================================================================================
        // Lead
        // ==========================================================================================
        { .name = "Amber Rain",
          .category = "Lead",
          .description = "Saw at frame 9, three voices spread 8 semitones across 30% of the image, sync "
                         "warp at a third, high pass with a 12 dB slope.",
          .a = { .table = kSawFade, .position = 9, .unison = 3, .detune = 8, .stereo = 30,
                 .warp1 = "kSync", .warp1Amount = 0.35 },
          .filter1 = { .type = "H12", .cutoff = 0.6, .resonance = 10 },
          .amp = { .attack = 0.01, .decay = 0.3, .sustain = 0.8, .release = 0.3 },
          .mod = { { .row = 0, .source = "Velo", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 30 },
                   { .row = 1, .source = "Mod Wheel", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 40 } } },

        { .name = "Paper Lantern",
          .category = "Lead",
          .description = "High harmonic stack at frame 6, two voices, a high shelf rather than a filter, "
                         "and a 10 ms attack.",
          .a = { .table = kGlassStack, .position = 6, .unison = 2, .detune = 5, .stereo = 20 },
          .filter1 = { .type = "HEQ12", .cutoff = 0.7 },
          .amp = { .attack = 0.005, .decay = 0.5, .sustain = 0.7, .release = 0.25 } },

        { .name = "Neon Signal",
          .category = "Lead",
          .description = "Sync sweep at frame 8 with a pulse-width warp layered on, into a 12 dB low pass "
                         "at 18% resonance.",
          .a = { .table = kSyncSweep, .position = 8, .warp2 = "kPWM", .warp2Amount = 0.4 },
          .filter1 = { .type = "L12", .cutoff = 0.55, .resonance = 18 },
          .amp = { .attack = 0.006, .decay = 0.35, .sustain = 0.8, .release = 0.22 } },

        { .name = "Copper Chord",
          .category = "Lead",
          .description = "Odd harmonics at frame 5, four voices 12 semitones apart across 45%, Moog ladder, "
                         "legato with 20 ms glide.",
          .a = { .table = kOddStack, .position = 5, .unison = 4, .detune = 12, .stereo = 45 },
          .filter1 = { .type = "LadderMg", .cutoff = 0.5, .resonance = 15 },
          .amp = { .attack = 0.02, .decay = 0.4, .sustain = 0.85, .release = 0.35 },
          .mono = true,
          .legato = true,
          .glide = 0.02 },

        { .name = "Skyline Solo",
          .category = "Lead",
          .description = "Harmonics appearing across 24 frames, taken at frame 12, phase-distorted by its "
                         "own oscillator, 24 dB low pass at 22%.",
          .a = { .table = kHarmonicSweep, .position = 12, .unison = 3, .detune = 10, .stereo = 35,
                 .warp1 = "kPD_OSC", .warp1Amount = 0.3 },
          .filter1 = { .type = "L24", .cutoff = 0.6, .resonance = 22 },
          .amp = { .attack = 0.008, .decay = 0.4, .sustain = 0.8, .release = 0.28 } },

        { .name = "Salt Flare",
          .category = "Lead",
          .description = "Half-integer sync at frame 7, ring-modulated by the other side, band passed at "
                         "20% with two voices 15 semitones apart.",
          .a = { .table = kMetalSync, .position = 7, .unison = 2, .detune = 15, .stereo = 25,
                 .warp1 = "kRM_OSC", .warp1Amount = 0.35 },
          .filter1 = { .type = "BP12", .cutoff = 0.62, .resonance = 20 },
          .amp = { .attack = 0.004, .decay = 0.45, .sustain = 0.78, .release = 0.3 } },

        { .name = "Bright Static",
          .category = "Lead",
          .description = "The 19th and 23rd harmonics of Sparkle at frame 8, tube-distorted, 24 dB high "
                         "pass: almost nothing but air.",
          .a = { .table = kSparkle, .position = 8, .unison = 2, .detune = 6,
                 .warp1 = "kDistTube", .warp1Amount = 0.25 },
          .filter1 = { .type = "H24", .cutoff = 0.65 },
          .amp = { .attack = 0.003, .decay = 0.3, .sustain = 0.75, .release = 0.2 } },

        { .name = "Cathedral Wire",
          .category = "Lead",
          .description = "Five voices 14 semitones apart across 60% of the image, on a bright formant at "
                         "frame 4 with a gentle 12 dB low pass.",
          .a = { .table = kFormantHigh, .position = 4, .unison = 5, .detune = 14, .stereo = 60 },
          .filter1 = { .type = "L12", .cutoff = 0.62, .resonance = 12 },
          .amp = { .attack = 0.03, .decay = 0.5, .sustain = 0.85, .release = 1.2 } },

        { .name = "Glass Arrow",
          .category = "Lead",
          .description = "High glass stack at frame 9, frequency-modulated by its own oscillator, through "
                         "a 12 dB high pass — a thin, immediate lead.",
          .a = { .table = kGlassStack, .position = 9, .warp1 = "kFM_OSC", .warp1Amount = 0.4 },
          .filter1 = { .type = "HP12", .cutoff = 0.55, .resonance = 10 },
          .amp = { .attack = 0.002, .decay = 0.35, .sustain = 0.8, .release = 0.25 } },

        { .name = "Fever Line",
          .category = "Lead",
          .description = "The vowel sweep taken high, at frame 14, synced and pushed through an acid "
                         "ladder at 28% resonance.",
          .a = { .table = kFormantSweep, .position = 14, .warp1 = "kSync", .warp1Amount = 0.3 },
          .filter1 = { .type = "LadderAcid", .cutoff = 0.5, .resonance = 28 },
          .amp = { .attack = 0.004, .decay = 0.4, .sustain = 0.75, .release = 0.22 } },

        { .name = "Sunspot",
          .category = "Lead",
          .description = "A saturated ramp at frame 6 with tube distortion on top, two voices, and an "
                         "18 dB Moog ladder at 18%.",
          .a = { .table = kGrowlFold, .position = 6, .unison = 2, .detune = 7, .stereo = 20,
                 .warp1 = "kDistTube", .warp1Amount = 0.45 },
          .filter1 = { .type = "MgL18", .cutoff = 0.52, .resonance = 18 },
          .amp = { .attack = 0.005, .decay = 0.35, .sustain = 0.78, .release = 0.25 } },

        { .name = "Cathode Ray",
          .category = "Lead",
          .description = "A sine ring-modulated by a quotient of itself, frame 5, amplitude-modulated "
                         "again, through an 18 dB low pass with three voices.",
          .a = { .table = kRingSwell, .position = 5, .unison = 3, .detune = 9, .stereo = 30,
                 .warp1 = "kAM_OSC", .warp1Amount = 0.3 },
          .filter1 = { .type = "L18", .cutoff = 0.45, .resonance = 15 },
          .amp = { .attack = 0.006, .decay = 0.4, .sustain = 0.8, .release = 0.3 } },

        { .name = "Monsoon Reed",
          .category = "Lead",
          .description = "A narrowing pulse at frame 6, pulse-width warped to 50%, EMS ladder at 20%, two "
                         "voices 7 semitones apart.",
          .a = { .table = kPulseGrit, .position = 6, .unison = 2, .detune = 7, .stereo = 25,
                 .warp1 = "kPWM", .warp1Amount = 0.5 },
          .filter1 = { .type = "LadderEMS", .cutoff = 0.48, .resonance = 20 },
          .amp = { .attack = 0.012, .decay = 0.45, .sustain = 0.8, .release = 0.28 } },

        { .name = "Velvet Blade",
          .category = "Lead",
          .description = "Saw at frame 5, four voices across 11 semitones and 40% of the image, 24 dB low "
                         "pass at 25% resonance and a 20 ms attack.",
          .a = { .table = kSawFade, .position = 5, .unison = 4, .detune = 11, .stereo = 40 },
          .filter1 = { .type = "L24", .cutoff = 0.44, .resonance = 25 },
          .amp = { .attack = 0.02, .decay = 0.5, .sustain = 0.85, .release = 0.4 },
          .mod = { { .row = 0, .source = "Mod Wheel", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 45 } } },

        { .name = "Silver Tongue",
          .category = "Lead",
          .description = "A two-formant stack at frame 7, three voices, 12 dB low pass, and LFO 1 rocking "
                         "the cutoff by 10% either side.",
          .a = { .table = kVocalStack, .position = 7, .unison = 3, .detune = 8, .stereo = 30 },
          .filter1 = { .type = "L12", .cutoff = 0.58, .resonance = 14 },
          .amp = { .attack = 0.01, .decay = 0.4, .sustain = 0.85, .release = 0.35 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 10, .bipolar = true } } },

        { .name = "Beacon",
          .category = "Lead",
          .description = "Sync sweep at frame 16 with a hard sync warp, single voice, 50 ms glide into a "
                         "24 dB Moog ladder at 20%.",
          .a = { .table = kSyncSweep, .position = 16, .warp1 = "kSync", .warp1Amount = 0.5 },
          .filter1 = { .type = "MgL24", .cutoff = 0.6, .resonance = 20 },
          .amp = { .attack = 0.004, .decay = 0.4, .sustain = 0.85, .release = 0.3 },
          .mono = true,
          .legato = true,
          .glide = 0.05 },

        // ==========================================================================================
        // Pad
        // ==========================================================================================
        { .name = "Static Cathedral",
          .category = "Pad",
          .description = "Two oscillators — harmonics at frame 6 and a glass stack at frame 10 — five "
                         "voices across 55%, a 1.6 s attack, and LFO 1 breathing the cutoff.",
          .a = { .table = kHarmonicSweep, .position = 6, .unison = 5, .detune = 8, .stereo = 55 },
          .b = { .table = kGlassStack, .position = 10, .octave = 1, .level = 0.45 },
          .filter1 = { .type = "L12", .cutoff = 0.5, .resonance = 10 },
          .amp = { .attack = 1.6, .decay = 2.0, .sustain = 0.8, .release = 4.0 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 12, .bipolar = true },
                   { .row = 1, .source = "Mod Wheel", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 35 } } },

        { .name = "Distant Weather",
          .category = "Pad",
          .description = "A vowel sweep at frame 8 over a noise blend at frame 3, six voices across 60%, "
                         "2.4 s attack and a 6 s release.",
          .a = { .table = kFormantSweep, .position = 8, .unison = 6, .detune = 9, .stereo = 60 },
          .b = { .table = kNoiseBlend, .position = 3, .level = 0.4 },
          .sub = "Sine",
          .subOctave = -2,
          .subLevel = 0.35,
          .filter1 = { .type = "L24", .cutoff = 0.42, .resonance = 12 },
          .amp = { .attack = 2.4, .decay = 1.5, .sustain = 0.9, .release = 6.0 } },

        { .name = "Ocean Bed",
          .category = "Pad",
          .description = "Odd harmonics at frame 4 under a two-formant stack at frame 9, four voices "
                         "across 45%, Moog ladder, 1.2 s attack.",
          .a = { .table = kOddStack, .position = 4, .unison = 4, .detune = 7, .stereo = 45 },
          .b = { .table = kVocalStack, .position = 9, .level = 0.5 },
          .filter1 = { .type = "LadderMg", .cutoff = 0.42, .resonance = 12 },
          .amp = { .attack = 1.2, .decay = 3.0, .sustain = 0.85, .release = 5.5 } },

        { .name = "Slow Aperture",
          .category = "Pad",
          .description = "Sync sweep at frame 10 with sparkle at frame 6 an octave up, 3.2 s attack, 7 s "
                         "release, and a 24 dB low pass LFO 1 opens by 15%.",
          .a = { .table = kSyncSweep, .position = 10, .unison = 4, .detune = 6, .stereo = 40 },
          .b = { .table = kSparkle, .position = 6, .octave = 1, .level = 0.35 },
          .filter1 = { .type = "L24", .cutoff = 0.46, .resonance = 18 },
          .amp = { .attack = 3.2, .decay = 4.0, .sustain = 0.7, .release = 7.0 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 15, .bipolar = true },
                   { .row = 1, .source = "Mod Wheel", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 40 } } },

        { .name = "Night Blooming",
          .category = "Pad",
          .description = "Two-formant stack at frame 5 with a bright formant at frame 8, five voices "
                         "across 50%, and a 2 s attack into a 5 s tail.",
          .a = { .table = kVocalStack, .position = 5, .unison = 5, .detune = 10, .stereo = 50 },
          .b = { .table = kFormantHigh, .position = 8, .level = 0.4 },
          .filter1 = { .type = "L12", .cutoff = 0.52, .resonance = 10 },
          .amp = { .attack = 2.0, .decay = 2.5, .sustain = 0.85, .release = 5.0 } },

        { .name = "Held Breath",
          .category = "Pad",
          .description = "A sine under a noise blend at frame 6, three voices narrowly spread, 3.5 s "
                         "attack and the longest release in the bank at 8 s.",
          .a = { .table = kSine, .unison = 3, .detune = 4, .stereo = 30 },
          .b = { .table = kNoiseBlend, .position = 6, .level = 0.55 },
          .sub = "Sine",
          .subOctave = -2,
          .subLevel = 0.3,
          .filter1 = { .type = "L6", .cutoff = 0.6 },
          .amp = { .attack = 3.5, .decay = 2.0, .sustain = 0.9, .release = 8.0 } },

        { .name = "Ash Garden",
          .category = "Pad",
          .description = "A saturated ramp at frame 7 with the eighth frame of Dust mixed under it, five "
                         "voices across 65%, 12 dB low pass at 14%.",
          .a = { .table = kGrowlFold, .position = 7, .unison = 5, .detune = 12, .stereo = 65 },
          .b = { .table = kDust, .position = 4, .level = 0.3 },
          .filter1 = { .type = "L12", .cutoff = 0.4, .resonance = 14 },
          .amp = { .attack = 1.8, .decay = 3.0, .sustain = 0.8, .release = 4.5 } },

        { .name = "Parallel Shores",
          .category = "Pad",
          .description = "Harmonics at frame 14 against a vowel sweep at frame 3, six voices across 70%, "
                         "2.6 s attack into a 6.5 s release.",
          .a = { .table = kHarmonicSweep, .position = 14, .unison = 6, .detune = 11, .stereo = 70 },
          .b = { .table = kFormantSweep, .position = 3, .octave = -1, .level = 0.5 },
          .filter1 = { .type = "L18", .cutoff = 0.48, .resonance = 12 },
          .amp = { .attack = 2.6, .decay = 3.5, .sustain = 0.82, .release = 6.5 } },

        { .name = "Lantern Field",
          .category = "Pad",
          .description = "Sparkle at frame 4 under a glass stack at frame 7 an octave up, four voices "
                         "across 40%, and a high shelf for the top.",
          .a = { .table = kSparkle, .position = 4, .unison = 4, .detune = 6, .stereo = 40 },
          .b = { .table = kGlassStack, .position = 7, .octave = 1, .level = 0.5 },
          .filter1 = { .type = "HEQ12", .cutoff = 0.6 },
          .amp = { .attack = 1.4, .decay = 2.5, .sustain = 0.8, .release = 4.0 } },

        { .name = "Warm Machinery",
          .category = "Pad",
          .description = "A quantised ramp at frame 8 with a narrowing pulse at frame 5 under it, four "
                         "voices across 35%, EMS ladder at 20% resonance.",
          .a = { .table = kBitCrush, .position = 8, .unison = 4, .detune = 9, .stereo = 35 },
          .b = { .table = kPulseGrit, .position = 5, .octave = -1, .level = 0.45 },
          .filter1 = { .type = "LadderEMS", .cutoff = 0.38, .resonance = 20 },
          .amp = { .attack = 1.0, .decay = 2.0, .sustain = 0.85, .release = 3.5 } },

        { .name = "Drift Compass",
          .category = "Pad",
          .description = "Half-integer sync at frame 9 with a two-formant stack at frame 2 beneath it, "
                         "five voices across 55%, and a 5 s release.",
          .a = { .table = kMetalSync, .position = 9, .unison = 5, .detune = 13, .stereo = 55 },
          .b = { .table = kVocalStack, .position = 2, .level = 0.45 },
          .filter1 = { .type = "L24", .cutoff = 0.44, .resonance = 14 },
          .amp = { .attack = 2.2, .decay = 3.0, .sustain = 0.8, .release = 5.0 } },

        { .name = "Fog Bank",
          .category = "Pad",
          .description = "Noise blend at frame 8 with eight frames of Dust at frame 6 under it, six voices "
                         "across 75% of the image, and a 3 s attack.",
          .a = { .table = kNoiseBlend, .position = 8, .unison = 6, .detune = 15, .stereo = 75 },
          .b = { .table = kDust, .position = 6, .level = 0.4 },
          .noise = "Brown",
          .noiseLevel = 0.15,
          .filter1 = { .type = "L24", .cutoff = 0.35, .resonance = 12 },
          .amp = { .attack = 3.0, .decay = 2.0, .sustain = 0.9, .release = 7.5 } },

        { .name = "Quiet Orbit",
          .category = "Pad",
          .description = "A sine with harmonics at frame 20 of 24 rising behind it, three voices narrowly "
                         "spread, 1.5 s attack into a 5 s tail.",
          .a = { .table = kSine, .unison = 3, .detune = 5, .stereo = 25 },
          .b = { .table = kHarmonicSweep, .position = 20, .level = 0.5 },
          .sub = "Sine",
          .subOctave = -1,
          .subLevel = 0.4,
          .filter1 = { .type = "L12", .cutoff = 0.58 },
          .amp = { .attack = 1.5, .decay = 2.5, .sustain = 0.85, .release = 5.0 },
          .mod = { { .row = 0, .source = "Mod Wheel", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 40 } } },

        { .name = "Sleeping Satellite",
          .category = "Pad",
          .description = "The vowel sweep high at frame 17 with sparkle at frame 10 on top, five voices "
                         "across 60%, 2.8 s attack and a 6 s release.",
          .a = { .table = kFormantSweep, .position = 17, .unison = 5, .detune = 8, .stereo = 60 },
          .b = { .table = kSparkle, .position = 10, .octave = 1, .level = 0.4 },
          .filter1 = { .type = "L24", .cutoff = 0.5, .resonance = 10 },
          .amp = { .attack = 2.8, .decay = 3.0, .sustain = 0.8, .release = 6.0 } },

        { .name = "Velvet Fog",
          .category = "Pad",
          .description = "Saw at frame 7 with a square fading to sine at frame 4 underneath, four voices "
                         "across 30%, and a gentle 12 dB low pass.",
          .a = { .table = kSawFade, .position = 7, .unison = 4, .detune = 7, .stereo = 30 },
          .b = { .table = kSquareFade, .position = 4, .octave = -1, .level = 0.5 },
          .filter1 = { .type = "L12", .cutoff = 0.55, .resonance = 8 },
          .amp = { .attack = 1.1, .decay = 2.0, .sustain = 0.85, .release = 3.8 } },

        { .name = "Halcyon",
          .category = "Pad",
          .description = "Odd harmonics at frame 8 with a harmonic sweep at frame 3 an octave down, five "
                         "voices across 45%, and LFO 1 drifting the cutoff.",
          .a = { .table = kOddStack, .position = 8, .unison = 5, .detune = 9, .stereo = 45 },
          .b = { .table = kHarmonicSweep, .position = 3, .octave = -1, .level = 0.45 },
          .filter1 = { .type = "L18", .cutoff = 0.52, .resonance = 10 },
          .amp = { .attack = 2.0, .decay = 3.0, .sustain = 0.85, .release = 4.8 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 8, .bipolar = true },
                   { .row = 1, .source = "Mod Wheel", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 35 } } },

        // ==========================================================================================
        // Keys
        // ==========================================================================================
        { .name = "Bell Jar",
          .category = "Keys",
          .description = "Phase-modulated sine at frame 9 with kFM_OSC, a 1.2 s decay, and velocity "
                         "opening the 12 dB low pass by 35%.",
          .a = { .table = kFmMorph, .position = 9, .warp1 = "kFM_OSC", .warp1Amount = 0.3 },
          .filter1 = { .type = "L12", .cutoff = 0.45, .resonance = 10 },
          .amp = { .attack = 0.002, .decay = 1.2, .sustain = 0.25, .release = 0.6 },
          .mod = { { .row = 0, .source = "Velo", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 60 } } },

        { .name = "Cold Piano",
          .category = "Keys",
          .description = "Saw at frame 6 with a sine underneath it, a 1.6 s decay to 20%, and a 24 dB low "
                         "pass at 12%.",
          .a = { .table = kSawFade, .position = 6 },
          .b = { .table = kSine, .octave = -1, .level = 0.5 },
          .filter1 = { .type = "L24", .cutoff = 0.52, .resonance = 12 },
          .amp = { .attack = 0.003, .decay = 1.6, .sustain = 0.2, .release = 0.5 } },

        { .name = "Music Box Engine",
          .category = "Keys",
          .description = "Glass stack at frame 5, two voices four semitones apart, 0.9 s decay to 10% and "
                         "a 12 dB high pass.",
          .a = { .table = kGlassStack, .position = 5, .unison = 2, .detune = 4, .stereo = 15 },
          .filter1 = { .type = "HP12", .cutoff = 0.5 },
          .amp = { .attack = 0.001, .decay = 0.9, .sustain = 0.1, .release = 0.4 } },

        { .name = "Ivory Static",
          .category = "Keys",
          .description = "A quantised ramp at frame 9 with a quantise warp on top, an 18 dB low pass at "
                         "16% resonance and a 1.1 s decay.",
          .a = { .table = kBitCrush, .position = 9, .warp1 = "kQuantize", .warp1Amount = 0.3 },
          .filter1 = { .type = "L18", .cutoff = 0.48, .resonance = 16 },
          .amp = { .attack = 0.002, .decay = 1.1, .sustain = 0.3, .release = 0.45 } },

        { .name = "Tine Garden",
          .category = "Keys",
          .description = "Phase-modulated sine at frame 14 — the brightest of its frames — with a 1.8 s "
                         "decay to 15% and a light 12 dB low pass.",
          .a = { .table = kFmMorph, .position = 14 },
          .filter1 = { .type = "L12", .cutoff = 0.58, .resonance = 10 },
          .amp = { .attack = 0.004, .decay = 1.8, .sustain = 0.15, .release = 0.7 } },

        { .name = "Dust Harp",
          .category = "Keys",
          .description = "Eight frames of flat noise at frame 3 with a sine under it, band passed at 20% "
                         "resonance, 0.8 s decay to almost nothing.",
          .a = { .table = kDust, .position = 3 },
          .b = { .table = kSine, .octave = -1, .level = 0.6 },
          .filter1 = { .type = "BP12", .cutoff = 0.55, .resonance = 20 },
          .amp = { .attack = 0.003, .decay = 0.8, .sustain = 0.05, .release = 0.5 } },

        { .name = "Clockwork",
          .category = "Keys",
          .description = "Half-integer sync at frame 6 with a sync warp on top, an acid ladder at 22%, and "
                         "a short 0.7 s decay.",
          .a = { .table = kMetalSync, .position = 6, .warp1 = "kSync", .warp1Amount = 0.3 },
          .filter1 = { .type = "LadderAcid", .cutoff = 0.45, .resonance = 22 },
          .amp = { .attack = 0.001, .decay = 0.7, .sustain = 0.2, .release = 0.35 } },

        { .name = "Salt Keys",
          .category = "Keys",
          .description = "The vowel sweep at frame 10 with a plain saw an octave down, a 1.4 s decay and a "
                         "12 dB low pass at 10%.",
          .a = { .table = kFormantSweep, .position = 10 },
          .b = { .table = kSaw, .octave = -1, .level = 0.45 },
          .filter1 = { .type = "L12", .cutoff = 0.5, .resonance = 10 },
          .amp = { .attack = 0.005, .decay = 1.4, .sustain = 0.35, .release = 0.6 } },

        { .name = "Vellum",
          .category = "Keys",
          .description = "Saw at frame 4 with a noise blend at frame 2 under it, a soft 8 ms attack and a "
                         "1 s decay to 40%.",
          .a = { .table = kSawFade, .position = 4 },
          .b = { .table = kNoiseBlend, .position = 2, .level = 0.35 },
          .filter1 = { .type = "LadderMg", .cutoff = 0.44, .resonance = 14 },
          .amp = { .attack = 0.008, .decay = 1.0, .sustain = 0.4, .release = 0.55 } },

        { .name = "Glass Clav",
          .category = "Keys",
          .description = "The glass stack at frame 8, soft-clipped by 35%, a 0.5 s decay to silence and a "
                         "12 dB high pass.",
          .a = { .table = kGlassStack, .position = 8, .warp1 = "kDistSoftClip", .warp1Amount = 0.35 },
          .filter1 = { .type = "H12", .cutoff = 0.6, .resonance = 18 },
          .amp = { .attack = 0.001, .decay = 0.5, .sustain = 0.0, .release = 0.3 } },

        { .name = "Reed Organ",
          .category = "Keys",
          .description = "Odd harmonics at frame 6 with a narrowing pulse at frame 6 an octave below, two "
                         "voices, and a sustained 12 dB low pass.",
          .a = { .table = kOddStack, .position = 6, .unison = 2, .detune = 5, .stereo = 20 },
          .b = { .table = kPulseGrit, .position = 6, .octave = -1, .level = 0.5 },
          .filter1 = { .type = "L12", .cutoff = 0.5, .resonance = 10 },
          .amp = { .attack = 0.01, .decay = 0.5, .sustain = 0.85, .release = 0.35 } },

        { .name = "Paper Piano",
          .category = "Keys",
          .description = "Saw at frame 3 with a triangle underneath, a 1.3 s decay to 18%, and velocity "
                         "opening the 24 dB low pass by 30%.",
          .a = { .table = kSawFade, .position = 3 },
          .b = { .table = kTriangle, .octave = -1, .level = 0.5 },
          .filter1 = { .type = "L24", .cutoff = 0.46, .resonance = 12 },
          .amp = { .attack = 0.003, .decay = 1.3, .sustain = 0.18, .release = 0.5 },
          .mod = { { .row = 0, .source = "Velo", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 30 } } },

        // ==========================================================================================
        // Pluck
        // ==========================================================================================
        { .name = "Iron Rain",
          .category = "Pluck",
          .description = "A phase-bent sine at frame 5 with a positive bend warp, 0.35 s decay to "
                         "silence, 24 dB low pass at 20%.",
          .a = { .table = kBendSweep, .position = 5, .warp1 = "kBendPos", .warp1Amount = 0.4 },
          .filter1 = { .type = "L24", .cutoff = 0.55, .resonance = 20 },
          .amp = { .attack = 0.001, .decay = 0.35, .sustain = 0.0, .release = 0.2 } },

        { .name = "Fired Clay",
          .category = "Pluck",
          .description = "A saw through a rising sync ratio at frame 4, synced again, 0.45 s decay to 5% "
                         "through a Moog ladder.",
          .a = { .table = kSyncSaw, .position = 4, .warp1 = "kSync", .warp1Amount = 0.35 },
          .filter1 = { .type = "LadderMg", .cutoff = 0.48, .resonance = 18 },
          .amp = { .attack = 0.001, .decay = 0.45, .sustain = 0.05, .release = 0.25 } },

        { .name = "Thistle",
          .category = "Pluck",
          .description = "The 19th and 23rd harmonics at frame 5, a 0.25 s decay to silence, 12 dB high "
                         "pass: a short, thin tick.",
          .a = { .table = kSparkle, .position = 5 },
          .filter1 = { .type = "H12", .cutoff = 0.6, .resonance = 15 },
          .amp = { .attack = 0.001, .decay = 0.25, .sustain = 0.0, .release = 0.15 } },

        { .name = "Cut Glass",
          .category = "Pluck",
          .description = "Glass stack at frame 6, soft-clipped 30%, a 0.4 s decay to silence and a 24 dB "
                         "high pass at 20% resonance.",
          .a = { .table = kGlassStack, .position = 6, .warp1 = "kDistSoftClip", .warp1Amount = 0.3 },
          .filter1 = { .type = "H24", .cutoff = 0.58, .resonance = 20 },
          .amp = { .attack = 0.001, .decay = 0.4, .sustain = 0.0, .release = 0.25 } },

        { .name = "Snapped Wire",
          .category = "Pluck",
          .description = "Half-integer sync at frame 5, ring-modulated by its own other side, band passed "
                         "at 25% with a 0.3 s decay.",
          .a = { .table = kMetalSync, .position = 5, .warp1 = "kRM_OSC", .warp1Amount = 0.3 },
          .filter1 = { .type = "BP12", .cutoff = 0.55, .resonance = 25 },
          .amp = { .attack = 0.001, .decay = 0.3, .sustain = 0.0, .release = 0.18 } },

        { .name = "Plum Stone",
          .category = "Pluck",
          .description = "A square fading to sine, taken at frame 5, a 0.6 s decay to 5% through a "
                         "12 dB low pass.",
          .a = { .table = kSquareFade, .position = 5 },
          .filter1 = { .type = "L12", .cutoff = 0.5, .resonance = 12 },
          .amp = { .attack = 0.002, .decay = 0.6, .sustain = 0.05, .release = 0.3 } },

        { .name = "Reed Snap",
          .category = "Pluck",
          .description = "A narrowing pulse at frame 4, pulse-width warped, through an EMS ladder at 22% "
                         "resonance and a 0.35 s decay.",
          .a = { .table = kPulseGrit, .position = 4, .warp1 = "kPWM", .warp1Amount = 0.4 },
          .filter1 = { .type = "LadderEMS", .cutoff = 0.46, .resonance = 22 },
          .amp = { .attack = 0.001, .decay = 0.35, .sustain = 0.0, .release = 0.2 } },

        { .name = "Rustic Pluck",
          .category = "Pluck",
          .description = "A saturated ramp at frame 3, tube-distorted 40%, an 18 dB low pass at 16% and a "
                         "0.5 s decay to silence.",
          .a = { .table = kGrowlFold, .position = 3, .warp1 = "kDistTube", .warp1Amount = 0.4 },
          .filter1 = { .type = "L18", .cutoff = 0.5, .resonance = 16 },
          .amp = { .attack = 0.001, .decay = 0.5, .sustain = 0.0, .release = 0.25 } },

        { .name = "Bright Twig",
          .category = "Pluck",
          .description = "Sync sweep at frame 6, a 0.3 s decay to silence, and a 12 dB high pass at 12% — "
                         "the brightest, shortest member of the sync family.",
          .a = { .table = kSyncSweep, .position = 6 },
          .filter1 = { .type = "H12", .cutoff = 0.62, .resonance = 12 },
          .amp = { .attack = 0.001, .decay = 0.3, .sustain = 0.0, .release = 0.15 } },

        { .name = "Tin Petal",
          .category = "Pluck",
          .description = "Harmonics at frame 8 with a 0.55 s decay to 5%, 24 dB low pass at 14% and a "
                         "2 ms attack.",
          .a = { .table = kHarmonicSweep, .position = 8 },
          .filter1 = { .type = "L24", .cutoff = 0.54, .resonance = 14 },
          .amp = { .attack = 0.002, .decay = 0.55, .sustain = 0.05, .release = 0.28 } },

        { .name = "Dry Reed",
          .category = "Pluck",
          .description = "Odd harmonics at frame 7, a 0.4 s decay to silence, 12 dB low pass at 18%.",
          .a = { .table = kOddStack, .position = 7 },
          .filter1 = { .type = "L12", .cutoff = 0.52, .resonance = 18 },
          .amp = { .attack = 0.001, .decay = 0.4, .sustain = 0.0, .release = 0.2 } },

        { .name = "Marble Drop",
          .category = "Pluck",
          .description = "A sine ring-modulated by a quotient of itself, frame 4, an acid ladder at 20%, "
                         "and a 0.7 s decay to silence.",
          .a = { .table = kRingSwell, .position = 4 },
          .filter1 = { .type = "LadderAcid", .cutoff = 0.5, .resonance = 20 },
          .amp = { .attack = 0.001, .decay = 0.7, .sustain = 0.0, .release = 0.35 } },

        { .name = "Quick Silver",
          .category = "Pluck",
          .description = "Phase-modulated sine at frame 6 with kFM_OSC, a 0.28 s decay to silence and a "
                         "12 dB high pass.",
          .a = { .table = kFmMorph, .position = 6, .warp1 = "kFM_OSC", .warp1Amount = 0.35 },
          .filter1 = { .type = "H12", .cutoff = 0.6, .resonance = 10 },
          .amp = { .attack = 0.001, .decay = 0.28, .sustain = 0.0, .release = 0.12 } },

        { .name = "Flax Pluck",
          .category = "Pluck",
          .description = "Saw at frame 10 — nearly its brightest — a 0.65 s decay to 10%, and a 24 dB low "
                         "pass at 12%.",
          .a = { .table = kSawFade, .position = 10 },
          .filter1 = { .type = "L24", .cutoff = 0.5, .resonance = 12 },
          .amp = { .attack = 0.002, .decay = 0.65, .sustain = 0.1, .release = 0.32 } },

        // ==========================================================================================
        // FX
        // ==========================================================================================
        { .name = "Broken Radio",
          .category = "FX",
          .description = "Dust at frame 5 under a half-integer sync at frame 8, amplitude-modulated by its "
                         "own other side, band passed at 40%, with LFO 1 and the mod wheel both on the "
                         "cutoff.",
          .a = { .table = kDust, .position = 5 },
          .b = { .table = kMetalSync, .position = 8, .warp1 = "kAM_OSC", .warp1Amount = 0.6 },
          .noise = "White",
          .noiseLevel = 0.2,
          .filter1 = { .type = "BP12", .cutoff = 0.5, .resonance = 40 },
          .amp = { .attack = 0.05, .decay = 1.0, .sustain = 0.6, .release = 1.5 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 50, .bipolar = true },
                   { .row = 1, .source = "Mod Wheel", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 50 } } },

        { .name = "Machine Dust",
          .category = "FX",
          .description = "The coarsest frame of Bit Crush, 11, through an acid ladder at 45% and driven "
                         "40%, with a 200 ms attack and a 2 s tail.",
          .a = { .table = kBitCrush, .position = 11 },
          .noise = "Pink",
          .noiseLevel = 0.18,
          .filter1 = { .type = "LadderAcid", .cutoff = 0.35, .resonance = 45, .drive = 40 },
          .amp = { .attack = 0.2, .decay = 1.5, .sustain = 0.7, .release = 2.0 } },

        { .name = "Riser Ladder",
          .category = "FX",
          .description = "Harmonics at frame 23 of 24 — the top of the stack — with LFO 1 sweeping the "
                         "oscillator's pitch 30% either side, under a 12 dB low pass.",
          .a = { .table = kHarmonicSweep, .position = 23 },
          .filter1 = { .type = "L12", .cutoff = 0.6, .resonance = 20 },
          .amp = { .attack = 0.5, .decay = 2.0, .sustain = 0.9, .release = 2.5 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kOscillatorClass, .instance = 0,
                     .destination = "kParamPitch", .amount = 30, .bipolar = true } } },

        { .name = "Alarm Bloom",
          .category = "FX",
          .description = "Sync sweep at frame 20 over a ring-swollen sine at frame 9, hard-synced, with "
                         "LFO 1 on the pitch and a high shelf on the top.",
          .a = { .table = kSyncSweep, .position = 20, .warp1 = "kSync", .warp1Amount = 0.6 },
          .b = { .table = kRingSwell, .position = 9, .level = 0.5 },
          .filter1 = { .type = "HEQ12", .cutoff = 0.55 },
          .amp = { .attack = 0.02, .decay = 1.0, .sustain = 0.8, .release = 1.8 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kOscillatorClass, .instance = 0,
                     .destination = "kParamPitch", .amount = 20, .bipolar = true } } },

        { .name = "Siren Drift",
          .category = "FX",
          .description = "Half-integer sync at frame 10, frequency-modulated 50%, with LFO 1 swinging the "
                         "pitch 40% — a slow, wide vibrato.",
          .a = { .table = kMetalSync, .position = 10, .warp1 = "kFM_OSC", .warp1Amount = 0.5 },
          .filter1 = { .type = "L18", .cutoff = 0.5, .resonance = 25 },
          .amp = { .attack = 0.05, .decay = 1.5, .sustain = 0.85, .release = 2.0 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kOscillatorClass, .instance = 0,
                     .destination = "kParamPitch", .amount = 40, .bipolar = true } } },

        { .name = "Collapse",
          .category = "FX",
          .description = "The vowel sweep at frame 22 — its highest peak — tube-distorted 70%, into an acid "
                         "ladder at 50% resonance driven 50%.",
          .a = { .table = kFormantSweep, .position = 22, .warp1 = "kDistTube", .warp1Amount = 0.7 },
          .filter1 = { .type = "LadderAcid", .cutoff = 0.3, .resonance = 50, .drive = 50 },
          .amp = { .attack = 0.02, .decay = 1.2, .sustain = 0.75, .release = 1.5 } },

        { .name = "Antenna Sweep",
          .category = "FX",
          .description = "The top frame of Sync Sweep under a hard sync warp, high-shelfed, with LFO 1 "
                         "moving the cutoff by 60%.",
          .a = { .table = kSyncSweep, .position = 24, .warp1 = "kSync", .warp1Amount = 0.5 },
          .filter1 = { .type = "HEQ12", .cutoff = 0.5 },
          .amp = { .attack = 0.1, .decay = 1.8, .sustain = 0.85, .release = 2.2 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 60, .bipolar = true } } },

        { .name = "Rocket Wash",
          .category = "FX",
          .description = "Noise blend at frame 10 with Dust at frame 7 under it, a 500 ms attack, a 24 dB "
                         "low pass at 20%, and LFO 1 on the cutoff.",
          .a = { .table = kNoiseBlend, .position = 10 },
          .b = { .table = kDust, .position = 7, .level = 0.6 },
          .noise = "White",
          .noiseLevel = 0.25,
          .filter1 = { .type = "L24", .cutoff = 0.4, .resonance = 20 },
          .amp = { .attack = 0.5, .decay = 2.0, .sustain = 0.85, .release = 3.0 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 40, .bipolar = true } } },

        { .name = "Error Signal",
          .category = "FX",
          .description = "Bit Crush at frame 6 crushed further by a quantise warp at 80%, band passed at "
                         "35%, with LFO 1 stepping the pitch.",
          .a = { .table = kBitCrush, .position = 6, .warp1 = "kQuantize", .warp1Amount = 0.8 },
          .filter1 = { .type = "BP12", .cutoff = 0.5, .resonance = 35 },
          .amp = { .attack = 0.01, .decay = 0.8, .sustain = 0.7, .release = 1.2 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kOscillatorClass, .instance = 0,
                     .destination = "kParamPitch", .amount = 20, .bipolar = true } } },

        { .name = "Feedback Bloom",
          .category = "FX",
          .description = "A ring-swollen sine at frame 7, ring-modulated again by its own other side, into "
                         "an EMS ladder at 55% resonance driven 35%.",
          .a = { .table = kRingSwell, .position = 7, .warp1 = "kRM_OSC", .warp1Amount = 0.6 },
          .filter1 = { .type = "LadderEMS", .cutoff = 0.35, .resonance = 55, .drive = 35 },
          .amp = { .attack = 0.15, .decay = 1.5, .sustain = 0.8, .release = 2.5 } },

        // ==========================================================================================
        // Texture
        // ==========================================================================================
        { .name = "Static Bloom",
          .category = "Texture",
          .description = "Dust at frame 2 under harmonics at frame 4, four voices across 60%, a 1.2 s "
                         "attack and a 12 dB low pass at 45%.",
          .a = { .table = kDust, .position = 2 },
          .b = { .table = kHarmonicSweep, .position = 4, .level = 0.5 },
          .filter1 = { .type = "L12", .cutoff = 0.45, .resonance = 12 },
          .amp = { .attack = 1.2, .decay = 2.0, .sustain = 0.85, .release = 3.0 } },

        { .name = "Rain On Metal",
          .category = "Texture",
          .description = "Half-integer sync at frame 7 with a noise blend at frame 5 beneath it, three "
                         "voices, band passed at 30%.",
          .a = { .table = kMetalSync, .position = 7, .unison = 3, .detune = 9, .stereo = 30 },
          .b = { .table = kNoiseBlend, .position = 5, .level = 0.5 },
          .filter1 = { .type = "BP12", .cutoff = 0.5, .resonance = 30 },
          .amp = { .attack = 0.8, .decay = 1.5, .sustain = 0.8, .release = 2.5 } },

        { .name = "Tape Hiss",
          .category = "Texture",
          .description = "Dust at frame 6 soft-clipped 20% into a high shelf, with the NOISE slot's own "
                         "white colour at 30% and a 300 ms attack.",
          .a = { .table = kDust, .position = 6, .warp1 = "kDistSoftClip", .warp1Amount = 0.2 },
          .noise = "White",
          .noiseLevel = 0.3,
          .filter1 = { .type = "HEQ12", .cutoff = 0.7 },
          .amp = { .attack = 0.3, .decay = 1.0, .sustain = 0.9, .release = 1.5 } },

        { .name = "Industrial Wind",
          .category = "Texture",
          .description = "Noise blend at frame 3, five voices across 70%, a 24 dB low pass at 25% resonance "
                         "with LFO 1 moving it, and a 2 s attack.",
          .a = { .table = kNoiseBlend, .position = 3, .unison = 5, .detune = 14, .stereo = 70 },
          .sub = "Sine",
          .subOctave = -2,
          .subLevel = 0.3,
          .filter1 = { .type = "L24", .cutoff = 0.3, .resonance = 25 },
          .amp = { .attack = 2.0, .decay = 2.5, .sustain = 0.9, .release = 4.0 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kVoiceFilterClass, .instance = 0,
                     .destination = "kParamFreq", .amount = 30, .bipolar = true } } },

        { .name = "Deep Current",
          .category = "Texture",
          .description = "The vowel sweep at frame 5 over Dust at frame 3, four voices across 40%, a 2.5 s "
                         "attack into a 5 s tail.",
          .a = { .table = kFormantSweep, .position = 5, .unison = 4, .detune = 6, .stereo = 40 },
          .b = { .table = kDust, .position = 3, .level = 0.35 },
          .sub = "Sine",
          .subOctave = -2,
          .subLevel = 0.35,
          .filter1 = { .type = "L6", .cutoff = 0.42 },
          .amp = { .attack = 2.5, .decay = 3.0, .sustain = 0.9, .release = 5.0 } },

        { .name = "Cinder Field",
          .category = "Texture",
          .description = "A saturated ramp at frame 11 with the second frame of Bit Crush under it, a 1 s "
                         "attack and a Moog ladder at 18%.",
          .a = { .table = kGrowlFold, .position = 11 },
          .b = { .table = kBitCrush, .position = 2, .level = 0.45 },
          .filter1 = { .type = "LadderMg", .cutoff = 0.32, .resonance = 18 },
          .amp = { .attack = 1.0, .decay = 2.0, .sustain = 0.85, .release = 3.5 } },

        { .name = "Ghost Choir",
          .category = "Texture",
          .description = "The two-formant stack at frame 11 with odd harmonics at frame 3 beneath, six "
                         "voices across 65% of the image, a 1.8 s attack.",
          .a = { .table = kVocalStack, .position = 11, .unison = 6, .detune = 10, .stereo = 65 },
          .b = { .table = kOddStack, .position = 3, .octave = -1, .level = 0.5 },
          .filter1 = { .type = "L12", .cutoff = 0.5, .resonance = 10 },
          .amp = { .attack = 1.8, .decay = 2.5, .sustain = 0.85, .release = 4.0 } },

        { .name = "Morse",
          .category = "Texture",
          .description = "A narrowing pulse at frame 9 crushed by a quantise warp, with LFO 1 turning the "
                         "oscillator's level over by 60% — a pulse pattern rather than a tone.",
          .a = { .table = kPulseGrit, .position = 9, .warp1 = "kQuantize", .warp1Amount = 0.5 },
          .filter1 = { .type = "L18", .cutoff = 0.5, .resonance = 20 },
          .amp = { .attack = 0.2, .decay = 1.0, .sustain = 0.9, .release = 1.5 },
          .mod = { { .row = 0, .source = "LFO 1", .destinationClass = kOscillatorClass, .instance = 0,
                     .destination = "kParamVolume", .amount = 60, .bipolar = true } } },

        { .name = "Grain Field",
          .category = "Texture",
          .description = "Noise blend at frame 7 over Dust at frame 5, five voices across 55%, a 1.4 s "
                         "attack and a 24 dB low pass at 12%.",
          .a = { .table = kNoiseBlend, .position = 7, .unison = 5, .detune = 13, .stereo = 55 },
          .b = { .table = kDust, .position = 5, .level = 0.45 },
          .noise = "Geiger",
          .noiseLevel = 0.12,
          .filter1 = { .type = "L24", .cutoff = 0.42, .resonance = 12 },
          .amp = { .attack = 1.4, .decay = 2.0, .sustain = 0.88, .release = 3.2 } },

        { .name = "Furnace Room",
          .category = "Texture",
          .description = "A saturated ramp at frame 8 with a noise blend at frame 9 under it, an acid "
                         "ladder at 30% driven 20%, and a 2.2 s attack.",
          .a = { .table = kGrowlFold, .position = 8 },
          .b = { .table = kNoiseBlend, .position = 9, .level = 0.5 },
          .filter1 = { .type = "LadderAcid", .cutoff = 0.3, .resonance = 30, .drive = 20 },
          .amp = { .attack = 2.2, .decay = 2.5, .sustain = 0.85, .release = 4.5 } },

        // ==========================================================================================
        // Percussion
        // ==========================================================================================
        { .name = "Anvil Strike",
          .category = "Percussion",
          .description = "A saturated ramp at frame 5, tube-distorted 40%, a 0.22 s decay to silence "
                         "through a Moog ladder at 25%.",
          .a = { .table = kGrowlFold, .position = 5, .warp1 = "kDistTube", .warp1Amount = 0.4 },
          .filter1 = { .type = "LadderMg", .cutoff = 0.5, .resonance = 25 },
          .amp = { .attack = 0.001, .decay = 0.22, .sustain = 0.0, .release = 0.12 } },

        { .name = "Tin Snare",
          .category = "Percussion",
          .description = "Dust at frame 4 with a square underneath, the NOISE slot's white colour at 40%, "
                         "band passed at 30% with a 0.16 s decay.",
          .a = { .table = kDust, .position = 4 },
          .b = { .table = kSquare, .octave = -1, .level = 0.5 },
          .noise = "White",
          .noiseLevel = 0.4,
          .filter1 = { .type = "BP12", .cutoff = 0.55, .resonance = 30 },
          .amp = { .attack = 0.001, .decay = 0.16, .sustain = 0.0, .release = 0.08 } },

        { .name = "Kettle Iron",
          .category = "Percussion",
          .description = "Half-integer sync at frame 6, ring-modulated 40%, an 18 dB low pass at 35% "
                         "resonance and a 0.3 s decay.",
          .a = { .table = kMetalSync, .position = 6, .warp1 = "kRM_OSC", .warp1Amount = 0.4 },
          .filter1 = { .type = "L18", .cutoff = 0.45, .resonance = 35 },
          .amp = { .attack = 0.001, .decay = 0.3, .sustain = 0.0, .release = 0.15 } },

        { .name = "Wood Block",
          .category = "Percussion",
          .description = "A narrowing pulse at frame 3 with a positive bend warp, a 12 dB low pass at 40% "
                         "resonance and the shortest decay in the bank, 0.12 s.",
          .a = { .table = kPulseGrit, .position = 3, .warp1 = "kBendPos", .warp1Amount = 0.3 },
          .filter1 = { .type = "L12", .cutoff = 0.52, .resonance = 40 },
          .amp = { .attack = 0.001, .decay = 0.12, .sustain = 0.0, .release = 0.06 } },

        { .name = "Thunder Cake",
          .category = "Percussion",
          .description = "A sine two octaves down with Dust at frame 2 under it, a 0.8 s decay to silence "
                         "and a 24 dB low pass at 12%.",
          .a = { .table = kSine, .octave = -2 },
          .b = { .table = kDust, .position = 2, .level = 0.3 },
          .sub = "Sine",
          .subOctave = -2,
          .subLevel = 0.5,
          .filter1 = { .type = "L24", .cutoff = 0.28, .resonance = 12 },
          .amp = { .attack = 0.002, .decay = 0.8, .sustain = 0.0, .release = 0.4 } },

        { .name = "Bass Drop",
          .category = "Percussion",
          .description = "Saw at frame 2 two octaves down with a negative bend warp, a 0.5 s decay to "
                         "silence and a 24 dB low pass at 18%.",
          .a = { .table = kSawFade, .position = 2, .octave = -2,
                 .warp1 = "kBendNeg", .warp1Amount = 0.5 },
          .sub = "kSaw",
          .subOctave = -2,
          .filter1 = { .type = "L24", .cutoff = 0.32, .resonance = 18 },
          .amp = { .attack = 0.001, .decay = 0.5, .sustain = 0.0, .release = 0.2 } },

        { .name = "Marble Strike",
          .category = "Percussion",
          .description = "Glass stack at frame 4, a 0.35 s decay to silence, and a 12 dB high pass at 25% "
                         "resonance.",
          .a = { .table = kGlassStack, .position = 4 },
          .filter1 = { .type = "H12", .cutoff = 0.6, .resonance = 25 },
          .amp = { .attack = 0.001, .decay = 0.35, .sustain = 0.0, .release = 0.2 } },

        { .name = "Temple Bell",
          .category = "Percussion",
          .description = "Phase-modulated sine at frame 18, a 1.6 s decay to silence, and a band pass at "
                         "20% resonance.",
          .a = { .table = kFmMorph, .position = 18 },
          .filter1 = { .type = "BP12", .cutoff = 0.55, .resonance = 20 },
          .amp = { .attack = 0.002, .decay = 1.6, .sustain = 0.0, .release = 1.2 } },

        { .name = "Ratchet",
          .category = "Percussion",
          .description = "Bit Crush at frame 8 over Dust at frame 6, crushed again by a quantise warp at "
                         "40%, an acid ladder at 30%, and a 0.18 s decay.",
          .a = { .table = kBitCrush, .position = 8, .warp1 = "kQuantize", .warp1Amount = 0.4 },
          .b = { .table = kDust, .position = 6, .level = 0.4 },
          .filter1 = { .type = "LadderAcid", .cutoff = 0.48, .resonance = 30 },
          .amp = { .attack = 0.001, .decay = 0.18, .sustain = 0.0, .release = 0.1 } },

        { .name = "Iron Gong",
          .category = "Percussion",
          .description = "A ring-swollen sine at frame 8, soft-clipped by 35%, an EMS ladder left open "
                         "enough to pass the ring's partials, and a 1.4 s decay to silence.",
          .a = { .table = kRingSwell, .position = 8, .warp1 = "kDistSoftClip", .warp1Amount = 0.35 },
          .filter1 = { .type = "LadderEMS", .cutoff = 0.68, .resonance = 20 },
          .amp = { .attack = 0.004, .decay = 1.4, .sustain = 0.0, .release = 0.9 } },
    };

    return recipes;
}

} // namespace

// ------------------------------------------------------------------------------------ the builder, in full

namespace
{

/// Resolve a recipe into the document the product is keyed by, recording anything the engine's own
/// vocabulary does not recognise.
model::PresetDocument build (const PresetRecipe& recipe, std::vector<std::string>& problems)
{
    model::PresetDocument document;
    document.name = recipe.name;
    document.schemaVersion = std::string (model::SupportCatalogue::v1().schemaVersions.back());

    Builder builder (document, problems, document.name);

    // --- global ----------------------------------------------------------------------------------
    const auto global = [] (const char* leaf) { return std::string ("Global0/plainParams/") + leaf; };

    builder.whole (global ("kParamPolyCount"), recipe.polyphony);
    builder.value (global ("kParamPortamentoTime"), recipe.glide);
    builder.value (global ("kParamPortamentoCurve"), 50.0);
    builder.value (global ("kParamMasterVolume"), recipe.level);
    document.parameters[global ("kParamMonoToggle")] = flag (recipe.mono);
    document.parameters[global ("kParamLegato")] = flag (recipe.legato);

    // --- the three wavetable slots ---------------------------------------------------------------
    const auto buildOsc = [&builder, &document] (const int slot, const OscRecipe& osc)
    {
        if (osc.table == nullptr)
            return;

        const auto base = "Oscillator" + std::to_string (slot) + "/plainParams/";
        const auto table = "Oscillator" + std::to_string (slot) + "/WTOsc" + std::to_string (slot) + "/";

        builder.on (base + "kParamEnable");
        builder.value (base + "kParamVolume", osc.level);
        builder.value (base + "kParamOctave", osc.octave);
        builder.value (base + "kParamPitch", osc.pitch);
        builder.value (base + "kParamFine", osc.fine);
        builder.whole (base + "kParamUnison", osc.unison);
        builder.value (base + "kParamUnisonRange", osc.detune);
        builder.value (base + "kParamUnisonStereo", osc.stereo);

        // The table, and the frame the preset reads from it. A one-frame table takes any position.
        document.parameters[table + "relativePathToWT"] = text (osc.table);
        builder.value (table + "plainParams/kParamTablePos", osc.position);

        const auto warp = [&builder, &table, &document] (const int index, const char* mode, const double amount)
        {
            if (mode == nullptr || std::string_view (mode).empty())
                return;

            if (dsp::warpModeFromName (mode) == dsp::WarpMode::Unknown)
            {
                builder.problem (std::string ("warp mode \"") + mode + "\" is not one the engine builds");
                return;
            }

            if (! inReferenceVocabulary ("Oscillator{N}/WTOsc{N}/plainParams/kParamWarpMenu", mode))
            {
                builder.problem (std::string ("warp mode \"") + mode + "\" is not one of the warp names the "
                                 "reference's own vocabulary carries, and xerum's choice list is built from "
                                 "that vocabulary");
                return;
            }

            const auto menu = "kParamWarpMenu" + std::string (index == 0 ? "" : std::to_string (index + 1));
            const auto depth = "kParamWarp" + std::string (index == 0 ? "" : std::to_string (index + 1));

            document.parameters[table + "plainParams/" + menu] = text (mode);
            builder.value (table + "plainParams/" + depth, amount);
        };

        warp (0, osc.warp1, osc.warp1Amount);
        warp (1, osc.warp2, osc.warp2Amount);
    };

    buildOsc (0, recipe.a);
    buildOsc (1, recipe.b);
    buildOsc (2, recipe.c);

    // --- NOISE -----------------------------------------------------------------------------------
    if (recipe.noise != nullptr)
    {
        if (! dsp::noiseTypeFromName (recipe.noise).has_value())
            builder.problem (std::string ("noise colour \"") + recipe.noise + "\" is not one the engine has");

        builder.on ("Oscillator3/plainParams/kParamEnable");
        builder.value ("Oscillator3/plainParams/kParamVolume", recipe.noiseLevel);
        document.parameters["Oscillator3/NoiseOsc3/plainParams/kParamNoiseType"] = text (recipe.noise);
        builder.value ("Oscillator3/NoiseOsc3/plainParams/kParamRandomPhase", recipe.noiseRandomPhase ? 100.0 : 0.0);
    }

    // --- SUB -------------------------------------------------------------------------------------
    if (recipe.sub != nullptr)
    {
        if (! dsp::subShapeFromName (recipe.sub).has_value())
            builder.problem (std::string ("sub shape \"") + recipe.sub + "\" is not one the engine has");

        builder.on ("Oscillator4/plainParams/kParamEnable");
        builder.value ("Oscillator4/plainParams/kParamVolume", recipe.subLevel);
        builder.value ("Oscillator4/plainParams/kParamOctave", recipe.subOctave);
        builder.value ("Oscillator4/plainParams/kParamFine", recipe.subFine);
        document.parameters["Oscillator4/SubOsc4/plainParams/kParamShape"] = text (recipe.sub);
    }

    // --- the voice filters -----------------------------------------------------------------------
    const auto buildFilter = [&builder] (const int slot, const FilterRecipe& filter)
    {
        if (filter.type == nullptr)
            return;

        const auto base = "VoiceFilter" + std::to_string (slot) + "/plainParams/";

        if (dsp::filterSpecFromName (filter.type).model == dsp::FilterModel::Unimplemented)
            builder.problem (std::string ("filter type \"") + filter.type + "\" is not one the engine builds");

        if (! inReferenceVocabulary ("VoiceFilter{N}/plainParams/kParamType", filter.type))
            builder.problem (std::string ("filter type \"") + filter.type + "\" is not one of the names the "
                             "reference's own filter vocabulary carries, and xerum's choice list is built "
                             "from that vocabulary");

        builder.on (base + "kParamEnable");
        builder.token (base + "kParamType", filter.type);
        builder.value (base + "kParamFreq", filter.cutoff);
        builder.value (base + "kParamReso", filter.resonance);
        builder.value (base + "kParamDrive", filter.drive);
        builder.value (base + "kParamVar", filter.variation);
    };

    buildFilter (0, recipe.filter1);
    buildFilter (1, recipe.filter2);

    // --- the amplitude envelope ------------------------------------------------------------------
    const auto amp = [] (const char* leaf) { return std::string ("Env0/plainParams/") + leaf; };

    builder.value (amp ("kParamAttack"), recipe.amp.attack);
    builder.value (amp ("kParamDecay"), recipe.amp.decay);
    builder.value (amp ("kParamSustain"), recipe.amp.sustain);
    builder.value (amp ("kParamRelease"), recipe.amp.release);

    // --- the modulation rows ---------------------------------------------------------------------
    for (const auto& row : recipe.mod)
    {
        const auto source = dsp::modSourceFromName (row.source);

        if (source == 0)
        {
            builder.problem (std::string ("modulation source \"") + std::string (row.source)
                             + "\" is not one the reference names");
            continue;
        }

        if (! dsp::modSourceIsImplemented (source))
            builder.problem (std::string ("modulation source \"") + std::string (row.source)
                             + "\" is named and not evaluated by v1");

        if (! dsp::modDestinationClassKnown (row.destinationClass))
        {
            builder.problem (std::string ("modulation destination class \"")
                             + std::string (row.destinationClass) + "\" is not one the reference addresses");
            continue;
        }

        if (! dsp::modDestinationInstanceKnown (row.destinationClass, row.instance))
            builder.problem (std::string ("modulation destination instance ") + std::to_string (row.instance)
                             + " is out of range for " + std::string (row.destinationClass));

        const auto parameter = destinationId (row.destinationClass, row.destination);

        if (parameter < 0)
        {
            builder.problem (std::string ("modulation destination \"") + std::string (row.destinationClass)
                             + "." + std::string (row.destination) + "\" is not one the voice reads");
            continue;
        }

        const auto prefix = "ModSlot" + std::to_string (row.row) + "/";

        builder.whole (prefix + "source[0]", source);
        builder.whole (prefix + "source[1]", 0);
        builder.value (prefix + "plainParams/kParamAmount", row.amount);
        builder.value (prefix + "plainParams/kParamBipolar", row.bipolar ? 1.0 : 0.0);
        builder.token (prefix + "destModuleTypeString", row.destinationClass);
        builder.whole (prefix + "destModuleID", row.instance);
        builder.whole (prefix + "destModuleParamID", parameter);

        // The row's **destination name**. Not decoration: `ModulationMatrix::setSlots` drops a row whose
        // destination has no name ("the row names no destination parameter"), because the name is the only
        // stable identity a destination has across schema versions. Without it the row is inert while the
        // preset claims it is wired.
        builder.token (prefix + "destModuleParamName", row.destination);
    }

    // The document's own list of what it needs, so a preset says out loud which tables it plays.
    for (const auto& [path, value] : document.parameters)
        if (path.ends_with ("/relativePathToWT") && value.isText())
            document.assetReferences.push_back (value.text);

    return document;
}

/// Built once. The problems are collected as the bank is built, so a wrong token anywhere is a fact the
/// tool and the tests can read rather than a silence nobody notices.
struct BuiltBank
{
    std::vector<FactoryPreset> presets;
    std::vector<std::string> problems;
};

const BuiltBank& built()
{
    static const BuiltBank bank = []
    {
        BuiltBank out;
        std::set<std::string> names;
        std::set<std::string> paths;

        for (const auto& recipe : recipes())
        {
            if (recipe.name == nullptr || *recipe.name == '\0')
            {
                out.problems.push_back ("a preset has no name");
                continue;
            }

            const std::string_view category = recipe.category == nullptr ? std::string_view() : recipe.category;

            if (! std::count (categories().begin(), categories().end(), category))
                out.problems.push_back (std::string (recipe.name) + ": category \"" + recipe.category
                                        + "\" is not one of the eight the bank browses by");

            if (! names.insert (recipe.name).second)
                out.problems.push_back (std::string (recipe.name) + ": the name is used twice");

            FactoryPreset preset;
            preset.name = recipe.name;
            preset.category = recipe.category == nullptr ? std::string() : recipe.category;
            preset.description = recipe.description == nullptr ? std::string() : recipe.description;
            preset.document = build (recipe, out.problems);

            if (preset.document.name != preset.name)
                preset.document.name = preset.name;

            const auto path = presetFilePath (preset);

            if (! paths.insert (path).second)
                out.problems.push_back (preset.name + ": two presets would be written to " + path);

            if (preset.description.empty())
                out.problems.push_back (preset.name + ": no description of what it is made of");

            out.presets.push_back (std::move (preset));
        }

        return out;
    }();

    return bank;
}

} // namespace

const std::vector<std::string_view>& categories()
{
    static const std::vector<std::string_view> list { kBass, kLead, kPad, kKeys, kPluck, kFx, kTexture, kPercussion };
    return list;
}

const std::vector<FactoryPreset>& bank()
{
    return built().presets;
}

std::vector<std::string> bankProblems()
{
    return built().problems;
}

std::string presetFilePath (const FactoryPreset& preset)
{
    return std::string (kBankDirectory) + "/" + preset.category + "/" + preset.name
           + std::string (kPresetExtension);
}

std::vector<std::byte> encodePresetFile (const FactoryPreset& preset)
{
    // An empty report, deliberately: the bank is authored against the engine's cut, so a preset that had
    // anything to report would be a preset asking for something the product does not do.
    return model::encodePresetBlock (preset.document, {}, 0, model::kPresetBlockMagic,
                                     model::kPresetBlockFormat);
}

std::vector<std::string> tablesOf (const FactoryPreset& preset)
{
    std::set<std::string> tables;

    for (const auto& [path, value] : preset.document.parameters)
        if (path.ends_with ("/relativePathToWT") && value.isText())
            tables.insert (value.text);

    return { tables.begin(), tables.end() };
}

//=======================================================================================
// The manifest (ticket 55)

BankManifest readBankManifest (const std::string_view json)
{
    BankManifest out;

    const auto parsed = model::decodeJson (json);

    if (! parsed.ok)
    {
        out.error = "the bank manifest is not readable JSON: " + parsed.error;
        return out;
    }

    const auto* categories = parsed.value.find ("categories");

    if (categories == nullptr || categories->type != model::JsonValue::Type::Array)
    {
        out.error = "the bank manifest names no categories";
        return out;
    }

    for (const auto& category : categories->items)
    {
        if (! category.isString() || category.text.empty())
        {
            out.error = "the bank manifest has a category that is not a name";
            return out;
        }

        out.categories.push_back (category.text);
    }

    if (out.categories.empty())
    {
        out.error = "the bank manifest names no categories";
        return out;
    }

    // The provenance, shown as such rather than invented: a browser says where its bank came from.
    out.origin = parsed.value.stringOr ("origin");
    out.licence = parsed.value.stringOr ("licence");

    const auto* presets = parsed.value.find ("presets");

    if (presets == nullptr || presets->type != model::JsonValue::Type::Array)
    {
        out.error = "the bank manifest has no presets list";
        return out;
    }

    // Built into a local list and handed over only once the whole manifest has been accepted: a reader
    // that fails hands back an **error and nothing else**, never a half-built catalogue that a caller
    // could browse by mistake.
    std::vector<BankManifestPreset> parsedPresets;

    for (const auto& entry : presets->items)
    {
        BankManifestPreset preset;
        preset.name = entry.stringOr ("name");
        preset.category = entry.stringOr ("category");
        preset.path = entry.stringOr ("path");
        preset.description = entry.stringOr ("description");
        preset.sha256 = entry.stringOr ("sha256");

        if (const auto* bytes = entry.find ("bytes"); bytes != nullptr && bytes->isNumber())
            preset.bytes = static_cast<std::size_t> (bytes->number);

        // A preset the browser cannot name, locate, or file under a category it knows is a preset the
        // browser must not offer: an entry it cannot show is worse than an entry it does not have.
        if (preset.name.empty() || preset.path.empty())
        {
            out.error = "the bank manifest has a preset with no name or no path";
            return out;
        }

        if (std::find (out.categories.begin(), out.categories.end(), preset.category) == out.categories.end())
        {
            out.error = "the bank manifest files " + preset.name + " under \"" + preset.category
                        + "\", which is not one of its categories";
            return out;
        }

        parsedPresets.push_back (std::move (preset));
    }

    if (parsedPresets.empty())
    {
        out.error = "the bank manifest lists no presets";
        return out;
    }

    out.presets = std::move (parsedPresets);
    out.ok = true;
    return out;
}

std::string manifestPathUnder (const std::string_view root, const BankManifestPreset& preset)
{
    if (root.empty())
        return preset.path;

    return (std::filesystem::path (root) / preset.path).lexically_normal().string();
}

} // namespace xerum::presets
