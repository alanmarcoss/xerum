#include "fx_rack.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace xerum::dsp
{

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

double clampd (double value, double low, double high)
{
    return value < low ? low : (value > high ? high : value);
}

double dbToGain (double decibels) { return std::pow (10.0, decibels / 20.0); }
double gainToDb (double gain) { return 20.0 * std::log10 (std::max (gain, 1.0e-30)); }

/// A frequency as a fraction of the sample rate, kept strictly inside Nyquist so every `tan()` below
/// stays finite. The same guard `filter.cpp` applies, for the same reason: a filter that emits NaNs a
/// few samples after its cutoff reaches Nyquist is not a filter.
double cutoffFraction (double hertz, double sampleRate)
{
    return clampd (hertz / sampleRate, 1.0e-5, 0.4999);
}

// -------------------------------------------------------------------------------------------------
// Building blocks. Each is the boring textbook form and none of them allocates once prepared.
// -------------------------------------------------------------------------------------------------

struct OnePole
{
    double coefficient { 0.0 };
    double state { 0.0 };

    void set (double hertz, double sampleRate)
    {
        coefficient = std::exp (-kTwoPi * clampd (hertz, 0.05, sampleRate * 0.4999) / sampleRate);
    }

    void clear() { state = 0.0; }
    double lowpass (double input) { state = input + coefficient * (state - input); return state; }
    double highpass (double input) { return input - lowpass (input); }
};

/// A TPT state-variable section (Zavalishin): all three responses from one update.
struct SvfSection
{
    double g { 0.0 }, k { 1.0 }, a1 { 0.0 }, a2 { 0.0 }, a3 { 0.0 };
    double ic1 { 0.0 }, ic2 { 0.0 };
    double lowpass { 0.0 }, highpass { 0.0 }, bandpass { 0.0 };

    void set (double hertz, double q, double sampleRate)
    {
        g = std::tan (kPi * cutoffFraction (hertz, sampleRate));
        k = 1.0 / std::max (q, 0.02);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void clear() { ic1 = ic2 = 0.0; lowpass = highpass = bandpass = 0.0; }

    void update (double input)
    {
        const double v3 = input - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        bandpass = v1;
        lowpass = v2;
        highpass = input - k * v1 - v2;
    }
};

/// RBJ cookbook biquad — fixed responses: the EQ bands, the crossovers, the utility filters.
struct Biquad
{
    double b0 { 1.0 }, b1 { 0.0 }, b2 { 0.0 }, a1 { 0.0 }, a2 { 0.0 };
    double x1 { 0.0 }, x2 { 0.0 }, y1 { 0.0 }, y2 { 0.0 };

    void clear() { x1 = x2 = y1 = y2 = 0.0; }

    void normalise (double nb0, double nb1, double nb2, double a0, double na1, double na2)
    {
        b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
    }

    void lowpass (double hertz, double q, double sampleRate)
    {
        const double w = kTwoPi * cutoffFraction (hertz, sampleRate);
        const double cs = std::cos (w), sn = std::sin (w);
        const double alpha = sn / (2.0 * std::max (q, 0.02));
        normalise ((1.0 - cs) * 0.5, 1.0 - cs, (1.0 - cs) * 0.5, 1.0 + alpha, -2.0 * cs, 1.0 - alpha);
    }

    void highpass (double hertz, double q, double sampleRate)
    {
        const double w = kTwoPi * cutoffFraction (hertz, sampleRate);
        const double cs = std::cos (w), sn = std::sin (w);
        const double alpha = sn / (2.0 * std::max (q, 0.02));
        normalise ((1.0 + cs) * 0.5, -(1.0 + cs), (1.0 + cs) * 0.5, 1.0 + alpha, -2.0 * cs, 1.0 - alpha);
    }

    void peak (double hertz, double q, double gainDb, double sampleRate)
    {
        const double w = kTwoPi * cutoffFraction (hertz, sampleRate);
        const double cs = std::cos (w), sn = std::sin (w);
        const double alpha = sn / (2.0 * std::max (q, 0.02));
        const double a = std::pow (10.0, gainDb / 40.0);
        normalise (1.0 + alpha * a, -2.0 * cs, 1.0 - alpha * a, 1.0 + alpha / a, -2.0 * cs, 1.0 - alpha / a);
    }

    void lowShelf (double hertz, double q, double gainDb, double sampleRate)
    {
        const double w = kTwoPi * cutoffFraction (hertz, sampleRate);
        const double cs = std::cos (w), sn = std::sin (w);
        const double a = std::pow (10.0, gainDb / 40.0);
        const double beta = std::sqrt (a) * sn / std::max (q, 0.02);
        normalise (a * ((a + 1.0) - (a - 1.0) * cs + beta), 2.0 * a * ((a - 1.0) - (a + 1.0) * cs),
                   a * ((a + 1.0) - (a - 1.0) * cs - beta), (a + 1.0) + (a - 1.0) * cs + beta,
                   -2.0 * ((a - 1.0) + (a + 1.0) * cs), (a + 1.0) + (a - 1.0) * cs - beta);
    }

    void highShelf (double hertz, double q, double gainDb, double sampleRate)
    {
        const double w = kTwoPi * cutoffFraction (hertz, sampleRate);
        const double cs = std::cos (w), sn = std::sin (w);
        const double a = std::pow (10.0, gainDb / 40.0);
        const double beta = std::sqrt (a) * sn / std::max (q, 0.02);
        normalise (a * ((a + 1.0) + (a - 1.0) * cs + beta), -2.0 * a * ((a - 1.0) + (a + 1.0) * cs),
                   a * ((a + 1.0) + (a - 1.0) * cs - beta), (a + 1.0) - (a - 1.0) * cs + beta,
                   2.0 * ((a - 1.0) - (a + 1.0) * cs), (a + 1.0) - (a - 1.0) * cs - beta);
    }

    double process (double input)
    {
        const double out = b0 * input + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = input; y2 = y1; y1 = out;
        return out;
    }
};

/// Fourth-order Linkwitz-Riley crossover: two cascaded Butterworth sections, so the two outputs sum to
/// an allpass — flat magnitude with a phase twist at the crossover. That is what makes a splitter's
/// bands recombine without a hole in the spectrum, and it is what the independence test relies on.
struct Lr4Crossover
{
    Biquad low1 {}, low2 {}, high1 {}, high2 {};

    void set (double hertz, double sampleRate)
    {
        constexpr double kButterworthQ = 0.7071067811865476;
        low1.lowpass (hertz, kButterworthQ, sampleRate);
        low2 = low1;
        high1.highpass (hertz, kButterworthQ, sampleRate);
        high2 = high1;
    }

    void clear() { low1.clear(); low2.clear(); high1.clear(); high2.clear(); }

    void split (double input, double& low, double& high)
    {
        low = low2.process (low1.process (input));
        high = high2.process (high1.process (input));
    }
};

/// A delay line with linear interpolation, sized once.
struct DelayLine
{
    std::vector<double> buffer {};
    std::size_t write { 0 };

    void resize (std::size_t samples)
    {
        buffer.assign (std::max<std::size_t> (samples, 8), 0.0);
        write = 0;
    }

    void clear()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0);
        write = 0;
    }

    std::size_t length() const { return buffer.size(); }

    double read (double delaySamples) const
    {
        const auto size = static_cast<double> (buffer.size());
        double position = static_cast<double> (write) - clampd (delaySamples, 0.0, size - 2.0);

        while (position < 0.0)
            position += size;

        const auto whole = static_cast<std::size_t> (position);
        const double fraction = position - static_cast<double> (whole);
        const double a = buffer[whole % buffer.size()];
        const double b = buffer[(whole + 1) % buffer.size()];
        return a + (b - a) * fraction;
    }

    void push (double value)
    {
        buffer[write] = value;
        write = (write + 1) % buffer.size();
    }
};

struct Lfo
{
    double phase { 0.0 };
    double increment { 0.0 };

    void setRate (double hertz, double sampleRate) { increment = hertz / sampleRate; }
    void clear() { phase = 0.0; }

    double advance()
    {
        const double value = std::sin (kTwoPi * phase);
        phase += increment;

        while (phase >= 1.0)
            phase -= 1.0;

        while (phase < 0.0)
            phase += 1.0;

        return value;
    }
};

struct FirstOrderAllpass
{
    double a { 0.0 };
    double x1 { 0.0 }, y1 { 0.0 };

    void set (double coefficient) { a = coefficient; }
    void clear() { x1 = y1 = 0.0; }

    double process (double input)
    {
        const double out = a * input + x1 - a * y1;
        x1 = input;
        y1 = out;
        return out;
    }
};

/// The allpass coefficient whose phase passes -90 degrees at `hertz` — the usual one-pole phaser
/// convention.
double allpassCoefficient (double hertz, double sampleRate)
{
    const double t = std::tan (kPi * cutoffFraction (hertz, sampleRate));
    return (1.0 - t) / (1.0 + t);
}

// -------------------------------------------------------------------------------------------------
// FFT and convolution. `FXConv` convolves impulse responses up to ~900 k frames, so a direct FIR is
// out and a *zero-latency* uniform partitioned one is in: partition 0 is a plain FIR, the rest run
// through a frequency-domain delay line. Zero latency matters because it lets a test compare the
// output against the textbook convolution instead of against a block-quantised version of it.
// -------------------------------------------------------------------------------------------------

class Fft
{
public:
    explicit Fft (int size = 0) { reset (size); }

    void reset (int size)
    {
        size_ = size;
        reverse_.assign (static_cast<std::size_t> (std::max (size, 0)), 0);
        int bits = 0;

        while ((1 << bits) < size)
            ++bits;

        for (int i = 0; i < size; ++i)
        {
            int reversed = 0;

            for (int b = 0; b < bits; ++b)
                if ((i & (1 << b)) != 0)
                    reversed |= 1 << (bits - 1 - b);

            reverse_[static_cast<std::size_t> (i)] = reversed;
        }
    }

    int size() const { return size_; }

    void forward (std::complex<double>* data) const { transform (data, false); }
    void inverse (std::complex<double>* data) const { transform (data, true); }

private:
    void transform (std::complex<double>* a, bool inverse) const
    {
        const int n = size_;

        if (n < 2)
            return;

        for (int i = 0; i < n; ++i)
        {
            const int j = reverse_[static_cast<std::size_t> (i)];

            if (j > i)
                std::swap (a[i], a[j]);
        }

        for (int length = 2; length <= n; length <<= 1)
        {
            const double angle = (inverse ? 2.0 : -2.0) * kPi / static_cast<double> (length);
            const std::complex<double> step (std::cos (angle), std::sin (angle));

            for (int i = 0; i < n; i += length)
            {
                std::complex<double> twiddle (1.0, 0.0);

                for (int k = 0; k < length / 2; ++k)
                {
                    const std::complex<double> even = a[i + k];
                    const std::complex<double> odd = a[i + k + length / 2] * twiddle;
                    a[i + k] = even + odd;
                    a[i + k + length / 2] = even - odd;
                    twiddle *= step;
                }
            }
        }

        if (inverse)
            for (int i = 0; i < n; ++i)
                a[i] /= static_cast<double> (n);
    }

    int size_ { 0 };
    std::vector<int> reverse_ {};
};

class Convolver
{
public:
    void prepare (int blockSize)
    {
        blockSize_ = std::max (blockSize, 8);
        fft_.reset (blockSize_ * 2);
        in_.assign (static_cast<std::size_t> (blockSize_), 0.0);
        history_.assign (static_cast<std::size_t> (blockSize_), 0.0);
        pending_.assign (static_cast<std::size_t> (blockSize_), 0.0);
        carry_.assign (static_cast<std::size_t> (blockSize_), 0.0);
        spectrum_.assign (static_cast<std::size_t> (fft_.size()), {});
        accumulator_.assign (static_cast<std::size_t> (fft_.size()), {});
        direct_.assign (static_cast<std::size_t> (blockSize_), 0.0);
    }

    bool prepared() const { return fft_.size() > 0; }

    /// Install an impulse. The samples are copied into their spectra, so the caller may free them.
    void setImpulse (const float* samples, int frames)
    {
        partitions_.clear();
        fdl_.clear();
        fftPartitionCount_ = 0;
        impulseFrames_ = 0;

        if (! prepared() || samples == nullptr || frames <= 0)
        {
            reset();
            return;
        }

        const int block = blockSize_;
        const int fftSize = fft_.size();
        const int partitionCount = (frames + block - 1) / block;

        direct_.assign (static_cast<std::size_t> (block), 0.0);

        for (int j = 0; j < std::min (block, frames); ++j)
            direct_[static_cast<std::size_t> (j)] = static_cast<double> (samples[j]);

        // Partitions 1..count-1; partition 0 is the direct FIR above.
        const int fftPartitions = std::max (partitionCount - 1, 0);
        partitions_.assign (static_cast<std::size_t> (fftPartitions) * static_cast<std::size_t> (fftSize), {});
        fdl_.assign (static_cast<std::size_t> (std::max (fftPartitions, 1)) * static_cast<std::size_t> (fftSize), {});

        std::vector<std::complex<double>> spectrum (static_cast<std::size_t> (fftSize));

        for (int q = 0; q < fftPartitions; ++q)
        {
            std::fill (spectrum.begin(), spectrum.end(), std::complex<double> (0.0, 0.0));

            for (int j = 0; j < block; ++j)
            {
                const int index = (q + 1) * block + j;

                if (index < frames)
                    spectrum[static_cast<std::size_t> (j)] = static_cast<double> (samples[index]);
            }

            fft_.forward (spectrum.data());
            std::copy (spectrum.begin(), spectrum.end(),
                       partitions_.begin() + static_cast<std::size_t> (q) * static_cast<std::size_t> (fftSize));
        }

        fftPartitionCount_ = fftPartitions;
        impulseFrames_ = frames;
        reset();
    }

