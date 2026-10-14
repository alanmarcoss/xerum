// The modulation matrix's arithmetic.
//
// These tests measure the four things the value of a modulation row is made of — depth, the polarity of
// the depth, the polarity switch, and the curve — plus the two presentation properties that ticket `13`
// chose for the whole engine: a **continuous** destination arrives as a ramp that does not step at a
// block boundary, and a **discrete** one arrives as one value for the block.
//
// The two properties worth stating because a weaker test would pass without them:
//
//   * "it interpolates" is checked by *sampling consecutive frames across a block join* and bounding the
//     frame-to-frame step. The naive implementation — hold the block's value — passes any test that only
//     looks inside a block, and fails this one by a factor of the block length.
//   * "discrete is held" is checked with a *continuous* destination driven identically beside it in the
//     same matrix, so the test cannot pass by having nothing to compare against.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "modulation.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace xerum::dsp;
using Catch::Approx;

namespace
{

constexpr int kFrames = 64;

ModSlot slot (int source, std::string destinationClass, int instance, int parameter,
              std::string parameterName = "kParamFreq")
{
    ModSlot s;
    s.used = true;
    s.source = source;
    s.destinationClass = std::move (destinationClass);
    s.destinationInstance = instance;
    s.destinationParameter = parameter;
    s.destinationName = std::move (parameterName);
    return s;
}

/// The settled value of a destination: two blocks with the same inputs, so `start` has caught up to
/// `end` and the reading is not a ramp in progress.
double settled (ModulationMatrix& matrix, const ModulationInputs& inputs,
                std::string_view destinationClass, int instance, int parameter)
{
    matrix.beginBlock (inputs, kFrames);
    matrix.beginBlock (inputs, kFrames);

    return matrix.rampFor (destinationClass, instance, parameter).end;
}

/// The reference's own fixed source: a row with it and a depth of 100 contributes one full range, which
/// is what makes it the right probe for everything else.
constexpr int kFixed = 38;
constexpr int kModWheel = 1;

} // namespace

TEST_CASE ("the source table is the reference's 60, 1-based, and named verbatim", "[modulation]")
{
    REQUIRE (kReferenceSourceCount == 60);

    CHECK (modSourceName (0).empty());
    CHECK (modSourceName (61).empty());
    CHECK (modSourceName (-1).empty());

    CHECK (modSourceName (1) == "Mod Wheel");
    CHECK (modSourceName (60) == "ModSlot");

    // The spellings the reference clipped, kept clipped: a name we improve on is a name a user cannot
    // find in the manual or in the reference's own menu.
    CHECK (modSourceName (19) == "Poly Aftertch");
    CHECK (modSourceName (23) == "NoteOn Alt.");
    CHECK (modSourceName (24) == "NoteOn Alt.2");
    CHECK (modSourceName (34) == "Expr X (Pan)");
    CHECK (modSourceName (36) == "Expr Z (Press.)");
    CHECK (modSourceName (47) == "LFO 9 Y");
    CHECK (modSourceName (52) == "SUB OSC");
    CHECK (modSourceName (59) == "NoteOn Rand (Discrete)");

    std::size_t named = 0;

    for (int value = 1; value <= kReferenceSourceCount; ++value)
    {
        const auto name = modSourceName (value);

        REQUIRE_FALSE (name.empty());
        ++named;

        // The reverse mapping has to be the inverse of the forward one, or the coverage walk cannot
        // report anything by the name a user would search for.
        CHECK (modSourceFromName (name) == value);
    }

    CHECK (named == 60);
    CHECK (modSourceFromName ("Nonsense") == 0);
    CHECK (modSourceFromName ("Mod Wheel ") == 0);
}

