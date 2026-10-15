#include "limiter.h"

#include <algorithm>
#include <cmath>

namespace xerum::dsp
{

void Limiter::prepare (const double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    // The lookahead **is** the latency, rounded to whole samples because that is the only unit a host
    // compensates in. 1.5 ms is 66 samples at 44.1 kHz, 72 at 48 kHz and 144 at 96 kHz.
    const auto requested = static_cast<long long> (std::llround (lookaheadMs_ * sampleRate_ / 1000.0));
    latency_ = static_cast<int> (std::max<long long> (1, requested));
    capacity_ = latency_ + 1;

    delayLeft_.assign (static_cast<std::size_t> (capacity_), 0.0f);
    delayRight_.assign (static_cast<std::size_t> (capacity_), 0.0f);
    requirement_.assign (static_cast<std::size_t> (capacity_), 1.0f);
    window_.assign (static_cast<std::size_t> (capacity_), 0);

    // A one-pole whose time constant is the release: 63 % of the way back to unity after `releaseMs_`.
    const auto releaseSamples = std::max (1.0, releaseMs_ * sampleRate_ / 1000.0);
    releaseCoefficient_ = static_cast<float> (1.0 - std::exp (-1.0 / releaseSamples));

    // The steepest descent that still reaches zero from unity within one lookahead — which is exactly the
    // warning the windowed minimum gives, so the ramp can always arrive before the peak does. The clamp in
    // `process` is what makes the ceiling unconditional; this only shapes the approach.
    attackStep_ = 1.0f / static_cast<float> (latency_);

    ceiling_ = static_cast<float> (std::pow (10.0, ceilingDb_ / 20.0));

    clearState();
}

void Limiter::clearState() noexcept
{
    std::fill (delayLeft_.begin(), delayLeft_.end(), 0.0f);
    std::fill (delayRight_.begin(), delayRight_.end(), 0.0f);
    std::fill (requirement_.begin(), requirement_.end(), 1.0f);

    position_ = 0;
    windowHead_ = 0;
    windowSize_ = 0;
    gain_ = 1.0;
}

void Limiter::reset()
{
    clearState();
}

void Limiter::setEnabled (const bool enabled) noexcept
{
    if (enabled == enabled_)
        return;

    enabled_ = enabled;

    // Re-arm the detector on the way in. The window is filled from the samples as they go by, so a window
    // left over from before the ceiling was switched off would be an envelope computed from samples that
    // have already passed. The delay line is **not** cleared: the latency does not move, and the audio
    // already on its way through is the audio the caller is about to hear.
    if (enabled_)
    {
        std::fill (requirement_.begin(), requirement_.end(), 1.0f);
        windowHead_ = 0;
        windowSize_ = 0;
    }
}

void Limiter::setCeilingDb (const double decibels) noexcept
{
    ceilingDb_ = std::clamp (decibels, kMasterCeilingMinDb, kMasterCeilingMaxDb);
    ceiling_ = static_cast<float> (std::pow (10.0, ceilingDb_ / 20.0));
}

void Limiter::setReleaseMs (const double milliseconds) noexcept
{
    releaseMs_ = std::max (0.01, milliseconds);

    const auto releaseSamples = std::max (1.0, releaseMs_ * sampleRate_ / 1000.0);
    releaseCoefficient_ = static_cast<float> (1.0 - std::exp (-1.0 / releaseSamples));
}

void Limiter::process (float* left, float* right, int frames) noexcept
{
    if (left == nullptr || right == nullptr || frames <= 0 || latency_ <= 0)
        return;

    // **Off, and at unity: a delay line and nothing else.** The gain is exactly `1.0`, so the
    // multiplication would be exact anyway; skipping it says out loud that nothing here touches the
    // sample values. The detector is re-armed by `setEnabled(true)` rather than kept warm, because a
    // stale window would be an envelope computed from samples that have already gone by.
    if (! enabled_ && gain_ == 1.0)
    {
        for (int i = 0; i < frames; ++i)
        {
            const auto slot = slotOf (position_);
            const auto read = slotOf (position_ - latency_);

            delayLeft_[slot] = left[i];
            delayRight_[slot] = right[i];

            left[i] = delayLeft_[read];
            right[i] = delayRight_[read];

            ++position_;
        }

        return;
    }

    const auto ceiling = ceiling_;

    for (int i = 0; i < frames; ++i)
    {
        const auto xl = left[i];
        const auto xr = right[i];

        // The detector is the larger channel, so one gain attenuates both and the stereo image does not
        // wander when only one side peaks — the reference's `LinkAmount = 1`.
        const auto magnitude = std::max (std::fabs (xl), std::fabs (xr));

        // `1` wherever the sample is within the ceiling, which is what makes the transparent case exact
        // rather than merely close: a division that would land on 1.0000001 is never taken.
        const auto requirement = magnitude <= ceiling ? 1.0f : ceiling / magnitude;

        const auto index = position_;
        const auto write = slotOf (index);

        delayLeft_[write] = xl;
        delayRight_[write] = xr;
        requirement_[write] = requirement;

        // The window is `[index - latency_, index]`: the newest `latency_ + 1` required-gain values.
        const auto oldest = index - latency_;

        while (windowSize_ > 0 && window_[windowHead_] < oldest)
        {
            windowHead_ = (windowHead_ + 1) % capacity_;
            --windowSize_;
        }

        while (windowSize_ > 0
               && requirement_[slotOf (window_[(windowHead_ + windowSize_ - 1) % capacity_])] >= requirement)
            --windowSize_;

        window_[(windowHead_ + windowSize_) % capacity_] = index;
        ++windowSize_;

        const auto lookaheadMinimum = requirement_[slotOf (window_[windowHead_])];

        const auto read = slotOf (index - latency_);
        const auto delayedLeft = delayLeft_[read];
        const auto delayedRight = delayRight_[read];
        const auto requiredNow = requirement_[read];

        // The target is the windowed minimum, so the reduction has already begun when the peak reaches
        // the output; when the ceiling is off the target is unity and the gain releases back to it.
        const auto target = static_cast<double> (enabled_ ? lookaheadMinimum : 1.0f);

        if (target < gain_)
            gain_ = std::max (target, gain_ - static_cast<double> (attackStep_));
        else
            gain_ += (target - gain_) * static_cast<double> (releaseCoefficient_);

        // **The brickwall.** Not the ramp above — this line. A sample is scaled by no more than the
        // factor that puts it on the ceiling, whatever the envelope was doing, so the output cannot
        // exceed `ceiling_` even if the ramp was too slow or the detector was caught out.
        if (enabled_ && gain_ > static_cast<double> (requiredNow))
            gain_ = static_cast<double> (requiredNow);

        if (gain_ > 1.0)
            gain_ = 1.0;

        // Multiplied in `double` so a gain that is within a hair of unity **rounds to the delayed sample
        // exactly**, which is what makes the transparency invariant hold after a release and not only
        // before the first engagement. See the note on `gain_`.
        left[i] = static_cast<float> (static_cast<double> (delayedLeft) * gain_);
        right[i] = static_cast<float> (static_cast<double> (delayedRight) * gain_);

        ++position_;
    }
}

} // namespace xerum::dsp