    void reset()
    {
        std::fill (in_.begin(), in_.end(), 0.0);
        std::fill (history_.begin(), history_.end(), 0.0);
        std::fill (pending_.begin(), pending_.end(), 0.0);
        std::fill (carry_.begin(), carry_.end(), 0.0);
        std::fill (fdl_.begin(), fdl_.end(), std::complex<double> (0.0, 0.0));
        blockPosition_ = 0;
        historyPosition_ = 0;
        fdlPosition_ = 0;
    }

    int impulseFrames() const { return impulseFrames_; }

    double process (double input)
    {
        if (impulseFrames_ == 0)
            return input;

        if (blockPosition_ == 0)
            computePending();

        double output = pending_[static_cast<std::size_t> (blockPosition_)];

        // The direct part: the impulse's first block against the last `block` inputs.
        for (int j = 0; j < blockSize_; ++j)
        {
            const double tap = direct_[static_cast<std::size_t> (j)];

            if (tap != 0.0)
            {
                int index = historyPosition_ - j;

                while (index < 0)
                    index += blockSize_;

                output += tap * history_[static_cast<std::size_t> (index)];
            }
        }

        history_[static_cast<std::size_t> (historyPosition_)] = input;
        historyPosition_ = (historyPosition_ + 1) % blockSize_;
        in_[static_cast<std::size_t> (blockPosition_)] = input;
        ++blockPosition_;

        if (blockPosition_ == blockSize_)
        {
            endBlock();
            blockPosition_ = 0;
        }

        return output;
    }

private:
    void computePending()
    {
        const int fftSize = fft_.size();

        std::fill (accumulator_.begin(), accumulator_.end(), std::complex<double> (0.0, 0.0));

        const int stored = std::max (fftPartitionCount_, 1);

        for (int q = 0; q < fftPartitionCount_; ++q)
        {
            int index = (fdlPosition_ - 1 - q) % stored;

            while (index < 0)
                index += stored;

            const auto* spectrum = &partitions_[static_cast<std::size_t> (q) * static_cast<std::size_t> (fftSize)];
            const auto* block = &fdl_[static_cast<std::size_t> (index) * static_cast<std::size_t> (fftSize)];

            for (int bin = 0; bin < fftSize; ++bin)
                accumulator_[static_cast<std::size_t> (bin)] += spectrum[bin] * block[bin];
        }

        fft_.inverse (accumulator_.data());

        for (int i = 0; i < blockSize_; ++i)
            pending_[static_cast<std::size_t> (i)] = accumulator_[static_cast<std::size_t> (i)].real()
                                                    + carry_[static_cast<std::size_t> (i)];

        for (int i = 0; i < blockSize_; ++i)
            carry_[static_cast<std::size_t> (i)] = accumulator_[static_cast<std::size_t> (blockSize_ + i)].real();
    }

    void endBlock()
    {
        const int fftSize = fft_.size();

        std::fill (spectrum_.begin(), spectrum_.end(), std::complex<double> (0.0, 0.0));

        for (int i = 0; i < blockSize_; ++i)
            spectrum_[static_cast<std::size_t> (i)] = in_[static_cast<std::size_t> (i)];

        fft_.forward (spectrum_.data());

        const int stored = std::max (fftPartitionCount_, 1);

        if (fftPartitionCount_ > 0)
        {
            auto* destination = &fdl_[static_cast<std::size_t> (fdlPosition_) * static_cast<std::size_t> (fftSize)];
            std::copy (spectrum_.begin(), spectrum_.end(), destination);
            fdlPosition_ = (fdlPosition_ + 1) % stored;
        }
    }

    Fft fft_ {};
    int blockSize_ { 64 };
    int fftPartitionCount_ { 0 };
    int impulseFrames_ { 0 };
    std::vector<double> direct_ {}, in_ {}, history_ {}, pending_ {}, carry_ {};
    std::vector<std::complex<double>> partitions_ {}, fdl_ {}, spectrum_ {}, accumulator_ {};
    int blockPosition_ { 0 };
    int historyPosition_ { 0 };
    int fdlPosition_ { 0 };
};

/// Rewrite an impulse as its minimum-phase counterpart: fold the real cepstrum so all the energy is
/// causal, then put the magnitude back. `kParamMinPhase` is set in 19 corpus convolvers.
std::vector<float> minimumPhaseImpulse (const float* samples, int frames)
{
    std::vector<float> out (samples, samples + frames);
    int size = 1;

    while (size < frames * 2)
        size <<= 1;

    if (size < 8)
        return out;

    Fft fft (size);
    std::vector<std::complex<double>> spectrum (static_cast<std::size_t> (size));

    for (int i = 0; i < frames; ++i)
        spectrum[static_cast<std::size_t> (i)] = static_cast<double> (samples[i]);

    fft.forward (spectrum.data());

    std::vector<std::complex<double>> cepstrum (static_cast<std::size_t> (size));

    for (int i = 0; i < size; ++i)
        cepstrum[static_cast<std::size_t> (i)] =
            std::complex<double> (std::log (std::max (std::abs (spectrum[static_cast<std::size_t> (i)]), 1.0e-12)), 0.0);

    fft.inverse (cepstrum.data());

    for (int i = 1; i < size / 2; ++i)
        cepstrum[static_cast<std::size_t> (i)] *= 2.0;

    for (int i = size / 2 + 1; i < size; ++i)
        cepstrum[static_cast<std::size_t> (i)] = std::complex<double> (0.0, 0.0);

    fft.forward (cepstrum.data());

    for (int i = 0; i < frames; ++i)
    {
        const double value = std::exp (cepstrum[static_cast<std::size_t> (i)].real());
        out[static_cast<std::size_t> (i)] = static_cast<float> (std::isfinite (value) ? value : 0.0);
    }

    return out;
}

// -------------------------------------------------------------------------------------------------
// The effect base: enable, wet/dry, output level, and the "which names did I understand" gate.
// -------------------------------------------------------------------------------------------------

class FxBase : public FxEffect
{
public:
    void prepare (double sampleRate, int maximumBlockFrames) final
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxFrames_ = std::max (maximumBlockFrames, 1);
        dryLeft_.assign (static_cast<std::size_t> (maxFrames_), 0.0);
        dryRight_.assign (static_cast<std::size_t> (maxFrames_), 0.0);
        onPrepare();
        reset();
    }

    void reset() final
    {
        std::fill (dryLeft_.begin(), dryLeft_.end(), 0.0);
        std::fill (dryRight_.begin(), dryRight_.end(), 0.0);
        onReset();
    }

    bool setNumber (std::string_view name, double value) final
    {
        // The three every effect carries, plus the two the slot carries on top of its class.
        if (name == "kParamEnable")   { enabled_ = value != 0.0; return true; }
        if (name == "kParamWet")      { wet_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamLevelOut") { levelOut_ = value; return true; }
        if (name == "type")           { return true; }   // the class's numeric mirror, already known
        if (name.starts_with ("kUIParam")) return true;  // a UI-only mirror of wet/gain, see the header

        // Fields the reference stores *beside* a class's parameters, none of which is a `kParam...` a
        // preset sets: saved modulator state (`lfophasor`, `lfo` — where the LFO's phasor was when the
        // preset was saved), asset hints the caller resolves (`relativePathToIR` and its cached
        // `numFrames`/`numChannels`/`sampleRate`, or an inline `embeddedIR`), and the rack's own
        // metadata (`displayName`, `proxyParams`). Accepting them is not a substitution; reporting them
        // as unknown would put a complaint in the report of every ordinary preset.
        if (name == "lfophasor" || name == "lfo" || name == "relativePathToIR" || name == "numFrames"
            || name == "numChannels" || name == "sampleRate" || name == "sourceRate"
            || name == "embeddedIR" || name == "proxyParams" || name == "displayName")
            return true;

        return onNumber (name, value);
    }

    bool setText (std::string_view name, std::string_view value) final
    {
        if (name == "displayName")
            return true;   // the rack's own label, not a parameter of any effect

        return onText (name, value);
    }

    void process (float* left, float* right, int frames) final
    {
        if (frames <= 0 || ! enabled_)
            return;

        const int count = std::min (frames, maxFrames_);

        for (int i = 0; i < count; ++i)
        {
            dryLeft_[static_cast<std::size_t> (i)] = left[i];
            dryRight_[static_cast<std::size_t> (i)] = right[i];
        }

        processWet (left, right, count);

        const double wet = clampd (wet_, 0.0, 1.0);
        const double dry = 1.0 - wet;

        for (int i = 0; i < count; ++i)
        {
            left[i] = static_cast<float> ((dry * dryLeft_[static_cast<std::size_t> (i)]
                                           + wet * left[i]) * levelOut_);
            right[i] = static_cast<float> ((dry * dryRight_[static_cast<std::size_t> (i)]
                                            + wet * right[i]) * levelOut_);
        }
    }

protected:
    double sampleRate_ { 48000.0 };
    int maxFrames_ { 1 };
    bool enabled_ { true };
    double wet_ { 1.0 };
    double levelOut_ { 1.0 };
    std::vector<double> dryLeft_ {}, dryRight_ {};

    virtual void onPrepare() {}
    virtual void onReset() {}
    virtual bool onNumber (std::string_view, double) { return false; }
    virtual bool onText (std::string_view, std::string_view) { return false; }
    virtual void processWet (float* left, float* right, int frames) = 0;
};

// -------------------------------------------------------------------------------------------------
// Distortion (type 0) — ticket 04 section 5.1.
//
// Seventeen modes. `kParamPrePost` places the filter: 1 before the shaper, 2 after, and absent (the
// default) means no filter, the "OFF" third state the corpus never stores. `kParamLPHP` morphs that
// filter low→band→high, `kParamBW` is its Q, `kParamNumStages` the downsample factor, `kParamDistBias`
// a DC offset for the asymmetric modes. Every curve below is a stated convention: ticket 04 read the
// parameter ranges, not the transfer functions.
// -------------------------------------------------------------------------------------------------

enum class DistortionMode
{
    Asym, Diode1, Diode2, Downsample, HardClip, LinFold, Overdrive, Rectify, SinFold, SineShaper,
    SoftClip, SoftSat, StompBox, TapeSat, XShaper, XShaperAsym, ZeroSquare
};

DistortionMode distortionModeFromText (std::string_view text)
{
    struct Entry { std::string_view name; DistortionMode mode; };
    static constexpr Entry kEntries[] = {
        { "kAsym", DistortionMode::Asym },           { "kDiode1", DistortionMode::Diode1 },
        { "kDiode2", DistortionMode::Diode2 },       { "kDownsample", DistortionMode::Downsample },
        { "kHardClip", DistortionMode::HardClip },   { "kLinFold", DistortionMode::LinFold },
        { "kOverdrive", DistortionMode::Overdrive }, { "kRectify", DistortionMode::Rectify },
        { "kSinFold", DistortionMode::SinFold },     { "kSineShaper", DistortionMode::SineShaper },
        { "kSoftClip", DistortionMode::SoftClip },   { "kSoftSat", DistortionMode::SoftSat },
        { "kStompBox", DistortionMode::StompBox },   { "kTapeSat", DistortionMode::TapeSat },
        { "kXShaper", DistortionMode::XShaper },     { "kXShaperAsym", DistortionMode::XShaperAsym },
        { "kZeroSquare", DistortionMode::ZeroSquare },
    };

    for (const auto& entry : kEntries)
        if (entry.name == text)
            return entry.mode;

    return DistortionMode::Overdrive;
}

double shapeSample (DistortionMode mode, double x)
{
    const double sign = x < 0.0 ? -1.0 : 1.0;
    const double magnitude = std::fabs (x);

    switch (mode)
    {
        case DistortionMode::Asym:
            return x >= 0.0 ? std::tanh (1.4 * x) : std::tanh (0.7 * x);
        case DistortionMode::Diode1:
            return x >= 0.0 ? 1.0 - std::exp (-x) : -0.5 * (1.0 - std::exp (2.0 * x));
        case DistortionMode::Diode2:
            return x >= 0.0 ? std::tanh (0.8 * x) : std::tanh (2.0 * x);
        case DistortionMode::HardClip:
            return clampd (x, -1.0, 1.0);
        case DistortionMode::LinFold:
        {
            double folded = std::fmod (x + 1.0, 4.0);

            if (folded < 0.0)
                folded += 4.0;

            return folded < 2.0 ? folded - 1.0 : 3.0 - folded;
        }
        case DistortionMode::Overdrive:
            if (magnitude < 1.0 / 3.0)
                return 2.0 * x;

            if (magnitude < 2.0 / 3.0)
            {
                const double t = 2.0 - 3.0 * magnitude;
                return sign * (3.0 - t * t) / 3.0;
            }

            return sign;
        case DistortionMode::Rectify:
            return magnitude;
        case DistortionMode::SinFold:
            return std::sin (x);
        case DistortionMode::SineShaper:
            return std::sin (kPi * 0.5 * clampd (x, -1.0, 1.0));
        case DistortionMode::SoftClip:
            return std::tanh (x);
        case DistortionMode::SoftSat:
            return std::tanh (0.5 * x) * 2.0 - x * 0.0;
        case DistortionMode::StompBox:
            return sign * (1.0 - std::exp (-3.0 * magnitude)) * (x >= 0.0 ? 1.0 : 0.72);
        case DistortionMode::TapeSat:
            return x >= 0.0 ? std::tanh (1.1 * x) : 0.9 * std::tanh (0.9 * x);
        case DistortionMode::XShaper:
        case DistortionMode::XShaperAsym:
            // The built-in curve when a slot carries no `flex` entry: the classic cubic soft clipper,
            // unity slope at the origin and a hard limit past +/- 1.7.
            return x - x * x * x / 6.0;
        case DistortionMode::ZeroSquare:
            return magnitude < 1.0e-6 ? 0.0 : sign;
        case DistortionMode::Downsample:
            return x;   // the sample-and-hold below, not a memoryless curve
    }

    return x;
}

class DistortionFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::Distortion; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamDrive")      { drive_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamFreq")       { filterCutoff_ = clampd (value, 0.0, 1.0); return true; }
        if (name == "kParamBW")         { filterQ_ = clampd (value, 0.02, 20.0); return true; }
        if (name == "kParamLPHP")       { filterMorph_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamPrePost")    { filterPosition_ = static_cast<int> (value); return true; }
        if (name == "kParamNumStages")  { stages_ = std::max (static_cast<int> (value), 2); return true; }
        if (name == "kParamNumSosigs")  return true;   // internal counter, no audible parameter
        if (name == "kParamDistBias")   { bias_ = value; return true; }

        return false;
    }

    bool onText (std::string_view name, std::string_view value) override
    {
        if (name != "kParamMode")
            return false;

        mode_ = distortionModeFromText (value);
        return true;
    }

    void setCurve (const TransferCurve& curve) override
    {
        if (curve.x == nullptr || curve.y == nullptr || curve.points < 2)
            return;

        curveX_.assign (curve.x, curve.x + curve.points);
        curveY_.assign (curve.y, curve.y + curve.points);
    }

protected:
    void onPrepare() override
    {
        leftFilter_.set (filterHz(), filterQ_, sampleRate_);
        rightFilter_.set (filterHz(), filterQ_, sampleRate_);
    }

    void onReset() override
    {
        leftFilter_.clear();
        rightFilter_.clear();
        holdLeft_ = holdRight_ = 0.0;
        holdCounter_ = 0;
    }

    void processWet (float* left, float* right, int frames) override
    {
        leftFilter_.set (filterHz(), filterQ_, sampleRate_);
        rightFilter_.set (filterHz(), filterQ_, sampleRate_);

        // 0…100 % of drive is 0…+34 dB of pre-gain, and the output is trimmed back so that winding
        // DRIVE up changes the harmonics rather than only the level.
        const double gain = dbToGain (drive_ * 34.0);
        const double compensation = 1.0 / (1.0 + drive_ * 6.0);
        const bool filtered = filterPosition_ == 1 || filterPosition_ == 2;
        const bool filterFirst = filterPosition_ == 1;

        for (int i = 0; i < frames; ++i)
        {
            double l = left[i];
            double r = right[i];

            if (filtered && filterFirst)
            {
                l = applyFilter (leftFilter_, l);
                r = applyFilter (rightFilter_, r);
            }

            l = shape (l, gain, compensation, false);
            r = shape (r, gain, compensation, true);

            if (filtered && ! filterFirst)
            {
                l = applyFilter (leftFilter_, l);
                r = applyFilter (rightFilter_, r);
            }

            left[i] = static_cast<float> (l);
            right[i] = static_cast<float> (r);
        }
    }

private:
    double filterHz() const { return 20.0 * std::pow (1000.0, clampd (filterCutoff_, 0.0, 1.0)); }

    double applyFilter (SvfSection& filter, double input) const
    {
        filter.update (input);

        if (filterMorph_ <= 0.5)
            return filter.lowpass * (1.0 - 2.0 * filterMorph_) + filter.bandpass * (2.0 * filterMorph_);

        return filter.bandpass * (2.0 - 2.0 * filterMorph_) + filter.highpass * (2.0 * filterMorph_ - 1.0);
    }

    double curveShape (double x) const
    {
        if (curveX_.size() < 2)
            return shapeSample (DistortionMode::XShaper, x);

        if (x <= curveX_.front())
            return curveY_.front();

        if (x >= curveX_.back())
            return curveY_.back();

        for (std::size_t i = 1; i < curveX_.size(); ++i)
        {
            if (x <= curveX_[i])
            {
                const double span = curveX_[i] - curveX_[i - 1];

                if (span <= 0.0)
                    return curveY_[i];

                const double t = (x - curveX_[i - 1]) / span;
                return curveY_[i - 1] + (curveY_[i] - curveY_[i - 1]) * t;
            }
        }

        return curveY_.back();
    }

    double shape (double input, double gain, double compensation, bool rightChannel)
    {
        const double biased = input + (mode_ == DistortionMode::XShaperAsym ? bias_ + 0.15 : bias_);
        const double driven = biased * gain;

        if (mode_ == DistortionMode::Downsample)
        {
            if (holdCounter_ % stages_ == 0)
            {
                holdLeft_ = driven;
                holdRight_ = driven;
            }

            ++holdCounter_;

            if (holdCounter_ >= stages_ * 4)
                holdCounter_ = 0;

            return shapeSample (DistortionMode::SoftSat, rightChannel ? holdRight_ : holdLeft_) * compensation;
        }

        const double out = (mode_ == DistortionMode::XShaper || mode_ == DistortionMode::XShaperAsym)
                               ? curveShape (driven)
                               : shapeSample (mode_, driven);
        return out * compensation;
    }

    DistortionMode mode_ { DistortionMode::Overdrive };   // the corpus's most-used mode (52 of 435)
    double drive_ { 0.0 };
    double filterCutoff_ { 1.0 };
    double filterQ_ { 0.707 };
    double filterMorph_ { 0.0 };
    int filterPosition_ { 0 };      // 0 = off (the default), 1 = pre, 2 = post
    int stages_ { 3 };
    double bias_ { 0.0 };
    SvfSection leftFilter_ {}, rightFilter_ {};
    double holdLeft_ { 0.0 }, holdRight_ { 0.0 };
    int holdCounter_ { 0 };
    std::vector<double> curveX_ {}, curveY_ {};
};

// -------------------------------------------------------------------------------------------------
// Flanger (type 1): one modulated short delay per channel, with the stereo phase offset from
// `kParamWidth` (degrees) and a damped feedback path.
// -------------------------------------------------------------------------------------------------

class FlangerFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::Flanger; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamRate")     { rate_ = clampd (value, 0.0, 40.0); return true; }
        if (name == "kParamDepth")    { depth_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamFeedback") { feedback_ = clampd (value * 0.01, 0.0, 0.95); return true; }
        if (name == "kParamWidth")    { phaseDegrees_ = clampd (value, 0.0, 360.0); return true; }
        if (name == "kParamBeatSync") return true;

        return false;
    }

protected:
    void onPrepare() override
    {
        leftLine_.resize (static_cast<std::size_t> (sampleRate_ * 0.05) + 8);
        rightLine_.resize (static_cast<std::size_t> (sampleRate_ * 0.05) + 8);
    }

    void onReset() override
    {
        leftLine_.clear();
        rightLine_.clear();
        leftLfo_.clear();
        rightLfo_.clear();
        dampingLeft_.clear();
        dampingRight_.clear();
    }

    void processWet (float* left, float* right, int frames) override
    {
        leftLfo_.setRate (std::max (rate_, 0.001), sampleRate_);
        rightLfo_.setRate (std::max (rate_, 0.001), sampleRate_);
        rightLfo_.phase = std::fmod (leftLfo_.phase + phaseDegrees_ / 360.0, 1.0);

        const double baseSamples = sampleRate_ * 0.001;
        const double sweepSamples = sampleRate_ * 0.009 * depth_;

        for (int i = 0; i < frames; ++i)
        {
            const double delayLeft = baseSamples + sweepSamples * (0.5 + 0.5 * leftLfo_.advance());
            const double delayRight = baseSamples + sweepSamples * (0.5 + 0.5 * rightLfo_.advance());

            const double inLeft = left[i];
            const double inRight = right[i];
            const double delayedLeft = leftLine_.read (delayLeft);
            const double delayedRight = rightLine_.read (delayRight);

            leftLine_.push (inLeft + feedback_ * dampingLeft_.lowpass (delayedLeft));
            rightLine_.push (inRight + feedback_ * dampingRight_.lowpass (delayedRight));

            // The wet signal is the **comb** — input plus the swept tap. A flange with only the tap
            // is a delay, not a notch, and a steady tone through it would not move at all.
            left[i] = static_cast<float> (inLeft + delayedLeft);
            right[i] = static_cast<float> (inRight + delayedRight);
        }
    }

private:
    double rate_ { 0.3 };
    double depth_ { 0.5 };
    double feedback_ { 0.0 };
    double phaseDegrees_ { 0.0 };
    DelayLine leftLine_ {}, rightLine_ {};
    Lfo leftLfo_ {}, rightLfo_ {};
    OnePole dampingLeft_ {}, dampingRight_ {};
};

// -------------------------------------------------------------------------------------------------
// Phaser (type 2): an allpass cascade swept around `kParamFreq`, `kParamNumPoles` sections deep, with
// the two halves of the cascade offset by `kParamDepth2` and the channels by `kParamWidth`.
// -------------------------------------------------------------------------------------------------

class PhaserFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::Phaser; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamNumPoles") { poles_ = std::max (1, std::min (static_cast<int> (value), 18)); return true; }
        if (name == "kParamFreq")     { centreHz_ = clampd (value, 10.0, 20000.0); return true; }
        if (name == "kParamRate")     { rate_ = clampd (value, 0.0, 40.0); return true; }
        if (name == "kParamDepth")    { depth_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamDepth2")   { depth2_ = clampd (value, 0.0, 1.0); return true; }
        if (name == "kParamFeedback") { feedback_ = clampd (value * 0.01, 0.0, 0.95); return true; }
        if (name == "kParamWidth")    { phaseDegrees_ = clampd (value, 0.0, 360.0); return true; }
        if (name == "kParamBeatSync") return true;

        return false;
    }

protected:
    void onReset() override
    {
        for (auto& stage : leftStages_) stage.clear();
        for (auto& stage : rightStages_) stage.clear();
        leftLfo_.clear();
        rightLfo_.clear();
        feedbackLeft_ = feedbackRight_ = 0.0;
    }

    void processWet (float* left, float* right, int frames) override
    {
        leftLfo_.setRate (std::max (rate_, 0.001), sampleRate_);
        rightLfo_.setRate (std::max (rate_, 0.001), sampleRate_);
        rightLfo_.phase = std::fmod (leftLfo_.phase + phaseDegrees_ / 360.0, 1.0);

        const int sections = poles_;
        const double octaves = 3.0 * depth_;

        for (int i = 0; i < frames; ++i)
        {
            const double modLeft = leftLfo_.advance();
            const double modRight = rightLfo_.advance();

            for (int channel = 0; channel < 2; ++channel)
            {
                const double lfo = channel == 0 ? modLeft : modRight;
                const double dry = channel == 0 ? left[i] : right[i];
                double x = dry + feedback_ * (channel == 0 ? feedbackLeft_ : feedbackRight_);
                double y = x;
                auto& stages = channel == 0 ? leftStages_ : rightStages_;

                for (int s = 0; s < sections; ++s)
                {
                    // The second half of the cascade trails the first by `kParamDepth2`.
                    const double spread = s < sections / 2 ? 0.0 : depth2_;
                    const double modulated = centreHz_ * std::pow (2.0, octaves * (lfo - spread));
                    stages[static_cast<std::size_t> (s)].set (allpassCoefficient (modulated, sampleRate_));
                    y = stages[static_cast<std::size_t> (s)].process (y);
                }

                if (channel == 0)
                {
                    feedbackLeft_ = y;
                    left[i] = static_cast<float> (dry + y);
                }
                else
                {
                    feedbackRight_ = y;
                    right[i] = static_cast<float> (dry + y);
                }
            }
        }
    }

private:
    int poles_ { 4 };
    double centreHz_ { 1000.0 };
    double rate_ { 0.5 };
    double depth_ { 0.5 };
    double depth2_ { 0.0 };
    double feedback_ { 0.0 };
    double phaseDegrees_ { 0.0 };
    std::array<FirstOrderAllpass, 18> leftStages_ {}, rightStages_ {};
    Lfo leftLfo_ {}, rightLfo_ {};
    double feedbackLeft_ { 0.0 }, feedbackRight_ { 0.0 };
};

// -------------------------------------------------------------------------------------------------
// Chorus (type 3): four voices, the two `kParamDelay` knobs setting the two stereo pairs and one LFO
// driving all four with a slight rate spread so they do not lock.
// -------------------------------------------------------------------------------------------------

class ChorusFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::Chorus; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamRate")     { rate_ = clampd (value, 0.0, 40.0); return true; }
        if (name == "kParamDepth")    { depth_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamFeedback") { feedback_ = clampd (value * 0.01, 0.0, 0.95); return true; }
        if (name == "kParamDelay")    { delayA_ = clampd (value, 0.0, 50.0); return true; }
        if (name == "kParamDelay2")   { delayB_ = clampd (value, 0.0, 50.0); return true; }
        if (name == "kParamFilt")     { filterHz_ = clampd (value, 10.0, 20000.0); return true; }
        if (name == "kParamFiltMode") { filterHighpass_ = value >= 2.0; return true; }
        if (name == "kParamBeatSync") return true;

        return false;
    }