TEST_CASE ("v1 evaluates every source the contract carries, and names the ones it does not", "[modulation]")
{
    // The line ticket 27 drew: a source is shipped when `ModulationInputs` carries a value for it.
    std::vector<int> unimplemented;

    for (int value = 1; value <= kReferenceSourceCount; ++value)
        if (! modSourceIsImplemented (value))
            unimplemented.push_back (value);

    CHECK (unimplemented == std::vector<int> { 22, 60 });

    CHECK (modSourceName (22) == "NoteOn Rand2");
    CHECK (modSourceName (60) == "ModSlot");

    // Value 0 is "no source", not a source that went missing.
    CHECK_FALSE (modSourceIsImplemented (0));
    CHECK_FALSE (modSourceIsImplemented (61));

    // `NoteOn Alt.2` is shipped without an input of its own because the manual defines it as the inverse
    // of `NoteOn Alt.1`; a test that only counted inputs would have reported it as unimplemented.
    CHECK (modSourceIsImplemented (24));

    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);
    matrix.setSlots ({ slot (38, "Env", 0, 0, "kParamAttack") });

    ModulationInputs inputs;
    inputs.noteOnAlternate = 0.25;

    // Alt 2 = 1 - Alt 1, at full depth.
    ModSlot alt2 = slot (24, "Env", 1, 0, "kParamAttack");
    alt2.amount = 100.0;
    matrix.setSlots ({ alt2 });

    CHECK (settled (matrix, inputs, "Env", 1, 0) == Approx (0.75));
}

TEST_CASE ("a slot's amount and the sign of the amount both reach the destination", "[modulation]")
{
    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);

    ModulationInputs inputs;

    ModSlot row = slot (kFixed, "VoiceFilter", 0, 3);
    row.amount = 50.0;

    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (0.5));

    // The header's own rule: "the sign IS the polarity".
    row.amount = -50.0;
    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (-0.5));

    // 100% is one full range of the destination, which is the unit the caller multiplies by.
    row.amount = 100.0;
    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (1.0));
}

TEST_CASE ("bipolar re-centres the source on the middle of its travel", "[modulation]")
{
    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);

    ModulationInputs inputs;
    inputs.modWheel = 0.25;

    ModSlot row = slot (kModWheel, "VoiceFilter", 0, 3);
    row.amount = 100.0;

    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (0.25));

    // The manual: unidirectional puts the destination control "at the beginning", bi-directional "at the
    // middle" — so 0.25 of a 0..1 source is -0.5 of full depth when bipolar.
    row.bipolar = true;
    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (-0.5));

    // The source's midpoint is the one place the switch makes no difference.
    inputs.modWheel = 0.5;
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (0.0));

    inputs.modWheel = 1.0;
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (1.0));
}

TEST_CASE ("the row's curve and its output curve both bend the modulation", "[modulation]")
{
    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);

    ModulationInputs inputs;
    inputs.modWheel = 0.25;

    ModSlot row = slot (kModWheel, "VoiceFilter", 0, 3);
    row.amount = 100.0;

    // The contract's declared default is the neutral, so a row nobody has curved is a straight line —
    // which is also what the reference means by "when the curve is gray, it is bypassed".
    CHECK (row.curveIn == 1.0);

    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (0.25));

    // The bend is a power law on the same scale the reference's percent control uses: 100 above neutral
    // is the square root, 100 below it the square, and both pass through 0 and 1.
    row.curveIn = 101.0;
    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (0.5));      // sqrt(0.25)

    row.curveIn = -99.0;
    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (0.0625));   // 0.25 squared

    // The two curve controls sit at different ends of the row: `curveOut` bends what the row produced
    // *after* the depth, so it is not interchangeable with `curveIn`.
    row.curveIn = 1.0;
    row.curveOut = 101.0;
    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (0.5));

    // A curve bends sign-preservingly, so an inverted row stays inverted.
    row.amount = -100.0;
    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (-0.5));

    row.curveOut = -99.0;
    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (-0.0625));
}

