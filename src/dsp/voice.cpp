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
} // namespace

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

    for (auto& filter : filters_)
        filter.prepare (sampleRate_);

    noise_.prepare (sampleRate_);
    sub_.prepare (sampleRate_);

    envelope_.prepare (sampleRate_);
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
        filters_[f].setSpec (fs.spec);
        filters_[f].setCutoff (clampDouble (fs.cutoff, 0.0, 1.0));
        filters_[f].setResonance (clampDouble (fs.resonance, 0.0, 100.0));
        filters_[f].setDrive (clampDouble (fs.drive, 0.0, 100.0));
        filters_[f].setMorph (clampDouble (fs.morph, 0.0, 1.0));
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
}

void Voice::release()
{
    if (stage_ != VoiceStage::On)
        return;

    stage_ = VoiceStage::Releasing;
    envelope_.releaseStage();
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
}

void Voice::render (float* out, int frames, ModulationMatrix* matrix, const ModulationInputs& inputs)
{
    if (stage_ == VoiceStage::Idle || frames <= 0)
        return;

    if (stage_ == VoiceStage::Releasing && envelope_.idle())
    {
        stage_ = VoiceStage::Idle;
        return;
    }

    // ---- Control rate: one matrix evaluation per block, ramps read per sample below.
    const bool modulated = matrix != nullptr && ! matrix->slots().empty();
    if (modulated)
        matrix->beginBlock (inputs, frames);

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

    for (int s = 0; s < kOscillatorSlots; ++s)
    {
        const auto& os = settings_.oscillators[s];
        const int n = clampInt (os.unison, 1, kMaxUnison);
        unison[s] = n;
        unisonNorm[s] = 1.0 / std::sqrt (static_cast<double> (n));

        for (int i = 0; i < n; ++i)
        {
            const double position = n > 1 ? static_cast<double> (i) / static_cast<double> (n - 1) - 0.5 : 0.0;
            detune[s][i] = std::exp2 (os.unisonRangeSemitones * position / 12.0);
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

            filters_[f].setCutoff (clampDouble (fs.cutoff + cutoffRamp[f].start * kCutoffModulationSpan,
                                                0.0, 1.0));
            filters_[f].setResonance (clampDouble (
                fs.resonance + resoRamp[f].start * kResonanceModulationSpan, 0.0, 100.0));
        }
    };

    if (! pitchVaries)
        setConstantOscillatorFrequencies();
    if (! subPitchVaries)
        setConstantSubFrequency();
    if (! filterVaries)
        setConstantFilter();

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
        float sample = 0.0f;

        // The two fixed slots first. Their output is mixed like any other slot's — the reference's own
        // mixer sums the five — and generating them here costs nothing, because neither reads another
        // slot.
        //
        // NOISE with **no** colour is the reference's sample mode, which xerum does not render: the slot
        // is silent here and the plugin reports it by name rather than generating a colour the preset did
        // not ask for.
        if (settings_.noise.enabled && settings_.noise.type.has_value())
        {
            const double volume = clampDouble (
                settings_.noise.volume + volumeRamp[kNoiseSlot].at (frame, frames) * kVolumeModulationSpan,
                0.0, 2.0);
            sample += noise_.next() * static_cast<float> (volume);
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
            sample += sub_.next() * static_cast<float> (volume);
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

            float sum = 0.0f;
            for (int i = 0; i < unison[s]; ++i)
                sum += oscillators_[s][i].next();

            const double volume = clampDouble (
                os.volume + volumeRamp[s].at (frame, frames) * kVolumeModulationSpan, 0.0, 2.0);
            sample += sum * static_cast<float> (unisonNorm[s] * volume);
        }

        for (int f = 0; f < kFilterSlots; ++f)
        {
            const auto& fs = settings_.filters[f];
            if (! fs.enabled)
                continue;

            if (filterVaries)
            {
                filters_[f].setCutoff (clampDouble (fs.cutoff
                                                        + cutoffRamp[f].at (frame, frames)
                                                              * kCutoffModulationSpan,
                                                    0.0, 1.0));
                filters_[f].setResonance (clampDouble (fs.resonance
                                                           + resoRamp[f].at (frame, frames)
                                                                 * kResonanceModulationSpan,
                                                       0.0, 100.0));
            }

            sample = filters_[f].process (sample);
        }

        const double envelope = envelope_.level();
        envelope_.advance();

        if (gliding && ++glideSamplesDone_ >= glideSamplesTotal_)
        {
            gliding = false;
            glideFrom_ = glideTarget_;
        }

        out[frame] += sample * static_cast<float> (envelope * velocity_ * fade);
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

    for (int i = 0; i < kMaxVoices; ++i)
    {
        voices_[i].prepare (sampleRate_);
        voices_[i].setVoiceIndex (i);
        voices_[i].setSettings (settings_);
    }

    reset();
}

void VoiceEngine::reset()
{
    for (auto& voice : voices_)
        voice.kill();

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

void VoiceEngine::process (float* out, int frames)
{
    if (out == nullptr || frames <= 0)
        return;

    std::fill_n (out, frames, 0.0f);

    const bool matrixActive = ! matrix_.slots().empty();
    const int active = activeVoiceCount();

    for (int i = 0; i < polyphony_; ++i)
    {
        auto& voice = voices_[i];
        if (! voice.active())
            continue;

        ModulationInputs inputs = globalInputs_;
        inputs.voiceIndex = polyphony_ > 1
                                ? static_cast<double> (i) / static_cast<double> (polyphony_ - 1)
                                : 0.0;
        inputs.activeVoices = static_cast<double> (active) / static_cast<double> (polyphony_);
        inputs.noteNumber = static_cast<double> (voice.note()) / 127.0;
        inputs.velocity = voice.velocity();
        inputs.envelope[0] = voice.level();

        voice.render (out, frames, matrixActive ? &matrix_ : nullptr, inputs);
    }
}

} // namespace xerum::dsp