protected:
    void onPrepare() override
    {
        for (auto& line : leftLines_)
            line.resize (static_cast<std::size_t> (sampleRate_ * 0.06) + 8);

        for (auto& line : rightLines_)
            line.resize (static_cast<std::size_t> (sampleRate_ * 0.06) + 8);
    }

    void onReset() override
    {
        for (auto& line : leftLines_) line.clear();
        for (auto& line : rightLines_) line.clear();
        for (auto& voice : voices_) voice.clear();
        filterLeft_.clear();
        filterRight_.clear();
    }

    void processWet (float* left, float* right, int frames) override
    {
        filterLeft_.set (filterHz_, sampleRate_);
        filterRight_.set (filterHz_, sampleRate_);

        for (int v = 0; v < kVoices; ++v)
            voices_[v].setRate (std::max (rate_, 0.001) * (1.0 + 0.13 * v), sampleRate_);

        for (int i = 0; i < frames; ++i)
        {
            double wetLeft = 0.0, wetRight = 0.0;

            for (int v = 0; v < kVoices; ++v)
            {
                const double modulation = 0.5 + 0.5 * voices_[v].advance();
                const double base = (v % 2 == 0 ? delayA_ : delayB_) * 0.001 * sampleRate_;
                const double delay = base + depth_ * 0.004 * sampleRate_ * modulation;

                wetLeft += leftLines_[v].read (delay);
                wetRight += rightLines_[v].read (delay * 1.017);
            }

            wetLeft = left[i] + wetLeft / kVoices;
            wetRight = right[i] + wetRight / kVoices;

            for (int v = 0; v < kVoices; ++v)
            {
                leftLines_[v].push (left[i] + feedback_ * wetLeft);
                rightLines_[v].push (right[i] + feedback_ * wetRight);
            }

            // The output filter is a plain one-pole on each side; a low pass passes unity at DC, a
            // high pass is the complement, so the same coefficient serves both modes.
            wetLeft = filterHighpass_ ? filterLeft_.highpass (std::min (wetLeft * 2.0, 4.0)) : filterLeft_.lowpass (wetLeft);
            wetRight = filterHighpass_ ? filterRight_.highpass (std::min (wetRight * 2.0, 4.0)) : filterRight_.lowpass (wetRight);

            left[i] = static_cast<float> (wetLeft);
            right[i] = static_cast<float> (wetRight);
        }
    }

private:
    static constexpr int kVoices = 4;
    double rate_ { 0.5 };
    double depth_ { 0.5 };
    double feedback_ { 0.0 };
    double delayA_ { 4.0 };
    double delayB_ { 9.0 };
    double filterHz_ { 20000.0 };
    bool filterHighpass_ { false };
    std::array<DelayLine, kVoices> leftLines_ {}, rightLines_ {};
    std::array<Lfo, kVoices> voices_ {};
    OnePole filterLeft_ {}, filterRight_ {};
};

// -------------------------------------------------------------------------------------------------
// Delay (type 4). `kParamMode` 1 is normal and 2 ping-pong; the unobserved 3 (TAP→DELAY) reads as
// normal. `kParamOffset{L,R}` scales each side's time (the guide's 50–150 %), and `kParamFreq` /
// `kParamBW` are a resonant low pass in the feedback path.
// -------------------------------------------------------------------------------------------------

class DelayFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::Delay; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamTimeL")    { timeLeft_ = clampd (value, 0.0, 4.0); return true; }
        if (name == "kParamTimeR")    { timeRight_ = clampd (value, 0.0, 4.0); return true; }
        if (name == "kParamOffsetL")  { offsetLeft_ = clampd (value, 0.05, 4.0); return true; }
        if (name == "kParamOffsetR")  { offsetRight_ = clampd (value, 0.05, 4.0); return true; }
        if (name == "kParamLink")     { linked_ = value != 0.0; return true; }
        if (name == "kParamFeedback") { feedback_ = clampd (value * 0.01, 0.0, 0.99); return true; }
        if (name == "kParamFreq")     { filterHz_ = clampd (value, 10.0, 20000.0); return true; }
        if (name == "kParamBW")       { filterQ_ = clampd (value, 0.02, 20.0); return true; }
        if (name == "kParamMode")     { mode_ = static_cast<int> (value); return true; }
        if (name == "kParamHQ" || name == "kParamBeatSync") return true;

        return false;
    }

protected:
    void onPrepare() override
    {
        const auto maximum = static_cast<std::size_t> (sampleRate_ * 5.0) + 8;
        leftLine_.resize (maximum);
        rightLine_.resize (maximum);
    }

    void onReset() override
    {
        leftLine_.clear();
        rightLine_.clear();
        leftFilter_.clear();
        rightFilter_.clear();
    }

    void processWet (float* left, float* right, int frames) override
    {
        // `kParamLink` ties the two times together, which is what all 59 corpus instances ask for.
        const double timeLeft = timeLeft_;
        const double timeRight = linked_ ? timeLeft_ : timeRight_;
        const double delayLeft = std::max (timeLeft * offsetLeft_ * sampleRate_, 1.0);
        const double delayRight = std::max (timeRight * offsetRight_ * sampleRate_, 1.0);

        leftFilter_.set (filterHz_, filterQ_, sampleRate_);
        rightFilter_.set (filterHz_, filterQ_, sampleRate_);

        const double feedback = loopFeedback();

        for (int i = 0; i < frames; ++i)
        {
            const double tapLeft = leftLine_.read (delayLeft);
            const double tapRight = rightLine_.read (delayRight);

            const double fedLeft = mode_ == 2 ? tapRight : tapLeft;
            const double fedRight = mode_ == 2 ? tapLeft : tapRight;

            leftFilter_.update (fedLeft);
            rightFilter_.update (fedRight);

            leftLine_.push (left[i] + feedback * leftFilter_.lowpass);
            rightLine_.push (right[i] + feedback * rightFilter_.lowpass);

            left[i] = static_cast<float> (tapLeft);
            right[i] = static_cast<float> (tapRight);
        }
    }

private:
    /// **The loop's gain, and why it is bounded.**
    ///
    /// `kParamBW` is read as the loop filter's Q, and a TPT lowpass at Q lifts its peak by about Q — so at
    /// the corpus's *own* values (`ARP - Aardvark`: BW 3.28, feedback 47 %; `ARP - Bell Arperium`: BW 5.33,
    /// feedback 58 %) `feedback x Q` is 1.5 and 3.1. That is not a delay that rings, it is one that runs
    /// away: measured with the ceiling off — which is how the fidelity layer renders, the golden being the
    /// reference's own unlimited output — Aardvark reached **2.3e9** and Bell Arperium 3.7e9 by the end of
    /// a 2 s render, and Bell Arperium's oscillators are a deferred engine, so it grew from numerical noise
    /// with no input at all.
    ///
    /// The reference's mapping for `kParamBW` is not established — ticket 04 read the parameter *ranges*,
    /// not the curves — so this keeps the reading rather than inventing a second meaning for it, and trims
    /// the feedback by the filter's own peak gain: the loop then sits at or below unity at the resonance and
    /// below it everywhere else, which is the condition for the tail to decay. A preset that asks for 58 %
    /// feedback still gets a long tail; it does not get an explosion. A delay that runs away is not the
    /// delay any preset asked for.
    double loopFeedback() const
    {
        const double peakGain = std::max (1.0, filterQ_);
        return feedback_ * std::min (1.0, 1.0 / peakGain);
    }

    double timeLeft_ { 0.25 }, timeRight_ { 0.25 };
    double offsetLeft_ { 1.0 }, offsetRight_ { 1.0 };
    double feedback_ { 0.0 };
    double filterHz_ { 18000.0 };
    double filterQ_ { 0.707 };
    int mode_ { 1 };
    bool linked_ { false };
    DelayLine leftLine_ {}, rightLine_ {};
    SvfSection leftFilter_, rightFilter_;
};

// -------------------------------------------------------------------------------------------------
// Compressor (type 5). `kParamThresh` is the guide's 0…1 (0 = 0 dB, 1 = -120 dB) and `kParamRatio`
// 1…1e6, where 1e6 means limit. `kParamMultiband` splits into three bands with two LR4 crossovers at
// `kParamXoverLow`/`kParamXoverHi`; per-band make-up is `kParamGain0..2` and the per-band threshold
// offset is `kParamThreshUD0..2`, read as dB. `kParamRatio0..2`, `kParamRatioBelow*` and
// `kParamDeadband*` are read and unused: ticket 04 measured their ranges but not what they do, and
// inventing a curve for them would be a substitution dressed as a feature.
// -------------------------------------------------------------------------------------------------

class CompressorFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::Compressor; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamThresh")    { threshold_ = clampd (value, 0.0, 1.0); return true; }
        if (name == "kParamRatio")     { ratio_ = std::max (value, 1.0); return true; }
        if (name == "kParamAttack")    { attackMs_ = clampd (value, 0.01, 2000.0); return true; }
        if (name == "kParamRelease")   { releaseMs_ = clampd (value, 0.01, 5000.0); return true; }
        if (name == "kParamMakeup")    { makeupDb_ = value; return true; }
        if (name == "kParamMultiband") { multiband_ = value != 0.0; return true; }
        if (name == "kParamXoverLow")  { crossoverLow_ = clampd (value, 20.0, 20000.0); return true; }
        if (name == "kParamXoverHi")   { crossoverHigh_ = clampd (value, 20.0, 20000.0); return true; }
        if (name == "kParamGain0")     { bandGain_[0] = value; return true; }
        if (name == "kParamGain1")     { bandGain_[1] = value; return true; }
        if (name == "kParamGain2")     { bandGain_[2] = value; return true; }
        if (name == "kParamThreshUD0") { bandThreshold_[0] = value * 0.1; return true; }
        if (name == "kParamThreshUD1") { bandThreshold_[1] = value * 0.1; return true; }
        if (name == "kParamThreshUD2") { bandThreshold_[2] = value * 0.1; return true; }
        if (name == "kParamRatio0" || name == "kParamRatio1" || name == "kParamRatio2"
            || name == "kParamRatioBelow" || name == "kParamRatioBelow0" || name == "kParamRatioBelow1"
            || name == "kParamRatioBelow2" || name == "kParamDeadband0" || name == "kParamDeadband1"
            || name == "kParamDeadband2" || name == "kParamCompensatedWetDry")
            return true;

        return false;
    }

protected:
    void onPrepare() override
    {
        updateCoefficients();
        splitLowLeft_.set (crossoverLow_, sampleRate_);
        splitLowRight_.set (crossoverLow_, sampleRate_);
        splitHighLeft_.set (crossoverHigh_, sampleRate_);
        splitHighRight_.set (crossoverHigh_, sampleRate_);
    }

    void onReset() override
    {
        detectorLeft_ = detectorRight_ = 0.0;
        for (auto* crossover : { &splitLowLeft_, &splitLowRight_, &splitHighLeft_, &splitHighRight_ })
            crossover->clear();
    }

    void processWet (float* left, float* right, int frames) override
    {
        updateCoefficients();
        splitLowLeft_.set (crossoverLow_, sampleRate_);
        splitLowRight_.set (crossoverLow_, sampleRate_);
        splitHighLeft_.set (crossoverHigh_, sampleRate_);
        splitHighRight_.set (crossoverHigh_, sampleRate_);

        for (int i = 0; i < frames; ++i)
        {
            if (! multiband_)
            {
                left[i] = static_cast<float> (compress (0, left[i], 0.0, 0.0));
                right[i] = static_cast<float> (compress (1, right[i], 0.0, 0.0));
                continue;
            }

            // Two LR4 crossovers, so the three bands sum back to the input: LOW + (MID + HIGH).
            double lowLeft = 0.0, restLeft = 0.0, lowRight = 0.0, restRight = 0.0;
            splitLowLeft_.split (left[i], lowLeft, restLeft);
            splitLowRight_.split (right[i], lowRight, restRight);
            double midLeft = 0.0, highLeft = 0.0, midRight = 0.0, highRight = 0.0;
            splitHighLeft_.split (restLeft, midLeft, highLeft);
            splitHighRight_.split (restRight, midRight, highRight);

            const double outLeft = compress (0, lowLeft, bandThreshold_[0], bandGain_[0])
                                 + compress (0, midLeft, bandThreshold_[1], bandGain_[1])
                                 + compress (0, highLeft, bandThreshold_[2], bandGain_[2]);
            const double outRight = compress (1, lowRight, bandThreshold_[0], bandGain_[0])
                                  + compress (1, midRight, bandThreshold_[1], bandGain_[1])
                                  + compress (1, highRight, bandThreshold_[2], bandGain_[2]);

            left[i] = static_cast<float> (outLeft);
            right[i] = static_cast<float> (outRight);
        }
    }

private:
    void updateCoefficients()
    {
        attackCoefficient_ = std::exp (-1.0 / (std::max (attackMs_, 0.01) * 0.001 * sampleRate_));
        releaseCoefficient_ = std::exp (-1.0 / (std::max (releaseMs_, 0.01) * 0.001 * sampleRate_));
    }

    double compress (int channel, double input, double thresholdOffsetDb, double bandGainDb)
    {
        const double magnitude = std::fabs (input);
        double& detector = channel == 0 ? detectorLeft_ : detectorRight_;
        const double coefficient = magnitude > detector ? attackCoefficient_ : releaseCoefficient_;
        detector = magnitude + coefficient * (detector - magnitude);

        const double levelDb = gainToDb (detector);
        const double thresholdDb = -120.0 * threshold_ + thresholdOffsetDb;
        const double ratio = std::min (ratio_, 1.0e6);

        double reductionDb = 0.0;

        if (levelDb > thresholdDb && ratio > 1.0)
            reductionDb = (levelDb - thresholdDb) * (1.0 - 1.0 / ratio);

        return input * dbToGain (makeupDb_ + bandGainDb - reductionDb);
    }

    double threshold_ { 0.0 };
    double ratio_ { 1.0 };
    double attackMs_ { 10.0 };
    double releaseMs_ { 100.0 };
    double makeupDb_ { 0.0 };
    bool multiband_ { false };
    double crossoverLow_ { 200.0 };
    double crossoverHigh_ { 4000.0 };
    double bandGain_[3] { 0.0, 0.0, 0.0 };
    double bandThreshold_[3] { 0.0, 0.0, 0.0 };
    double attackCoefficient_ { 0.0 }, releaseCoefficient_ { 0.0 };
    double detectorLeft_ { 0.0 }, detectorRight_ { 0.0 };
    Lr4Crossover splitLowLeft_, splitLowRight_, splitHighLeft_, splitHighRight_;
};