TEST_CASE ("the aux source scales the row, as the manual describes", "[modulation]")
{
    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);

    ModulationInputs inputs;
    inputs.modWheel = 0.5;

    // "The two modulation sources are multiplied together so that one is scaling the other" — so a
    // fixed source at full depth through the mod wheel is the mod wheel.
    ModSlot row = slot (kFixed, "VoiceFilter", 0, 3);
    row.amount = 100.0;
    row.auxSource = kModWheel;
    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (0.5));

    // No aux source is a scale of one, not a scale of zero.
    row.auxSource = 0;
    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (1.0));

    // The depth and the aux both scale, and the order of the two does not matter.
    row.amount = 200.0;
    row.auxSource = kModWheel;
    matrix.setSlots ({ row });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (1.0));
}

TEST_CASE ("a continuous destination does not step at a block boundary", "[modulation]")
{
    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);

    ModSlot row = slot (kModWheel, "VoiceFilter", 0, 3);
    row.amount = 100.0;
    matrix.setSlots ({ row });

    // The mod wheel is held at 0 for one block and then jumps to 1 — the worst case for a control-rate
    // evaluator, and the one a hold-per-block implementation turns into a click.
    ModulationInputs inputs;
    inputs.modWheel = 0.0;
    matrix.beginBlock (inputs, kFrames);

    const auto quiet = matrix.rampFor ("VoiceFilter", 0, 3);
    CHECK_FALSE (quiet.discrete);
    CHECK (quiet.end == Approx (0.0));

    inputs.modWheel = 1.0;
    matrix.beginBlock (inputs, kFrames);

    const auto ramp = matrix.rampFor ("VoiceFilter", 0, 3);

    CHECK (ramp.start == Approx (0.0));      // where the previous block ended
    CHECK (ramp.end == Approx (1.0));        // where this one ends

    // It really interpolates rather than holding: a frame inside the block moves by one in-block step,
    // and the block's end anchor is the whole change.
    const auto step = 1.0 / static_cast<double> (kFrames);

    CHECK (ramp.at (1, kFrames) - ramp.at (0, kFrames) == Approx (step).epsilon (1e-9));
    CHECK (ramp.at (kFrames, kFrames) == Approx (1.0));
    CHECK (ramp.at (kFrames / 2, kFrames) == Approx (0.5));

    // The join is *inside* that line rather than a step across it: block N's frame 0 is exactly block
    // N-1's frame `kFrames`, which is 0 here — a hold-per-block implementation would put the whole 1.0
    // at the join and pass neither this nor the bound below.
    CHECK (ramp.at (0, kFrames) == quiet.at (kFrames, kFrames));

    // Now drive the source *through* a block boundary, which is where the interpolation has to hold: the
    // destination must not jump at the join, even though the source moved a whole block's worth between
    // the two anchor points.
    ModSlot second = slot (kModWheel, "VoiceFilter", 1, 3);
    second.amount = 100.0;
    matrix.setSlots ({ second });

    inputs.modWheel = 0.0;
    matrix.beginBlock (inputs, kFrames);

    inputs.modWheel = 0.5;
    matrix.beginBlock (inputs, kFrames);

    const auto first = matrix.rampFor ("VoiceFilter", 1, 3);

    inputs.modWheel = 1.0;
    matrix.beginBlock (inputs, kFrames);

    const auto next = matrix.rampFor ("VoiceFilter", 1, 3);

    std::vector<double> sequence;
    sequence.push_back (first.at (kFrames - 1, kFrames));

    for (int frame = 0; frame < kFrames; ++frame)
        sequence.push_back (next.at (frame, kFrames));

    double largest = 0.0;

    for (std::size_t i = 1; i < sequence.size(); ++i)
        largest = std::max (largest, std::fabs (sequence[i] - sequence[i - 1]));

    // One in-block step is 0.5/64; every step, including the one across the join, is within 5% of it.
    CHECK (largest <= (0.5 / static_cast<double> (kFrames)) * 1.05);
    CHECK (next.start == Approx (0.5));
    CHECK (next.end == Approx (1.0));

    // And it does settle: once the source stops moving, the destination is exactly at its value rather
    // than a block behind it.
    inputs.modWheel = 1.0;
    matrix.beginBlock (inputs, kFrames);

    const auto settledRamp = matrix.rampFor ("VoiceFilter", 1, 3);

    CHECK (settledRamp.start == Approx (1.0));
    CHECK (settledRamp.end == Approx (1.0));
}

