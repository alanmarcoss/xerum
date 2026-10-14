#include "spectral_morph.h"

#include <algorithm>
#include <cmath>

namespace xerum::dsp
{

namespace
{

constexpr double kTwoPi = 6.283185307179586476925286766559;

} // namespace

double wrapToPi (double radians)
{
    // remainder(x, 2pi) is x - round(x/2pi) * 2pi, i.e. the shortest signed distance to a multiple of a
    // turn, and it already lands in [-pi, pi].
    return std::remainder (radians, kTwoPi);
}

SpectralMorph::SpectralMorph (std::size_t frameSize)
    : frameSize_ (frameSize)
{
    if (frameSize_ < 2)
        return;

    powerOfTwo_ = (frameSize_ & (frameSize_ - 1)) == 0;

    // Radix-2 needs half a turn's worth of twiddles; the direct transform indexes a full turn. One pair
    // of tables serves both.
    const std::size_t count = powerOfTwo_ ? frameSize_ / 2 : frameSize_;
    cosTable_.resize (count);
    sinTable_.resize (count);

    for (std::size_t i = 0; i < count; ++i)
    {
        const double angle = kTwoPi * double (i) / double (frameSize_);
        cosTable_[i] = std::cos (angle);
        sinTable_[i] = std::sin (angle);
    }

    lowerRe_.assign (frameSize_, 0.0);
    lowerIm_.assign (frameSize_, 0.0);
    upperRe_.assign (frameSize_, 0.0);
    upperIm_.assign (frameSize_, 0.0);

    // Only the non-power-of-two path needs a separate transform output, so the real path does not carry
    // 32 KiB of scratch it will never touch.
    if (! powerOfTwo_)
    {
        dftRe_.assign (frameSize_, 0.0);
        dftIm_.assign (frameSize_, 0.0);
    }
}

void SpectralMorph::transform (double* re, double* im, bool inverse)
{
    const std::size_t n = frameSize_;

    if (! powerOfTwo_)
    {
        // Direct transform, O(n^2): the shipped format is 2048 samples per cycle and always a power of
        // two, so this branch exists only so that an unusual frame size is morphed rather than refused.
        // It is a load-time path; nothing here is called per sample.
        //
        // The sums have to land in scratch and be copied back afterwards: bin k is a sum over every
        // input bin, so writing bin k in place would feed the already-transformed value into bin k+1.
        for (std::size_t k = 0; k < n; ++k)
        {
            double sumRe = 0.0;
            double sumIm = 0.0;

            for (std::size_t i = 0; i < n; ++i)
            {
                const auto index = (k * i) % n;
                const double c = cosTable_[index];
                const double s = inverse ? -sinTable_[index] : sinTable_[index];

                // (re[i] + i im[i]) * (c + i s)
                sumRe += re[i] * c - im[i] * s;
                sumIm += re[i] * s + im[i] * c;
            }

            dftRe_[k] = sumRe;
            dftIm_[k] = sumIm;
        }

        for (std::size_t i = 0; i < n; ++i)
        {
            re[i] = inverse ? dftRe_[i] / double (n) : dftRe_[i];
            im[i] = inverse ? dftIm_[i] / double (n) : dftIm_[i];
        }

        return;
    }

    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;

        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;

        j ^= bit;

        if (i < j)
        {
            std::swap (re[i], re[j]);
            std::swap (im[i], im[j]);
        }
    }

    for (std::size_t length = 2; length <= n; length <<= 1)
    {
        const std::size_t half = length >> 1;
        const std::size_t stride = n / length;

        for (std::size_t start = 0; start < n; start += length)
        {
            for (std::size_t k = 0; k < half; ++k)
            {
                const double wr = cosTable_[k * stride];
                const double wi = inverse ? sinTable_[k * stride] : -sinTable_[k * stride];

                const double xr = re[start + k + half];
                const double xi = im[start + k + half];

                const double tr = xr * wr - xi * wi;
                const double ti = xr * wi + xi * wr;

                const double lr = re[start + k];
                const double li = im[start + k];

                re[start + k] = lr + tr;
                im[start + k] = li + ti;
                re[start + k + half] = lr - tr;
                im[start + k + half] = li - ti;
            }
        }
    }

    if (inverse)
        for (std::size_t i = 0; i < n; ++i)
        {
            re[i] /= double (n);
            im[i] /= double (n);
        }
}