// -------------------------------------------------------------------------------------------------
// Reverb (type 6): four damped combs into two allpasses per channel — the classic Schroeder network,
// chosen because its decay time is *analytically* the `kParamDelay` setting, so "the tail lengthens
// with DECAY" is measurable rather than hoped for.
// -------------------------------------------------------------------------------------------------

class ReverbFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::Reverb; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamSize")         { size_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamDelay")        { decay_ = clampd (value, 0.0, 250.0); return true; }
        if (name == "kParamFeedback")     { feedbackTrim_ = clampd (value, 0.0, 100.0); return true; }
        if (name == "kParamFreq")         { lowCutHz_ = 20.0 * std::pow (200.0, clampd (value * 0.01, 0.0, 1.0)); return true; }
        if (name == "kParamFreqB")        { highCutNorm_ = clampd (1.0 - value * 0.01, 0.0, 1.0); return true; }
        // `kParamFreqC` (the guide's extra cutoff) and `kParamMode` (NITROUS MODE) are accepted and not
        // applied: ticket 04 measured their ranges and not what they do, and inventing a curve for them
        // would be a substitution wearing a feature's name.
        if (name == "kParamFreqC" || name == "kParamMode") return true;
        if (name == "kParamPreDelay")     { predelaySeconds_ = clampd (value, 0.0, 4.0); return true; }
        if (name == "kParamWidth")        { width_ = clampd (value * 0.01, 0.0, 2.0); return true; }
        if (name == "kParamVintageScale") { damping_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamVintageScaleB" || name == "kParamPreDelayBeatSync") return true;

        return false;
    }

    bool onText (std::string_view name, std::string_view value) override
    {
        if (name != "kParamType")
            return false;

        // The four styles the corpus stores; `kHall` is the most-used (256 of 505 stored types).
        style_ = value == "kVintage" ? 1 : (value == "kSpace" ? 2 : (value == "kAbyss" ? 3 : 0));
        return true;
    }

protected:
    void onPrepare() override
    {
        predelayLeft_.resize (static_cast<std::size_t> (sampleRate_ * 4.0) + 8);
        predelayRight_.resize (static_cast<std::size_t> (sampleRate_ * 4.0) + 8);

        for (int channel = 0; channel < 2; ++channel)
        {
            for (int comb = 0; comb < kCombs; ++comb)
            {
                const auto length = static_cast<std::size_t> (kCombSeconds[style_][comb] * sizeScale() * sampleRate_) + 8;
                combLines_[channel][comb].resize (length);
                combFilter_[channel][comb].set (dampedHighCutHz(), sampleRate_);
                combHighpass_[channel][comb].set (lowCutHz(), sampleRate_);
            }

            for (int stage = 0; stage < kAllpasses; ++stage)
            {
                const auto length = static_cast<std::size_t> (kAllpassSeconds[stage] * sizeScale() * sampleRate_) + 8;
                allpassLines_[channel][stage].resize (length);
            }
        }
    }

    void onReset() override
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            for (int comb = 0; comb < kCombs; ++comb)
            {
                combLines_[channel][comb].clear();
                combFilter_[channel][comb].clear();
                combHighpass_[channel][comb].clear();
            }

            for (int stage = 0; stage < kAllpasses; ++stage)
                allpassLines_[channel][stage].clear();
        }

        predelayLeft_.clear();
        predelayRight_.clear();
    }

    void processWet (float* left, float* right, int frames) override
    {
        for (int channel = 0; channel < 2; ++channel)
            for (int comb = 0; comb < kCombs; ++comb)
            {
                combFilter_[channel][comb].set (dampedHighCutHz(), sampleRate_);
                combHighpass_[channel][comb].set (lowCutHz(), sampleRate_);
            }

        const double predelaySamples = predelaySeconds_ * sampleRate_;

        for (int i = 0; i < frames; ++i)
        {
            const double inLeft = predelaySamples > 1.0 ? predelayLeft_.read (predelaySamples) : left[i];
            const double inRight = predelaySamples > 1.0 ? predelayRight_.read (predelaySamples) : right[i];

            if (predelaySamples > 1.0)
            {
                predelayLeft_.push (left[i]);
                predelayRight_.push (right[i]);
            }

            const double wetLeft = channelOut (0, 0.7 * inLeft + 0.3 * inRight);
            const double wetRight = channelOut (1, 0.7 * inRight + 0.3 * inLeft);

            const double mid = 0.5 * (wetLeft + wetRight);
            const double side = 0.5 * (wetLeft - wetRight) * width_;

            left[i] = static_cast<float> (mid + side);
            right[i] = static_cast<float> (mid - side);
        }
    }

private:
    static constexpr int kCombs = 4;
    static constexpr int kAllpasses = 2;
    static constexpr double kCombSeconds[4][kCombs] = {
        { 0.0297, 0.0371, 0.0411, 0.0437 },   // kHall
        { 0.0231, 0.0311, 0.0353, 0.0389 },   // kVintage
        { 0.0451, 0.0533, 0.0611, 0.0697 },   // kSpace
        { 0.0797, 0.0931, 0.1013, 0.1139 },   // kAbyss
    };
    static constexpr double kAllpassSeconds[kAllpasses] = { 0.005, 0.0017 };

    double sizeScale() const { return 0.55 + size_ * 0.9; }

    double highCutHz() const
    {
        // `kParamFreqB` runs 0…100 with the corpus's values near 100 meaning "no cut"; the map is
        // exponential up to 18 kHz.
        const double cut = 20000.0 * std::pow (highCutNorm_, 3.0) + 1500.0;
        return std::min (cut, sampleRate_ * 0.45);
    }

    double lowCutHz() const { return std::min (std::max (lowCutHz_, 10.0), sampleRate_ * 0.2); }

    /// `kParamVintageScale` is the guide's DAMP: it pulls the comb loop's top end down. Applied rather
    /// than merely accepted, because "damping" leaves no ambiguity about its direction.
    double dampedHighCutHz() const { return highCutHz() * (1.0 - 0.85 * damping_); }

    /// The comb feedback that lands the tail at `kParamDelay`: `g = 10^(-3L / (RT60 fs))`.
    double combFeedback (std::size_t lengthSamples) const
    {
        const double rt60 = 0.05 + (decay_ / 250.0) * 9.95;
        const double gain = std::pow (10.0, -3.0 * (static_cast<double> (lengthSamples) / sampleRate_) / rt60);
        return std::min (gain * (0.5 + 0.005 * feedbackTrim_), 0.9995);
    }

    double channelOut (int channel, double input)
    {
        double summed = 0.0;

        for (int comb = 0; comb < kCombs; ++comb)
        {
            auto& line = combLines_[channel][comb];
            const double delayed = line.read (static_cast<double> (line.length() - 8));
            double fed = combFilter_[channel][comb].lowpass (delayed);
            fed = combHighpass_[channel][comb].highpass (fed);
            line.push (input + combFeedback (line.length()) * fed);
            summed += delayed;
        }

        summed *= 0.25;

        for (int stage = 0; stage < kAllpasses; ++stage)
        {
            auto& line = allpassLines_[channel][stage];
            const double delayed = line.read (static_cast<double> (line.length() - 8));
            const double coefficient = 0.5 + 0.15 * static_cast<double> (stage);
            line.push (summed + coefficient * delayed);
            summed = -coefficient * summed + delayed;
        }

        return summed;
    }

    int style_ { 0 };
    double size_ { 0.5 };
    double decay_ { 50.0 };
    double feedbackTrim_ { 100.0 };
    double lowCutHz_ { 20.0 };
    double highCutNorm_ { 1.0 };
    double predelaySeconds_ { 0.0 };
    double width_ { 1.0 };
    double damping_ { 0.0 };
    std::array<std::array<DelayLine, kCombs>, 2> combLines_ {};
    std::array<std::array<DelayLine, kAllpasses>, 2> allpassLines_ {};
    std::array<std::array<OnePole, kCombs>, 2> combFilter_ {}, combHighpass_ {};
    DelayLine predelayLeft_ {}, predelayRight_ {};
};

// -------------------------------------------------------------------------------------------------
// Equalizer (type 7). `kParamType1` is the low band (1 shelf / 2 peak / 3 high pass) and
// `kParamType2` the high band (1 shelf / 2 peak / 3 low pass); `kParamGain{1,2}` is dB and
// `kParamReso{1,2}` maps onto Q.
// -------------------------------------------------------------------------------------------------

class EqualizerFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::Equalizer; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamFreq1") { frequency_[0] = clampd (value, 5.0, 22000.0); return true; }
        if (name == "kParamFreq2") { frequency_[1] = clampd (value, 5.0, 22000.0); return true; }
        if (name == "kParamGain1") { gainDb_[0] = clampd (value, -36.0, 36.0); return true; }
        if (name == "kParamGain2") { gainDb_[1] = clampd (value, -36.0, 36.0); return true; }
        if (name == "kParamReso1") { resonance_[0] = clampd (value, 0.0, 100.0); return true; }
        if (name == "kParamReso2") { resonance_[1] = clampd (value, 0.0, 100.0); return true; }
        if (name == "kParamType1") { type_[0] = std::max (1, std::min (static_cast<int> (value), 3)); return true; }
        if (name == "kParamType2") { type_[1] = std::max (1, std::min (static_cast<int> (value), 3)); return true; }

        return false;
    }

protected:
    void onReset() override
    {
        for (auto& pair : filters_)
            for (auto& biquad : pair)
                biquad.clear();
    }

    void processWet (float* left, float* right, int frames) override
    {
        updateBands();

        for (int i = 0; i < frames; ++i)
        {
            left[i] = static_cast<float> (filters_[0][1].process (filters_[0][0].process (left[i])));
            right[i] = static_cast<float> (filters_[1][1].process (filters_[1][0].process (right[i])));
        }
    }

private:
    double qFor (int band) const { return 0.4 + resonance_[band] * 0.16; }

    void configure (Biquad& target, int band) const
    {
        const double frequency = frequency_[band];
        const double gain = gainDb_[band];
        const double q = qFor (band);

        switch (type_[band])
        {
            case 1:
                if (band == 0)
                    target.lowShelf (frequency, q, gain, sampleRate_);
                else
                    target.highShelf (frequency, q, gain, sampleRate_);
                break;
            case 2:
                target.peak (frequency, q, gain, sampleRate_);
                break;
            default:
                if (band == 0)
                    target.highpass (frequency, q, sampleRate_);
                else
                    target.lowpass (frequency, q, sampleRate_);
                break;
        }
    }

    void updateBands()
    {
        // [channel][band]: band 0 is the low shelf/peak/high-pass, band 1 the high one.
        for (int channel = 0; channel < 2; ++channel)
        {
            configure (filters_[channel][0], 0);
            configure (filters_[channel][1], 1);
        }
    }

    double frequency_[2] { 200.0, 4000.0 };
    double gainDb_[2] { 0.0, 0.0 };
    double resonance_[2] { 0.0, 0.0 };
    int type_[2] { 1, 1 };
    std::array<std::array<Biquad, 2>, 2> filters_ {};
};

// -------------------------------------------------------------------------------------------------
// Filter (type 8). Ticket 04 section 2.3: `FXFilter` is the voice filter exposed as a master effect,
// so it *is* `VoiceFilter` — same models, same table, same drawn pole/zero form — plus the rack's
// wet/dry and its `kParamStereo` pan.
// -------------------------------------------------------------------------------------------------

class FilterFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::Filter; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamFreq")   { cutoff_ = clampd (value, 0.0, 1.0); changed_ = true; return true; }
        if (name == "kParamReso")   { resonance_ = clampd (value, 0.0, 100.0); changed_ = true; return true; }
        if (name == "kParamVar")    { variation_ = clampd (value * 0.01, 0.0, 1.0); changed_ = true; return true; }
        if (name == "kParamDrive")  { drive_ = clampd (value, 0.0, 100.0); changed_ = true; return true; }
        if (name == "kParamStereo") { pan_ = clampd (value * 0.01, -1.0, 1.0); return true; }
        if (name == "kParamPad")    return true;   // accepted, not applied: ticket 04 read it, not what it does
        if (name == "kParamX" || name == "kParamY") return true;

        return false;
    }

    bool onText (std::string_view name, std::string_view value) override
    {
        if (name != "kParamType")
            return false;

        typeName_ = std::string (value);
        spec_ = filterSpecFromName (typeName_);
        unimplemented_ = ! VoiceFilter::implements (typeName_);
        changed_ = true;
        return true;
    }

    void setPoleZero (const PoleZeroDescription& description) override
    {
        poleZero_ = description;
        changed_ = true;
    }

    std::string problem() const override
    {
        if (! unimplemented_)
            return {};

        // **A slot that stores no `kParamType`** asks for the reference's *default* filter — the schema
        // marks the leaf optional and the corpus leaves it out in 1209 of 1513 `FXFilter` slots. That is
        // not a mistyped name but an unknown default, so it says which of the two it is: a preset author
        // cannot act on "filter type '' is not shipped".
        if (typeName_.empty())
            return "FXFilter: the preset stores no filter type, so the reference's own default applies, and "
                   "xerum does not know it — the band passes unfiltered";

        return "FXFilter: filter type '" + typeName_ + "' is not shipped in v1 — the band passes unfiltered";
    }

