#include "voice.h"

#include <algorithm>
#include <cmath>

namespace xerum::dsp
{
namespace
{
/// A4, the reference pitch every note number is measured against.
constexpr double kA4Hz = 440.0;

double clampDouble (double value, double low, double high)
{
    return value < low ? low : (value > high ? high : value);
}

int clampInt (int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

/// The half-turn in radians and `sqrt(2)`, for the equal-power pan below.
constexpr double kPi = 3.14159265358979323846;
constexpr double kSqrt2 = 1.4142135623730951;

/// The generators' settings, or an empty set when a voice has not been wired to an engine's — a voice is
/// usable on its own in tests, and it must not need one to render.
const ModSourceSettings& sourcesOf (const ModSourceSettings* settings)
{
    static const ModSourceSettings none {};
    return settings != nullptr ? *settings : none;
}

/// One bipolar sample as the modulation source the matrix reads: the convention is that every source
/// reports its output on the reference's 0..1 range (modulation.h), so a signal that swings either side
/// of zero is centred at 0.5.
double unipolarSource (double sample)
{
    return clampDouble (0.5 * (sample + 1.0), 0.0, 1.0);
}

/// Equal-power pan gains for one unison sub-voice.
///
/// **A stated convention, not a measurement.** The reference's own law for `kParamUnisonStereo` is not
/// established (ticket 28 recorded the parameter and stopped there), so this is the standard
/// constant-power law with one property chosen on purpose: **at `spread == 0` both gains are exactly 1**,
/// so a preset that does not ask for width renders exactly what it rendered before the bus was stereo.
/// A plain `cos`/`sin` pair would put 0.707 in each channel at centre and change every existing render.
///
/// `position` is the sub-voice's place in the stack, -0.5..0.5 (the same one that sets its detune), and
/// `spread` is `kParamUnisonStereo` as a fraction, clamped to [-1, 1]. Its **sign mirrors the image**:
/// negative values spread the stack the other way round, which is what the corpus's negative values
/// (-100 occurs four times) have to mean rather than being clamped to nothing.
void panGains (double position, double spread, double& left, double& right)
{
    const double pan = clampDouble (2.0 * position * spread, -1.0, 1.0);

    if (spread == 0.0)
    {
        left = 1.0;
        right = 1.0;
        return;
    }

    const double angle = (pan + 1.0) * kPi / 4.0;   // 0 at hard left, pi/2 at hard right
    left = std::cos (angle) * kSqrt2;
    right = std::sin (angle) * kSqrt2;
}
} // namespace

bool BusSendSettings::any() const
{
    for (const auto& row : gain)
        for (const auto level : row)
            if (level != 0.0)
                return true;

    return false;
}

bool BusSends::wanted() const
{
    return (left[0] != nullptr && right[0] != nullptr) || (left[1] != nullptr && right[1] != nullptr);
}

VoiceMode voiceModeFor (bool mono, bool legato)
{
    if (! mono)
        return VoiceMode::Poly;
    return legato ? VoiceMode::MonoLegato : VoiceMode::Mono;
}

// ---------------------------------------------------------------- AmpEnvelope

void AmpEnvelope::prepare (double rate)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
}

void AmpEnvelope::set (double attackSeconds, double decaySeconds, double sustainLevel, double releaseSeconds)
{
    attack = attackSeconds < 0.0 ? 0.0 : attackSeconds;
    decay = decaySeconds < 0.0 ? 0.0 : decaySeconds;
    sustain = clampDouble (sustainLevel, 0.0, 1.0);
    release = releaseSeconds < 0.0 ? 0.0 : releaseSeconds;
}

void AmpEnvelope::trigger()
{
    value = 0.0;
    stage = Stage::Attack;

    // From the current value to full, so a retrigger from a sustaining level does not jump.
    attackRate = attack > 0.0 ? 1.0 / (attack * sampleRate) : 1.0;
}

void AmpEnvelope::releaseStage()
{
    stage = Stage::Release;

    // Linear from wherever the envelope is when the key lifts.
    releaseRate = release > 0.0 ? value / (release * sampleRate) : value;
}

void AmpEnvelope::advance()
{
    switch (stage)
    {
        case Stage::Idle:
            value = 0.0;
            break;

        case Stage::Attack:
            value += attackRate;
            if (value >= 1.0)
            {
                value = 1.0;
                stage = Stage::Decay;
                decayRate = decay > 0.0 ? (1.0 - sustain) / (decay * sampleRate) : (1.0 - sustain);
            }
            break;

        case Stage::Decay:
            value -= decayRate;
            if (value <= sustain)
            {
                value = sustain;
                stage = Stage::Sustain;
            }
            break;

        case Stage::Sustain:
            value = sustain;
            break;

        case Stage::Release:
            value -= releaseRate;
            if (value <= 0.0)
            {
                value = 0.0;
                stage = Stage::Idle;
            }
            break;
    }
}

// ----------------------------------------------------------------------- Voice

void Voice::prepare (double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    for (auto& slot : oscillators_)
        for (auto& oscillator : slot)
            oscillator.prepare (sampleRate_);

    for (auto& slot : filters_)
        for (auto& filter : slot)
            filter.prepare (sampleRate_);

    noise_.prepare (sampleRate_);
    sub_.prepare (sampleRate_);

    envelope_.prepare (sampleRate_);
    generator_.prepare (sampleRate_);

    lastSlotSample_.fill (0.0);
    lastFilterSample_.fill (0.0);
}

void Voice::setSettings (const VoiceSettings& settings)
{
    settings_ = settings;
    settings_.envelope.prepare (sampleRate_);

    // Times only — the live level and stage carry on, so a settings change mid-note is not a retrigger.
    envelope_.set (settings.envelope.attack, settings.envelope.decay, settings.envelope.sustain,
                   settings.envelope.release);

    for (int s = 0; s < kOscillatorSlots; ++s)
    {
        const auto& os = settings_.oscillators[s];

        for (int i = 0; i < kMaxUnison; ++i)
        {
            auto& oscillator = oscillators_[s][i];
            oscillator.setTable (os.enabled ? os.table : nullptr);
            oscillator.setWarp (0, os.warpMode[0], os.warpAmount[0]);
            oscillator.setWarp (1, os.warpMode[1], os.warpAmount[1]);
            oscillator.setFramePosition (os.framePosition);
        }
    }

    for (int f = 0; f < kFilterSlots; ++f)
    {
        const auto& fs = settings_.filters[f];

        for (auto& filter : filters_[f])
        {
            filter.setSpec (fs.spec);
            filter.setCutoff (clampDouble (fs.cutoff, 0.0, 1.0));
            filter.setResonance (clampDouble (fs.resonance, 0.0, 100.0));
            filter.setDrive (clampDouble (fs.drive, 0.0, 100.0));
            filter.setMorph (clampDouble (fs.morph, 0.0, 1.0));

            // The drawn response is the *filter* for a `PoleZero` spec, so it is bound here, with the
            // model, rather than left for whoever built the settings to remember. It is a no-op for the
            // other models, which `process` never routes through `processPoleZero`.
            if (fs.spec.model == FilterModel::PoleZero)
                filter.setPoleZero (fs.poleZero);
        }
    }

    if (settings_.noise.type.has_value())
        noise_.setType (*settings_.noise.type);

    sub_.setShape (settings_.sub.shape);
    sub_.setInitialPhase (settings_.sub.initialPhaseTurns);
    sub_.setContiguousPhase (settings_.sub.contiguousPhase);
}

double Voice::glidePitch() const
{
    if (glideSamplesTotal_ <= 0)
        return glideTarget_;

    const double progress = clampDouble (static_cast<double> (glideSamplesDone_)
                                             / static_cast<double> (glideSamplesTotal_),
                                         0.0, 1.0);

    // Curve 50% is a straight line; below eases out, above eases in. A stated convention (see the
    // header): the reference's portamento curve is not established.
    const double shape = std::pow (progress, settings_.portamentoCurve / 50.0);
    return glideFrom_ + (glideTarget_ - glideFrom_) * shape;
}

double Voice::pitchSemitones() const
{
    const bool gliding = glideSamplesTotal_ > 0 && glideSamplesDone_ < glideSamplesTotal_;
    return gliding ? glidePitch() : glideTarget_;
}

void Voice::start (int note, double velocity, double startPitchSemitones)
{
    stealSamplesTotal_ = 0;
    stealSamplesLeft_ = 0;
    hasPending_ = false;
    note_ = note;
    velocity_ = velocity;
    stage_ = VoiceStage::On;
    envelope_.trigger();

    // The matrix envelopes retrigger with the note, and the LFOs unless the preset marks them
    // free-running. A steal's fade keeps the old note's generators running until the new note starts.
    generator_.noteOn (sourcesOf (sourceSettings_), false);

    // The two fixed slots start with the note. NOISE replays from the note's start state unless
    // `kParamRandomPhase` asks for a different one per note; SUB restarts at its initial phase unless
    // `kParamContiguousPhase` says to carry the previous note's.
    noise_.trigger (settings_.noise.randomPhase);
    sub_.trigger();

    const double target = static_cast<double> (note) - 69.0;
    glideTarget_ = target;
    glideFrom_ = startPitchSemitones;

    if (settings_.portamentoTime > 0.0 && std::abs (startPitchSemitones - target) > 1e-9)
    {
        glideSamplesTotal_ = std::max (1, static_cast<int> (settings_.portamentoTime * sampleRate_));
        glideSamplesDone_ = 0;
    }
    else
    {
        glideSamplesTotal_ = 0;
        glideSamplesDone_ = 0;
        glideFrom_ = target;
    }
}

void Voice::retarget (int note, double targetPitchSemitones)
{
    const double from = pitchSemitones();

    note_ = note;
    glideTarget_ = targetPitchSemitones;
    glideFrom_ = from;

    if (settings_.portamentoTime > 0.0 && std::abs (from - targetPitchSemitones) > 1e-9)
    {
        glideSamplesTotal_ = std::max (1, static_cast<int> (settings_.portamentoTime * sampleRate_));
        glideSamplesDone_ = 0;
    }
    else
    {
        glideSamplesTotal_ = 0;
        glideSamplesDone_ = 0;
        glideFrom_ = targetPitchSemitones;
    }

    // A legato note change retriggers the generators that do not opt out of it.
    generator_.noteOn (sourcesOf (sourceSettings_), true);
}

void Voice::release()
{
    if (stage_ != VoiceStage::On)
        return;

    stage_ = VoiceStage::Releasing;
    envelope_.releaseStage();
    generator_.noteOff (sourcesOf (sourceSettings_));
}

void Voice::steal (int note, double velocity, double startPitchSemitones)
{
    pendingNote_ = note;
    pendingVelocity_ = velocity;
    pendingStartPitch_ = startPitchSemitones;

    if (stage_ == VoiceStage::Idle)
    {
        start (note, velocity, startPitchSemitones);
        return;
    }

    hasPending_ = true;
    stealSamplesTotal_ = std::max (1, static_cast<int> (kStealFadeSeconds * sampleRate_));
    stealSamplesLeft_ = stealSamplesTotal_;
    stage_ = VoiceStage::Stealing;
}

int Voice::pendingNote() const
{
    return stage_ == VoiceStage::Stealing && hasPending_ ? pendingNote_ : -1;
}

void Voice::cancelPending (int note)
{
    if (stage_ == VoiceStage::Stealing && hasPending_ && pendingNote_ == note)
        hasPending_ = false;
}

void Voice::kill()
{
    stage_ = VoiceStage::Idle;
    envelope_.value = 0.0;
    envelope_.stage = AmpEnvelope::Stage::Idle;
    stealSamplesTotal_ = 0;
    stealSamplesLeft_ = 0;
    hasPending_ = false;

    // The generators have no envelope of their own, so a killed voice would otherwise leave the noise's
    // filter history and the sub's phase running into the next note it takes.
    noise_.reset();
    sub_.reset();

    generator_.reset();
    lastSlotSample_.fill (0.0);
    lastFilterSample_.fill (0.0);
}

void Voice::render (float* outLeft, float* outRight, const BusSends& sends, int frames, ModulationMatrix* matrix,
                    const ModulationInputs& inputs)
{
    if (stage_ == VoiceStage::Idle || frames <= 0)
        return;

    if (stage_ == VoiceStage::Releasing && envelope_.idle())
    {
        stage_ = VoiceStage::Idle;
        return;
    }

    // ---- Control rate: one matrix evaluation per block, ramps read per sample below.
    //
    // The voice's own generators are advanced *first*, into this block's copy of the engine's inputs,
    // because the matrix reads a source at the block's end (ticket 13). Four sources only the voice can
    // supply are added here too — its own samples, one frame old:
    //
    //   * `osc[0..2]`, `noiseOsc`, `subOsc` and `voiceFilter[0..1]` are the audio-rate sources. Reading
    //     the previous frame is what makes a cross-oscillator warp causal without reading any oscillator
    //     twice, and it is the honest control-rate answer for a matrix that is evaluated per block.
    ModulationInputs in = inputs;
    generator_.advance (sourcesOf (sourceSettings_), in, frames);

    for (int s = 0; s < kOscillatorSlots; ++s)
        in.osc[s] = unipolarSource (lastSlotSample_[static_cast<std::size_t> (s)]);

    in.noiseOsc = unipolarSource (lastSlotSample_[static_cast<std::size_t> (kNoiseSlot)]);
    in.subOsc = unipolarSource (lastSlotSample_[static_cast<std::size_t> (kSubSlot)]);

    for (int f = 0; f < kFilterSlots; ++f)
        in.voiceFilter[f] = unipolarSource (lastFilterSample_[static_cast<std::size_t> (f)]);

    const bool modulated = matrix != nullptr && ! matrix->slots().empty();
    if (modulated)
        matrix->beginBlock (in, frames);

    // Indexed by the preset's own slot number, so NOISE (3) and SUB (4) are addressed exactly as the
    // bank addresses them — `Oscillator/3/kParamVolume` and `Oscillator/4/kParamVolume` are 513 of the
    // bank's modulation rows, and the sub's octave and coarse pitch are 28 more.
    std::array<ModulatedRamp, kSlotCount> pitchRamp {};
    std::array<ModulatedRamp, kSlotCount> volumeRamp {};
    ModulatedRamp octaveRamp {};
    ModulatedRamp coarseRamp {};
    std::array<ModulatedRamp, kFilterSlots> cutoffRamp {};
    std::array<ModulatedRamp, kFilterSlots> resoRamp {};

    if (modulated)
    {
        for (int s = 0; s < kSlotCount; ++s)
        {
            pitchRamp[s] = matrix->rampFor (voiceDestination::kOscillatorClass, s,
                                            voiceDestination::kParamPitch);
            volumeRamp[s] = matrix->rampFor (voiceDestination::kOscillatorClass, s,
                                             voiceDestination::kParamVolume);
        }

        octaveRamp = matrix->rampFor (voiceDestination::kOscillatorClass, kSubSlot,
                                      voiceDestination::kParamOctave);
        coarseRamp = matrix->rampFor (voiceDestination::kOscillatorClass, kSubSlot,
                                      voiceDestination::kParamCoarsePit);

        for (int f = 0; f < kFilterSlots; ++f)
        {
            cutoffRamp[f] = matrix->rampFor (voiceDestination::kVoiceFilterClass, f,
                                             voiceDestination::kParamFreq);
            resoRamp[f] = matrix->rampFor (voiceDestination::kVoiceFilterClass, f,
                                           voiceDestination::kParamReso);
        }
    }

    // ---- Unison layout, constant for the block.
    std::array<int, kOscillatorSlots> unison {};
    std::array<double, kOscillatorSlots> unisonNorm {};
    std::array<std::array<double, kMaxUnison>, kOscillatorSlots> detune {};
    std::array<std::array<double, kMaxUnison>, kOscillatorSlots> panLeft {};
    std::array<std::array<double, kMaxUnison>, kOscillatorSlots> panRight {};

    for (int s = 0; s < kOscillatorSlots; ++s)
    {
        const auto& os = settings_.oscillators[s];
        const int n = clampInt (os.unison, 1, kMaxUnison);
        unison[s] = n;
        unisonNorm[s] = 1.0 / std::sqrt (static_cast<double> (n));

        const double spread = clampDouble (os.unisonStereo / 100.0, -1.0, 1.0);

        for (int i = 0; i < n; ++i)
        {
            const double position = n > 1 ? static_cast<double> (i) / static_cast<double> (n - 1) - 0.5 : 0.0;
            detune[s][i] = std::exp2 (os.unisonRangeSemitones * position / 12.0);
            panGains (position, spread, panLeft[s][i], panRight[s][i]);
        }
    }

    // Whether pitch or filter settings move within the block decides the per-sample cost. A block with
    // no modulation and no glide sets them once.
    bool gliding = glideSamplesTotal_ > 0 && glideSamplesDone_ < glideSamplesTotal_;
    bool pitchVaries = gliding;
    bool subPitchVaries = gliding;
    bool filterVaries = false;

    for (int s = 0; s < kOscillatorSlots; ++s)
        if (settings_.oscillators[s].enabled && unison[s] > 0
            && (pitchRamp[s].start != pitchRamp[s].end || gliding))
            pitchVaries = true;

    // NOISE has no pitch at all (a generated colour has no rate to track), so only the sub's three pitch
    // destinations — pitch, octave and coarse pitch — can move it within a block.
    if (settings_.sub.enabled
        && (pitchRamp[kSubSlot].start != pitchRamp[kSubSlot].end
            || octaveRamp.start != octaveRamp.end || coarseRamp.start != coarseRamp.end))
        subPitchVaries = true;

    for (int f = 0; f < kFilterSlots; ++f)
        if (settings_.filters[f].enabled
            && (cutoffRamp[f].start != cutoffRamp[f].end || resoRamp[f].start != resoRamp[f].end))
            filterVaries = true;

    /// The sub oscillator's frequency, in hertz: the note it is playing, its own octave/coarse/pitch/fine
    /// summed into `pitchSemitones`, and the three modulation deltas that address it.
    auto subFrequency = [&] (double glideNow, double pitchMod, double octaveMod, double coarseMod)
    {
        return kA4Hz * std::exp2 ((glideNow + settings_.sub.pitchSemitones + pitchMod + octaveMod + coarseMod)
                                  / 12.0);
    };

    auto setConstantSubFrequency = [&]
    {
        if (! settings_.sub.enabled)
            return;

        sub_.setFrequency (subFrequency (gliding ? glidePitch() : glideTarget_,
                                         pitchRamp[kSubSlot].start * kPitchModulationSpanSemitones,
                                         octaveRamp.start * kOctaveModulationSpanSemitones,
                                         coarseRamp.start * kCoarseModulationSpanSemitones));
    };

    auto setConstantOscillatorFrequencies = [&]
    {
        const double glideNow = gliding ? glidePitch() : glideTarget_;

        for (int s = 0; s < kOscillatorSlots; ++s)
        {
            const auto& os = settings_.oscillators[s];
            if (! os.enabled || unison[s] <= 0)
                continue;

            const double m = std::exp2 ((glideNow + pitchRamp[s].start * kPitchModulationSpanSemitones
                                         + os.pitchSemitones) / 12.0);
            const double baseHz = kA4Hz * m;

            for (int i = 0; i < unison[s]; ++i)
                oscillators_[s][i].setFrequency (baseHz * detune[s][i]);
        }
    };

    auto setConstantFilter = [&]
    {
        for (int f = 0; f < kFilterSlots; ++f)
        {
            const auto& fs = settings_.filters[f];
            if (! fs.enabled)
                continue;

            for (auto& filter : filters_[f])
            {
                filter.setCutoff (clampDouble (fs.cutoff + cutoffRamp[f].start * kCutoffModulationSpan,
                                               0.0, 1.0));
                filter.setResonance (clampDouble (
                    fs.resonance + resoRamp[f].start * kResonanceModulationSpan, 0.0, 100.0));
            }
        }
    };

    /// **What a warp reads.** The source the mode's name encodes, resolved against the samples this voice
    /// has produced: the two other tabled oscillators, the sub, the noise colour, either filter, or — for
    /// `kSelfPD` — the oscillator's own previous sample.
    ///
    /// One frame old, like the audio-rate matrix sources above and for the same reason: a warp may be on
    /// the first slot and its source on the third, so nothing here can be read this frame without reading
    /// an oscillator twice.
    const auto warpModulatorValue = [&] (WarpSource source, int slot) -> double
    {
        const int index = warpSourceSlot (source, slot, kOscillatorSlots);

        if (index >= 0)
            return lastSlotSample_[static_cast<std::size_t> (index)];

        switch (source)
        {
            case WarpSource::Self:    return lastSlotSample_[static_cast<std::size_t> (slot)];
            case WarpSource::Sub:     return lastSlotSample_[static_cast<std::size_t> (kSubSlot)];
            case WarpSource::Noise:   return lastSlotSample_[static_cast<std::size_t> (kNoiseSlot)];
            case WarpSource::Filter1: return lastFilterSample_[0];
            case WarpSource::Filter2: return lastFilterSample_[1];
            default:                  return 0.0;
        }
    };

    if (! pitchVaries)
        setConstantOscillatorFrequencies();
    if (! subPitchVaries)
        setConstantSubFrequency();
    if (! filterVaries)
        setConstantFilter();

    // **Whether this voice sends anything** (ticket `64`), decided once per call: a preset with no
    // routing-matrix levels, or a caller with no bus buffers, records no taps and writes nothing — so a
    // render that asked for no sends is the render it was before the sends existed.
    const bool sending = sends.wanted() && settings_.sends.any();

    /// One pair per routing row, filled in the frame loop below and read by the send that closes it.
    std::array<float, kRoutingRowCount> tapLeft {};
    std::array<float, kRoutingRowCount> tapRight {};

    for (int frame = 0; frame < frames; ++frame)
    {
        // A steal that finishes mid-block hands the voice to its pending note right here, so there is no
        // silence between the fade and the new note. A cancelled pending note ends the voice instead.
        if (stage_ == VoiceStage::Stealing && stealSamplesLeft_ <= 0)
        {
            if (! hasPending_)
            {
                kill();
                return;
            }

            const int pendingNote = pendingNote_;
            const double pendingVelocity = pendingVelocity_;
            const double pendingStart = pendingStartPitch_;

            start (pendingNote, pendingVelocity, pendingStart);

            gliding = glideSamplesTotal_ > 0 && glideSamplesDone_ < glideSamplesTotal_;
            pitchVaries = gliding;
            for (int s = 0; s < kOscillatorSlots; ++s)
                if (settings_.oscillators[s].enabled && unison[s] > 0
                    && (pitchRamp[s].start != pitchRamp[s].end || gliding))
                    pitchVaries = true;

            subPitchVaries = gliding;

            if (! pitchVaries)
                setConstantOscillatorFrequencies();
            if (! subPitchVaries)
                setConstantSubFrequency();
        }

        double fade = 1.0;
        if (stage_ == VoiceStage::Stealing)
        {
            fade = static_cast<double> (stealSamplesLeft_) / static_cast<double> (stealSamplesTotal_);
            --stealSamplesLeft_;
        }

        const double glideNow = gliding ? glidePitch() : glideTarget_;
        float left = 0.0f;
        float right = 0.0f;

        // **What each routing row contributes, before the voice filters** — the source of a bus send
        // (ticket `64`). Reset per frame so a row whose source is off this frame sends nothing rather
        // than the last frame's sample.
        if (sending)
        {
            tapLeft.fill (0.0f);
            tapRight.fill (0.0f);
        }

        // The two fixed slots first. Their output is mixed like any other slot's — the reference's own
        // mixer sums the five — and generating them here costs nothing, because neither reads another
        // slot. Neither is panned either, but for a different reason from a wavetable slot: **xerum wires
        // no `kParamPan` at all** (ticket 41's scope was the unison spread), so every slot's own pan is
        // unapplied and sits centred. The corpus does store the field — measured: 53/59/57 presets on the
        // wavetable slots and 28/8 on NOISE/SUB — so that is a real, separate gap rather than a fact about
        // the bank.
        //
        // NOISE with **no** colour is the reference's sample mode, which xerum does not render: the slot
        // is silent here and the plugin reports it by name rather than generating a colour the preset did
        // not ask for.
        if (settings_.noise.enabled && settings_.noise.type.has_value())
        {
            const double volume = clampDouble (
                settings_.noise.volume + volumeRamp[kNoiseSlot].at (frame, frames) * kVolumeModulationSpan,
                0.0, 2.0);
            const auto value = noise_.next() * static_cast<float> (volume);
            left += value;
            right += value;

            if (sending)
            {
                tapLeft[kNoiseSlot] = value;
                tapRight[kNoiseSlot] = value;
            }

            lastSlotSample_[static_cast<std::size_t> (kNoiseSlot)] = value;
        }

        if (settings_.sub.enabled)
        {
            if (subPitchVaries)
                sub_.setFrequency (subFrequency (
                    glideNow,
                    pitchRamp[kSubSlot].at (frame, frames) * kPitchModulationSpanSemitones,
                    octaveRamp.at (frame, frames) * kOctaveModulationSpanSemitones,
                    coarseRamp.at (frame, frames) * kCoarseModulationSpanSemitones));

            const double volume = clampDouble (
                settings_.sub.volume + volumeRamp[kSubSlot].at (frame, frames) * kVolumeModulationSpan,
                0.0, 2.0);
            const auto value = sub_.next() * static_cast<float> (volume);
            left += value;
            right += value;

            if (sending)
            {
                tapLeft[kSubSlot] = value;
                tapRight[kSubSlot] = value;
            }

            lastSlotSample_[static_cast<std::size_t> (kSubSlot)] = value;
        }

        for (int s = 0; s < kOscillatorSlots; ++s)
        {
            const auto& os = settings_.oscillators[s];
            if (! os.enabled || unison[s] <= 0)
                continue;

            if (pitchVaries)
            {
                const double modPitch = pitchRamp[s].at (frame, frames) * kPitchModulationSpanSemitones;
                const double m = std::exp2 ((glideNow + modPitch + os.pitchSemitones) / 12.0);
                const double baseHz = kA4Hz * m;

                for (int i = 0; i < unison[s]; ++i)
                    oscillators_[s][i].setFrequency (baseHz * detune[s][i]);
            }

            // **The warp's modulator.** Ticket 25 built the source-driven families to take their modulator
            // as an argument and nothing ever called `setWarpModulator`, so `kPD_OSC`, `kFM_OSC`, the
            // `kAM_*` family and the rest were transparent in the product while passing their own unit
            // tests. Here the source the *name* encodes reaches the warp that reads it — before the table
            // is read, because a phase-domain warp rewrites the read position.
            for (int w = 0; w < WavetableOscillator::kWarpSlotCount; ++w)
            {
                if (os.warpMode[w] == WarpMode::None || os.warpSource[w] == WarpSource::None)
                    continue;

                const double modulator = warpModulatorValue (os.warpSource[w], s);

                for (int i = 0; i < unison[s]; ++i)
                    oscillators_[s][i].setWarpModulator (w, modulator);
            }

            // **The stereo spread is here.** Each sub-voice is placed across the image by its own position
            // in the stack — the same position that sets its detune — so the voice's width comes from the
            // unison stack itself rather than from any post-processing.
            float sumLeft = 0.0f;
            float sumRight = 0.0f;

            for (int i = 0; i < unison[s]; ++i)
            {
                const auto value = oscillators_[s][i].next();
                sumLeft += value * static_cast<float> (panLeft[s][i]);
                sumRight += value * static_cast<float> (panRight[s][i]);
            }

            const double volume = clampDouble (
                os.volume + volumeRamp[s].at (frame, frames) * kVolumeModulationSpan, 0.0, 2.0);
            const auto amplitude = static_cast<float> (unisonNorm[s] * volume);
            left += sumLeft * amplitude;
            right += sumRight * amplitude;

            if (sending)
            {
                tapLeft[s] = sumLeft * amplitude;
                tapRight[s] = sumRight * amplitude;
            }

            // What this slot contributed, which is both its `OSC A/B/C` source value and what any warp
            // reading *this* oscillator sees. Post-level, so a preset that mutes the source stops the
            // modulation with it rather than leaving it running silently.
            lastSlotSample_[static_cast<std::size_t> (s)]
                = 0.5 * (static_cast<double> (sumLeft) + static_cast<double> (sumRight))
                  * static_cast<double> (amplitude);
        }

        for (int f = 0; f < kFilterSlots; ++f)
        {
            const auto& fs = settings_.filters[f];

            if (fs.enabled)
            {
                if (filterVaries)
                {
                    const auto cutoff = clampDouble (fs.cutoff
                                                         + cutoffRamp[f].at (frame, frames)
                                                               * kCutoffModulationSpan,
                                                     0.0, 1.0);
                    const auto resonance = clampDouble (fs.resonance
                                                            + resoRamp[f].at (frame, frames)
                                                                  * kResonanceModulationSpan,
                                                        0.0, 100.0);

                    for (auto& filter : filters_[f])
                    {
                        filter.setCutoff (cutoff);
                        filter.setResonance (resonance);
                    }
                }

                left = filters_[f][0].process (left);
                right = filters_[f][1].process (right);

                // `Filter 1`/`Filter 2` as modulation sources: the signal leaving that filter. Filter 2's
                // input is filter 1's output, so the two sources are what the chain actually produces.
                lastFilterSample_[static_cast<std::size_t> (f)]
                    = 0.5 * (static_cast<double> (left) + static_cast<double> (right));
            }

            // **The `Filter 1` / `Filter 2` routing rows** (ticket `64`): the signal *leaving* this stage.
            // A disabled filter passes its input through, so its row carries the pre-filter mix — which is
            // also why this sits outside the `enabled` branch rather than with the filter's own source.
            if (sending)
            {
                tapLeft[static_cast<std::size_t> (kSlotCount + f)] = left;
                tapRight[static_cast<std::size_t> (kSlotCount + f)] = right;
            }
        }

        const double envelope = envelope_.level();
        envelope_.advance();

        if (gliding && ++glideSamplesDone_ >= glideSamplesTotal_)
        {
            gliding = false;
            glideFrom_ = glideTarget_;
        }

        const auto amplitude = static_cast<float> (envelope * velocity_ * fade);
        outLeft[frame] += left * amplitude;
        outRight[frame] += right * amplitude;

        // **The sends** (ticket `64`) close the frame, with the same amplitude the main path just took, so
        // a released or stolen voice's bus send fades with it. Each bus sums its rows' taps at their own
        // levels and *adds* — the reference's send is a parallel tap, and the main path above is the
        // source's own either way.
        if (sending)
        {
            for (int bus = 0; bus < 2; ++bus)
            {
                if (sends.left[bus] == nullptr || sends.right[bus] == nullptr)
                    continue;

                float busLeft = 0.0f;
                float busRight = 0.0f;

                for (int row = 0; row < kRoutingRowCount; ++row)
                {
                    const auto gain = static_cast<float> (settings_.sends.gain[row][bus]);

                    if (gain == 0.0f)
                        continue;

                    busLeft += tapLeft[static_cast<std::size_t> (row)] * gain;
                    busRight += tapRight[static_cast<std::size_t> (row)] * gain;
                }

                sends.left[bus][frame] += busLeft * amplitude;
                sends.right[bus][frame] += busRight * amplitude;
            }
        }
    }

    if (stage_ == VoiceStage::Releasing && envelope_.idle())
        stage_ = VoiceStage::Idle;
}

// ----------------------------------------------------------------- VoiceEngine

void VoiceEngine::prepare (double sampleRate, int maxBlockFrames, int polyphony)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    polyphony_ = clampInt (polyphony, 1, kMaxVoices);

