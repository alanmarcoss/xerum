// xerum::dsp — the effect rack.
//
// Ticket `04` established the shape this file builds, and it is a *topology* rather than a list of
// effects. A preset's `FXRack0/1/2` are **MAIN / BUS 1 / BUS 2** (guide p.153), each holding one flat
// `FX` array evaluated **serially top-to-bottom** (guide p.155). Three of the sixteen classes are
// **splitters**: a splitter claims the *next* `kParamModuleCount1..3` slots of that same array as
// parallel bands, after which serial resumes. Nothing nests — ticket `04` validated the rule over the
// corpus and found zero nested splitters, and `tests/model/fx_topology_test.cpp` re-derives that result
// here rather than trusting it.
//
// **One correction to ticket 04, measured on the same corpus.** Ticket 04 wrote that
// `kParamModuleCount*` is observed 1–5 and "a branch was never empty". The stored range is 1–5, but the
// writer is sparse: an **absent count means zero**, and 14 racks in the corpus have an empty band. Read
// with 1 as the default, those 14 racks overrun their own array (a splitter claiming more slots than
// exist); the whole corpus reconstructs with zero leftovers only when absence means zero. So that is the
// rule, and an empty band is a band the splitter passes through unprocessed.
//
// **The cut.** Ticket `12` shipped "the 14 of 16 classes that cover 99%", measured over the 626 factory
// presets; `corpus-usage.py` reports 99% at exactly 14 classes, and the two outside are `FXSplit3`
// (19 uses, 0.5%) and `FXSplitMS` (15, 0.4%). Those two are **deferred**: recognised by name, reported,
// never silently dropped. The other fourteen are built here — the thirteen effects plus `FXSplit`.
//
// **Rate.** Per ticket `13` (D4) the rack runs at the **base rate**: warp, filtering and saturation run
// oversampled, the FX rack does not. The cost is that the rack's nonlinear stages (the distortion's
// curves and the compressor's detector) alias at 44.1/48 kHz; the benefit is that the rack adds no
// up/downsampling, so it is the cheapest stage per sample. Recorded, not hidden.
//
// **Convolution and a missing impulse.** `FXConv` convolves with an impulse response the *caller*
// supplies — either the `embeddedIR` a preset can carry inline or a file the caller resolved from
// `relativePathToIR`, which is how the shipped 109-file `Impulses/` corpus (ticket `04` §6, 78 distinct
// references in the factory bank) reaches the effect. This layer touches no filesystem, so the reference
// is a borrowed pointer and the resolution belongs to the content layer, which is where ticket `64` put it
// (`content::TableLibrary::loadAsset` under `Impulses/`, decoded by the plugin). When it is null the
// convolver **reports** through `problem()` and passes the dry signal; it never convolves with nothing,
// which would be exactly the silent drop `docs/preset-import-contract.md` forbids.
//
// **Defaults.** A preset never stores a value equal to its default, so every default is a convention
// here until ticket `38`'s table establishes one. Each effect states its conventions where it reads
// them; none of them is silently zero.
//
// **Call order.** `prepare()` and `setSlots()` may come in either order; the rack rebuilds itself when
// the second one arrives. `prepare`'s `maximumBlockFrames` is a contract: `process` never allocates and
// never grows a buffer, so a caller that exceeds it loses the tail of the block rather than crashing.

#pragma once