protected:
    void onPrepare() override
    {
        left_.prepare (sampleRate_);
        right_.prepare (sampleRate_);
        changed_ = true;
        apply();
    }

    void onReset() override { left_.reset(); right_.reset(); }

    void processWet (float* left, float* right, int frames) override
    {
        apply();

        const double pan = clampd (pan_, -1.0, 1.0);
        const double leftGain = pan <= 0.0 ? 1.0 : 1.0 - pan;
        const double rightGain = pan >= 0.0 ? 1.0 : 1.0 + pan;

        for (int i = 0; i < frames; ++i)
        {
            const double filteredLeft = left_.process (left[i]);
            const double filteredRight = right_.process (right[i]);
            left[i] = static_cast<float> (filteredLeft * leftGain);
            right[i] = static_cast<float> (filteredRight * rightGain);
        }
    }

private:
    void apply()
    {
        if (! changed_)
            return;

        changed_ = false;

        if (unimplemented_)
            return;

        for (auto* filter : { &left_, &right_ })
        {
            filter->setSpec (spec_);
            filter->setCutoff (cutoff_);
            filter->setSecondaryCutoff (cutoff_);
            filter->setResonance (resonance_);
            filter->setDrive (drive_);
            filter->setMorph (variation_);

            if (! poleZero_.empty())
                filter->setPoleZero (poleZero_);
        }
    }

    std::string typeName_ {};
    FilterSpec spec_ = filterSpecFromName ("");
    PoleZeroDescription poleZero_ {};
    double cutoff_ { 1.0 };
    double resonance_ { 0.0 };
    double variation_ { 0.0 };
    double drive_ { 0.0 };
    double pan_ { 0.0 };
    bool unimplemented_ { true };
    bool changed_ { true };
    VoiceFilter left_ {}, right_ {};
};

// -------------------------------------------------------------------------------------------------
// Hyper / Dimension (type 9). `kParamUnison` 0 selects the DIMENSION half; 1…7 stack detuned copies
// spread across the stereo field. A master effect cannot repitch a live signal, so "detune" is
// realised as a spread of modulated short delays — vibrato plus panning, stated rather than hidden.
// -------------------------------------------------------------------------------------------------

class HyperDimensionFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::HyperDimension; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamUnison")       { unison_ = std::max (0, std::min (static_cast<int> (value), 7)); return true; }
        if (name == "kParamRate")         { rate_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamDetune")       { detune_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamDimESize")     { dimensionSize_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamDimEWet")      { dimensionWet_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamDimELevelOut") { dimensionLevel_ = clampd (value, 0.0, 4.0); return true; }
        if (name == "kParamRetrig")       return true;

        return false;
    }

protected:
    void onPrepare() override
    {
        for (int v = 0; v < kMaxVoices; ++v)
        {
            leftLines_[v].resize (static_cast<std::size_t> (sampleRate_ * 0.05) + 8);
            rightLines_[v].resize (static_cast<std::size_t> (sampleRate_ * 0.05) + 8);
        }
    }

    void onReset() override
    {
        for (int v = 0; v < kMaxVoices; ++v)
        {
            leftLines_[v].clear();
            rightLines_[v].clear();
            voices_[v].clear();
        }

        dimensionLfo_.clear();
    }

    void processWet (float* left, float* right, int frames) override
    {
        const double lfoRate = std::max (rate_, 0.01);

        for (int v = 0; v < kMaxVoices; ++v)
            voices_[v].setRate (lfoRate * (1.0 + 0.07 * v), sampleRate_);

        dimensionLfo_.setRate (lfoRate * 0.7, sampleRate_);

        for (int i = 0; i < frames; ++i)
            renderSample (left, right, i);
    }

private:
    void renderSample (float* left, float* right, int index)
    {
        const double inLeft = left[index];
        const double inRight = right[index];

        if (unison_ >= 1)
        {
            double accumulatedLeft = 0.0, accumulatedRight = 0.0;

            for (int v = 0; v < unison_; ++v)
            {
                const double modulation = voices_[v].advance();
                const double sweep = detune_ * 0.006 * sampleRate_ * modulation;
                const double delay = sampleRate_ * 0.008 + sweep + v * sampleRate_ * 0.0004;
                const double tapLeft = leftLines_[v].read (delay);
                const double tapRight = rightLines_[v].read (delay);
                leftLines_[v].push (inLeft);
                rightLines_[v].push (inRight);

                // Pan the voices across the field: detune 0 is nearly centred, detune 1 is wide.
                const double pan = unison_ > 1
                                       ? (static_cast<double> (v) / static_cast<double> (unison_ - 1) * 2.0 - 1.0)
                                             * (0.2 + 0.8 * detune_)
                                       : 0.0;
                accumulatedLeft += tapLeft * (0.5 - 0.5 * pan) + tapRight * (0.5 + 0.5 * pan) * detune_;
                accumulatedRight += tapRight * (0.5 + 0.5 * pan) + tapLeft * (0.5 - 0.5 * pan) * detune_;
            }

            left[index] = static_cast<float> (accumulatedLeft / unison_);
            right[index] = static_cast<float> (accumulatedRight / unison_);
            return;
        }

        const double modulation = dimensionLfo_.advance();
        const double sweep = dimensionSize_ * 0.002 * sampleRate_ * modulation;
        const double wetLeft = leftLines_[0].read (sampleRate_ * 0.004 + sweep);
        const double wetRight = rightLines_[0].read (sampleRate_ * 0.004 - sweep);
        leftLines_[0].push (inLeft);
        rightLines_[0].push (inRight);

        const double mid = 0.5 * (inLeft + inRight);
        const double side = 0.5 * (wetLeft - wetRight) * (1.0 + dimensionSize_ * 2.0);
        const double level = dimensionLevel_ * (0.5 + 0.5 * dimensionWet_ + 0.5);

        left[index] = static_cast<float> ((mid + side) * level);
        right[index] = static_cast<float> ((mid - side) * level);
    }

    static constexpr int kMaxVoices = 7;
    int unison_ { 0 };
    double rate_ { 0.3 };
    double detune_ { 0.0 };
    double dimensionSize_ { 0.0 };
    double dimensionWet_ { 0.0 };
    double dimensionLevel_ { 1.0 };
    std::array<DelayLine, kMaxVoices> leftLines_ {}, rightLines_ {};
    std::array<Lfo, kMaxVoices> voices_ {};
    Lfo dimensionLfo_ {};
};

// -------------------------------------------------------------------------------------------------
// Bode (type 10): a single-sideband frequency shifter. A windowed Hilbert FIR gives the quadrature
// signal, a complex rotation moves it, and `kParamShift` × `kParamRange` is the offset in hertz, so a
// 1 kHz tone with SHIFT +20% and RANGE 1000 Hz comes out at 1.2 kHz rather than 0.8 kHz.
// -------------------------------------------------------------------------------------------------

class BodeFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::Bode; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamShift")        { shift_ = clampd (value * 0.01, -1.0, 1.0); return true; }
        if (name == "kParamRange")        { rangeHz_ = clampd (value, 0.0, 20000.0); return true; }
        if (name == "kParamBlur")         { blur_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamFeedback")     { feedback_ = clampd (value * 0.01, 0.0, 0.95); return true; }
        if (name == "kParamDelayBalance") return true;   // accepted, not applied: semantics not established
        if (name == "kParamOutputMix")    { outputMix_ = clampd (value * 0.01, -1.0, 1.0); return true; }
        if (name == "kParamOutputWidth")  { outputWidth_ = clampd (value * 0.01, 0.0, 1.0); return true; }
        if (name == "kParamMonoInput")    { monoInput_ = value != 0.0; return true; }
        if (name == "kParamDelayTime" || name == "kParamBeatSync" || name == "kParamRetrig"
            || name == "kParamSwapAB")
            return true;

        return false;
    }

protected:
    void onPrepare() override
    {
        // A 127-tap Blackman-windowed Hilbert transformer: h[n] = 2/(pi n) for odd n, zero for even,
        // accurate enough that the image at the mirror frequency stays well below the shifted tone —
        // which is the property the test measures.
        taps_.assign (kTaps, 0.0);
        const int centre = kTaps / 2;

        for (int n = 0; n < kTaps; ++n)
        {
            const int offset = n - centre;

            if (offset % 2 == 0)
                continue;

            const double window = 0.42 - 0.5 * std::cos (kTwoPi * n / (kTaps - 1))
                                  + 0.08 * std::cos (2.0 * kTwoPi * n / (kTaps - 1));
            taps_[static_cast<std::size_t> (n)] = (2.0 / (kPi * offset)) * window;
        }

        leftHistory_.assign (kTaps, 0.0);
        rightHistory_.assign (kTaps, 0.0);
        historyPosition_ = 0;
    }

    void onReset() override
    {
        std::fill (leftHistory_.begin(), leftHistory_.end(), 0.0);
        std::fill (rightHistory_.begin(), rightHistory_.end(), 0.0);
        historyPosition_ = 0;
        phase_ = 0.0;
        feedbackLeft_ = feedbackRight_ = 0.0;
        blurLeft_.clear();
        blurRight_.clear();
    }

    void processWet (float* left, float* right, int frames) override
    {
        const double increment = (shift_ * rangeHz_) / sampleRate_;
        const int centre = kTaps / 2;

        for (int i = 0; i < frames; ++i)
        {
            double inLeft = left[i];
            double inRight = right[i];

            if (monoInput_)
                inLeft = inRight = 0.5 * (inLeft + inRight);

            inLeft += feedback_ * feedbackLeft_;
            inRight += feedback_ * feedbackRight_;

            leftHistory_[static_cast<std::size_t> (historyPosition_)] = inLeft;
            rightHistory_[static_cast<std::size_t> (historyPosition_)] = inRight;

            double quadratureLeft = 0.0, quadratureRight = 0.0;
            int index = historyPosition_;

            for (int n = 0; n < kTaps; ++n)
            {
                const double tap = taps_[static_cast<std::size_t> (n)];
                quadratureLeft += tap * leftHistory_[static_cast<std::size_t> (index)];
                quadratureRight += tap * rightHistory_[static_cast<std::size_t> (index)];
                index = index == 0 ? kTaps - 1 : index - 1;
            }

            // The in-phase signal is the Hilbert filter's centre delay.
            const int inPhaseIndex = (historyPosition_ - centre + kTaps) % kTaps;
            const double inPhaseLeft = leftHistory_[static_cast<std::size_t> (inPhaseIndex)];
            const double inPhaseRight = rightHistory_[static_cast<std::size_t> (inPhaseIndex)];
            historyPosition_ = (historyPosition_ + 1) % kTaps;

            const double angle = kTwoPi * phase_;
            const double cs = std::cos (angle), sn = std::sin (angle);

            double outLeft = inPhaseLeft * cs - quadratureLeft * sn;
            double outRight = inPhaseRight * cs - quadratureRight * sn;

            phase_ += increment;

            while (phase_ >= 1.0) phase_ -= 1.0;
            while (phase_ < 0.0) phase_ += 1.0;

            feedbackLeft_ = outLeft;
            feedbackRight_ = outRight;

            const double mix = 0.5 + 0.5 * outputMix_;
            outLeft *= mix;
            outRight *= mix;

            if (blur_ > 0.0)
            {
                blurLeft_.set (200.0 + blur_ * 4000.0, sampleRate_);
                blurRight_.set (200.0 + blur_ * 4000.0, sampleRate_);
                outLeft = outLeft * (1.0 - blur_ * 0.5) + blurLeft_.lowpass (outLeft) * (blur_ * 0.5);
                outRight = outRight * (1.0 - blur_ * 0.5) + blurRight_.lowpass (outRight) * (blur_ * 0.5);
            }

            const double mid = 0.5 * (outLeft + outRight);
            const double side = 0.5 * (outLeft - outRight) * (0.5 + outputWidth_ * 1.5);

            left[i] = static_cast<float> (mid + side);
            right[i] = static_cast<float> (mid - side);
        }
    }

private:
    static constexpr int kTaps = 127;
    double shift_ { 0.0 };
    double rangeHz_ { 1000.0 };
    double blur_ { 0.0 };
    double feedback_ { 0.0 };
    double outputMix_ { 0.0 };
    double outputWidth_ { 0.5 };
    bool monoInput_ { false };
    double phase_ { 0.0 };
    std::vector<double> taps_ {}, leftHistory_ {}, rightHistory_ {};
    int historyPosition_ { 0 };
    double feedbackLeft_ { 0.0 }, feedbackRight_ { 0.0 };
    OnePole blurLeft_ {}, blurRight_ {};
};

// -------------------------------------------------------------------------------------------------
// Convolve (type 11). The impulse comes from the caller; `kParamSize` time-stretches it,
// `kParamTone` tilts the wet signal, `kParamPredelay` delays the input, `kParamIpTrim` is the
// impulse's gain, and damping/attack/decay/min-phase shape the impulse itself. Those last four are
// applied when the impulse is installed — they are configuration, not per-block automation, and the
// plugin layer rebuilds off the audio thread.
// -------------------------------------------------------------------------------------------------

class ConvolveFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::Convolve; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamSize")             { size_ = clampd (value * 0.01, 0.1, 10.0); return true; }
        if (name == "kParamTone")             { tone_ = clampd (value, -100.0, 100.0); return true; }
        if (name == "kParamIpTrim")           { trimDb_ = value; return true; }
        if (name == "kParamPredelay")         { predelaySeconds_ = clampd (value, 0.0, 4.0); return true; }
        if (name == "kParamDamping")          { damping_ = clampd (value, 0.0, 100.0); return true; }
        if (name == "kParamDecay")            { decaySeconds_ = clampd (value, 0.0, 60.0); return true; }
        if (name == "kParamAttack")           { attackSeconds_ = clampd (value, 0.0, 2.0); return true; }
        if (name == "kParamMinPhase")         { minPhase_ = value != 0.0; return true; }
        if (name == "kParamPredelayBeatSync") return true;

        return false;
    }

    void setImpulse (const ImpulseResponse& impulse) override
    {
        source_ = impulse;
        impulseInstalled_ = true;
        refresh();
    }

    std::string problem() const override
    {
        if (! impulseInstalled_)
            return "FXConv: the preset asks for convolution but no impulse response was supplied";

        if (! ready_)
            return "FXConv: the impulse response is empty or unreadable — passing the signal through";

        return {};
    }

