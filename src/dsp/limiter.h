// xerum::dsp — the master ceiling. Ticket `58`.
//
// ## What it is, and where the numbers come from
//
// A **lookahead brickwall**: the reference figures this models are Ableton's Limiter at its defaults, and
// they are taken from **Ableton's own factory default preset** rather than from recall —
// `Contents/App-Resources/Core Library/Defaults/Audio Effects/Limiter.adv` inside the installed
// `Ableton Live 12 Suite.app` (gzipped XML, `Limiter` device). Decompressed, its defaults are
//
//   | control       | stored value        | reads as      |
//   |---------------|---------------------|---------------|
//   | `Gain`        | `0`                 | 0 dB (unity)  |
//   | `Ceiling`     | `-0.2999997139`     | **-0.30 dB**  |
//   | `Release`     | `99.9999542`        | **100 ms**    |
//   | `AutoRelease` | `false`             | manual release |
//   | `Lookahead`   | `0`                 | **1.5 ms** (index 0 of the enumerated 1.5 / 3 / 6 ms) |
//   | `Routing`     | `0`                 | Left/Right    |
//   | `Mode`        | `0`                 | Standard (the manual's stated default) |
//   | `LinkAmount`  | `1`                 | 100 % — gain reduction shared across the channels |
//
// The enumerations are confirmed by the same app's Max reference for the device,
// `…/packages/ableton-dsp/docs/abl.device.limiter~.maxref.xml`: `enumvals` of size 3 for lookahead
// (`1.5 ms`, `3 ms`, `6 ms`), of size 3 for the ceiling mode (`Standard`, `Soft clip`, `True peak`) and
// of size 2 for the routing (`Left/Right`, `Mid/Side`). The Live 12 manual's effect reference (§29.24)
// supplies the shape rather than the numbers: the ceiling "sets the absolute maximum level that Limiter
// will output", "if your incoming signal level has no peaks that are higher than the ceiling, no limiting
// will be applied", and lookahead "introduces latency: the longer the lookahead time, the higher the
// latency". Both the stored preset and the manual were read on this machine on 2026-10-15.
//
// ## The four properties this implementation is built to have
//
// 1. **Brickwall, unconditionally.** The gain applied to a sample is never greater than
//    `ceiling / |sample|`, which is enforced by a clamp rather than by the envelope's shape — so no
//    amount of transient behaviour can push a sample over the ceiling. See `process`.
// 2. **Transparent when it is not engaging — exactly.** When every sample lies within the ceiling the
//    gain is unity and the output is the delayed input times unity, which IEEE 754 makes bit-identical to
//    the delayed input. Nothing about this limiter colours a signal it does not attenuate, which is what
//    stops a safety limiter rewriting the fidelity work underneath it. The envelope state is a `double`
//    precisely so that this survives a *release* and not only the untouched case; see the note on `gain_`.
// 3. **Lookahead-shaped attack.** The reduction is begun a whole lookahead *before* the peak reaches the
//    output and ramps down over it, instead of being applied as a step at the peak.
// 4. **Stereo-linked.** The detector is the larger of the two channels, so the two are attenuated by one
//    gain — the reference's `LinkAmount = 1`, and no "wobble" of the stereo image on a one-sided peak.
//
// ## Latency, and why it does not move
//
// The lookahead **is** the latency: `round(lookaheadMs * sampleRate / 1000)` samples, which the plugin
// declares to the host (`setLatencySamples`). The delay line runs whether the ceiling is being enforced
// or not, so the stage's latency does **not** change when the on/off parameter moves — a constant
// declared latency is what a host's delay compensation can rely on, and a latency that changed under
// automation would be a worse bug than the clipping this exists to fix. Turning the limiter off pins
// the gain to unity (over the release, so a toggle does not click); it does not remove the delay.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xerum::dsp
{

/// The reference's default ceiling, release and lookahead, in the units the reference stores them in.
/// Taken from the factory default preset described at the top of this file.
inline constexpr double kMasterCeilingDb = -0.30;
inline constexpr double kMasterReleaseMs = 100.0;
inline constexpr double kMasterLookaheadMs = 1.5;

/// The reference's own range for the ceiling control (`Ceiling`'s `MidiControllerRange` is `-24 … 0`).
inline constexpr double kMasterCeilingMinDb = -24.0;
inline constexpr double kMasterCeilingMaxDb = 0.0;

/// A lookahead limiter at the reference's defaults. One instance is a master stage, called last.
///
/// `prepare` sizes every buffer, and `process` allocates nothing and takes no lock — the same contract
/// the rest of this layer keeps.
class Limiter
{
public:
    /// Size the delay line and the detector for `sampleRate`. Sizes the lookahead from the value set by
    /// `setLookaheadMs`, so that setter takes effect here and not before.
    void prepare (double sampleRate);

    /// Clear the delay line, the detector and the envelope to unity. Called by `prepare`, and by a caller
    /// that wants a defined starting state (a fresh render). Note that **`setEnabled` does not call this**:
    /// switching the ceiling off clears only the detector, so the audio already in the delay line keeps
    /// flowing and the declared latency does not move.
    void reset();

    /// Off means the ceiling is **not enforced**: the gain is released back to unity, so the output
    /// becomes the delayed input exactly. The delay line keeps running either way, which is what keeps
    /// the declared latency constant across the toggle.
    void setEnabled (bool enabled) noexcept;
    bool enabled() const noexcept { return enabled_; }

    /// The ceiling, in dBFS. Clamped to the reference's own range. The default is `kMasterCeilingDb`.
    void setCeilingDb (double decibels) noexcept;
    double ceilingDb() const noexcept { return ceilingDb_; }

    /// How long the gain takes to return to unity once the signal falls back below the ceiling. The
    /// reference's default is 100 ms.
    void setReleaseMs (double milliseconds) noexcept;

    /// The lookahead in milliseconds, which is also the latency. Sizes the delay line at the next
    /// `prepare`, because latency must not move while a host is compensating for it.
    void setLookaheadMs (double milliseconds) noexcept { lookaheadMs_ = milliseconds; }
    double lookaheadMs() const noexcept { return lookaheadMs_; }

    /// The delay this stage introduces at the sample rate it was prepared at. Constant across the on/off
    /// parameter, by design — see the header note.
    int latencySamples() const noexcept { return latency_; }

    /// The gain the most recent sample was scaled by — `1` when the limiter is not engaging, and `< 1`
    /// by exactly the factor that puts the peak on the ceiling when it is. For metering and for tests;
    /// never read on the audio path.
    double currentGain() const noexcept { return gain_; }

    /// In place, `frames` long. Stereo; the two channels are attenuated together.
    void process (float* left, float* right, int frames) noexcept;

private:
    /// The slot an absolute sample index occupies in the `latency_ + 1`-long rings. The window a sample
    /// can be read from is `[position - latency, position]`, and `latency_ + 1` distinct slots hold it.
    int slotOf (std::int64_t index) const noexcept
    {
        const auto wrapped = index % static_cast<std::int64_t> (capacity_);
        return static_cast<int> (wrapped < 0 ? wrapped + capacity_ : wrapped);
    }

    void clearState() noexcept;

    double sampleRate_ { 48000.0 };
    double ceilingDb_ { kMasterCeilingDb };
    double releaseMs_ { kMasterReleaseMs };
    double lookaheadMs_ { kMasterLookaheadMs };

    float ceiling_ { 1.0f };        ///< `ceilingDb_` as a linear amplitude
    float releaseCoefficient_ { 0.0f };
    float attackStep_ { 1.0f };     ///< the largest gain change one sample may make while descending

    int latency_ { 0 };
    int capacity_ { 0 };            ///< `latency_ + 1`

    bool enabled_ { true };

    /// **The envelope state is a `double`, and that is a correctness decision rather than a precision
    /// one.** As a `float` near unity the release adder saturates — the step `(1 - gain) * coefficient`
    /// falls below half an ulp of 1.0 and the gain sticks at roughly `1 - 3e-8 * releaseSamples`, which at
    /// the default release is about `-77 dB` short of unity and never arrives. In `double` the decay
    /// continues until the perturbation is far below a `float` ulp, so the product below rounds to the
    /// delayed sample **exactly** — and the transparency invariant holds after a release, not only before
    /// the first one.
    double gain_ { 1.0 };

    /// The absolute index of the next sample to be written. Absolute rather than wrapped so the detector
    /// can tell "older than the window" from "newer" without a separate count.
    std::int64_t position_ { 0 };

    std::vector<float> delayLeft_ {};
    std::vector<float> delayRight_ {};

    /// One required-gain value per sample: `min(1, ceiling / |sample|)`, so `1` wherever the sample is
    /// within the ceiling. The windowed minimum over these is the envelope's target.
    std::vector<float> requirement_ {};

    /// A monotonic queue of absolute indices into `requirement_`, `requirement_` non-decreasing from
    /// front to back, so its front is the window's minimum in O(1) amortised per sample.
    std::vector<std::int64_t> window_ {};
    int windowHead_ { 0 };
    int windowSize_ { 0 };
};

} // namespace xerum::dsp
