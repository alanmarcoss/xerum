// xerum::dsp — the modulation generators. See `mod_sources.h` for what is generated and why.

#include "mod_sources.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace xerum::dsp
{

namespace
{
double clamp01 (double value)
{
    return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}
} // namespace

double LfoCurve::samplePath (const double t, const bool x) const
{
    if (count <= 0)
        return 0.0;

    // The path is traversed in the order the preset drew it, once per cycle, regardless of how its x
    // wanders — which is what makes it a path rather than a curve. A closed path has one more segment
    // than an open one: the join from the last point back to the first.
    const int segments = count > 1 ? (closed ? count : count - 1) : 1;
    const double u = clamp01 (t) * static_cast<double> (segments);
    const int index = std::min (static_cast<int> (u), segments - 1);
    const int next = closed ? (index + 1) % count : index + 1;
    const double fraction = u - static_cast<double> (index);

    const double from = x ? points[static_cast<std::size_t> (index)].x
                          : points[static_cast<std::size_t> (index)].y;
    const double to = x ? points[static_cast<std::size_t> (next)].x
                        : points[static_cast<std::size_t> (next)].y;
    return from + (to - from) * fraction;
}

namespace
{
constexpr double kPi = 3.14159265358979323846;

double wrap01 (double value)
{
    return value - std::floor (value);
}

/// A per-cycle deterministic value for the sample-and-hold type. Deterministic on purpose: an LFO whose
/// output changed between two identical renders would make every render test a coin toss, and the
/// reference's own randomness is seeded per note anyway.
double heldValue (long cycle, int index)
{
    std::uint64_t x = static_cast<std::uint64_t> (cycle) * 0x9E3779B97F4A7C15ull
                      + static_cast<std::uint64_t> (index + 1) * 0xBF58476D1CE4E5B9ull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;

    return static_cast<double> (x >> 11) / 9007199254740992.0;   // 2^53
}

/// The phase an LFO is read at: its own start phase, the preset's direction, and its swing.
double readPhase (const LfoSettings& settings, double phase)
{
    auto t = wrap01 (phase + settings.phaseTurns);

    const double swing = std::clamp (settings.swing, 0.0, 1.0);

    if (swing > 0.0)
    {
        // Swing lengthens the first half of the cycle: at `swing` the first half takes `1 + swing`
        // times as long as the second. A stated convention — the reference's own skew is not established —
        // but it is the definition every sequencer uses and it is monotone in `swing`.
        const double firstHalf = 0.5 * (1.0 + swing);

        t = t < firstHalf ? 0.5 * t / firstHalf : 0.5 + 0.5 * (t - firstHalf) / (1.0 - firstHalf);
    }

    if (settings.direction < 0.0)
        t = 1.0 - t;

    return t;
}

double builtInShape (LfoShape shape, double t)
{
    switch (shape)
    {
        case LfoShape::Sine:
            return 0.5 + 0.5 * std::sin (2.0 * kPi * t);

        case LfoShape::Triangle:
        {
            // In phase with the sine: 0.5 at t = 0, 1 at a quarter, 0.5 at half, 0 at three quarters, back
            // to 0.5 at the top. (`4t` rising, then down through zero, then back up.)
            const double tri = t < 0.25 ? 4.0 * t : (t < 0.75 ? 2.0 - 4.0 * t : 4.0 * t - 4.0);
            return 0.5 + 0.5 * tri;
        }

        case LfoShape::SawUp:
            return t;

        case LfoShape::SawDown:
            return 1.0 - t;

        case LfoShape::Square:
            return t < 0.5 ? 1.0 : 0.0;
    }

    return 0.5;
}

/// The main output: the path's x for a `Path`-type LFO, a held random for the sample-and-hold type, and
/// otherwise the drawn curve or the built-in shape.
double mainValue (const LfoSettings& settings, double t, long cycle, int index)
{
    if (settings.type == LfoType::Path && ! settings.path.empty())
        return settings.path.samplePathX (t);

    if (settings.type == LfoType::RandomSampleHold)
        return heldValue (cycle, index);

    if (settings.hasDrawnCurve())
        return settings.curve.sampleCurve (t);

    return builtInShape (settings.shape, t);
}

double secondValue (const LfoSettings& settings, double t, long cycle, int index)
{
    if (! settings.path.empty())
        return settings.path.samplePathY (t);

    return mainValue (settings, t, cycle, index);
}

} // namespace

void LfoCurve::add (const double x, const double y, const double curve)
{
    if (count >= kMaxLfoPoints)
        return;

    points[static_cast<std::size_t> (count)] = LfoPoint { x, y, curve };
    ++count;
}

double LfoCurve::sampleCurve (const double x) const
{
    if (count <= 0)
        return 0.0;

    if (count == 1)
        return points[0].y;

    const double target = clamp01 (x);

    // Searched from the last segment back, so that a target sitting exactly on a boundary takes the
    // segment that *starts* there — which is what makes a repeated x the value after the edge.
    for (int i = count - 2; i >= 0; --i)
    {
        const auto& from = points[static_cast<std::size_t> (i)];
        const auto& to = points[static_cast<std::size_t> (i + 1)];

        if (target < from.x || target > to.x)
            continue;

        const double span = to.x - from.x;

        // A repeated x is a vertical edge: the corpus draws hard steps this way (`LD - Artform`,
        // `xVals 0.125, 0.125` with `yVals 0.097, 1.0`), and the later point is the value after it.
        if (span <= 0.0)
            return to.y;

        // `curveVals` is the reference's own bend, on the 0..1 scale whose neutral is 0.5; the shaper is
        // written in the percent the envelope curves use.
        const double shaped = shapedSegment ((target - from.x) / span, from.curve * 100.0);
        return from.y + (to.y - from.y) * shaped;
    }

    return points[static_cast<std::size_t> (count - 1)].y;
}

double LfoCurve::samplePathX (const double t) const
{
    return samplePath (t, /* x */ true);
}

double LfoCurve::samplePathY (const double t) const
{
    return samplePath (t, /* x */ false);
}

double lfoRateHz (const double ratePercent, const bool rate10x)
{
    const auto rate = std::clamp (ratePercent, 0.0, 100.0) / 100.0;
    return rate * 20.0 * (rate10x ? 10.0 : 1.0);
}

double shapedSegment (const double progress, const double curvePercent)
{
    const auto p = clamp01 (progress);
    const auto curve = std::clamp (curvePercent, 0.0, 100.0);

    // 50 is a straight line, which is the corpus's own modal value for every envelope curve
    // (`kParamCurve1...3` are 50 in the factory bank); 0 and 100 are the two extremes of the bend.
    const double exponent = std::exp2 ((50.0 - curve) / 50.0);

    if (p <= 0.0)
        return 0.0;

    return std::pow (p, exponent);
}

double lfoValue (const LfoSettings& settings, const double phaseTurns, const long cycleIndex)
{
    const double t = readPhase (settings, phaseTurns);
    return clamp01 (mainValue (settings, t, cycleIndex, 0));
}

double lfoSecondValue (const LfoSettings& settings, const double phaseTurns, const long cycleIndex)
{
    const double t = readPhase (settings, phaseTurns);
    return clamp01 (secondValue (settings, t, cycleIndex, 0));
}

// ---------------------------------------------------------------- MatrixEnvelope

void MatrixEnvelope::prepare (const double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
}

void MatrixEnvelope::reset()
{
    stage_ = Stage::Idle;
    value_ = 0.0;
    progress_ = 0.0;
    rate_ = 0.0;
    releaseFrom_ = 0.0;
}

void MatrixEnvelope::enterStage (const MatrixEnvelopeSettings& settings, const Stage stage)
{
    stage_ = stage;
    progress_ = 0.0;

    switch (stage)
    {
        case Stage::Idle:
            rate_ = 0.0;
            value_ = clamp01 (settings.end);
            break;

        case Stage::Attack:
            rate_ = settings.attack > 0.0 ? 1.0 / settings.attack : 0.0;
            value_ = clamp01 (settings.start);
            break;

        case Stage::Hold:
            rate_ = settings.hold > 0.0 ? 1.0 / settings.hold : 0.0;
            value_ = 1.0;
            break;

        case Stage::Decay:
            rate_ = settings.decay > 0.0 ? 1.0 / settings.decay : 0.0;
            value_ = 1.0;
            break;

        case Stage::Sustain:
            rate_ = 0.0;
            value_ = clamp01 (settings.sustain);
            break;

        case Stage::Release:
            rate_ = settings.release > 0.0 ? 1.0 / settings.release : 0.0;
            value_ = releaseFrom_;
            break;
    }
}

double MatrixEnvelope::sample (const MatrixEnvelopeSettings& settings) const
{
    switch (stage_)
    {
        case Stage::Idle:
            return clamp01 (settings.end);

        case Stage::Attack:
        {
            const double from = clamp01 (settings.start);
            return from + (1.0 - from) * shapedSegment (progress_, settings.curve1);
        }

        case Stage::Hold:
            return 1.0;

        case Stage::Decay:
        {
            const double sustain = clamp01 (settings.sustain);
            return 1.0 + (sustain - 1.0) * shapedSegment (progress_, settings.curve2);
        }

        case Stage::Sustain:
            return clamp01 (settings.sustain);

        case Stage::Release:
        {
            const double end = clamp01 (settings.end);
            return releaseFrom_ + (end - releaseFrom_) * shapedSegment (progress_, settings.curve3);
        }
    }

    return value_;
}

void MatrixEnvelope::trigger (const MatrixEnvelopeSettings& settings, const bool legato)
{
    if (legato && settings.legatoInverted)
        return;   // the preset asks a legato note change not to restart this envelope

    enterStage (settings, Stage::Attack);
}

void MatrixEnvelope::release (const MatrixEnvelopeSettings& settings)
{
    if (stage_ == Stage::Idle)
        return;

    releaseFrom_ = value_;
    enterStage (settings, Stage::Release);
}

double MatrixEnvelope::advance (const MatrixEnvelopeSettings& settings, const int frames)
{
    if (frames <= 0)
        return value_;

    // A segment's level is a function of its progress, so advancing progress by the whole block is the
    // exact value at the block's end — no per-sample loop, and the matrix's ramp carries the shape
    // between blocks (ticket 13's control rate).
    //
    // The stages are walked rather than assumed, because a stage may be shorter than the block *and* may
    // be zero-length: `kParamHold` is absent from most presets, and a state machine that waits for a zero
    // rate to reach 1.0 would hold at the attack's peak for the rest of the note.
    double remaining = static_cast<double> (frames) / sampleRate_;

    for (int guard = 0; guard < 8 && segmented(); ++guard)
    {
        if (rate_ <= 0.0)
            progress_ = 1.0;
        else
        {
            const double needed = (1.0 - progress_) / rate_;

            if (remaining < needed)
            {
                progress_ += remaining * rate_;
                remaining = 0.0;
            }
            else
            {
                remaining -= needed;
                progress_ = 1.0;
            }
        }

        if (progress_ < 1.0)
            break;

        switch (stage_)
        {
            case Stage::Attack:  enterStage (settings, Stage::Hold); break;
            case Stage::Hold:    enterStage (settings, Stage::Decay); break;
            case Stage::Decay:   enterStage (settings, Stage::Sustain); break;
            case Stage::Release: enterStage (settings, Stage::Idle); break;
            default:             break;
        }
    }

    value_ = sample (settings);
    return value_;
}

// -------------------------------------------------------------- ModulationGenerator

void ModulationGenerator::prepare (const double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    for (auto& envelope : envelopes_)
        envelope.prepare (sampleRate_);

    reset();
}

void ModulationGenerator::reset()
{
    lfos_.fill (LfoState {});

    for (auto& envelope : envelopes_)
        envelope.reset();
}

double ModulationGenerator::lfoPhase (const int index) const
{
    if (index < 0 || index >= kLfoCount)
        return 0.0;

    return wrap01 (lfos_[static_cast<std::size_t> (index)].phase);
}

void ModulationGenerator::noteOn (const ModSourceSettings& settings, const bool legato)
{
    for (int i = 0; i < kLfoCount; ++i)
    {
        const auto& s = settings.lfos[static_cast<std::size_t> (i)];

        // A `Mono` LFO is the engine's, and a note must not reset it.
        if (s.mono)
            continue;

        auto& state = lfos_[static_cast<std::size_t> (i)];

        // The two modes the schema carries: `Envelope` is one cycle per note, and `Free` is free-running
        // — a note-on does not restart it. A preset that stores neither gets the retriggered loop, which
        // is what an LFO with no stated mode has to do to be reproducible.
        if (! s.freeRunning || ! state.started)
        {
            state = LfoState {};
            state.started = true;
        }
    }

    for (int e = 0; e < kMatrixEnvelopeCount; ++e)
        envelopes_[static_cast<std::size_t> (e)].trigger (settings.envelopes[static_cast<std::size_t> (e)],
                                                          legato);
}

void ModulationGenerator::noteOff (const ModSourceSettings& settings)
{
    for (int e = 0; e < kMatrixEnvelopeCount; ++e)
        envelopes_[static_cast<std::size_t> (e)].release (settings.envelopes[static_cast<std::size_t> (e)]);
}

void ModulationGenerator::restartOneShotLfos (const ModSourceSettings& settings)
{
    for (int i = 0; i < kLfoCount; ++i)
    {
        const auto& s = settings.lfos[static_cast<std::size_t> (i)];

        if (! s.mono || ! s.oneShot)
            continue;

        lfos_[static_cast<std::size_t> (i)] = LfoState {};
        lfos_[static_cast<std::size_t> (i)].started = true;
    }
}

void ModulationGenerator::advance (const ModSourceSettings& settings, ModulationInputs& inputs,
                                   const int frames)
{
    advanceLfos (settings, inputs, frames, /* monoOnly */ false);

    for (int e = 0; e < kMatrixEnvelopeCount; ++e)
        inputs.envelope[e] = envelopes_[static_cast<std::size_t> (e)].advance (
            settings.envelopes[static_cast<std::size_t> (e)], frames);
}

void ModulationGenerator::advanceMonoLfos (const ModSourceSettings& settings, ModulationInputs& inputs,
                                           const int frames)
{
    advanceLfos (settings, inputs, frames, /* monoOnly */ true);
}

void ModulationGenerator::advanceLfos (const ModSourceSettings& settings, ModulationInputs& inputs,
                                       const int frames, const bool monoOnly)
{
    if (frames <= 0)
        return;

    const double dt = static_cast<double> (frames) / sampleRate_;

    for (int i = 0; i < kLfoCount; ++i)
    {
        const auto& s = settings.lfos[static_cast<std::size_t> (i)];

        if (s.mono != monoOnly)
            continue;

        auto& state = lfos_[static_cast<std::size_t> (i)];

        if (! state.started)
        {
            state.started = true;
            state.phase = 0.0;
        }

        state.elapsedSeconds += dt;
        state.phase += s.rateHz * dt;

        if (s.oneShot)
            state.phase = std::min (state.phase, 1.0);
        else
            while (state.phase >= 1.0)
            {
                state.phase -= 1.0;
                ++state.cycles;
            }

        const double t = readPhase (s, state.phase);
        double value = clamp01 (mainValue (s, t, state.cycles, i));
        double second = clamp01 (secondValue (s, t, state.cycles, i));

        // The delay holds the output at the value the LFO starts from, and the rise fades from there to
        // the running shape — both in seconds since the note, which is what `kParamDelay`/`kParamRise`
        // are stored in.
        const double startValue = clamp01 (mainValue (s, readPhase (s, 0.0), 0, i));

        if (state.elapsedSeconds < s.delaySeconds)
        {
            value = startValue;
            second = clamp01 (secondValue (s, readPhase (s, 0.0), 0, i));
        }
        else if (s.riseSeconds > 0.0 && state.elapsedSeconds < s.delaySeconds + s.riseSeconds)
        {
            const double gain = (state.elapsedSeconds - s.delaySeconds) / s.riseSeconds;
            value = startValue + (value - startValue) * gain;
            second = clamp01 (secondValue (s, readPhase (s, 0.0), 0, i)) * (1.0 - gain) + second * gain;
        }

        // `kParamSmooth` is a one-pole on the output, and its time constant is a stated convention: the
        // schema gives the field's range (1..100) and not its unit, so it scales to 0..50 ms.
        const double tau = std::clamp (s.smooth, 0.0, 1.0) * 0.05;
        const double coefficient = tau > 0.0 ? 1.0 - std::exp (-dt / tau) : 1.0;

        state.smoothed += (value - state.smoothed) * coefficient;
        state.smoothedY += (second - state.smoothedY) * coefficient;

        inputs.lfo[i] = clamp01 (state.smoothed);
        inputs.lfoY[i] = clamp01 (state.smoothedY);
    }
}

} // namespace xerum::dsp