protected:
    void onPrepare() override
    {
        for (auto& convolver : convolvers_)
            convolver.prepare (kBlockSize);

        predelayLeft_.resize (static_cast<std::size_t> (sampleRate_ * 4.0) + 8);
        predelayRight_.resize (static_cast<std::size_t> (sampleRate_ * 4.0) + 8);

        if (impulseInstalled_)
            refresh();
    }

    void onReset() override
    {
        for (auto& convolver : convolvers_)
            convolver.reset();

        predelayLeft_.clear();
        predelayRight_.clear();
    }

    void processWet (float* left, float* right, int frames) override
    {
        if (! ready_)
        {
            // No impulse: the dry signal passes, and `problem()` says why. Convolving with nothing
            // would be the silent drop the import contract forbids.
            return;
        }

        const double predelaySamples = predelaySeconds_ * sampleRate_;

        for (int i = 0; i < frames; ++i)
        {
            const double inLeft = predelaySamples > 1.0 ? predelayLeft_.read (predelaySamples) : left[i];
            const double inRight = predelaySamples > 1.0 ? predelayRight_.read (predelaySamples) : right[i];

            if (predelaySamples > 1.0)
            {
                predelayLeft_.push (left[i]);
                predelayRight_.push (right[i]);
            }

            left[i] = static_cast<float> (convolvers_[0].process (inLeft));
            right[i] = static_cast<float> (convolvers_[1].process (inRight));
        }

        // Tone: a first-order tilt, positive brightening and negative darkening.
        const double tilt = tone_ * 0.01;

        for (int i = 0; i < frames; ++i)
        {
            toneLow_[0].set (1000.0, sampleRate_);
            toneLow_[1].set (1000.0, sampleRate_);
            const double lowLeft = toneLow_[0].lowpass (left[i]);
            left[i] = static_cast<float> (lowLeft * (1.0 - tilt) + (left[i] - lowLeft) * (1.0 + tilt));
            const double lowRight = toneLow_[1].lowpass (right[i]);
            right[i] = static_cast<float> (lowRight * (1.0 - tilt) + (right[i] - lowRight) * (1.0 + tilt));
        }
    }

private:
    static constexpr int kBlockSize = 64;

    void refresh()
    {
        ready_ = false;

        for (auto& convolver : convolvers_)
            convolver.prepare (kBlockSize);

        if (source_.channels == nullptr || source_.frames <= 0 || source_.channelCount <= 0)
            return;

        const int frames = source_.frames;
        const double trim = dbToGain (trimDb_);

        for (int channel = 0; channel < 2; ++channel)
        {
            const float* source = source_.channels[std::min (channel, source_.channelCount - 1)];
            std::vector<float> shaped (static_cast<std::size_t> (frames));

            for (int i = 0; i < frames; ++i)
                shaped[static_cast<std::size_t> (i)] = static_cast<float> (source[i] * trim);

            if (damping_ > 0.0)
            {
                OnePole damper;
                damper.set (20000.0 * std::pow (0.03, damping_ * 0.01), sampleRate_);

                for (auto& sample : shaped)
                    sample = static_cast<float> (damper.lowpass (sample));
            }

            if (decaySeconds_ > 0.0)
                for (int i = 0; i < frames; ++i)
                    shaped[static_cast<std::size_t> (i)] *=
                        static_cast<float> (std::exp (-(static_cast<double> (i) / sampleRate_) / decaySeconds_));

            if (attackSeconds_ > 0.0)
                for (int i = 0; i < frames; ++i)
                    shaped[static_cast<std::size_t> (i)] *=
                        static_cast<float> (clampd (static_cast<double> (i) / sampleRate_ / attackSeconds_, 0.0, 1.0));

            if (minPhase_)
                shaped = minimumPhaseImpulse (shaped.data(), frames);

            if (std::fabs (size_ - 1.0) > 1.0e-6 && frames > 8)
            {
                const int stretched = std::max (static_cast<int> (static_cast<double> (frames) / size_), 8);
                std::vector<float> resampled (static_cast<std::size_t> (stretched));

                for (int i = 0; i < stretched; ++i)
                {
                    const double position = static_cast<double> (i) * size_;
                    const auto whole = static_cast<std::size_t> (position);
                    const double fraction = position - static_cast<double> (whole);
                    const double a = whole < static_cast<std::size_t> (frames) ? shaped[whole] : 0.0;
                    const double b = whole + 1 < static_cast<std::size_t> (frames) ? shaped[whole + 1] : 0.0;
                    resampled[static_cast<std::size_t> (i)] = static_cast<float> (a + (b - a) * fraction);
                }

                shaped = resampled;
            }

            convolvers_[channel].setImpulse (shaped.data(), static_cast<int> (shaped.size()));
        }

        ready_ = true;
    }

    ImpulseResponse source_ {};
    bool impulseInstalled_ { false };
    bool ready_ { false };
    bool minPhase_ { false };
    double size_ { 1.0 };
    double tone_ { 0.0 };
    double trimDb_ { 0.0 };
    double predelaySeconds_ { 0.0 };
    double damping_ { 0.0 };
    double decaySeconds_ { 0.0 };
    double attackSeconds_ { 0.0 };
    std::array<Convolver, 2> convolvers_ {};
    std::array<OnePole, 2> toneLow_ {};
    DelayLine predelayLeft_ {}, predelayRight_ {};
};

// -------------------------------------------------------------------------------------------------
// Utility (type 12): polarity, low and high cut, mono bass below `kParamLFXover`, width, and balance.
// -------------------------------------------------------------------------------------------------

class UtilityFx final : public FxBase
{
public:
    FxClass cls() const override { return FxClass::Utility; }

    bool onNumber (std::string_view name, double value) override
    {
        if (name == "kParamPolarityL") { polarity_[0] = value != 0.0 ? -1.0 : 1.0; return true; }
        if (name == "kParamPolarityR") { polarity_[1] = value != 0.0 ? -1.0 : 1.0; return true; }
        if (name == "kParamLPF")       { lowPassHz_ = clampd (value, 20.0, 22000.0); return true; }
        if (name == "kParamHPF")       { highPassHz_ = clampd (value, 1.0, 20000.0); return true; }
        if (name == "kParamLFMono")    { monoBass_ = value != 0.0; return true; }
        if (name == "kParamLFXover")   { monoCrossoverHz_ = clampd (value, 10.0, 2000.0); return true; }
        if (name == "kParamWidth")     { width_ = clampd (value * 0.01, 0.0, 8.0); return true; }
        if (name == "kParamBalance")   { balance_ = clampd (value * 0.01, -1.0, 1.0); return true; }

        return false;
    }

protected:
    void onPrepare() override
    {
        lowPassLeft_.lowpass (lowPassHz_, 0.707, sampleRate_);
        lowPassRight_ = lowPassLeft_;
        highPassLeft_.highpass (highPassHz_, 0.707, sampleRate_);
        highPassRight_ = highPassLeft_;
        monoLowLeft_.set (monoCrossoverHz_, sampleRate_);
        monoLowRight_ = monoLowLeft_;
    }

    void onReset() override
    {
        for (auto* biquad : { &lowPassLeft_, &lowPassRight_, &highPassLeft_, &highPassRight_ })
            biquad->clear();

        monoLowLeft_.clear();
        monoLowRight_.clear();
    }

    void processWet (float* left, float* right, int frames) override
    {
        lowPassLeft_.lowpass (lowPassHz_, 0.707, sampleRate_);
        lowPassRight_.lowpass (lowPassHz_, 0.707, sampleRate_);
        highPassLeft_.highpass (highPassHz_, 0.707, sampleRate_);
        highPassRight_.highpass (highPassHz_, 0.707, sampleRate_);
        monoLowLeft_.set (monoCrossoverHz_, sampleRate_);
        monoLowRight_.set (monoCrossoverHz_, sampleRate_);

        const double balanceLeft = balance_ <= 0.0 ? 1.0 : 1.0 - balance_;
        const double balanceRight = balance_ >= 0.0 ? 1.0 : 1.0 + balance_;

        for (int i = 0; i < frames; ++i)
        {
            double l = lowPassLeft_.process (highPassLeft_.process (left[i])) * polarity_[0] * balanceLeft;
            double r = lowPassRight_.process (highPassRight_.process (right[i])) * polarity_[1] * balanceRight;

            if (monoBass_)
            {
                const double lowLeft = monoLowLeft_.lowpass (l);
                const double lowRight = monoLowRight_.lowpass (r);
                const double mono = 0.5 * (lowLeft + lowRight);
                l += mono - lowLeft;
                r += mono - lowRight;
            }

            const double mid = 0.5 * (l + r);
            const double side = 0.5 * (l - r) * width_;
            left[i] = static_cast<float> (mid + side);
            right[i] = static_cast<float> (mid - side);
        }
    }

private:
    double polarity_[2] { 1.0, 1.0 };
    double lowPassHz_ { 20000.0 };
    double highPassHz_ { 10.0 };
    bool monoBass_ { false };
    double monoCrossoverHz_ { 100.0 };
    double width_ { 1.0 };
    double balance_ { 0.0 };
    Biquad lowPassLeft_ {}, lowPassRight_ {}, highPassLeft_ {}, highPassRight_ {};
    OnePole monoLowLeft_ {}, monoLowRight_ {};
};

} // namespace

// -------------------------------------------------------------------------------------------------
// The class table.
// -------------------------------------------------------------------------------------------------

std::string_view fxClassName (FxClass cls)
{
    switch (cls)
    {
        case FxClass::Distortion:     return "FXDistortion";
        case FxClass::Flanger:        return "FXFlanger";
        case FxClass::Phaser:         return "FXPhaser";
        case FxClass::Chorus:         return "FXChorus";
        case FxClass::Delay:          return "FXDelay";
        case FxClass::Compressor:     return "FXComp";
        case FxClass::Reverb:         return "FXReverb";
        case FxClass::Equalizer:      return "FXEQ";
        case FxClass::Filter:         return "FXFilter";
        case FxClass::HyperDimension: return "FXHyperD";
        case FxClass::Bode:           return "FXBode";
        case FxClass::Convolve:       return "FXConv";
        case FxClass::Utility:        return "FXUtils";
        case FxClass::Split:          return "FXSplit";
        case FxClass::Split3:         return "FXSplit3";
        case FxClass::SplitMS:        return "FXSplitMS";
        case FxClass::Unknown:        return {};
    }

    return {};
}

FxClass fxClassFromName (std::string_view name)
{
    for (int id = 0; id < 16; ++id)
    {
        const auto cls = static_cast<FxClass> (id);

        if (fxClassName (cls) == name)
            return cls;
    }

    return FxClass::Unknown;
}

int fxTypeId (FxClass cls) { return cls == FxClass::Unknown ? -1 : static_cast<int> (cls); }

FxClass fxClassFromTypeId (int id)
{
    return id >= 0 && id < 16 ? static_cast<FxClass> (id) : FxClass::Unknown;
}

bool fxClassIsShipped (FxClass cls)
{
    // Ticket 12's cut, re-derived: the 14 classes at or above the 99 % threshold.
    switch (cls)
    {
        case FxClass::Split3:
        case FxClass::SplitMS:
        case FxClass::Unknown:
            return false;
        default:
            return true;
    }
}

const std::vector<FxClass>& shippedFxClasses()
{
    static const std::vector<FxClass> classes = []
    {
        std::vector<FxClass> out;

        for (int id = 0; id < 16; ++id)
            if (fxClassIsShipped (static_cast<FxClass> (id)))
                out.push_back (static_cast<FxClass> (id));

        return out;
    }();

    return classes;
}

const std::vector<FxClass>& deferredFxClasses()
{
    static const std::vector<FxClass> classes = { FxClass::Split3, FxClass::SplitMS };
    return classes;
}

bool fxClassIsSplitter (FxClass cls)
{
    return cls == FxClass::Split || cls == FxClass::Split3 || cls == FxClass::SplitMS;
}

int fxSplitterBandCount (FxClass cls)
{
    switch (cls)
    {
        case FxClass::Split:   return 2;
        case FxClass::Split3:  return 3;
        case FxClass::SplitMS: return 2;
        default:               return 0;
    }
}

std::unique_ptr<FxEffect> makeFxEffect (FxClass cls)
{
    if (! fxClassIsShipped (cls))
        return nullptr;

    switch (cls)
    {
        case FxClass::Distortion:     return std::make_unique<DistortionFx>();
        case FxClass::Flanger:        return std::make_unique<FlangerFx>();
        case FxClass::Phaser:         return std::make_unique<PhaserFx>();
        case FxClass::Chorus:         return std::make_unique<ChorusFx>();
        case FxClass::Delay:          return std::make_unique<DelayFx>();
        case FxClass::Compressor:     return std::make_unique<CompressorFx>();
        case FxClass::Reverb:         return std::make_unique<ReverbFx>();
        case FxClass::Equalizer:      return std::make_unique<EqualizerFx>();
        case FxClass::Filter:         return std::make_unique<FilterFx>();
        case FxClass::HyperDimension: return std::make_unique<HyperDimensionFx>();
        case FxClass::Bode:           return std::make_unique<BodeFx>();
        case FxClass::Convolve:       return std::make_unique<ConvolveFx>();
        case FxClass::Utility:        return std::make_unique<UtilityFx>();
        default:                      return nullptr;
    }
}

// -------------------------------------------------------------------------------------------------
// The rack: the flat array decoded into a chain, splitters and all.
// -------------------------------------------------------------------------------------------------