    settings_.envelope.prepare (sampleRate_);
    matrix_.prepare (sampleRate_, maxBlockFrames);
    globalGenerator_.prepare (sampleRate_);

    for (int i = 0; i < kMaxVoices; ++i)
    {
        voices_[i].prepare (sampleRate_);
        voices_[i].setVoiceIndex (i);
        voices_[i].setModulationSources (&sourceSettings_);
        voices_[i].setSettings (settings_);
    }

    reset();
}

void VoiceEngine::reset()
{
    for (auto& voice : voices_)
        voice.kill();

    globalGenerator_.reset();

    noteStackSize_ = 0;
    noteCounter_ = 0;
    sustainDown_ = false;
    sustainHeld_.fill (false);
    monoSustainPending_ = false;
    lastPitchValid_ = false;
}

void VoiceEngine::setSettings (const VoiceSettings& settings)
{
    settings_ = settings;
    settings_.envelope.prepare (sampleRate_);

    for (auto& voice : voices_)
        voice.setSettings (settings_);
}

int VoiceEngine::activeVoiceCount() const
{
    int count = 0;
    for (int i = 0; i < polyphony_; ++i)
        if (voices_[i].active())
            ++count;
    return count;
}

int VoiceEngine::currentNote() const
{
    return noteStackSize_ > 0 ? noteStack_[noteStackSize_ - 1] : -1;
}