TEST_CASE ("a discrete destination takes one value for the whole block", "[modulation]")
{
    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);

    // Two destinations driven identically: one the reference types as an enum (`kParamWarpMenu`), one a
    // number. The comparison is the test — a hold that is indistinguishable from a ramp proves nothing.
    ModSlot discrete = slot (kModWheel, "WTOsc", 0, 2, "kParamWarpMenu");
    ModSlot continuous = slot (kModWheel, "VoiceFilter", 0, 3, "kParamFreq");
    discrete.amount = 100.0;
    continuous.amount = 100.0;

    matrix.setSlots ({ discrete, continuous });

    ModulationInputs inputs;
    inputs.modWheel = 0.0;
    matrix.beginBlock (inputs, kFrames);

    inputs.modWheel = 1.0;
    matrix.beginBlock (inputs, kFrames);

    const auto held = matrix.rampFor ("WTOsc", 0, 2);
    const auto ramped = matrix.rampFor ("VoiceFilter", 0, 3);

    CHECK (held.discrete);
    CHECK_FALSE (ramped.discrete);

    // One value, for every frame, and it is this block's value rather than last block's.
    for (int frame = 0; frame < kFrames; ++frame)
        CHECK (held.at (frame, kFrames) == Approx (1.0));

    CHECK (held.start == Approx (1.0));
    CHECK (held.at (0, kFrames) == held.at (kFrames - 1, kFrames));

    // A discrete destination has no ramp at all, so `start` and `end` agree: a caller that spreads the
    // pair itself gets the held value rather than an interpolation we do not want it to do.
    CHECK (held.start == held.end);

    // The continuous one beside it is mid-ramp in the same block, so "held" is a property of the
    // destination and not of the block.
    CHECK (ramped.at (0, kFrames) < ramped.at (kFrames - 1, kFrames));

    // And the held value does move between blocks — held per block is not frozen.
    inputs.modWheel = 0.0;
    matrix.beginBlock (inputs, kFrames);
    CHECK (matrix.rampFor ("WTOsc", 0, 2).at (0, kFrames) == Approx (0.0));
}

TEST_CASE ("rows sum onto their destination, and destinations stay apart", "[modulation]")
{
    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);

    ModulationInputs inputs;
    inputs.macro[0] = 0.5;
    inputs.macro[1] = 0.25;

    ModSlot first = slot (25, "VoiceFilter", 0, 3);      // Macro 1
    ModSlot second = slot (26, "VoiceFilter", 0, 3);     // Macro 2
    ModSlot other = slot (25, "VoiceFilter", 1, 3);      // a different instance

    first.amount = 100.0;
    second.amount = 100.0;
    other.amount = 100.0;

    matrix.setSlots ({ first, second, other });
    matrix.beginBlock (inputs, kFrames);
    matrix.beginBlock (inputs, kFrames);

    CHECK (matrix.modulatedDestinationCount() == 2);
    CHECK (matrix.rampFor ("VoiceFilter", 0, 3).end == Approx (0.75));
    CHECK (matrix.rampFor ("VoiceFilter", 1, 3).end == Approx (0.5));
    CHECK (matrix.rampFor ("VoiceFilter", 0, 4).end == Approx (0.0));    // a parameter nothing wrote

    // An unused row is kept in `slots()` — the index is the row's identity — but it contributes nothing.
    auto unused = second;
    unused.used = false;
    matrix.setSlots ({ first, unused, other });
    CHECK (matrix.slots().size() == 3);
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (0.5));
    CHECK (matrix.modulatedDestinationCount() == 2);

    // A bypassed row is not evaluated at all, so it does not even claim its destination.
    auto bypassed = second;
    bypassed.bypassed = true;
    matrix.setSlots ({ first, bypassed, other });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (0.5));
    CHECK (settled (matrix, inputs, "VoiceFilter", 1, 3) == Approx (0.5));
}

