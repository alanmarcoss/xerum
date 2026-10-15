// xerum::dsp — the voice, the fixed pool, and unison.
//
// This is the layer that turns oscillators and filters into a playable instrument: a fixed pool of
// voices allocated in `prepare()`, a stealing policy, unison, note lifecycle with sustain and
// portamento, and the place where modulation stops being a table and becomes pitch.
//
// ## Why the pool is fixed and where the ceilings come from
//
// An audio thread may not allocate, so every voice, every unison sub-oscillator and every filter in the
// pool is a member of `VoiceEngine` and is constructed by `prepare()`. Nothing is ever added to the pool
// while audio is running.
//
// The two ceilings are the reference's own, read out of the schema rather than guessed:
//
//   * **voices** — `Global{N}/plainParams/kParamPolyCount` ranges 1..32, so `kMaxVoices = 32`;
//   * **unison** — `Oscillator{N}/plainParams/kParamUnison` ranges 2..16 (1 is "not stored", which is
//     why the schema's minimum is 2), so `kMaxUnison = 16`.
//
// **Cost per voice is what sets that budget.** One voice is up to `kOscillatorSlots` oscillators, each
// of which may be up to `kMaxUnison` independent sub-oscillators — independent because a warp such as
// the filter-as-warp carries state, so unison is not one oscillator with several phases but several
// oscillators. That is 3 x 16 = 48 oscillator read paths per voice and 32 x 48 = **1536** in the worst
// case, which is the number that matters: it is why the ceilings are compile-time constants and why the
// *active* polyphony is a `prepare()` argument rather than something the pool grows to meet later.
//
// So: the pool capacity is compile-time (the arrays), the *active* voice count is set at `prepare()` and
// clamped to `kMaxVoices`, and the per-voice unison count is set from the preset and clamped to
// `kMaxUnison`. A preset asking for more than either is reported by the importer, not silently obeyed.
//
// ## Slots 3 and 4 are not wavetable slots
//
// The preset has **five** oscillator slots, and the last two are the reference's fixed single-engine
// slots: NOISE (`Oscillator3`, ticket 40's `NoiseOscillator`) and SUB (`Oscillator4`, `SubOscillator`).
// They carry no `kParamType`, so there is no engine to select and no unison to stack (the bank stores
// `kParamUnison` on neither), which is why they sit beside the three tabled slots' arrays rather than in
// them. They are still *slots*, though: `Oscillator/3/kParamVolume` and `Oscillator/4/kParamVolume` are
// 513 of the bank's modulation rows, and the sub's octave and coarse pitch are 28 more, so the
// modulation ramps are indexed by the preset's own five-wide numbering (`kSlotCount`) and the tabled
// arrays by `kOscillatorSlots`.
//
// ## Stealing
//
// Order (ticket 13): **quietest-releasing first, then oldest.** Concretely, in this order:
//
//   1. a voice that is `Idle` — free, and reusing it is not a steal at all;
//   2. otherwise a `Releasing` voice, the one with the lowest current envelope level;
//   3. otherwise the lowest-level `On` voice.
//
// Ties — and, importantly, the ordinary case of every voice sustaining at the same level — break by
// **age**, oldest first (`age` is a monotonic note-on counter, so smaller is older).
//
// "Quietest" is therefore always the **current amplitude-envelope level**: it means something concrete
// when voices are releasing (they are below their sustain level) and it degenerates to the age tie-break
// when they are not, which is the honest answer to "what is quietest when nothing is releasing". A
// level-only rule would otherwise steal a just-struck voice over a long-held one.
//
// A stolen voice is **faded, not hard-cut**. It enters `VoiceStage::Stealing`, ramps its amplitude to
// zero over `kStealFadeSeconds` (2 ms), and only then starts the pending note — so there is no waveform
// discontinuity and no click. This costs at most 2 ms of latency on the stolen note and is the reason a
// hard cut is not an option: a click is exactly the kind of thing a fidelity metric can pass while a
// user hears it.
//
// ## Mono and legato
//
// The preset selects these through `Global{N}/plainParams/kParamMonoToggle` and `.../kParamLegato`.
// `voiceModeFor()` turns those two flags into a `VoiceMode`; `Mono` retriggers its single voice on every
// new note, `MonoLegato` does not (the envelope runs through and only the pitch moves). Portamento is
// `Global{N}/plainParams/kParamPortamentoTime` / `kParamPortamentoCurve` / `kParamPortaAlways`.
//
// ## Modulation
//
// Per ticket 13 the engine is control-rate with in-block interpolation: at each block the voice builds
// its `ModulationInputs`, calls `ModulationMatrix::beginBlock` once, fetches the `ModulatedRamp` for each
// destination it owns, and reads it per sample with `ModulatedRamp::at`. The destinations are the ones
// the voice actually has state for, addressed exactly as ticket 03 established the reference addresses
// them — `(class string, instance id, param id)`:
//
//   * `Oscillator/{slot}/kParamPitch`  (id 4)  — semitones, span +-12 (`kParamPitch`'s own range), for
//     every slot 0..4 (NOISE has no pitch, so the ramp is read for the tabled slots and SUB);
//   * `Oscillator/{slot}/kParamVolume` (id 1)  — gain, 0..1, for every slot including NOISE and SUB;
//   * `Oscillator/4/kParamOctave`      (id 3)  — semitones, span +-48;
//   * `Oscillator/4/kParamCoarsePit`   (id 6)  — semitones, span +-64;
//   * `VoiceFilter/{slot}/kParamFreq`  (id 3)  — normalised cutoff, 0..1;
//   * `VoiceFilter/{slot}/kParamReso`  (id 4)  — resonance percent, 0..100.
//
// A destination's ramp is in units where a `kParamAmount` of 100 contributes 1.0 (modulation.h), so the
// spans above are what turns a ramp into a parameter delta. The spans are a **stated convention** kept
// equal to each parameter's schema range, not a measured curve; the fidelity suite replaces them.
//
// ## Where the generated half of the inputs comes from
//
// Ticket 48 — the four matrix envelopes, the ten LFOs (and their `LFO n Y` outputs), the eight macros and
// the audio-rate sources are **generated**, and each has a specific author:
//
//   * `mod_sources.h`'s `ModulationGenerator` is the four matrix envelopes and the ten LFOs. A voice has
//     one; the engine has one too, and it runs the LFOs a preset marks `Mono` **once for the pool** so a
//     shared LFO's phase belongs to the transport rather than to whichever voice is playing.
//   * the macros and the player's controls arrive through `setGlobalInputs` — the plugin fills them from
//     its parameters and the host's MIDI.
//   * `osc[0..2]`, `noiseOsc`, `subOsc` and `voiceFilter[0..1]` are **the voice's own samples from the
//     previous frame**, captured in `render`. One frame old on purpose: a warp may sit on the first slot
//     and read the third, so nothing here can be this frame's sample without reading an oscillator twice.
//   * **the warp's modulator** is the same signal, handed to `WavetableOscillator::setWarpModulator` in
//     `render`, resolved from the mode's name by `dsp::warpSourceFromName`. Ticket 25 built the
//     source-driven families to take their modulator as an argument and *nothing ever called that
//     function*, which left `kPD_OSC` (12.6% of the corpus's warps) and `kFM_OSC` (7.3%) transparent in
//     the product while passing their own unit tests.
//
// The four matrix envelopes are their own generators with their own state, **not** the amplitude envelope:
// `AmpEnvelope` shapes the voice's level from `VoiceSettings::envelope`, and `in.envelope[0..3]` are the
// four `Env0..3` the preset stores. The two must not be conflated, and `mod_sources_test.cpp` asserts that
// a matrix envelope with no destination leaves the render byte-identical while the amplitude envelope
// still shapes it.