int VoiceEngine::chooseVoice() const
{
    // 1. A free voice is not a steal.
    for (int i = 0; i < polyphony_; ++i)
        if (! voices_[i].active())
            return i;

    auto better = [] (const Voice& candidate, const Voice& best)
    {
        const double level = candidate.level();
        const double bestLevel = best.level();

        if (level != bestLevel)
            return level < bestLevel;
        return candidate.age() < best.age();   // equal level: oldest first
    };

    // 2. A releasing voice, quietest first.
    int best = -1;
    for (int i = 0; i < polyphony_; ++i)
    {
        if (! voices_[i].releasing())
            continue;
        if (best < 0 || better (voices_[i], voices_[best]))
            best = i;
    }
    if (best >= 0)
        return best;

    // 3. Nothing releasing: still quietest, which for equal sustain levels is the age tie-break.
    for (int i = 0; i < polyphony_; ++i)
        if (best < 0 || better (voices_[i], voices_[best]))
            best = i;

    return best;
}

bool VoiceEngine::anyNoteHeld() const
{
    for (int i = 0; i < polyphony_; ++i)
        if (voices_[i].active() && voices_[i].stage() == VoiceStage::On)
            return true;
    return false;
}

double VoiceEngine::glideOrigin (double targetSemitones, bool noteWasHeld) const
{
    if (settings_.portamentoTime <= 0.0 || ! lastPitchValid_)
        return targetSemitones;
    if (! settings_.portamentoAlways && ! noteWasHeld)
        return targetSemitones;
    return lastPitch_;
}

