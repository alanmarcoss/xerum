// xerum::dsp — the voice filter models.
//
// Ticket `04` established that the reference's **83 named filter types are not 83 filters**. A name
// decomposes into a response and a number, and the number is the **slope in dB/oct**:
//
//   * `L6/12/18/24`, `H6/12/18/24`, `B12/24`, `N12/24` are one state-variable filter at one of four
//     slopes — 6, 12, 18, 24 dB/oct, which is 1, 2, 3, 4 poles;
//   * **two** letters are a dual SVF, primary plus secondary: `LH12` is low then high, `PP12` two
//     peaks, `BN12` band then notch;
//   * **three** letters are the morphing SVF, morphing through the three responses they name in order:
//     `LBH24` low→band→high, `LNH24` low→notch→high, `BPN12` band→peak→notch;
//   * `MgL24`, `LadderMg`, `LadderEMS`, `LadderAcid`, `DirtyMg` are one ladder in different flavours;
//   * `Comb*`, `Combs`, `Flange*`, `DistComb*` are one delay line with a feedback polarity (`P`/`N`)
//     and an optional low/high filter in the loop;
//   * `Phase*` and `Allpasses` are an allpass cascade with a stage count;
//   * `Diffuser` is a chain of delayed allpasses;
//   * `PZ_SVF` is not a circuit at all but a **drawn response**: eight second-order pole/zero sections
//     inlined in the preset (tickets `02`/`04` — presets never reference the `PZ Filter/` files).
//
// **On the conventions.** Ticket `04` measured what each parameter *is* — a normalised logarithmic
// cutoff, a percent resonance — but not the exact curves, and the reference's curves can only be
// measured with the fidelity suite (tickets `09`/`30`). So each mapping here is a stated convention
// rather than a buried guess, and the tests assert the properties that are independent of it: an
// 18 dB/oct filter measures 18 dB/oct whatever the cutoff curve turns out to be.
//
// A name outside the table is not silently treated as "no filter": `filterSpecFromName` reports it as
// `Unimplemented`, and the caller reports *that*, per the import contract.