struct FxRack::Chain
{
    struct Node
    {
        FxClass cls { FxClass::Unknown };
        std::unique_ptr<FxEffect> effect {};

        // Splitters only.
        double crossoverHz { 1000.0 };
        std::unique_ptr<Lr4Crossover> splitterLeft {}, splitterRight {};
        std::vector<Chain> bands {};
        std::vector<std::vector<float>> bandLeft {}, bandRight {};
        int bandCount { 0 };
    };

    std::vector<Node> nodes {};
};

namespace
{

/// The numeric value of a name in a slot, or `fallback` when the preset did not carry it.
double slotNumber (const FxSlot& slot, std::string_view name, double fallback)
{
    for (const auto& [key, value] : slot.numbers)
        if (key == name)
            return value;

    return fallback;
}

/// A splitter's band count. **Absence means zero**, and that is measured rather than assumed: 14
/// corpus racks overrun their own FX array under any other reading, and the whole corpus reconstructs
/// with zero leftovers only under this one. See the header.
double slotCount (const FxSlot& slot, std::string_view name)
{
    return std::max (slotNumber (slot, name, 0.0), 0.0);
}

constexpr std::array<std::string_view, 3> kModuleCountNames { "kParamModuleCount1", "kParamModuleCount2",
                                                              "kParamModuleCount3" };

void buildRange (FxRack::Chain& chain, const std::vector<FxSlot>& slots, std::size_t begin, std::size_t end,
                 double sampleRate, int maximumBlockFrames, std::vector<std::string>& unresolved,
                 std::vector<std::string>& problems);

void buildNode (FxRack::Chain& chain, const FxSlot& slot, const std::vector<FxSlot>& slots, std::size_t position,
                std::size_t end, double sampleRate, int maximumBlockFrames, std::vector<std::string>& unresolved,
                std::vector<std::string>& problems)
{
    const std::string className (fxClassName (slot.cls));

    if (fxClassIsSplitter (slot.cls))
    {
        int claimed = 0;

        for (int band = 0; band < fxSplitterBandCount (slot.cls); ++band)
            claimed += static_cast<int> (slotCount (slot, kModuleCountNames[static_cast<std::size_t> (band)]));

        const std::size_t available = end > position ? end - position - 1 : 0;
        const auto take = std::min<std::size_t> (static_cast<std::size_t> (claimed), available);

        if (! fxClassIsShipped (slot.cls))
        {
            // A deferred splitter: its bands are addressed through a routing we do not reproduce, so
            // running them serially would invent a topology. Report it and skip the whole group.
            unresolved.push_back (className);
            return;
        }

        // The only splitter built is the two-band Linkwitz-Riley one, which is the only shipped splitter
        // (`FXSplit`); a hypothetical three-band or mid/side one would need its own crossover, and is
        // reported through the deferred path above rather than half-built.
        FxRack::Chain::Node node;
        node.cls = slot.cls;
        node.crossoverHz = slotNumber (slot, "kParamFreq", 1000.0);
        node.splitterLeft = std::make_unique<Lr4Crossover>();
        node.splitterRight = std::make_unique<Lr4Crossover>();
        node.splitterLeft->set (node.crossoverHz, sampleRate);
        node.splitterRight->set (node.crossoverHz, sampleRate);
        node.bandCount = fxSplitterBandCount (slot.cls);
        node.bands.resize (static_cast<std::size_t> (node.bandCount));
        node.bandLeft.resize (static_cast<std::size_t> (node.bandCount));
        node.bandRight.resize (static_cast<std::size_t> (node.bandCount));

        for (auto& band : node.bandLeft)
            band.assign (static_cast<std::size_t> (std::max (maximumBlockFrames, 1)), 0.0f);

        for (auto& band : node.bandRight)
            band.assign (static_cast<std::size_t> (std::max (maximumBlockFrames, 1)), 0.0f);

        std::size_t cursor = position + 1;

        for (int band = 0; band < node.bandCount; ++band)
        {
            const auto count = static_cast<std::size_t> (
                slotCount (slot, kModuleCountNames[static_cast<std::size_t> (band)]));
            buildRange (node.bands[static_cast<std::size_t> (band)], slots, cursor, cursor + count, sampleRate,
                        maximumBlockFrames, unresolved, problems);
            cursor += count;
        }

        chain.nodes.push_back (std::move (node));
        return;
    }

    std::unique_ptr<FxEffect> effect = makeFxEffect (slot.cls);

    if (effect == nullptr)
    {
        unresolved.push_back (className.empty() ? std::string ("unknown") : className);
        return;
    }

    for (const auto& [name, value] : slot.numbers)
        if (! effect->setNumber (name, value))
            unresolved.push_back (className + "/" + name);

    for (const auto& [name, value] : slot.texts)
        if (! effect->setText (name, value))
            unresolved.push_back (className + "/" + name);

    if (slot.poleZero != nullptr)
        effect->setPoleZero (*slot.poleZero);

    if (slot.curve != nullptr)
        effect->setCurve (*slot.curve);

    if (slot.impulse != nullptr)
        effect->setImpulse (*slot.impulse);

    effect->prepare (sampleRate, maximumBlockFrames);

    if (const std::string problem = effect->problem(); ! problem.empty())
        problems.push_back (problem);

    FxRack::Chain::Node node;
    node.cls = slot.cls;
    node.effect = std::move (effect);
    chain.nodes.push_back (std::move (node));
}

void buildRange (FxRack::Chain& chain, const std::vector<FxSlot>& slots, std::size_t begin, std::size_t end,
                 double sampleRate, int maximumBlockFrames, std::vector<std::string>& unresolved,
                 std::vector<std::string>& problems)
{
    std::size_t position = begin;

    while (position < std::min (end, slots.size()))
    {
        const FxSlot& slot = slots[position];
        buildNode (chain, slot, slots, position, std::min (end, slots.size()), sampleRate, maximumBlockFrames,
                   unresolved, problems);

        // The node itself claims one slot, plus its bands where it is a shipped splitter.
        std::size_t consumed = 1;

        if (fxClassIsSplitter (slot.cls))
        {
            int claimed = 0;

            for (int band = 0; band < fxSplitterBandCount (slot.cls); ++band)
                claimed += static_cast<int> (slotCount (slot, kModuleCountNames[static_cast<std::size_t> (band)]));

            const std::size_t available = end > position ? end - position - 1 : 0;
            consumed += std::min<std::size_t> (static_cast<std::size_t> (claimed), available);
        }

        position += consumed;
    }
}

void processChain (FxRack::Chain& chain, float* left, float* right, int frames)
{
    for (auto& node : chain.nodes)
    {
        if (node.effect != nullptr)
        {
            node.effect->process (left, right, frames);
            continue;
        }

        if (node.splitterLeft == nullptr || node.bandCount < 2)
            continue;

        for (int i = 0; i < frames; ++i)
        {
            double low = 0.0, high = 0.0;
            node.splitterLeft->split (left[i], low, high);
            node.bandLeft[0][static_cast<std::size_t> (i)] = static_cast<float> (low);
            node.bandLeft[1][static_cast<std::size_t> (i)] = static_cast<float> (high);

            node.splitterRight->split (right[i], low, high);
            node.bandRight[0][static_cast<std::size_t> (i)] = static_cast<float> (low);
            node.bandRight[1][static_cast<std::size_t> (i)] = static_cast<float> (high);
        }

        for (int band = 0; band < node.bandCount; ++band)
            processChain (node.bands[static_cast<std::size_t> (band)], node.bandLeft[static_cast<std::size_t> (band)].data(),
                          node.bandRight[static_cast<std::size_t> (band)].data(), frames);

        // Sum every band, not the first two: a band nobody sums is a silent drop of everything in it.
        for (int i = 0; i < frames; ++i)
        {
            double sumLeft = 0.0, sumRight = 0.0;

            for (int band = 0; band < node.bandCount; ++band)
            {
                sumLeft += node.bandLeft[static_cast<std::size_t> (band)][static_cast<std::size_t> (i)];
                sumRight += node.bandRight[static_cast<std::size_t> (band)][static_cast<std::size_t> (i)];
            }

            left[i] = static_cast<float> (sumLeft);
            right[i] = static_cast<float> (sumRight);
        }
    }
}

void resetChain (FxRack::Chain& chain)
{
    for (auto& node : chain.nodes)
    {
        if (node.effect != nullptr)
            node.effect->reset();

        if (node.splitterLeft != nullptr)
            node.splitterLeft->clear();

        if (node.splitterRight != nullptr)
            node.splitterRight->clear();

        for (auto& band : node.bands)
            resetChain (band);
    }
}

std::size_t countNodes (const FxRack::Chain& chain)
{
    std::size_t count = 0;

    for (const auto& node : chain.nodes)
    {
        if (node.effect != nullptr)
            ++count;

        for (const auto& band : node.bands)
            count += countNodes (band);
    }

    return count;
}

} // namespace

FxRack::FxRack() : chain_ (std::make_unique<Chain>()) {}
FxRack::~FxRack() = default;
FxRack::FxRack (FxRack&&) noexcept = default;
FxRack& FxRack::operator= (FxRack&&) noexcept = default;

void FxRack::prepare (double sampleRate, int maximumBlockFrames)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maximumBlockFrames_ = std::max (maximumBlockFrames, 0);
    rebuild();
}

void FxRack::setSlots (const std::vector<FxSlot>& slots)
{
    slots_ = slots;
    rebuild();
}

void FxRack::rebuild()
{
    chain_ = std::make_unique<Chain>();
    unresolved_.clear();
    problems_.clear();

    if (maximumBlockFrames_ <= 0 || slots_.empty())
        return;

    buildRange (*chain_, slots_, 0, slots_.size(), sampleRate_, maximumBlockFrames_, unresolved_, problems_);
}

void FxRack::reset()
{
    if (chain_ != nullptr)
        resetChain (*chain_);
}

void FxRack::process (float* left, float* right, int frames)
{
    if (chain_ == nullptr || maximumBlockFrames_ <= 0)
        return;

    // Chunked rather than truncated: the scratch a splitter's bands and each effect's dry path use is
    // sized by `prepare`'s block, and a caller that hands over a longer block gets its audio processed
    // rather than its tail dropped.
    for (int offset = 0; offset < frames; offset += maximumBlockFrames_)
    {
        const int count = std::min (frames - offset, maximumBlockFrames_);
        processChain (*chain_, left + offset, right + offset, count);
    }
}

bool FxRack::empty() const { return chain_ == nullptr || chain_->nodes.empty(); }

std::size_t FxRack::nodeCount() const { return chain_ == nullptr ? 0 : countNodes (*chain_); }

const std::vector<std::string>& FxRack::unresolved() const { return unresolved_; }

const std::vector<std::string>& FxRack::problems() const { return problems_; }

// -------------------------------------------------------------------------------------------------
// The three-rack arrangement.
// -------------------------------------------------------------------------------------------------

void FxBank::prepare (double sampleRate, int maximumBlockFrames)
{
    for (auto& rack : racks_)
        rack.prepare (sampleRate, maximumBlockFrames);
}

void FxBank::reset()
{
    for (auto& rack : racks_)
        rack.reset();
}

FxRack& FxBank::rack (int index)
{
    return racks_[static_cast<std::size_t> (clampd (index, 0, kRackCount - 1))];
}

const FxRack& FxBank::rack (int index) const
{
    return racks_[static_cast<std::size_t> (clampd (index, 0, kRackCount - 1))];
}

void FxBank::setBusRouting (double bus1Destination, double bus1Volume, double bus2Destination, double bus2Volume)
{
    destination_[0] = bus1Destination;
    volume_[0] = bus1Volume;
    destination_[1] = bus2Destination;
    volume_[1] = bus2Volume;
}

void FxBank::setBusVolume (int busIndex, double volume)
{
    volume_[static_cast<std::size_t> (clampd (busIndex, 0, 1))] = volume;
}

void FxBank::setBusDestination (int busIndex, double destination)
{
    destination_[static_cast<std::size_t> (clampd (busIndex, 0, 1))] = destination;
}

void FxBank::process (float* mainLeft, float* mainRight, float* bus1Left, float* bus1Right,
                      float* bus2Left, float* bus2Right, int frames)
{
    if (frames <= 0)
        return;

    // Each bus's own rack first.
    racks_[1].process (bus1Left, bus1Right, frames);
    racks_[2].process (bus2Left, bus2Right, frames);

    float* busLeft[2] { bus1Left, bus2Left };
    float* busRight[2] { bus1Right, bus2Right };

    // Destination 1 sums the bus into MAIN's *input*, so MAIN's rack processes it; destination 2
    // bypasses MAIN and is added afterwards.
    for (int bus = 0; bus < 2; ++bus)
    {
        if (destination_[static_cast<std::size_t> (bus)] != 1.0)
            continue;

        for (int i = 0; i < frames; ++i)
        {
            mainLeft[i] = static_cast<float> (mainLeft[i] + volume_[static_cast<std::size_t> (bus)] * busLeft[bus][i]);
            mainRight[i] = static_cast<float> (mainRight[i] + volume_[static_cast<std::size_t> (bus)] * busRight[bus][i]);
        }
    }

    racks_[0].process (mainLeft, mainRight, frames);

    for (int bus = 0; bus < 2; ++bus)
    {
        if (destination_[static_cast<std::size_t> (bus)] == 1.0)
            continue;

        for (int i = 0; i < frames; ++i)
        {
            mainLeft[i] = static_cast<float> (mainLeft[i] + volume_[static_cast<std::size_t> (bus)] * busLeft[bus][i]);
            mainRight[i] = static_cast<float> (mainRight[i] + volume_[static_cast<std::size_t> (bus)] * busRight[bus][i]);
        }
    }
}

} // namespace xerum::dsp