#include "filter.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xerum::dsp
{

/// The reference's sixteen rack module classes, in its own numeric `type` order (0..15).
enum class FxClass
{
    Distortion,      ///< 0
    Flanger,         ///< 1
    Phaser,          ///< 2
    Chorus,          ///< 3
    Delay,           ///< 4
    Compressor,      ///< 5
    Reverb,          ///< 6
    Equalizer,       ///< 7
    Filter,          ///< 8
    HyperDimension,  ///< 9
    Bode,            ///< 10
    Convolve,        ///< 11
    Utility,         ///< 12
    Split,           ///< 13, LOWS/HIGHS
    Split3,          ///< 14, LOWS/MIDS/HIGHS
    SplitMS,         ///< 15, MID/SIDE
    Unknown          ///< not one of the sixteen
};

/// The class name as a preset writes it — the key of the slot's keyed object, e.g. `"FXDistortion"`.
std::string_view fxClassName (FxClass cls);
FxClass fxClassFromName (std::string_view name);

/// The reference's numeric `type` id, or -1 for `Unknown`.
int fxTypeId (FxClass cls);
FxClass fxClassFromTypeId (int id);

/// Ticket 12's cut, re-derived from corpus usage in `.scratch/xerum/tools/corpus-usage.py`.
bool fxClassIsShipped (FxClass cls);
const std::vector<FxClass>& shippedFxClasses();    ///< 14
const std::vector<FxClass>& deferredFxClasses();   ///< 2 — `FXSplit3`, `FXSplitMS`

/// A splitter claims the next `sum(kParamModuleCount1..3)` slots of the flat array as parallel bands,
/// whatever its shipped status: the topology is the format's, not the cut's.
bool fxClassIsSplitter (FxClass cls);
int fxSplitterBandCount (FxClass cls);

/// An impulse response for `FXConvolve`, owned by the caller and borrowed here.
struct ImpulseResponse
{
    const float* const* channels { nullptr };   ///< per-channel sample pointers, `channelCount` of them
    int channelCount { 0 };
    int frames { 0 };
    double sampleRate { 0.0 };
};

/// A slot's custom waveshaper curve — the reference's `flex[0]`, a piecewise-linear table a
/// `FXDistortion` slot can carry (`XShape 1.SerumFX` in the corpus carries 480 points). Borrowed, like
/// the impulse: this layer owns no documents.
struct TransferCurve
{
    const float* x { nullptr };   ///< `points` abscissae, ascending, in [-1, 1]
    const float* y { nullptr };   ///< `points` ordinates, in [-1, 1]
    int points { 0 };
};

/// One slot of a rack's flat `FX` array: which class it is, and the values the preset carried for it.
///
/// The values are keyed by their **reference names** (`kParamDrive`, `kParamFreq`, …) because that is
/// how a preset addresses them — the translation from a document path to here is then mechanical and
/// lives with whoever owns the document. Only the values a preset *carried* are present; absence means
/// the reference's default, and each effect states its own convention for that.
struct FxSlot
{
    FxClass cls { FxClass::Unknown };
    std::vector<std::pair<std::string, double>> numbers;   ///< `kParam…` leaves
    std::vector<std::pair<std::string, std::string>> texts;///< `kParamMode`, `kParamType`
    const ImpulseResponse* impulse { nullptr };            ///< `FXConvolve` only
    const PoleZeroDescription* poleZero { nullptr };       ///< `FXFilter` with a drawn `PZ_SVF`
    const TransferCurve* curve { nullptr };                ///< `FXDistortion` with a drawn curve
};

/// One effect. Configured by name, prepared once, processed a block at a time.
class FxEffect
{
public:
    virtual ~FxEffect() = default;

    virtual FxClass cls() const = 0;

    /// `maximumBlockFrames` sizes the scratch the effect needs; `process` never allocates.
    virtual void prepare (double sampleRate, int maximumBlockFrames) = 0;
    virtual void reset() = 0;

    /// A `kParam…` name, or the slot-level `type`. Returns **false** for a name this effect does not
    /// know, so the caller reports it instead of a preset's parameter vanishing.
    virtual bool setNumber (std::string_view name, double value) = 0;

    /// `kParamMode` (distortion), `kParamType` (reverb, filter), …
    virtual bool setText (std::string_view name, std::string_view value) = 0;

    /// Only `FXConvolve` reads this. Everyone else ignores it.
    virtual void setImpulse (const ImpulseResponse&) {}

    /// Only `FXDistortion` reads this. Everyone else ignores it.
    virtual void setCurve (const TransferCurve&) {}

    /// Only `FXFilter` reads this — the drawn `PZ_SVF` response a preset can inline. Others ignore it.
    virtual void setPoleZero (const PoleZeroDescription&) {}

    /// A one-line reason this effect cannot do its job — a filter type we do not build, a missing
    /// impulse response. Empty when healthy. The rack collects these; nothing here is silent.
    virtual std::string problem() const { return {}; }

    virtual void process (float* left, float* right, int frames) = 0;
};

/// Build one effect. Returns null for a class v1 does not ship — a deferred or unknown class — which
/// the rack records rather than substituting.
std::unique_ptr<FxEffect> makeFxEffect (FxClass cls);

/// One rack: the serial chain the flat array encodes, with each shipped splitter's following slots
/// claimed as parallel bands.
class FxRack
{
public:
    FxRack();
    ~FxRack();

    FxRack (FxRack&&) noexcept;
    FxRack& operator= (FxRack&&) noexcept;
    FxRack (const FxRack&) = delete;
    FxRack& operator= (const FxRack&) = delete;

    /// The chain the flat array decodes into. Declared here because the rack's free-function helpers
    /// build and walk it; defined in the translation unit, and not part of any caller's business.
    struct Chain;

    void prepare (double sampleRate, int maximumBlockFrames);

    /// Rebuild from a rack's flat `FX` array, exactly as it is stored. A **shipped** splitter consumes
    /// the next `sum(kParamModuleCount1..3)` slots as its bands and is not itself a node; a **deferred**
    /// splitter is recorded in `unresolved()` and skipped together with the slots it claims, because
    /// those slots are addressed through a routing we do not reproduce — running them serially would
    /// invent a topology the preset does not have. An unknown class is recorded and skipped.
    void setSlots (const std::vector<FxSlot>& slots);

    void reset();

    void process (float* left, float* right, int frames);

    bool empty() const;

    /// How many effect nodes were actually built, across every band.
    std::size_t nodeCount() const;

    /// What this rack could not build, in array order: deferred and unknown **class names**, and
    /// `Class/kParamName` for a parameter an effect did not recognise. The caller reports these.
    const std::vector<std::string>& unresolved() const;

    /// Effect-level problems in chain order — a missing impulse response, a filter we do not build.
    const std::vector<std::string>& problems() const;

private:
    void rebuild();

    std::unique_ptr<Chain> chain_ {};
    std::vector<FxSlot> slots_ {};
    std::vector<std::string> unresolved_ {};
    std::vector<std::string> problems_ {};
    double sampleRate_ { 48000.0 };
    int maximumBlockFrames_ { 0 };
};

/// The three racks and the bus routing between them — ticket 04's MAIN / BUS 1 / BUS 2 arrangement.
///
/// The mixer (a channel sends to MAIN, BUS 1 or BUS 2) belongs to the voice layer; this owns what
/// happens *after* that split. `Global`'s `kParamFXBus{n}Dest`/`Vol` decide where each bus's output
/// goes: destination `1` (the reference's default, and what all but a handful of presets use) sums the
/// bus into MAIN's input, so MAIN's rack processes it; destination `2` bypasses MAIN and is summed
/// after it. That mapping is a convention: ticket 04 read the values, not their meaning.
class FxBank
{
public:
    static constexpr int kRackCount = 3;

    void prepare (double sampleRate, int maximumBlockFrames);
    void reset();

    FxRack& rack (int index);
    const FxRack& rack (int index) const;

    void setBusRouting (double bus1Destination, double bus1Volume, double bus2Destination, double bus2Volume);
    void setBusVolume (int busIndex, double volume);
    void setBusDestination (int busIndex, double destination);

    /// Process one block. `main`/`bus1`/`bus2` are what the mixer sent to each rack; each is processed
    /// by its own rack, then the busses are routed into or around MAIN. In place.
    void process (float* mainLeft, float* mainRight, float* bus1Left, float* bus1Right,
                  float* bus2Left, float* bus2Right, int frames);

private:
    FxRack racks_[kRackCount] {};
    double destination_[2] { 1.0, 1.0 };
    double volume_[2] { 1.0, 1.0 };
};

} // namespace xerum::dsp