TEST_CASE ("a destination nothing modulates reads as a zero ramp", "[modulation]")
{
    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);

    const ModulationInputs inputs;
    matrix.beginBlock (inputs, kFrames);

    const auto ramp = matrix.rampFor ("Env", 0, 0);

    CHECK (ramp.start == Approx (0.0));
    CHECK (ramp.end == Approx (0.0));
    CHECK_FALSE (ramp.discrete);
    CHECK (ramp.at (0, kFrames) == Approx (0.0));
    CHECK (matrix.modulatedDestinationCount() == 0);
    CHECK (matrix.unimplementedSourcesInUse().empty());
}

TEST_CASE ("rows whose source v1 does not evaluate contribute nothing and report by value",
           "[modulation]")
{
    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);

    ModSlot random2 = slot (22, "VoiceFilter", 0, 3);           // NoteOn Rand2
    ModSlot modSlotSource = slot (60, "VoiceFilter", 1, 3);     // ModSlot
    ModSlot good = slot (kFixed, "VoiceFilter", 0, 3);

    random2.amount = 100.0;
    modSlotSource.amount = 100.0;
    good.amount = 100.0;

    matrix.setSlots ({ random2, modSlotSource, good });

    ModulationInputs inputs;
    inputs.noteOnRandom = 1.0;

    CHECK (matrix.unimplementedSourcesInUse() == std::vector<int> { 22, 60 });
    CHECK (settled (matrix, inputs, "VoiceFilter", 0, 3) == Approx (1.0));   // only the good row
    CHECK (matrix.rampFor ("VoiceFilter", 1, 3).end == Approx (0.0));

    // For each reported value the caller has a name, which is the whole point of reporting by value.
    for (const auto value : matrix.unimplementedSourcesInUse())
        CHECK_FALSE (modSourceName (value).empty());

    // An unimplemented **aux** source is reported too, and the row is not half-evaluated: guessing the
    // scaling of a source we do not have would be guessing the sound.
    ModSlot auxRow = slot (kFixed, "VoiceFilter", 1, 3);
    auxRow.amount = 100.0;
    auxRow.auxSource = 22;

    matrix.setSlots ({ auxRow });
    CHECK (matrix.unimplementedSourcesInUse() == std::vector<int> { 22 });
    CHECK (settled (matrix, inputs, "VoiceFilter", 1, 3) == Approx (0.0));

    // A bypassed row does not report its source: it is not in use.
    auxRow.bypassed = true;
    matrix.setSlots ({ auxRow });
    CHECK (matrix.unimplementedSourcesInUse().empty());
}

TEST_CASE ("a destination that does not resolve reports why, and never writes anywhere", "[modulation]")
{
    ModSlot row = slot (kFixed, "VoiceFilter", 0, 3);
    CHECK (modDestinationUnresolvedReason (row).empty());

    // The 6 rows in `SY - Tetris` kept a source and an amount and lost their destination entirely.
    ModSlot dangling = slot (kFixed, "", 0, 0, "");
    CHECK (modDestinationUnresolvedReason (dangling) == "the row has no destination");

    // Ticket 03's `None` sentinel, unchanged in the corpus.
    ModSlot none = dangling;
    none.destinationClass = "None";
    CHECK (modDestinationUnresolvedReason (none) == "the row has no destination");

    ModSlot unknownClass = slot (kFixed, "FXMultiband", 0, 1);
    CHECK (modDestinationUnresolvedReason (unknownClass).find ("not one the reference uses")
           != std::string::npos);

    ModSlot badInstance = slot (kFixed, "VoiceFilter", 2, 3);
    CHECK (modDestinationUnresolvedReason (badInstance).find ("out of range") != std::string::npos);

    ModSlot unnamed = slot (kFixed, "VoiceFilter", 0, 3, "");
    CHECK (modDestinationUnresolvedReason (unnamed) == "the row names no destination parameter");

    // An unresolved row is dropped at `setSlots`, so it cannot land on a neighbouring parameter.
    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);
    matrix.setSlots ({ dangling, unknownClass, badInstance, unnamed });

    const ModulationInputs inputs;
    matrix.beginBlock (inputs, kFrames);

    CHECK (matrix.modulatedDestinationCount() == 0);
    CHECK (matrix.slots().size() == 4);
}