void SpectralMorph::morph (const float* lower, const float* upper, float t, float* out)
{
    if (frameSize_ < 2 || lower == nullptr || upper == nullptr || out == nullptr)
        return;

    t = std::clamp (t, 0.0f, 1.0f);

    if (t <= 0.0f)
    {
        std::copy (lower, lower + frameSize_, out);
        return;
    }

    if (t >= 1.0f)
    {
        std::copy (upper, upper + frameSize_, out);
        return;
    }

    for (std::size_t i = 0; i < frameSize_; ++i)
    {
        lowerRe_[i] = double (lower[i]);
        lowerIm_[i] = 0.0;
        upperRe_[i] = double (upper[i]);
        upperIm_[i] = 0.0;
    }

    transform (lowerRe_.data(), lowerIm_.data(), false);
    transform (upperRe_.data(), upperIm_.data(), false);

    // DC is self-conjugate: no partner bin to mirror, so the result has to stay real on its own and the
    // arc rule does not apply. Blend the value itself.
    lowerRe_[0] = (1.0 - double (t)) * lowerRe_[0] + double (t) * upperRe_[0];
    lowerIm_[0] = 0.0;

    // Every other bin below Nyquist pairs with one above it, so the magnitude is morphed once and the
    // conjugate is written for free.
    for (std::size_t k = 1; k <= (frameSize_ - 1) / 2; ++k)
    {
        const double aRe = lowerRe_[k];
        const double aIm = lowerIm_[k];
        const double bRe = upperRe_[k];
        const double bIm = upperIm_[k];

        const double magnitude = (1.0 - double (t)) * std::hypot (aRe, aIm) + double (t) * std::hypot (bRe, bIm);
        const double phase = std::atan2 (aIm, aRe)
                           + double (t) * wrapToPi (std::atan2 (bIm, bRe) - std::atan2 (aIm, aRe));

        const double re = magnitude * std::cos (phase);
        const double im = magnitude * std::sin (phase);

        lowerRe_[k] = re;
        lowerIm_[k] = im;
        lowerRe_[frameSize_ - k] = re;
        lowerIm_[frameSize_ - k] = -im;
    }

    // Nyquist, like DC, is its own partner — present only when the frame size is even.
    if (frameSize_ % 2 == 0)
    {
        const std::size_t nyquist = frameSize_ / 2;
        lowerRe_[nyquist] = (1.0 - double (t)) * lowerRe_[nyquist] + double (t) * upperRe_[nyquist];
        lowerIm_[nyquist] = 0.0;
    }

    transform (lowerRe_.data(), lowerIm_.data(), true);

    for (std::size_t i = 0; i < frameSize_; ++i)
        out[i] = float (lowerRe_[i]);
}

Wavetable expandWavetable (const Wavetable& table, std::size_t targetFrames)
{
    if (table.empty() || targetFrames == 0 || targetFrames <= table.frameCount)
        return table;

    SpectralMorph morph (table.frameSize);

    Wavetable expanded;
    expanded.frameSize = table.frameSize;
    expanded.frameCount = targetFrames;
    expanded.factory = table.factory;
    expanded.sixteenBit = table.sixteenBit;
    expanded.sampleRate = table.sampleRate;
    expanded.interpolation = FrameInterpolation::Linear;   // every slot is a real cycle now
    expanded.samples.resize (targetFrames * table.frameSize);

    // One cycle cannot be interpolated towards anything; every slot is that cycle.
    if (table.frameCount == 1)
    {
        for (std::size_t slot = 0; slot < targetFrames; ++slot)
            std::copy_n (table.samples.begin(), table.frameSize, expanded.samples.begin() + slot * table.frameSize);

        return expanded;
    }

    for (std::size_t slot = 0; slot < targetFrames; ++slot)
    {
        const double position = double (slot) * double (table.frameCount - 1) / double (targetFrames - 1);
        const auto lower = std::size_t (position);
        const auto fraction = float (position - double (lower));

        auto* out = expanded.samples.data() + slot * table.frameSize;

        if (fraction <= 0.0f)
            std::copy_n (table.samples.begin() + lower * table.frameSize, table.frameSize, out);
        else
            morph.morph (table.samples.data() + lower * table.frameSize,
                         table.samples.data() + (lower + 1) * table.frameSize,
                         fraction, out);
    }

    return expanded;
}

Wavetable tableForInterpolateAfterLoad (const Wavetable& table, bool embedded, int interpolateAfterLoad)
{
    // The path-referenced case is the one that must do nothing: ticket 37 measured the reference
    // ignoring the field there, byte for byte across the field's every value. Returning the table
    // unchanged is what keeps xerum's render identical to the reference's on those presets.
    if (embedded && interpolateAfterLoad == 2)
        return expandWavetable (table);

    return table;
}

} // namespace xerum::dsp
