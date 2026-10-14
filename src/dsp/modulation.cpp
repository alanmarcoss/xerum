// xerum::dsp — the modulation matrix.
//
// What this file is, and what it is not, in one paragraph: it is the **bus and curve arithmetic** — how
// a row's source, depth, polarity, curve and aux source combine, how several rows sum onto one
// destination, and how that sum is presented as a ramp across a block. It is not a modulator: envelopes
// and LFOs do not live here, they arrive as values in `ModulationInputs`, which is what makes the whole
// evaluator a pure function of its inputs and what makes moving to per-sample later a call-site change.
//
// **`mod_sources.h` is where those values are generated** (ticket `48`): ten LFOs, four matrix envelopes
// and the voice's own samples. Before it, thirty-seven of this file's fifty-seven source cases read a
// field nothing in `src/dsp` or `plugin/Source` ever assigned — so an editable matrix read a constant
// zero, which is worse than no matrix because it looks like it works.
//
// Three of its facts come from measurement rather than from the manual, and each one is load-bearing:
//
//   * **the source names and their count** — 60, 1-based, verbatim from the reference's own string
//     table (ticket `03` §3, recovered from the 2.1.5 binary). Value 0 is unused and no preset stores it.
//   * **the destination vocabulary** — the reference addresses a destination by
//     `(class string, instance id, param id)` with **FX instances encoding `rackIndex * 100 + slotIndex`**
//     (ticket `03` §4). The vocabulary below is the corpus's own, so a class outside it is reported
//     rather than silently written somewhere harmless.
//   * **`destModuleParamID` is version-dependent** — `Oscillator[10]` is `kParamDetune` at schema 3.0
//     and `kParamStart` at 4.0; `WTOsc[1]` is `kParamTablePos` at 3.0 and `kParamWarpVar` at 4.0. So
//     resolution and the discrete/continuous decision key on the **name**, which is stable, and never on
//     the id, which is not.
//
// On the conventions: the reference's exact curve and polarity curves can only be checked with the
// fidelity suite (tickets `09`/`30`), so where a mapping is a stated convention rather than a
// measurement it says so, and the tests assert the properties that hold regardless of it — a depth of
// 100 contributes one full range whatever the curve turns out to be, and a bipolar row's centre is the
// source's midpoint whatever the scale.