void VoiceEngine::startVoice (int index, int note, double velocity, double startPitch, bool fade)
{
    sustainHeld_[static_cast<std::size_t> (index)] = false;
    voices_[index].setAge (++noteCounter_);

    if (fade && voices_[index].active())
        voices_[index].steal (note, velocity, startPitch);
    else
        voices_[index].start (note, velocity, startPitch);
}

void VoiceEngine::pushNote (int note)
{
    for (int i = 0; i < noteStackSize_; ++i)
    {
        if (noteStack_[i] != note)
            continue;

        for (int j = i; j + 1 < noteStackSize_; ++j)
            noteStack_[j] = noteStack_[j + 1];

        --noteStackSize_;
        break;
    }

    if (noteStackSize_ < kMaxVoices)
        noteStack_[noteStackSize_++] = note;
}

void VoiceEngine::removeNote (int note)
{
    for (int i = 0; i < noteStackSize_; ++i)
    {
        if (noteStack_[i] != note)
            continue;

        for (int j = i; j + 1 < noteStackSize_; ++j)
            noteStack_[j] = noteStack_[j + 1];

        --noteStackSize_;
        return;
    }
}

int VoiceEngine::noteOn (int note, double velocity)
{
    // A `Mono` LFO is the whole pool's, so its one-shot restart belongs to the *phrase*: the first note
    // after silence. A voice's own one-shot LFOs restart per note, in `Voice::start`.
    if (activeVoiceCount() == 0)
        globalGenerator_.restartOneShotLfos (sourceSettings_);

    if (settings_.mode != VoiceMode::Poly)
    {
        const bool noteWasHeld = noteStackSize_ > 0;
        pushNote (note);

        auto& voice = voices_[0];
        const double target = static_cast<double> (note) - 69.0;

        if (! voice.active())
        {
            startVoice (0, note, velocity, glideOrigin (target, noteWasHeld), /* fade */ false);
        }
        else if (settings_.mode == VoiceMode::Mono)
        {
            // Retrigger, gliding from wherever the voice already is.
            startVoice (0, note, velocity, voice.pitchSemitones(), /* fade */ false);
        }
        else
        {
            // Legato: the envelope runs through, only the pitch moves.
            voice.setAge (++noteCounter_);
            voice.retarget (note, target);
        }

        lastPitch_ = target;
        lastPitchValid_ = true;
        return 0;
    }

    const int index = chooseVoice();
    if (index < 0)
        return -1;

    const double target = static_cast<double> (note) - 69.0;
    startVoice (index, note, velocity, glideOrigin (target, anyNoteHeld()), /* fade */ true);

    lastPitch_ = target;
    lastPitchValid_ = true;
    return index;
}