#pragma once

#include "filter.h"
#include "mod_sources.h"
#include "modulation.h"
#include "noise_osc.h"
#include "oscillator.h"
#include "sub_osc.h"
#include "wavetable.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace xerum::dsp
{

/// Pool capacity and per-voice ceilings. Compile-time because the pool is fixed; see the header note.
inline constexpr int kMaxVoices = 32;
inline constexpr int kMaxUnison = 16;
inline constexpr int kOscillatorSlots = 3;   ///< OSC A / B / C, the wavetable slots this layer builds
inline constexpr int kFilterSlots = 2;       ///< VoiceFilter0..1

/// **The preset's own slot numbering**, which is not `kOscillatorSlots`. Slots 3 and 4 are the fixed
/// single-engine slots ticket 05 established: NOISE in `Oscillator3` and SUB in `Oscillator4`, neither
/// selectable and neither a container. They are not wavetable slots, so they are outside the arrays sized
/// by `kOscillatorSlots` — but they are inside the *voice*, and their level and pitch are modulation
/// destinations under the same `Oscillator` class (`Oscillator/{3,4}/kParamVolume`, measured at 513 rows
/// in the factory bank), so the modulation ramps are indexed by this five-wide numbering instead.
inline constexpr int kNoiseSlot = 3;
inline constexpr int kSubSlot = 4;
inline constexpr int kSlotCount = kNoiseSlot + 2;   ///< 3 tabled + NOISE + SUB

/// **The reference's routing-matrix rows** (ticket `64`), in the order `RoutingSlot{N}` numbers them:
/// `OSC A`, `OSC B`, `OSC C`, `NOISE`, `SUB`, `Filter 1`, `Filter 2`. The five slots are the preset's own
/// slot numbering — `kNoiseSlot` and `kSubSlot` — and the last two are the voice filters, so a row is
/// addressed by the slot for 0..4 and by `kSlotCount + f` for a filter. The reference exposes exactly
/// these seven as host parameters (`A>BUS1` … `Filter 2>BUS2`), which is where the names and the order
/// come from.
inline constexpr int kRoutingRowCount = kSlotCount + kFilterSlots;   ///< 7

/// **The two FX busses' sends**, one gain per row per bus — amplitudes, not the preset's percent.
///
/// The reference's own numbers, measured through the host at the init patch (the plugin's normalised
/// value `n` is the preset's percent / 100):
///
///   * the send's gain is **cubic** in the preset's level — `n^3`, exact to the measurement (`0.05`,
///     `0.1`, `0.25`, `0.5`, `0.75` and `1.0` came back in the ratios 1/8000, 1/1000, 1/64, 1/8, 0.4219
///     and 1.0 of the full-level send);
///   * the send is a **parallel tap**: at full level with the bus at its own 0 dB point the render is
///     exactly twice the dry one, so the main path is not attenuated by the send;
///   * the tap is **before the voice filters**: with `Filter 1` enabled at a low cutoff the main path is
///     a hundredth of the dry render while the send still carries the bright unfiltered oscillator. The
///     `Filter 1`/`Filter 2` rows are therefore the signal *leaving* each filter, and the five source
///     rows are the source's own output;
///   * the master volume scales it: `Main Vol` at zero silences the bus as well as the main path.
///
/// Which of the two busses' sends a preset actually uses is the plugin's decode of
/// `RoutingSlot{N}/plainParams/kParamFXBus{1,2}Level`; this struct is what the voice reads.
struct BusSendSettings
{
    /// `[row][bus]`, bus 0 = BUS 1 and bus 1 = BUS 2. Zero is no send.
    std::array<std::array<double, 2>, kRoutingRowCount> gain {};

    bool any() const;
};

/// Where a voice's sends go: two stereo pairs, borrowed from the caller, one per bus.
///
/// A null pointer means "no such output" and the send to that bus is skipped, so a caller with no bus
/// buffers — every unit test that predates ticket `64` — pays nothing. A bus nothing is sent to is left
/// exactly as the caller left it, which is why the plugin clears them per block.
struct BusSends
{
    float* left[2] { nullptr, nullptr };
    float* right[2] { nullptr, nullptr };

    bool wanted() const;
};

/// The steal crossfade. Short enough to be a steal, long enough not to be a click.
inline constexpr double kStealFadeSeconds = 0.002;

/// Modulation spans — see the header note. One unit of a destination ramp is `kParamAmount` 100.
///
/// `kPitchModulationSpanSemitones` is `kParamPitch`'s own range (±12). The sub oscillator is the one
/// slot whose *octave* and *coarse pitch* are modulated too (28 rows in the bank), and each of those is
/// a semitone control with its own range, so each gets its own span rather than being folded into the
/// pitch one.
inline constexpr double kPitchModulationSpanSemitones = 12.0;
inline constexpr double kOctaveModulationSpanSemitones = 48.0;   ///< `kParamOctave`'s ±4 octaves
inline constexpr double kCoarseModulationSpanSemitones = 64.0;   ///< `kParamCoarsePit`'s ±64 semitones
inline constexpr double kVolumeModulationSpan = 1.0;
inline constexpr double kCutoffModulationSpan = 1.0;
inline constexpr double kResonanceModulationSpan = 100.0;

/// Destination addressing, from the compiled enums ticket 03 extracted (arm64 offsets in that ticket).
namespace voiceDestination
{
inline constexpr std::string_view kOscillatorClass = "Oscillator";
inline constexpr std::string_view kVoiceFilterClass = "VoiceFilter";

enum OscillatorParam
{
    kParamEnable = 0,
    kParamVolume = 1,
    kParamPan = 2,
    kParamOctave = 3,
    kParamPitch = 4,
    kParamFine = 5,
    kParamCoarsePit = 6,
};

enum VoiceFilterParam
{
    kParamFreq = 3,
    kParamReso = 4,
};
} // namespace voiceDestination

/// Where a voice is in its life.
enum class VoiceStage
{
    Idle,       ///< silent, reusable
    On,         ///< held
    Releasing,  ///< note off, envelope running down
    Stealing    ///< fading out ahead of a pending note
};

/// How the preset's mono and legato flags say to allocate.
enum class VoiceMode
{
    Poly,         ///< one voice per note
    Mono,         ///< one voice, retriggered on each note
    MonoLegato    ///< one voice, envelope runs through a legato note change
};

/// `Global{N}/plainParams/kParamMonoToggle` + `kParamLegato` -> the mode. Legato without mono is mono.
VoiceMode voiceModeFor (bool mono, bool legato);

/// A linear ADSR, per sample. Linear segments are a **stated convention** — the reference's envelope
/// curves are not established — but the shape is not what this ticket is about, and a linear envelope is
/// the honest placeholder until the fidelity suite can measure the real one.
struct AmpEnvelope
{
    double attack { 0.005 };
    double decay { 0.0 };
    double sustain { 1.0 };
    double release { 0.05 };

    double sampleRate { 48000.0 };
    double value { 0.0 };

    enum class Stage { Idle, Attack, Decay, Sustain, Release };
    Stage stage { Stage::Idle };

    double attackRate { 0.0 };
    double decayRate { 0.0 };
    double releaseRate { 0.0 };

    void prepare (double rate);
    void set (double attackSeconds, double decaySeconds, double sustainLevel, double releaseSeconds);
    void trigger();
    void releaseStage();

    double level() const { return value; }
    bool idle() const { return stage == Stage::Idle; }
    void advance();
};

/// One oscillator slot's preset settings. A table pointer is borrowed, never copied (a factory table is
/// up to 2 MB and every voice shares it).
struct OscillatorSettings
{
    bool enabled { false };
    const Wavetable* table { nullptr };
    double pitchSemitones { 0.0 };    ///< `kParamPitch` + 12 * `kParamOctave` + `kParamFine` / 100
    double volume { 1.0 };            ///< `kParamVolume`
    double framePosition { 0.0 };     ///< `kParamTablePos`
    int unison { 1 };                 ///< `kParamUnison`; 1 = off
    double unisonRangeSemitones { 0.0 };  ///< `kParamUnisonRange`, the total spread
    double unisonStereo { 0.0 };      ///< `kParamUnisonStereo`: percent spread; the sign mirrors it
    WarpMode warpMode[2] { WarpMode::None, WarpMode::None };
    double warpAmount[2] { 0.0, 0.0 };

    /// **What each warp slot is driven by**, decoded from the mode's name (`kPD_OSC` -> the other
    /// oscillator, `kFM_SUB` -> the sub, `kSelfPD` -> itself). Ticket `25` built the source-driven
    /// families to take their modulator as an argument and nothing ever handed them one, so the twelve
    /// warp modes whose name encodes a source were transparent in the product. This is the field that
    /// lets `Voice::render` call `setWarpModulator`.
    WarpSource warpSource[2] { WarpSource::None, WarpSource::None };
};

/// One filter slot's preset settings.
struct FilterSettings
{
    bool enabled { false };
    FilterSpec spec {};
    double cutoff { 1.0 };      ///< `kParamFreq`, normalised 0..1 (1 = open, for a disabled slot)
    double resonance { 0.0 };   ///< `kParamReso`, percent
    double drive { 0.0 };       ///< `kParamDrive`, percent
    double morph { 0.0 };       ///< `kParamVar`, 0..1

    /// The **drawn response**, for a `PoleZero` spec: the sections a preset inlines under `PZs[]`.
    ///
    /// Carried here rather than read from the document per block because `VoiceFilter::setPoleZero` is
    /// the only way the response reaches the engine, and the audio thread may not touch a document map.
    /// Empty for every other model, and for a `PoleZero` slot whose preset drew nothing — which is a
    /// pass-through, not a substituted filter.
    PoleZeroDescription poleZero {};
};

/// The NOISE slot's preset settings (`Oscillator3`).
///
/// `type` is **optional on purpose**: `kParamNoiseType`'s absence is the reference's *sample* mode, which
/// xerum does not render (the asset path is tickets 24/34's). An enabled NOISE slot with no colour
/// therefore renders silence *and* is reported by name, rather than being handed a colour the preset did
/// not ask for.
struct NoiseSettings
{
    bool enabled { false };
    std::optional<NoiseType> type {};
    double volume { 1.0 };    ///< `kParamVolume`
    bool randomPhase { false };   ///< `kParamRandomPhase != 0`
};

/// The SUB slot's preset settings (`Oscillator4`) — `Oscillator4/plainParams` plus `SubOsc4/plainParams`.
struct SubSettings
{
    bool enabled { false };
    SubShape shape { SubShape::Sine };
    /// `kParamOctave`, `kParamCoarsePit`, `kParamPitch` and `kParamFine` summed, in semitones. The engine
    /// takes the sum because that is what a frequency is: nothing downstream of the voice needs to know
    /// which of the four contributed.
    double pitchSemitones { 0.0 };
    double volume { 1.0 };
    double initialPhaseTurns { 0.0 };   ///< `kParamInitialPhase` / 360
    bool contiguousPhase { false };     ///< `kParamContiguousPhase`
};

/// Everything a preset says about a voice, shared by the whole pool.
struct VoiceSettings
{
    std::array<OscillatorSettings, kOscillatorSlots> oscillators {};
    std::array<FilterSettings, kFilterSlots> filters {};
    NoiseSettings noise {};
    SubSettings sub {};
    AmpEnvelope envelope {};
    VoiceMode mode { VoiceMode::Poly };
    double portamentoTime { 0.0 };      ///< seconds; 0 = instant
    double portamentoCurve { 50.0 };    ///< percent; 50 = linear (see `Voice::glidePitch`)
    bool portamentoAlways { false };    ///< glide even when no previous note is held

    /// **The bus sends** (ticket `64`). Not a bridged parameter — nothing in a reference preset puts the
    /// routing matrix on the host surface as a xerum parameter — so the plugin decodes it from the
    /// document, the same way it decodes the racks.
    BusSendSettings sends {};
};

/// One voice: oscillators, filters, an amplitude envelope, and the note it is playing.
///
/// Everything here is driven by `VoiceEngine`; a `Voice` is not meant to be used on its own, except in
/// tests.
class Voice
{
public:
    void prepare (double sampleRate);

    /// Apply preset settings. Rebuilds nothing that allocates; filter delay lines are sized in
    /// `prepare()`.
    void setSettings (const VoiceSettings& settings);
    const VoiceSettings& settings() const { return settings_; }

    /// The modulation generators' settings, owned by `VoiceEngine` and read (never copied) here.
    ///
    /// A pointer rather than a copy for a measured reason: one LFO's drawn shape is up to 256 points of
    /// three doubles, so a copy per voice would be 32 copies of the whole bank on every settings change.
    /// The engine sets it once in `prepare` and the object outlives every voice.
    void setModulationSources (const ModSourceSettings* settings) { sourceSettings_ = settings; }

    /// The voice's index in the pool. Carried into `ModulationInputs::voiceIndex`.
    void setVoiceIndex (int index) { voiceIndex_ = index; }
    int voiceIndex() const { return voiceIndex_; }

    /// Note-on. `startPitchSemitones` is where the pitch begins, in note-semitones: equal to the target
    /// for no glide, or the previous note for portamento.
    void start (int note, double velocity, double startPitchSemitones);

    /// Move to a new pitch without retriggering the envelope — mono legato, and the fall-back note when
    /// a mono stack is popped. `note` is the new note number, `targetPitchSemitones` its pitch (note - 69).
    void retarget (int note, double targetPitchSemitones);

    /// Note-off: the envelope releases.
    void release();

    /// Steal: fade out, then start `note`. The pending note is not heard until the fade completes.
    void steal (int note, double velocity, double startPitchSemitones);

    /// The note a stealing voice is about to start, or -1. A note released before the fade completes
    /// must be cancellable, or it would start after its own note-off and never stop.
    int pendingNote() const;

    /// Cancel the pending note if it matches, so the fade ends in silence instead of starting a note
    /// whose note-off has already arrived.
    void cancelPending (int note);

    /// Immediate silence, no fade. For `reset()` and for a stolen voice whose fade is not wanted.
    void kill();

    /// The age used by the stealing tie-break: a monotonic note-on counter, smaller = older.
    void setAge (long age) { age_ = age; }
    long age() const { return age_; }

    VoiceStage stage() const { return stage_; }
    bool active() const { return stage_ != VoiceStage::Idle; }
    bool releasing() const { return stage_ == VoiceStage::Releasing || stage_ == VoiceStage::Stealing; }
    int note() const { return note_; }
    double velocity() const { return velocity_; }

    /// The current amplitude-envelope level — the "quietest" measure the pool steals by.
    double level() const { return envelope_.level(); }

    /// The pitch the voice is actually at, in note-semitones, glide included.
    double pitchSemitones() const;

    /// Render `frames` samples into `outLeft`/`outRight`, adding. `matrix` may be null (no modulation);
    /// `inputs` is the engine's block inputs, which the voice augments with its own per-voice values.
    ///
    /// `sends` (ticket `64`) is where this voice's bus sends go, and its own levels are in `settings()`.
    /// Both may be empty: a caller with no busses gets exactly the render it got before the sends
    /// existed, because every tap is skipped when nothing is sent.
    void render (float* outLeft, float* outRight, const BusSends& sends, int frames, ModulationMatrix* matrix,
                 const ModulationInputs& inputs);

private:
    /// The pitch glide: a curved interpolation in semitones from where the note started to its target.
    double glidePitch() const;

    double sampleRate_ { 48000.0 };
    int voiceIndex_ { 0 };
    long age_ { 0 };

    VoiceSettings settings_ {};
    AmpEnvelope envelope_ {};

    std::array<std::array<WavetableOscillator, kMaxUnison>, kOscillatorSlots> oscillators_ {};

    /// **Two filters per slot, one per channel.** The voice bus is stereo from the oscillator's unison
    /// spread onward (see `render`), so a single mono filter here would sum the two channels back
    /// together and destroy exactly the width the oscillators just produced. Each channel's filter is
    /// independent, which doubles the filter state and its delay lines — sized in `prepare()`, off the
    /// audio thread.
    std::array<std::array<VoiceFilter, 2>, kFilterSlots> filters_ {};

    // The two fixed single-engine slots. One instance each: the bank never stores `kParamUnison` on
    // slots 3/4 (measured — 0 of 626), so there is no unison stack to allocate for.
    NoiseOscillator noise_ {};
    SubOscillator sub_ {};

    VoiceStage stage_ { VoiceStage::Idle };
    int note_ { 60 };
    double velocity_ { 1.0 };

    // Glide, in note-semitones. `glideSamplesTotal_ == 0` means no glide in progress.
    double glideFrom_ { 60.0 };
    double glideTarget_ { 60.0 };
    int glideSamplesTotal_ { 0 };
    int glideSamplesDone_ { 0 };

    // Steal fade.
    int stealSamplesTotal_ { 0 };
    int stealSamplesLeft_ { 0 };
    bool hasPending_ { false };
    int pendingNote_ { 60 };
    double pendingVelocity_ { 1.0 };
    double pendingStartPitch_ { 60.0 };

    // --- the modulation generators (ticket 48) ----------------------------------------------------------
    //
    // The per-voice half of `ModulationInputs`: the ten LFOs and the four matrix envelopes, plus the
    // voice's *own* samples, which are the audio-rate sources (`OSC A/B/C`, `Filter 1/2`, `NOISE`, `SUB`)
    // and the warp modulators. The samples are the previous frame's, one per source, which is what makes
    // a cross-oscillator warp causal without a second read of any oscillator.
    ModulationGenerator generator_ {};
    const ModSourceSettings* sourceSettings_ { nullptr };

    std::array<double, kSlotCount> lastSlotSample_ {};     ///< indexed like `kSlotCount`: A, B, C, NOISE, SUB
    std::array<double, kFilterSlots> lastFilterSample_ {};
};

/// The fixed voice pool. Allocates in `prepare()` and never again.
class VoiceEngine
{
public:
    /// `polyphony` is the active voice count, clamped to [1, kMaxVoices]. The pool is always `kMaxVoices`
    /// wide; this only decides how many of it the allocator will scan.
    void prepare (double sampleRate, int maxBlockFrames, int polyphony = 16);
    void reset();

    void setSettings (const VoiceSettings& settings);
    const VoiceSettings& settings() const { return settings_; }

    /// The matrix rows. Off the audio thread; `beginBlock` is what runs while audio does.
    void setModulationSlots (std::vector<ModSlot> slots) { matrix_.setSlots (std::move (slots)); }
    ModulationMatrix& modulation() { return matrix_; }

    /// Block-rate global modulation inputs (mod wheel, bend, macros). Per-voice values are filled in by
    /// the pool.
    void setGlobalInputs (const ModulationInputs& inputs) { globalInputs_ = inputs; }

    /// The modulation generators' settings: the ten LFOs and the four matrix envelopes (ticket 48).
    ///
    /// **Message thread only.** The block carries the drawn point arrays, so the swap is made while the
    /// audio thread is held out, the same handshake `setModulationSlots` uses — and the audio thread only
    /// ever *reads* the result, which is why a voice holds a pointer to it rather than a copy.
    void setModulationSources (const ModSourceSettings& settings) { sourceSettings_ = settings; }
    const ModSourceSettings& modulationSources() const { return sourceSettings_; }

    void setSustainPedal (bool down);

    /// Note on. Returns the index of the voice that took the note, or -1 if the note was dropped. In mono
    /// modes this is always 0.
    int noteOn (int note, double velocity);
    void noteOff (int note);

    /// Render `frames` samples of the whole pool, stereo, into `outLeft` and `outRight` (overwritten,
    /// not added to). Both must hold `frames` samples; the pool sums into each.
    ///
    /// `sends` is the mixer's other half (ticket `64`): the pool's voices add their bus sends into it, and
    /// a caller that passes no busses gets the pool's main output and nothing else. The bus buffers are
    /// **added to**, not overwritten, because each voice contributes its own — the caller clears them.
    void process (float* outLeft, float* outRight, const BusSends& sends, int frames);

    int polyphony() const { return polyphony_; }
    int activeVoiceCount() const;
    const Voice& voice (int index) const { return voices_[static_cast<std::size_t> (index)]; }

    /// For the mono stack and for tests: the note at the top of the held-note stack, or -1.
    int currentNote() const;

private:
    /// The allocator: which voice index a new note should take. The whole stealing policy lives here so
    /// tests can assert it without rendering.
    int chooseVoice() const;

    /// Start `note` on `index`. `fade` is true for a polyphonic steal (the old note is faded out first)
    /// and false for a mono retrigger, which restarts the same instrument and must not fade.
    void startVoice (int index, int note, double velocity, double startPitch, bool fade);

    /// Where a new note's pitch begins: the previous note's pitch when portamento applies, else the
    /// note itself.
    double glideOrigin (double targetSemitones, bool noteWasHeld) const;

    bool anyNoteHeld() const;
    void pushNote (int note);
    void removeNote (int note);

    std::array<Voice, kMaxVoices> voices_ {};
    VoiceSettings settings_ {};
    ModulationMatrix matrix_ {};
    ModulationInputs globalInputs_ {};

    /// The generators' settings and the **one** generator the whole pool shares — the LFOs a preset
    /// marks `Mono`. A voice's own generator advances only the other nine/non-mono ones.
    ModSourceSettings sourceSettings_ {};
    ModulationGenerator globalGenerator_ {};

    double sampleRate_ { 48000.0 };
    int polyphony_ { 16 };
    long noteCounter_ { 0 };

    bool sustainDown_ { false };
    std::array<bool, kMaxVoices> sustainHeld_ {};   ///< per voice: note-off deferred by the pedal
    bool monoSustainPending_ { false };

    // Mono held-note stack. Newest on top; the top note is the one sounding.
    std::array<int, kMaxVoices> noteStack_ {};
    int noteStackSize_ { 0 };

    // The last note the instrument played, for polyphonic portamento's glide origin.
    double lastPitch_ { 69.0 };
    bool lastPitchValid_ { false };
};

} // namespace xerum::dsp