#include "modulation.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace xerum::dsp
{

namespace
{

// ---------------------------------------------------------------------------------------------------
// The source table. Verbatim from the reference's compiled string table (ticket `03` §3) — including
// the two clipped spellings (`Poly Aftertch`, `NoteOn Alt.`), because a name that does not match the
// reference's own is a name a user cannot find in the manual.
// ---------------------------------------------------------------------------------------------------

struct SourceName
{
    int value;
    std::string_view name;
};

constexpr SourceName kSourceNames[] = {
    { 1, "Mod Wheel" },
    { 2, "Env 1" },
    { 3, "Env 2" },
    { 4, "Env 3" },
    { 5, "Env 4" },
    { 6, "LFO 1" },
    { 7, "LFO 2" },
    { 8, "LFO 3" },
    { 9, "LFO 4" },
    { 10, "LFO 5" },
    { 11, "LFO 6" },
    { 12, "LFO 7" },
    { 13, "LFO 8" },
    { 14, "LFO 9" },
    { 15, "LFO 10" },
    { 16, "Velo" },
    { 17, "Note#" },
    { 18, "Aftertouch" },
    { 19, "Poly Aftertch" },
    { 20, "Noise OSC" },
    { 21, "NoteOn Rand1" },
    { 22, "NoteOn Rand2" },
    { 23, "NoteOn Alt." },
    { 24, "NoteOn Alt.2" },
    { 25, "Macro 1" },
    { 26, "Macro 2" },
    { 27, "Macro 3" },
    { 28, "Macro 4" },
    { 29, "Macro 5" },
    { 30, "Macro 6" },
    { 31, "Macro 7" },
    { 32, "Macro 8" },
    { 33, "Pitch Bend" },
    { 34, "Expr X (Pan)" },
    { 35, "Expr Y (Timbre)" },
    { 36, "Expr Z (Press.)" },
    { 37, "Release Velo" },
    { 38, "Fixed" },
    { 39, "LFO 1 Y" },
    { 40, "LFO 2 Y" },
    { 41, "LFO 3 Y" },
    { 42, "LFO 4 Y" },
    { 43, "LFO 5 Y" },
    { 44, "LFO 6 Y" },
    { 45, "LFO 7 Y" },
    { 46, "LFO 8 Y" },
    { 47, "LFO 9 Y" },
    { 48, "LFO 10 Y" },
    { 49, "OSC A" },
    { 50, "OSC B" },
    { 51, "OSC C" },
    { 52, "SUB OSC" },
    { 53, "Filter 1" },
    { 54, "Filter 2" },
    { 55, "Active Voices" },
    { 56, "Voice Mod 1" },
    { 57, "Voice Mod 2" },
    { 58, "Voice Index" },
    { 59, "NoteOn Rand (Discrete)" },
    { 60, "ModSlot" },
};

static_assert (std::size (kSourceNames) == kReferenceSourceCount,
               "the source table is the reference's own 60 entries");

// ---------------------------------------------------------------------------------------------------
// The destination vocabulary.
// ---------------------------------------------------------------------------------------------------

struct DestinationClass
{
    std::string_view name;
    int minimumInstance;
    int maximumInstance;
    bool rackEncoded;      ///< FX classes: the instance is `rackIndex * 100 + slotIndex`
};

/// Ticket `03` §4's corpus-wide vocabulary, extended with `FXSplitMS` — the one effect class that is in
/// the reference's list but is never a matrix destination in the factory bank, which is a reason to
/// resolve it and not a reason to leave it unknown. `min`/`max` are the reference's own instance counts
/// (`Env0..3` is four envelopes, `LFO0..9` ten LFOs), confirmed against every destination in the corpus.
constexpr DestinationClass kDestinationClasses[] = {
    { "Oscillator", 0, 4, false },
    { "VoiceFilter", 0, 1, false },
    { "WTOsc", 0, 2, false },
    { "SampleOsc", 0, 2, false },
    { "MultiSampleOsc", 0, 2, false },
    { "GranularOsc", 0, 2, false },
    { "SpectralOsc", 0, 2, false },
    { "NoiseOsc", 3, 3, false },
    { "Env", 0, 3, false },
    { "LFO", 0, 9, false },
    { "Macro", 0, 7, false },
    { "RoutingSlot", 0, 6, false },
    { "LFOPointModBus", 0, 15, false },
    { "Global", 0, 0, false },
    { "Arp", 0, 0, false },
    { "MidiClip", 0, 1, false },
    { "VoicePanel", 0, 0, false },
    { "RetriggerState", 0, 0, false },
    { "ModSlot", 0, 63, false },
    { "FXEQ", 0, 0, true },
    { "FXComp", 0, 0, true },
    { "FXReverb", 0, 0, true },
    { "FXDelay", 0, 0, true },
    { "FXDistortion", 0, 0, true },
    { "FXFilter", 0, 0, true },
    { "FXConv", 0, 0, true },
    { "FXUtils", 0, 0, true },
    { "FXChorus", 0, 0, true },
    { "FXHyperD", 0, 0, true },
    { "FXBode", 0, 0, true },
    { "FXPhaser", 0, 0, true },
    { "FXFlanger", 0, 0, true },
    { "FXSplit", 0, 0, true },
    { "FXSplit3", 0, 0, true },
    { "FXSplitMS", 0, 0, true },
};

/// Prohibited from being a parameter, by the same reasoning as the source table: these are the names
/// the reference **types as an enum** rather than as a number, over every destination class's parameter
/// set. Derived from the parameter model itself (a leaf whose kind is not `float`), and held to it by
/// `modulation_coverage_test.cpp`, so a class that grows an enum parameter fails the build's tests
/// rather than silently ramping a string.
///
/// The four `kParam*Shape`/`StepAction`/`RangeWrapMode` names arrive through `Arp`: an `Arp` destination
/// names the arp's parameters, and the parameter model stores exactly that set under `ArpClip{N}`.
constexpr std::string_view kDiscreteParameterNames[] = {
    "kParamDensityMode",  "kParamDetuneMode",    "kParamLengthMode",     "kParamLoopMode",
    "kParamMidiOut",      "kParamNoiseType",     "kParamPhaseMemory",    "kParamPitchMode",
    "kParamPitchRatioModMode", "kParamPlaybackMode", "kParamRangeWrapMode", "kParamRoutingDest",
    "kParamShape",        "kParamSlicingEnabled", "kParamSpanKeyboardMode", "kParamStepAction",
    "kParamTransposeShape", "kParamType",        "kParamUnisonStack",    "kParamUnisonTrigPattern",
    "kParamVoicePriority", "kParamWarpMenu",     "kParamWarpMenu2",      "kParamWindowShape",
    "kParamYAxisAssignment",
};

/// `kParamMode` is the one name the reference types two ways: an enum on `LFO` and `FXDistortion`, and
/// a number on `FXDelay` and `FXReverb`. A flat name set would have to be wrong about two of them.
bool isModeEnum (std::string_view className)
{
    return className == "LFO" || className == "FXDistortion";
}

const DestinationClass* findClass (std::string_view className)
{
    for (const auto& candidate : kDestinationClasses)
        if (candidate.name == className)
            return &candidate;

    return nullptr;
}

// ---------------------------------------------------------------------------------------------------
// The arithmetic. Both helpers below are stated conventions, not measurements — see the file header.
// ---------------------------------------------------------------------------------------------------

/// The reference's CRV/OUT curve control is a percent that bends the response, and the manual puts its
/// linear setting at the middle of the UI range — so the reference's own raw zero is *a* neutral. The
/// contract declares `1.0` instead ("a straight line until the curve is decoded"), and a declaration
/// about a field's neutral wins over a reconstruction of the reference's raw unit: it keeps every row
/// that does not set a curve exactly linear, which is 12,900 of the corpus's 13,258 rows, at the cost of
/// a 1% offset in the bend of the 277 that do. `[INFERENCE]` on the shape: the drawn curve lives in the
/// row's `flex` payload, which the `ModSlot` contract does not carry, so the transfer here (a power law,
/// positive = convex) is a stated convention rather than a measurement.
constexpr double kCurveNeutral = 1.0;

double bend (double x, double curve)
{
    if (x == 0.0)
        return x;

    const auto t = std::clamp ((curve - kCurveNeutral) / 100.0, -1.0, 1.0);

    if (t == 0.0)
        return x;

    return std::copysign (std::pow (std::fabs (x), std::exp2 (-t)), x);
}

/// The output of one source, on the 0..1 range `ModulationInputs` documents. Value 0, and any value this
/// build does not evaluate, is 0 — which is also why a row whose source is unimplemented is never
/// evaluated at all rather than evaluated as zero.
double sourceOutput (int value, const ModulationInputs& in)
{
    switch (value)
    {
        case 1:  return in.modWheel;
        case 2:  return in.envelope[0];
        case 3:  return in.envelope[1];
        case 4:  return in.envelope[2];
        case 5:  return in.envelope[3];
        case 6:  return in.lfo[0];
        case 7:  return in.lfo[1];
        case 8:  return in.lfo[2];
        case 9:  return in.lfo[3];
        case 10: return in.lfo[4];
        case 11: return in.lfo[5];
        case 12: return in.lfo[6];
        case 13: return in.lfo[7];
        case 14: return in.lfo[8];
        case 15: return in.lfo[9];
        case 16: return in.velocity;
        case 17: return in.noteNumber;
        case 18: return in.aftertouch;
        case 19: return in.polyAftertouch;
        case 20: return in.noiseOsc;
        case 21: return in.noteOnRandom;
        case 23: return in.noteOnAlternate;
        case 24: return 1.0 - in.noteOnAlternate;   ///< the manual: "Alt 2 is the inverse of Alt 1"
        case 25: return in.macro[0];
        case 26: return in.macro[1];
        case 27: return in.macro[2];
        case 28: return in.macro[3];
        case 29: return in.macro[4];
        case 30: return in.macro[5];
        case 31: return in.macro[6];
        case 32: return in.macro[7];
        case 33: return in.pitchBend;
        case 34: return in.expressionX;
        case 35: return in.expressionY;
        case 36: return in.expressionZ;
        case 37: return in.releaseVelocity;
        case 38: return in.fixed;
        case 39: return in.lfoY[0];
        case 40: return in.lfoY[1];
        case 41: return in.lfoY[2];
        case 42: return in.lfoY[3];
        case 43: return in.lfoY[4];
        case 44: return in.lfoY[5];
        case 45: return in.lfoY[6];
        case 46: return in.lfoY[7];
        case 47: return in.lfoY[8];
        case 48: return in.lfoY[9];
        case 49: return in.osc[0];
        case 50: return in.osc[1];
        case 51: return in.osc[2];
        case 52: return in.subOsc;
        case 53: return in.voiceFilter[0];
        case 54: return in.voiceFilter[1];
        case 55: return in.activeVoices;
        case 56: return in.voiceMod1;
        case 57: return in.voiceMod2;
        case 58: return in.voiceIndex;
        case 59: return in.noteOnRandomDiscrete;
        default: return 0.0;   // 0, 22 (NoteOn Rand2) and 60 (ModSlot), the two reported as unimplemented
    }
}

} // namespace

std::string_view modSourceName (int referenceValue)
{
    for (const auto& entry : kSourceNames)
        if (entry.value == referenceValue)
            return entry.name;

    return {};
}

int modSourceFromName (std::string_view name)
{
    for (const auto& entry : kSourceNames)
        if (entry.name == name)
            return entry.value;

    return 0;
}

bool modSourceIsImplemented (int referenceValue)
{
    switch (referenceValue)
    {
        case 22:   // NoteOn Rand2 — the contract carries one random, and this is the second
        case 60:   // ModSlot — another row's output is a graph edge, not a value in ModulationInputs
            return false;
        default:
            return referenceValue >= 1 && referenceValue <= kReferenceSourceCount;
    }
}

bool modDestinationClassKnown (std::string_view className)
{
    return findClass (className) != nullptr;
}

bool modDestinationInstanceKnown (std::string_view className, int instance)
{
    const auto* found = findClass (className);

    if (found == nullptr || instance < 0)
        return false;

    if (! found->rackEncoded)
        return instance >= found->minimumInstance && instance <= found->maximumInstance;

    // The reference has three racks and encodes the slot in the lower two decimal digits, so this is a
    // check on the encoding. How deep a rack actually is the preset's `FXRack{r}/FX` array — the corpus's
    // racks hold up to 22 effects, and every one of its ~3,600 FX destination rows fits inside its own
    // rack's array, which is the check `modulation_coverage_test.cpp` makes with the document in hand.
    const auto rack = instance / 100;
    const auto slot = instance % 100;

    return rack <= 2 && slot <= 99;
}

bool modDestinationIsDiscrete (std::string_view className, std::string_view parameterName)
{
    for (const auto name : kDiscreteParameterNames)
        if (name == parameterName)
            return true;

    if (parameterName == "kParamMode")
        return isModeEnum (className);

    // An unnamed parameter cannot be classified, so it is treated as continuous: ramping a number we
    // cannot see is a smaller lie than freezing a quantity that should move.
    return false;
}

std::string modDestinationUnresolvedReason (const ModSlot& slot)
{
    if (slot.destinationClass.empty() || slot.destinationClass == "None")
        return "the row has no destination";   // 6 rows in `SY - Tetris`, and ticket 03's `None` sentinel

    if (! modDestinationClassKnown (slot.destinationClass))
        return "destination class '" + slot.destinationClass + "' is not one the reference uses";

    if (! modDestinationInstanceKnown (slot.destinationClass, slot.destinationInstance))
        return "instance " + std::to_string (slot.destinationInstance) + " is out of range for "
               + slot.destinationClass;

    if (slot.destinationName.empty())
        return "the row names no destination parameter";

    return {};
}

double ModulatedRamp::at (int frame, int frames) const
{
    // A discrete destination has nothing between the block boundaries, so it is the block's value.
    if (discrete || frames <= 0 || frame >= frames)
        return end;

    if (frame <= 0)
        return start;

    // The anchors are the block boundaries: `start` is where the previous block ended, `end` is where
    // this one ends, so block N's frame 0 is block N-1's frame `frames` and there is no step at the join.
    return start + (end - start) * (static_cast<double> (frame) / static_cast<double> (frames));
}

void ModulationMatrix::prepare (double sampleRate, int maximumBlockFrames)
{
    // Nothing here depends on the block length: `rampFor` hands back an anchor pair and the caller says
    // how many frames it is spreading them over, so a block longer than the ceiling cannot be truncated
    // by accident. Both values are recorded because the per-sample upgrade in ticket `13` will need them.
    sampleRate_ = sampleRate;
    maximumBlockFrames_ = maximumBlockFrames;
}

void ModulationMatrix::setSlots (std::vector<ModSlot> slots)
{
    // Called when a preset loads, never from the audio thread: this is where every allocation happens,
    // which is what lets `beginBlock` be allocation-free.
    slots_ = std::move (slots);

    rows_.clear();
    destinations_.clear();
    destinationClasses_.clear();
    unimplementedInUse_.clear();

    for (const auto& slot : slots_)
    {
        if (! slot.used || slot.bypassed)
            continue;

        // A row whose destination does not resolve is reported by the caller (through
        // `modDestinationUnresolvedReason`) and contributes nothing. Folding it onto some nearby
        // parameter would be the silent substitution the import contract forbids.
        if (! modDestinationUnresolvedReason (slot).empty())
            continue;

        // A source this build does not evaluate, main or aux, makes the whole row unevaluated: the
        // aux source *scales* the main one, so inventing a value for it would be inventing the sound.
        // The values are collected so the caller can report them by name.
        const bool sourceShipped = modSourceIsImplemented (slot.source);
        const bool auxShipped = slot.auxSource == 0 || modSourceIsImplemented (slot.auxSource);

        if (! sourceShipped || ! auxShipped)
        {
            if (! sourceShipped)
                unimplementedInUse_.push_back (slot.source);

            if (! auxShipped)
                unimplementedInUse_.push_back (slot.auxSource);

            continue;
        }

        int classIndex = -1;

        for (std::size_t i = 0; i < destinationClasses_.size(); ++i)
            if (destinationClasses_[i] == slot.destinationClass)
            {
                classIndex = static_cast<int> (i);
                break;
            }

        if (classIndex < 0)
        {
            classIndex = static_cast<int> (destinationClasses_.size());
            destinationClasses_.push_back (slot.destinationClass);
        }

        int destination = -1;

        for (std::size_t i = 0; i < destinations_.size(); ++i)
            if (destinations_[i].classIndex == classIndex
                && destinations_[i].instance == slot.destinationInstance
                && destinations_[i].parameter == slot.destinationParameter)
            {
                destination = static_cast<int> (i);
                break;
            }

        if (destination < 0)
        {
            Destination entry;
            entry.classIndex = classIndex;
            entry.instance = slot.destinationInstance;
            entry.parameter = slot.destinationParameter;
            entry.discrete = modDestinationIsDiscrete (slot.destinationClass, slot.destinationName);
            destination = static_cast<int> (destinations_.size());
            destinations_.push_back (entry);
        }

        Row row;
        row.source = slot.source;
        row.auxSource = slot.auxSource;
        row.depth = slot.amount / 100.0;
        row.bipolar = slot.bipolar;
        row.curveIn = slot.curveIn;
        row.curveOut = slot.curveOut;
        row.destination = destination;
        rows_.push_back (row);
    }

    std::sort (unimplementedInUse_.begin(), unimplementedInUse_.end());
    unimplementedInUse_.erase (std::unique (unimplementedInUse_.begin(), unimplementedInUse_.end()),
                              unimplementedInUse_.end());

    blockSums_.assign (destinations_.size(), 0.0);
    blockWritten_.assign (destinations_.size(), 0);
}

void ModulationMatrix::beginBlock (const ModulationInputs& inputs, int frames)
{
    if (frames <= 0)
        return;

    std::fill (blockSums_.begin(), blockSums_.end(), 0.0);
    std::fill (blockWritten_.begin(), blockWritten_.end(), 0);

    std::array<double, kReferenceSourceCount + 1> values {};

    for (int value = 1; value <= kReferenceSourceCount; ++value)
        values[static_cast<std::size_t> (value)] = sourceOutput (value, inputs);

    for (const auto& row : rows_)
    {
        // The row, in the order the manual lists it: the source is shaped by CRV, made uni- or
        // bi-directional by POL, scaled by the aux source (the manual: "the two modulation sources are
        // multiplied together so that one is scaling the other"), given its depth — whose **sign is the
        // polarity** — and finally shaped by the OUT curve.
        auto shaped = bend (values[static_cast<std::size_t> (row.source)], row.curveIn);

        if (row.bipolar)
            shaped = 2.0 * shaped - 1.0;

        const auto aux = row.auxSource != 0 ? values[static_cast<std::size_t> (row.auxSource)] : 1.0;
        const auto contribution = bend (shaped * aux * row.depth, row.curveOut);

        const auto index = static_cast<std::size_t> (row.destination);

        blockSums_[index] += contribution;
        blockWritten_[index] = 1;
    }

    for (std::size_t i = 0; i < destinations_.size(); ++i)
    {
        auto& destination = destinations_[i];

        if (destination.discrete)
        {
            // Nothing to interpolate, so there is no ramp to describe: start and end are the same value,
            // which means a caller that spreads `start..end` itself still gets the held value.
            destination.previous = blockSums_[i];
            destination.current = blockSums_[i];
            destination.hasPrevious = true;
            continue;
        }

        // The first block after `setSlots` anchors at its own value: ramping up from nothing would be a
        // fade-in the preset never asked for. After that the anchor is the previous block's value, which
        // is what makes the ramp continuous across the join.
        destination.previous = destination.hasPrevious ? destination.current : blockSums_[i];
        destination.current = blockSums_[i];
        destination.hasPrevious = true;
    }
}

ModulatedRamp ModulationMatrix::rampFor (std::string_view destinationClass, int instance,
                                        int parameter) const
{
    for (const auto& destination : destinations_)
        if (destination.instance == instance && destination.parameter == parameter
            && destinationClasses_[static_cast<std::size_t> (destination.classIndex)] == destinationClass)
        {
            ModulatedRamp ramp;
            ramp.start = destination.previous;
            ramp.end = destination.current;
            ramp.discrete = destination.discrete;
            return ramp;
        }

    return {};
}

std::size_t ModulationMatrix::modulatedDestinationCount() const
{
    return static_cast<std::size_t> (std::count (blockWritten_.begin(), blockWritten_.end(),
                                                static_cast<unsigned char> (1)));
}

std::vector<int> ModulationMatrix::unimplementedSourcesInUse() const
{
    return unimplementedInUse_;
}

} // namespace xerum::dsp