void VoiceEngine::noteOff (int note)
{
    if (settings_.mode != VoiceMode::Poly)
    {
        removeNote (note);

        if (noteStackSize_ > 0)
        {
            const int top = noteStack_[noteStackSize_ - 1];
            voices_[0].retarget (top, static_cast<double> (top) - 69.0);
        }
        else if (sustainDown_)
        {
            monoSustainPending_ = true;
        }
        else
        {
            voices_[0].release();
        }

        return;
    }

    for (int i = 0; i < polyphony_; ++i)
    {
        auto& voice = voices_[i];

        // A note that caused a steal and was released before it started must not start at all.
        if (voice.pendingNote() == note)
        {
            voice.cancelPending (note);
            continue;
        }

        if (! voice.active() || voice.note() != note || voice.stage() == VoiceStage::Stealing)
            continue;
        if (sustainHeld_[static_cast<std::size_t> (i)])
            continue;

        if (sustainDown_)
            sustainHeld_[static_cast<std::size_t> (i)] = true;
        else
            voice.release();
    }
}

void VoiceEngine::setSustainPedal (bool down)
{
    if (! down && sustainDown_)
    {
        for (int i = 0; i < kMaxVoices; ++i)
        {
            if (! sustainHeld_[static_cast<std::size_t> (i)])
                continue;

            voices_[i].release();
            sustainHeld_[static_cast<std::size_t> (i)] = false;
        }

        if (monoSustainPending_ && noteStackSize_ == 0)
            voices_[0].release();

        monoSustainPending_ = false;
    }

    sustainDown_ = down;
}