TEST_CASE ("FX instances are rack-encoded, so the same slot in two racks is two destinations",
           "[modulation]")
{
    CHECK (modDestinationInstanceKnown ("FXEQ", 0));
    CHECK (modDestinationInstanceKnown ("FXEQ", 5));
    CHECK (modDestinationInstanceKnown ("FXEQ", 105));    // rack 1, slot 5
    CHECK (modDestinationInstanceKnown ("FXEQ", 205));    // rack 2, slot 5
    CHECK (modDestinationInstanceKnown ("FXEQ", 299));    // rack 2, slot 99
    CHECK_FALSE (modDestinationInstanceKnown ("FXEQ", 300));
    CHECK_FALSE (modDestinationInstanceKnown ("FXEQ", -1));
    CHECK_FALSE (modDestinationInstanceKnown ("Nonsense", 0));

    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);

    ModSlot rack0 = slot (kFixed, "FXEQ", 5, 6, "kParamGain2");
    ModSlot rack1 = slot (kFixed, "FXEQ", 105, 6, "kParamGain2");
    rack0.amount = 100.0;

    matrix.setSlots ({ rack0, rack1 });

    ModulationInputs inputs;
    inputs.fixed = 0.25;

    matrix.beginBlock (inputs, kFrames);
    matrix.beginBlock (inputs, kFrames);

    CHECK (matrix.modulatedDestinationCount() == 2);
    CHECK (matrix.rampFor ("FXEQ", 5, 6).end == Approx (0.25));
    CHECK (matrix.rampFor ("FXEQ", 105, 6).end == Approx (0.0));   // depth 0: registered, contributing 0
    CHECK (matrix.rampFor ("FXFilter", 5, 6).end == Approx (0.0)); // and the class is part of the identity
}

TEST_CASE ("the destination vocabulary and the instance counts are the reference's", "[modulation]")
{
    // The classes ticket 03 measured across all 13,258 non-empty rows, plus the one effect class that is
    // in the reference's list but is never a matrix destination.
    for (const auto* name : { "Oscillator", "VoiceFilter", "WTOsc", "MultiSampleOsc", "SampleOsc",
                              "GranularOsc", "SpectralOsc", "NoiseOsc", "Env", "LFO", "Macro",
                              "RoutingSlot", "LFOPointModBus", "Global", "Arp", "MidiClip", "VoicePanel",
                              "RetriggerState", "ModSlot", "FXEQ", "FXComp", "FXReverb", "FXDelay",
                              "FXDistortion", "FXFilter", "FXConv", "FXUtils", "FXChorus", "FXHyperD",
                              "FXBode", "FXPhaser", "FXFlanger", "FXSplit", "FXSplit3", "FXSplitMS" })
        CHECK (modDestinationClassKnown (name));

    CHECK_FALSE (modDestinationClassKnown (""));
    CHECK_FALSE (modDestinationClassKnown ("None"));
    CHECK_FALSE (modDestinationClassKnown ("Osc"));      // a module the reference has, but not a destination

    CHECK (modDestinationInstanceKnown ("VoiceFilter", 1));
    CHECK_FALSE (modDestinationInstanceKnown ("VoiceFilter", 2));

    CHECK (modDestinationInstanceKnown ("Env", 3));
    CHECK_FALSE (modDestinationInstanceKnown ("Env", 4));

    CHECK (modDestinationInstanceKnown ("LFO", 9));
    CHECK_FALSE (modDestinationInstanceKnown ("LFO", 10));

    CHECK (modDestinationInstanceKnown ("Macro", 7));
    CHECK_FALSE (modDestinationInstanceKnown ("Macro", 8));

    CHECK (modDestinationInstanceKnown ("RoutingSlot", 6));
    CHECK_FALSE (modDestinationInstanceKnown ("RoutingSlot", 7));

    CHECK (modDestinationInstanceKnown ("LFOPointModBus", 15));
    CHECK_FALSE (modDestinationInstanceKnown ("LFOPointModBus", 16));

    CHECK (modDestinationInstanceKnown ("Oscillator", 4));
    CHECK_FALSE (modDestinationInstanceKnown ("Oscillator", 5));

    CHECK (modDestinationInstanceKnown ("ModSlot", 63));
    CHECK_FALSE (modDestinationInstanceKnown ("ModSlot", 64));

    // `NoiseOsc` is oscillator slot 3 and only slot 3 — the corpus's 64 rows all say so.
    CHECK (modDestinationInstanceKnown ("NoiseOsc", 3));
    CHECK_FALSE (modDestinationInstanceKnown ("NoiseOsc", 0));

    CHECK (modDestinationInstanceKnown ("Global", 0));
    CHECK_FALSE (modDestinationInstanceKnown ("Global", 1));

    CHECK (modDestinationInstanceKnown ("WTOsc", 2));
    CHECK_FALSE (modDestinationInstanceKnown ("WTOsc", 3));
}