#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace xerum::dsp
{

/// A slope in dB/oct as a pole count: 6/12/18/24 is 1/2/3/4.
inline constexpr int polesForSlope (int dBPerOctave) { return dBPerOctave / 6; }
inline constexpr int slopeForPoles (int poles) { return poles * 6; }

/// The response, independent of the circuit that produces it.
enum class FilterKind
{
    LowPass,
    HighPass,
    BandPass,
    Notch,
    Peak,
    LowShelf,
    HighShelf,
    AllPass
};

std::string_view toString (FilterKind kind);

/// The circuit family — what has to be built, not what the preset called it.
enum class FilterModel
{
    Svf,           ///< TPT state-variable, cascaded for slopes above 12 dB/oct
    DualSvf,       ///< two SVFs summed: a primary response plus a secondary one
    MorphSvf,      ///< SVFs whose outputs are mixed between three responses
    Ladder,        ///< one-pole stages with feedback, Moog-style, in flavours
    Comb,          ///< delay line with feedback, an optional loop filter and a polarity
    Allpass,       ///< first-order allpass cascade: `Phase*`, `Allpasses`
    Diffuser,      ///< allpasses with real delays
    PoleZero,      ///< `PZ_SVF`: eight second-order sections drawn in the preset
    Unimplemented  ///< named and deliberately not built — reported, never substituted
};

std::string_view toString (FilterModel model);

/// How a comb's feedback path is filtered, and which way it feeds back.
struct CombSpec
{
    bool negative { false };      ///< `N` inverts the feedback, `P` does not: `CombN` vs `CombP`
    int lowPassPoles { 0 };       ///< `L6`/`HL6` put a low pass in the loop
    int highPassPoles { 0 };      ///< `H6`/`HL6` put a high pass in the loop
    bool distorted { false };     ///< `DistComb*` drives the loop
};

/// Everything a model needs in order to be built.
struct FilterSpec
{
    FilterModel model { FilterModel::Unimplemented };
    FilterKind kind { FilterKind::LowPass };         ///< the response; `MorphSvf`: the first of three
    FilterKind secondary { FilterKind::LowPass };    ///< `DualSvf` the second; `MorphSvf` the middle
    FilterKind tertiary { FilterKind::LowPass };     ///< `MorphSvf`: the third
    int poles { 2 };
    int stages { 0 };                                ///< `Allpass`/`Diffuser`: how many stages
    CombSpec comb {};

    /// `Ladder` flavours: how hard the feedback stage saturates relative to the plain ladder.
    ///
    /// The four flavour names are four circuits in the reference, not one circuit under four names, and
    /// ticket `04` could not measure what distinguishes them — the guide names them and stops there. So
    /// the *structure* is shared and this is a stated constant per flavour, to be replaced by a measured
    /// curve in the fidelity suite. Recording it as an approximation is the honest version; shipping
    /// them identical would be a substitution, and reporting all four would drop 17% of filter usage.
    double flavourDrive { 1.0 };
};

/// The table, derived from the names rather than transcribed from a list. A name the table does not
/// cover comes back `Unimplemented`.
FilterSpec filterSpecFromName (std::string_view name);

/// One second-order pole/zero section, as a preset stores it.
///
/// `poleFrequency`/`zeroFrequency` are **fractions of Nyquist** — the reference normalises onto 0–1 and
/// many factory values snap to a 1/16 grid — and `poleQ`/`zeroQ` are its 0–1 "Q", which ticket `04`
/// measured as *not* a textbook Q.
struct PoleZeroSection
{
    double poleFrequency { 0.0 };
    double zeroFrequency { 0.0 };
    double poleQ { 0.0 };
    double zeroQ { 0.0 };
    double gain { 1.0 };
    int type { -1 };      ///< the reference's per-section label, 0–7; recorded, not used in the response
};

/// A drawn response: the pole/zero objects a `PZ_SVF` slot inlines.
///
/// **Two levels, and the difference matters.** One object is eight second-order sections — that is what
/// a `PZ Filter/*.XferFilterPZSingle` file holds, and what one entry of a preset's `PZs` array holds.
/// A preset may inline up to four such objects (the `Multi` form), which is up to **32** second-order
/// sections in series. Modelling one object as the whole filter would build a quarter of a
/// `Multi` — a wrong response, not a missing one.
struct PoleZeroDescription
{
    static constexpr std::size_t kSectionsPerObject = 8;
    static constexpr std::size_t kMaxObjects = 4;
    static constexpr std::size_t kMaxSections = kSectionsPerObject * kMaxObjects;

    std::array<PoleZeroSection, kMaxSections> sections {};
    std::size_t count { 0 };

    bool empty() const { return count == 0; }
};

/// Append one pole/zero object — the eight sections a preset or a `PZ Filter/*` file stores — to a
/// description. A null array means the field was absent, which the reference does write.
void appendPoleZeroObject (PoleZeroDescription& description, const double* poleFrequencies,
                           const double* zeroFrequencies, const double* poleQs, const double* zeroQs,
                           const double* gains, const int* types);

/// The voice filter: one filter, whichever model the preset named.
class VoiceFilter
{
public:
    void prepare (double sampleRate);

    /// Bind a model. An `Unimplemented` spec passes audio through unchanged, which is what a reported
    /// filter means: the caller has already told the user the preset asked for something we cannot build.
    void setSpec (const FilterSpec& spec);
    const FilterSpec& spec() const { return spec_; }

    void setCutoff (double normalised);
    double cutoffHz() const { return cutoffHz_; }

    /// `DualSvf` only: the second section's cutoff. The reference carries it in the type's VAR, whose
    /// mapping is not established, so it defaults to the primary cutoff rather than being invented.
    void setSecondaryCutoff (double normalised);

    /// `kParamReso`, 0–100 percent.
    void setResonance (double percent);

    /// `kParamDrive`, 0–100 percent: saturation in the resonance path, where the model has one.
    void setDrive (double percent);

    /// The morph position for `MorphSvf`: 0 is the first named response, 0.5 the second, 1 the third.
    void setMorph (double position);

    /// The drawn response, for `PoleZero`. Reading one out of a preset document is the model layer's
    /// job; this takes the numbers.
    void setPoleZero (const PoleZeroDescription& description);

    void reset();

    float process (float input);

    /// `kParamFreq` 0–1, logarithmic in frequency, mapped onto the audio band.
    static double cutoffHzFromNormalised (double normalised);

    /// The table's own claim, for the report and for the coverage test: is this name one we build?
    static bool implements (std::string_view name);

private:
    /// A TPT state-variable section (Zavalishin). Its state is updated in the order that keeps the
    /// filter stable at any cutoff below Nyquist — which a naive biquad design does not, and stability
    /// at 44.1 and 96 kHz is an acceptance criterion.
    struct SvfSection
    {
        double g { 0.0 };
        double k { 1.0 };
        double a1 { 0.0 };
        double a2 { 0.0 };
        double a3 { 0.0 };
        double ic1 { 0.0 };
        double ic2 { 0.0 };

        // All three outputs of one update, so a morphing filter can mix them without advancing the
        // state three times.
        double lowpass { 0.0 };
        double highpass { 0.0 };
        double bandpass { 0.0 };

        void set (double cutoffHz, double sampleRate, double damping);
        void clear() { ic1 = 0.0; ic2 = 0.0; }
        void update (double input);
        double take (FilterKind kind, double shelfGain, double dry) const;
    };

    struct OnePole
    {
        double coefficient { 0.0 };
        double state { 0.0 };

        void set (double cutoffHz, double sampleRate);
        void clear() { state = 0.0; }
        double lowpass (double input);
    };

    struct FirstOrderAllpass
    {
        double coefficient { 0.0 };
        double state { 0.0 };

        void set (double coefficient);
        void clear() { state = 0.0; }
        double process (double input);
    };

    /// A delay line with interpolation, used by the comb and by the diffuser's allpasses.
    struct DelayLine
    {
        std::vector<double> buffer {};
        std::size_t write { 0 };

        void resize (std::size_t samples);
        void clear();
        double read (double delaySamples) const;
        void writeSample (double value);
    };

    struct Biquad
    {
        double b0 { 1.0 }, b1 { 0.0 }, b2 { 0.0 }, a1 { 0.0 }, a2 { 0.0 };
        double x1 { 0.0 }, x2 { 0.0 }, y1 { 0.0 }, y2 { 0.0 };

        void clear() { x1 = x2 = y1 = y2 = 0.0; }
        double process (double input);
    };

    double sampleRate_ { 48000.0 };
    FilterSpec spec_ {};
    double cutoffHz_ { 1000.0 };
    double secondaryCutoffHz_ { 1000.0 };
    double resonance_ { 0.0 };      ///< 0..1
    double drive_ { 0.0 };          ///< 0..1
    double morph_ { 0.0 };          ///< 0..1

    // Four sections: one branch of a dual filter uses [0..1] and the other [2..3], and a plain filter
    // uses [0..1] cascaded. A dual 24 dB/oct needs both pairs.
    std::array<SvfSection, 4> sections_ {};
    std::array<OnePole, 4> ladder_ {};
    OnePole sparePole_ {};
    double ladderFeedbackState_ { 0.0 };

    std::array<FirstOrderAllpass, 48> allpasses_ {};
    std::array<DelayLine, 4> diffuserLines_ {};
    DelayLine comb_ {};
    std::array<OnePole, 2> loopFilters_ {};
    std::array<Biquad, PoleZeroDescription::kMaxSections> pz_ {};
    std::size_t poleZeroCount_ { 0 };

    void rebuild();
    double shelfGain() const;
    float processSvf (float input);
    float processDualSvf (float input);
    float processMorphSvf (float input);
    float processLadder (float input);
    float processComb (float input);
    float processAllpass (float input);
    float processDiffuser (float input);
    float processPoleZero (float input);
};

} // namespace xerum::dsp