void VoiceEngine::process (float* outLeft, float* outRight, const BusSends& sends, int frames)
{
    if (outLeft == nullptr || outRight == nullptr || frames <= 0)
        return;

    std::fill_n (outLeft, frames, 0.0f);
    std::fill_n (outRight, frames, 0.0f);

    const bool matrixActive = ! matrix_.slots().empty();
    const int active = activeVoiceCount();

    // The pool's own generators first: the LFOs a preset marks `Mono` run once for every voice, and their
    // value joins the block inputs so a voice's generator — which skips them — does not overwrite it.
    ModulationInputs base = globalInputs_;
    globalGenerator_.advanceMonoLfos (sourceSettings_, base, frames);

    for (int i = 0; i < polyphony_; ++i)
    {
        auto& voice = voices_[i];
        if (! voice.active())
            continue;

        ModulationInputs inputs = base;
        inputs.voiceIndex = polyphony_ > 1
                                ? static_cast<double> (i) / static_cast<double> (polyphony_ - 1)
                                : 0.0;
        inputs.activeVoices = static_cast<double> (active) / static_cast<double> (polyphony_);
        inputs.noteNumber = static_cast<double> (voice.note()) / 127.0;
        inputs.velocity = voice.velocity();

        // `inputs.envelope[0..3]` are **the voice's four matrix envelopes**, written by its generator in
        // `render`. They used to be the amplitude envelope's level, which is a different generator with
        // different settings and its own state, and the two must not be conflated: ticket 48 asserts a
        // matrix envelope moving a filter while the amplitude envelope shapes the level.

        voice.render (outLeft, outRight, sends, frames, matrixActive ? &matrix_ : nullptr, inputs);
    }
}

} // namespace xerum::dsp