TEST_CASE ("discreteness is the reference's parameter typing, read off the name", "[modulation]")
{
    // An enum cannot be interpolated, so it is the block's value; a number can, so it ramps.
    CHECK (modDestinationIsDiscrete ("VoiceFilter", "kParamType"));
    CHECK_FALSE (modDestinationIsDiscrete ("VoiceFilter", "kParamFreq"));
    CHECK (modDestinationIsDiscrete ("WTOsc", "kParamWarpMenu"));
    CHECK (modDestinationIsDiscrete ("WTOsc", "kParamWarpMenu2"));
    CHECK_FALSE (modDestinationIsDiscrete ("WTOsc", "kParamWarp"));
    CHECK (modDestinationIsDiscrete ("Env", "kParamAttack") == false);
    CHECK (modDestinationIsDiscrete ("LFO", "kParamRate") == false);
    CHECK (modDestinationIsDiscrete ("RoutingSlot", "kParamRoutingDest"));

    // `kParamMode` is the one name the reference types two ways. A flat name set has to be wrong here,
    // which is why the class is part of the question.
    CHECK (modDestinationIsDiscrete ("LFO", "kParamMode"));
    CHECK (modDestinationIsDiscrete ("FXDistortion", "kParamMode"));
    CHECK_FALSE (modDestinationIsDiscrete ("FXDelay", "kParamMode"));
    CHECK_FALSE (modDestinationIsDiscrete ("FXReverb", "kParamMode"));

    // An unnamed parameter is treated as continuous rather than frozen.
    CHECK_FALSE (modDestinationIsDiscrete ("VoiceFilter", ""));
}

TEST_CASE ("an empty block is a no-op rather than a zeroed destination", "[modulation]")
{
    ModulationMatrix matrix;
    matrix.prepare (48000.0, kFrames);

    ModSlot row = slot (kFixed, "VoiceFilter", 0, 3);
    row.amount = 100.0;
    matrix.setSlots ({ row });

    ModulationInputs inputs;
    inputs.fixed = 1.0;

    matrix.beginBlock (inputs, kFrames);
    CHECK (matrix.rampFor ("VoiceFilter", 0, 3).end == Approx (1.0));

    matrix.beginBlock (inputs, 0);

    // Nothing was evaluated, so nothing was rewritten: a zero-frame block cannot silently drop the
    // modulation of the block before it.
    CHECK (matrix.rampFor ("VoiceFilter", 0, 3).end == Approx (1.0));
}
