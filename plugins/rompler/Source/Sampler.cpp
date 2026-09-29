#include "Sampler.h"
#include "BandLimitedInterpolator.h"
#include "SF2Loader.h"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace aod
{

namespace
{
    // Fixed musical vibrato rate applied by CC1 (mod wheel). Not tempo-synced
    // or user-configurable per the spec; a gentle ~5.5 Hz reads as natural
    // vocal/string-style vibrato without sounding like a tremolo effect.
    constexpr float kVibratoRateHz = 5.5f;

    // The zone modulation envelope sweeps the filter, so the cutoff has to be
    // refreshed part way through a block rather than once per block. 16 samples
    // is short enough that the sweep tracks the envelope, and long enough that
    // the per-update cost (one tan plus a few divides, no allocation) stays
    // negligible against the per-sample work that follows it.
    constexpr int kModulationSubBlock = 16;

    // Lane layout for the synthetic stereo spread, indexed by voice slot.
    // Lane 0 stays in the centre, so the first voice of a fresh phrase (which
    // always takes the lowest idle slot) remains dual-mono. The remaining lanes
    // alternate in mirrored pairs, which keeps the position set exactly
    // left/right balanced, and their magnitude never exceeds 0.5 so the widest
    // lane still cannot saturate against the pan clamp at full width.
    constexpr std::array<float, 9> kStereoVoicePositions {
        0.0f, -0.30f, 0.30f, -0.48f, 0.48f, -0.14f, 0.14f, -0.38f, 0.38f
    };

    constexpr float kEqualPowerHalfPi = juce::MathConstants<float>::halfPi;
    // Equal power passes through 1/sqrt(2) at the centre. Scaling by sqrt(2)
    // keeps a centred voice at unity in both channels, so width zero reproduces
    // the legacy dual-mono signal exactly and every lane carries the same total
    // power (left^2 + right^2) regardless of where it sits in the image.
    constexpr float kEqualPowerUnityScale = juce::MathConstants<float>::sqrt2;

    /**
        SoundFont velocity to amplitude.

        The reference implementation measures 40*log10(velocity) dB at full
        velocity 127 - 11.9 dB at 64, 24.0 dB at 32, 84.2 dB at 1 - quantised to
        the 0.1 dB of the format's centibels. In amplitude terms that is a
        quadratic curve, which is what the banks are balanced against; a linear
        velocity multiply made every soft note too loud by up to 20 dB.

        Measured from FluidSynth 2.x rendering the local corpus (fresh synth per
        velocity, 48 kHz, reverb and chorus off): the max deviation from this
        curve over all 127 velocities was the 0.05 dB quantisation itself.
    */
    [[nodiscard]] float velocityGainFor (float velocity) noexcept
    {
        const float clamped = std::clamp (velocity, 1.0f / 127.0f, 1.0f);
        const float attenuationCb = std::round (400.0f * std::log10 (clamped));
        return std::pow (10.0f, attenuationCb * 0.1f / 20.0f);
    }
}

float Voice::Drive::curveValue (float driven, int curveId) noexcept
{
    if (curveId == 1)
        return tube_.process (driven);
    if (curveId == 2)
        return transformer_.process (driven);

    return tanh_.process (driven);
}

float Voice::Drive::directValue (float driven, int curveId) noexcept
{
    if (curveId == 1)
        return x10::dsp::curves::Tube::f (driven);
    if (curveId == 2)
        return x10::dsp::curves::Transformer::f (driven);

    return x10::dsp::curves::Tanh::f (driven);
}

float Voice::Drive::process (float x, int curveId, float blend, float gain) noexcept
{
    if (blend <= 0.0f || ! std::isfinite (gain) || gain <= 0.0f)
        return x;

    if (curveId != lastCurveId_)
    {
        // The cached F1 belongs to the curve that produced it, so entering a
        // different curve with that value would be a mismatched antiderivative.
        if (curveId == 1)
            tube_.reset();
        else if (curveId == 2)
            transformer_.reset();
        else
            tanh_.reset();
        lastCurveId_ = curveId;
    }

    const float driven = gain * x;
    const float coloured = (antialias_ ? curveValue (driven, curveId)
                                       : directValue (driven, curveId)) / gain;
    return x + blend * (coloured - x);
}

void Voice::Drive::reset() noexcept
{
    tanh_.reset();
    tube_.reset();
    transformer_.reset();
    lastCurveId_ = -1;
}

void Voice::VolumeEnvelope::reset (const x10::instrument::Envelope& parameters) noexcept
{
    parameters_ = parameters;
    level_ = 0.0f;
    increment_ = 0.0f;
    samplesRemaining_ = 0;
    stage_ = Stage::Delay;
    if (sampleRate_ > 0)
        enter (Stage::Delay);
}

void Voice::VolumeEnvelope::setParameters (const x10::instrument::Envelope& parameters) noexcept
{
    parameters_ = parameters;
}

int Voice::VolumeEnvelope::durationSamples (float seconds) const noexcept
{
    if (!std::isfinite (seconds) || seconds <= 0.0f || sampleRate_ <= 0)
        return 0;
    return std::max (0, juce::roundToInt (seconds * static_cast<float> (sampleRate_)));
}

void Voice::VolumeEnvelope::enter (Stage stage) noexcept
{
    stage_ = stage;
    increment_ = 0.0f;

    switch (stage_)
    {
        case Stage::Delay:
            level_ = 0.0f;
            samplesRemaining_ = durationSamples (parameters_.delaySeconds);
            break;
        case Stage::Attack:
            samplesRemaining_ = durationSamples (parameters_.attackSeconds);
            increment_ = samplesRemaining_ > 0 ? (1.0f - level_) / static_cast<float> (samplesRemaining_) : 0.0f;
            break;
        case Stage::Hold:
            level_ = 1.0f;
            samplesRemaining_ = durationSamples (parameters_.holdSeconds);
            break;
        case Stage::Decay:
        {
            const float sustain = std::clamp (parameters_.sustainLevel, 0.0f, 1.0f);
            samplesRemaining_ = durationSamples (parameters_.decaySeconds);
            increment_ = samplesRemaining_ > 0 ? (sustain - level_) / static_cast<float> (samplesRemaining_) : 0.0f;
            break;
        }
        case Stage::Sustain:
            level_ = std::clamp (parameters_.sustainLevel, 0.0f, 1.0f);
            samplesRemaining_ = 0;
            // A zone that decays to silence (sustain attenuation at maximum) is
            // finished: retiring here frees the slot instead of holding a
            // permanently silent voice until note-off.
            if (level_ <= 0.0f)
                enter (Stage::Idle);
            break;
        case Stage::Release:
            samplesRemaining_ = durationSamples (releaseSeconds_);
            increment_ = samplesRemaining_ > 0 ? -level_ / static_cast<float> (samplesRemaining_) : 0.0f;
            break;
        case Stage::Idle:
            level_ = 0.0f;
            samplesRemaining_ = 0;
            break;
    }
}

void Voice::VolumeEnvelope::startRelease (float seconds) noexcept
{
    if (stage_ == Stage::Idle || stage_ == Stage::Release)
        return;

    releaseSeconds_ = seconds;
    enter (Stage::Release);
}

void Voice::VolumeEnvelope::prepare (int sampleRate) noexcept
{
    const int safeRate = std::max (1, sampleRate);
    if (sampleRate_ == safeRate)
        return;

    sampleRate_ = safeRate;
    // The envelope is configured before a voice reaches render().  Re-entering
    // its current stage here turns its time value into source-rate samples once,
    // without any allocation or a per-block reset.
    enter (stage_);
}

float Voice::VolumeEnvelope::tick() noexcept
{
    if (stage_ == Stage::Idle)
        return 0.0f;

    switch (stage_)
    {
        case Stage::Delay:
            if (samplesRemaining_-- > 0)
                return 0.0f;
            enter (Stage::Attack);
            return level_;
        case Stage::Attack:
            if (samplesRemaining_ <= 0)
            {
                level_ = 1.0f;
                enter (Stage::Hold);
                return level_;
            }
            level_ += increment_;
            if (--samplesRemaining_ == 0)
            {
                level_ = 1.0f;
                enter (Stage::Hold);
            }
            return level_;
        case Stage::Hold:
            if (samplesRemaining_-- > 0)
                return level_;
            enter (Stage::Decay);
            return level_;
        case Stage::Decay:
            if (samplesRemaining_ <= 0)
            {
                enter (Stage::Sustain);
                return level_;
            }
            level_ += increment_;
            if (--samplesRemaining_ == 0)
                enter (Stage::Sustain);
            return level_;
        case Stage::Sustain:
            return level_;
        case Stage::Release:
            if (samplesRemaining_ <= 0)
            {
                enter (Stage::Idle);
                return 0.0f;
            }
            level_ += increment_;
            if (--samplesRemaining_ == 0)
                enter (Stage::Idle);
            return level_;
        case Stage::Idle:
            return 0.0f;
    }

    return 0.0f;
}

double Voice::computePlayRate(const Sample* sample, int midiNote) const noexcept
{
    const double semitones = static_cast<double> (midiNote - sample->rootKey)
        * static_cast<double> (sample->scaleTuningCentsPerKey) / 100.0
        + static_cast<double> (sample->tuneCents) / 100.0;
    return std::pow (2.0, semitones / 12.0);
}

Voice::~Voice()
{
    detachSample();
}

void Voice::bindSample(const Sample* sample, const SF2Loader* sampleOwner) noexcept
{
    const auto* previousOwner = sampleOwner_;
    if (sampleOwner != previousOwner && sampleOwner != nullptr)
        sampleOwner->retainVoiceSample();

    sample_ = sample;
    sampleOwner_ = sampleOwner;

    if (sampleOwner != previousOwner && previousOwner != nullptr)
        previousOwner->releaseVoiceSample();
}

void Voice::detachSample() noexcept
{
    const auto* previousOwner = sampleOwner_;
    sample_ = nullptr;
    sampleOwner_ = nullptr;
    if (previousOwner != nullptr)
        previousOwner->releaseVoiceSample();
}

void Voice::bindModulation (const Sample* sample) noexcept
{
    // Both depths are cents, and a depth of zero means "no modulation", so the
    // envelope is only worth running when at least one of them is non-zero.
    // Checking here rather than per sample keeps a preset with no modulation
    // generators on exactly the code path it used before this existed.
    modulationActive_ = sample->modEnvToPitchCents != 0.0f
                     || sample->modEnvToFilterCents != 0.0f;

    // A voice that has been modulated and then lands on an unmodulated zone
    // must not keep running the old envelope, so the idle case resets too.
    modulationEnvelope_.reset (modulationActive_ ? sample->modulationEnvelope
                                                 : x10::instrument::Envelope {});
}

void Voice::start(const Sample* sample, int midiNote, float velocity,
                  const SF2Loader* sampleOwner) noexcept
{
    bindSample (sample, sampleOwner);
    velocity_ = velocity;
    velocityGain_ = velocityGainFor (velocity);
    midiNote_ = midiNote;
    phase_ = 0.0;
    envPhase_ = 0.0f;
    envelopeLevel_ = 0.0f;
    vibratoPhase_ = 0.0;
    active_ = true;
    driveNeedsReset_ = true;
    drive_.reset();
    filterNeedsPrepare_ = true;
    attenuationGain_ = std::pow (10.0f,
                                 -std::clamp (std::isfinite (sample->attenuationDb)
                                                  ? sample->attenuationDb : 0.0f,
                                              0.0f, 144.0f) / 20.0f);
    volumeEnvelope_.reset (sample->volumeEnvelope);
    bindModulation (sample);
    zoneReleaseMs_ = std::isfinite (sample->volumeEnvelope.releaseSeconds)
        ? std::max (0.0f, sample->volumeEnvelope.releaseSeconds * 1000.0f)
        : 0.0f;

    // Copy loop points at start(): render() must not read through a sample
    // pointer that may belong to a retired loader once this voice is retriggered
    // against a newer one. Keeping the loop state here makes the audio thread
    // self-contained for the voice's lifetime.
    loopStart_ = sample->loopStart;
    loopEnd_   = sample->loopEnd;
    loopEnabled_ = sample->loopEnabled && loopEnd_ > loopStart_ + 1;
    hasLoopWrapped_ = false;

    // Pitch: the sample is recorded at rootKey. A note played N semitones above
    // rootKey must advance N semitones faster (pitch ratio 2^(N/12)); the
    // region's per-key scale (usually 100 cents/key) and constant tune offset
    // are folded in so a scale of 0 pins every note to the root pitch.
    playRate_ = computePlayRate (sample, midiNote);

    adsr_.noteOn();
}

void Voice::retarget(const Sample* sample, int midiNote, const SF2Loader* sampleOwner) noexcept
{
    if (!active_)
        return;

    // Same-sample legato preserves the current read position and envelope.
    // A different sample has different bounds and loop metadata, so retaining
    // the old phase/loop cache could index beyond the replacement buffer.
    if (sample_ != sample)
    {
        phase_ = 0.0;
        loopStart_ = sample->loopStart;
        loopEnd_ = sample->loopEnd;
        loopEnabled_ = sample->loopEnabled && loopEnd_ > loopStart_ + 1;
        hasLoopWrapped_ = false;
    }

    bindSample (sample, sampleOwner);
    midiNote_ = midiNote;
    playRate_ = computePlayRate (sample, midiNote);
    attenuationGain_ = std::pow (10.0f,
                                 -std::clamp (std::isfinite (sample->attenuationDb)
                                                  ? sample->attenuationDb : 0.0f,
                                              0.0f, 144.0f) / 20.0f);
    volumeEnvelope_.setParameters (sample->volumeEnvelope);
    // A legato move onto a different zone has to take that zone's modulation
    // with it, whether it means starting the envelope fresh or dropping back to
    // the unmodulated path. Restart rather than setParameters: a zone change is
    // a new note as far as the modulation envelope is concerned.
    bindModulation (sample);
    zoneReleaseMs_ = std::isfinite (sample->volumeEnvelope.releaseSeconds)
        ? std::max (0.0f, sample->volumeEnvelope.releaseSeconds * 1000.0f)
        : 0.0f;
}

void Voice::retire() noexcept
{
    active_ = false;
    midiNote_ = -1;
    detachSample();
    envelopeLevel_ = 0.0f;
    attenuationGain_ = 1.0f;
    zoneReleaseMs_ = 0.0f;
    drive_.reset();
}

void Voice::stop() noexcept
{
    if (!active_)
        return;
    adsr_.noteOff();
}

void Voice::choke() noexcept
{
    if (!active_)
        return;

    // The SoundFont release is the instrument's own fade for this zone, which
    // is exactly the length a choke wants: short enough that the new note is
    // not doubled, long enough that cutting mid-waveform cannot click.
    volumeEnvelope_.startRelease (juce::jlimit (0.0005f, 0.02f, zoneReleaseMs_ * 0.001f));
}

bool Voice::isReleasing() const noexcept
{
    return active_ && (adsr_.stage() == x10::dsp::Adsr::Stage::Release
                       || volumeEnvelope_.isReleasing());
}

void Voice::render(float* output, int numSamples, int hostSampleRate, float driveDb, float velToDriveDb,
                    int curveId, int filterRouting, float filterOffsetCents,
                    float attackMs, float decayMs, float sustainLevel, float releaseMs,
                    float pitchBendSemitones, float vibratoDepthCents,
                    const BandLimitedInterpolator& interpolator) noexcept
{
    if (!active_)
    {
        envelopeLevel_ = 0.0f;
        return;
    }

    if (sample_ == nullptr || sample_->data.empty())
    {
        active_ = false;
        detachSample();
        envelopeLevel_ = 0.0f;
        return;
    }

    const bool filterNeedsPrepare = filterNeedsPrepare_ || filterSampleRate_ != hostSampleRate;
    if (filterNeedsPrepare)
    {
        filter_.prepare (static_cast<double> (hostSampleRate));
        filterSampleRate_ = hostSampleRate;
        filterNeedsPrepare_ = false;
        // Prepare the envelope only on rate changes, not per block: prepare()
        // recomputes the current stage's increment, and doing that mid-Decay or
        // mid-Release every block would restart the ramp from the current
        // level, stretching what should be a fixed-time fade indefinitely.
        adsr_.prepare (static_cast<double> (hostSampleRate));
    }
    volumeEnvelope_.prepare (hostSampleRate);
    if (modulationActive_)
        modulationEnvelope_.prepare (hostSampleRate);

    std::uint32_t filterOffsetBits = 0;
    static_assert (sizeof (filterOffsetBits) == sizeof (filterOffsetCents),
                   "expected 32-bit float");
    std::memcpy (&filterOffsetBits, &filterOffsetCents, sizeof (filterOffsetBits));
    const bool filterParametersChanged = filterNeedsPrepare || ! filterParametersCached_
        || filterParameterSample_ != sample_
        || filterParameterOffsetBits_ != filterOffsetBits;
    if (filterParametersChanged)
    {
        const float cutoffHz = std::clamp (
            sample_->filterCutoffHz * std::pow (2.0f, filterOffsetCents / 1200.0f),
            20.0f, static_cast<float> (hostSampleRate) * 0.49f);
        filterQ_ = std::pow (10.0f, sample_->filterResonanceDb / 20.0f) * 0.7071068f;
        filter_.setCutoff (cutoffHz, filterQ_);
        filterParameterSample_ = sample_;
        filterParameterOffsetBits_ = filterOffsetBits;
        filterParametersCached_ = true;
    }

    // The SoundFont zone release is a floor, not a replacement: a preset that
    // states a long release keeps it, while the UI knob can only lengthen the
    // tail past it. Combining them as a lower bound preserves the previous
    // sound for the default 1 ms zone release.
    const float effectiveReleaseMs = std::max (releaseMs, zoneReleaseMs_);

    // Push ADSR parameters only on change: the setters recompute the current
    // stage's ramp even for identical values, which would restart a Decay or
    // Release fade from the current level every block.
    const float envParams[] = { attackMs, decayMs, sustainLevel, effectiveReleaseMs };
    std::uint32_t hash = 2166136261u;
    for (float v : envParams)
    {
        std::uint32_t bits = 0;
        static_assert (sizeof (bits) == sizeof (v), "expected 32-bit float");
        std::memcpy (&bits, &v, sizeof (bits));
        hash = (hash ^ bits) * 16777619u;
    }
    if (hash != envParamHash_)
    {
        envParamHash_ = hash;
        adsr_.setAttackSec (attackMs * 0.001f);
        adsr_.setDecaySec (decayMs * 0.001f);
        adsr_.setSustainLevel (std::clamp (sustainLevel, 0.0f, 1.0f));
        adsr_.setReleaseSec (effectiveReleaseMs * 0.001f);
    }

    const float* sampleData = sample_->data.data();
    const auto sampleCount = static_cast<std::int64_t>(sample_->data.size());

    // Velocity shapes the drive amount: velToDriveDb at 0% is neutral, +100%
    // makes hard hits drive harder and -100% does the inverse. This is an
    // additional dB offset centred so a velocity of 127 (1.0) is the reference.
    const float velDriveDb = driveDb + velToDriveDb * (velocity_ - 1.0f);
    if (driveNeedsReset_)
    {
        driveDbSmooth_.reset (static_cast<double> (hostSampleRate), 0.005);
        driveDbSmooth_.setCurrentAndTargetValue (velDriveDb);
        driveNeedsReset_ = false;
    }
    driveDbSmooth_.setTargetValue (velDriveDb);

    const bool driveIsSmoothing = driveDbSmooth_.isSmoothing();
    const float steadyDriveDb = driveDbSmooth_.getCurrentValue();
    const auto driveBlendForDb = [] (float dbValue) noexcept
    {
        const float position = std::clamp (std::abs (dbValue), 0.0f, 1.0f);
        return position * position * (3.0f - 2.0f * position);
    };
    if (! driveIsSmoothing)
    {
        std::uint32_t driveDbBits = 0;
        static_assert (sizeof (driveDbBits) == sizeof (steadyDriveDb),
                       "expected 32-bit float");
        std::memcpy (&driveDbBits, &steadyDriveDb, sizeof (driveDbBits));
        if (! steadyDriveCacheValid_ || steadyDriveDbBits_ != driveDbBits)
        {
            steadyDriveDbBits_ = driveDbBits;
            steadyDriveBlend_ = driveBlendForDb (steadyDriveDb);
            steadyDriveGain_ = steadyDriveBlend_ > 0.0f
                ? std::pow (10.0f, steadyDriveDb / 20.0f)
                : 1.0f;
            steadyDriveCacheValid_ = true;
        }
    }
    const float steadyDriveBlend = driveIsSmoothing ? 0.0f : steadyDriveBlend_;
    const float steadyDriveGain = driveIsSmoothing ? 1.0f : steadyDriveGain_;

    // Loop points as sample-frame indices into sampleData. While looping, phase_
    // wraps from loopEnd_ back to loopStart_ so sustained notes never run off
    // the end of the sample; during Release the loop is ignored and the tail
    // plays out so the ADSR release has real data to fade.
    const bool inRelease = adsr_.stage() == x10::dsp::Adsr::Stage::Release;
    const bool looping = loopEnabled_ && !inRelease;
    const auto loopStart = static_cast<std::int64_t>(loopStart_);
    const auto loopEnd = static_cast<std::int64_t>(loopEnd_);

    // Vibrato LFO angular increment per sample at the fixed musical rate.
    const double vibratoIncrement =
        2.0 * juce::MathConstants<double>::pi * static_cast<double> (kVibratoRateHz)
        / static_cast<double> (hostSampleRate);

    const double fixedRate = playRate_ * std::pow (2.0, static_cast<double> (pitchBendSemitones) / 12.0);
    const float fixedPosition = interpolator.positionForRate (fixedRate);
    // positionForRate is linear in log2(rate). Vibrato is additive in
    // semitones, so its table coordinate is an additive offset as well.
    const double fixedLog2Rate = vibratoDepthCents != 0.0f ? std::log2 (fixedRate) : 0.0;
    constexpr double rateBracketScale =
        static_cast<double> (BandLimitedInterpolator::kNumRateBrackets - 1) / 4.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const float globalEnv = adsr_.tick();
        const float zoneEnv = volumeEnvelope_.tick();
        const float env = globalEnv * zoneEnv;
        envelopeLevel_ = env;
        if (!adsr_.isActive() || !volumeEnvelope_.isActive())
        {
            active_ = false;
            detachSample();
            envelopeLevel_ = 0.0f;
            break;
        }

        // The zone modulation envelope drives pitch and filter. Its level is a
        // 0..1 ramp, and the SoundFont depths say how far the voice is thrown
        // at full level, so the modulation amount is the level times the depth.
        // Tick it once per sample and reuse the value for both destinations so
        // the two stay in lockstep.
        const float modLevel = modulationActive_ ? modulationEnvelope_.tick() : 0.0f;

        // Refresh the swept cutoff on a fixed sub-block cadence rather than
        // per sample: one tan plus a few divides is cheap, but doing it 48000
        // times a second per voice is not, and a smooth envelope does not need
        // that resolution to read as a sweep. setCutoff recomputes only the
        // coefficients and keeps the filter's delay line, so the cutoff glides
        // without a discontinuity that a re-prepare would introduce.
        if (modulationActive_ && sample_->modEnvToFilterCents != 0.0f
            && (i % kModulationSubBlock) == 0)
        {
            const float sweptCents = filterOffsetCents
                                   + modLevel * sample_->modEnvToFilterCents;
            const float cutoffHz = std::clamp (
                sample_->filterCutoffHz * std::pow (2.0f, sweptCents / 1200.0f),
                20.0f, static_cast<float> (hostSampleRate) * 0.49f);
            filter_.setCutoff (cutoffHz, filterQ_);
        }

        // Pitch bend and vibrato are combined as an additional semitone
        // offset applied per sample, on top of the cached playRate_. This
        // keeps a live wheel/CC1 change instantaneous without recomputing or
        // resetting the cached rate (which would otherwise jump the read
        // phase).
        double effectiveRate = fixedRate;
        float vibratoSemitones = 0.0f;
        if (vibratoDepthCents != 0.0f)
        {
            vibratoSemitones =
                (vibratoDepthCents * static_cast<float> (std::sin (vibratoPhase_))) / 100.0f;
            effectiveRate = fixedRate * std::exp2 (static_cast<double> (vibratoSemitones) / 12.0);
        }

        // The zone's own depth rides on top as an exponential, which keeps it
        // out of the additive vibrato term so a modEnv sweep and CC1 vibrato
        // scale each other the way a player would expect rather than fighting.
        if (modulationActive_ && sample_->modEnvToPitchCents != 0.0f)
        {
            effectiveRate *= std::exp2 (
                static_cast<double> (modLevel * sample_->modEnvToPitchCents) / 1200.0);
        }

        if (!looping)
        {
            const auto index = static_cast<std::int64_t>(phase_);
            if (index >= sampleCount - 1)
            {
                if (inRelease)
                {
                    // While releasing, hold the last sample position instead of
                    // falling off the end of the buffer: the fade must run to
                    // zero on its own, or the waveform is truncated mid-cycle
                    // and clicks.
                    phase_ = static_cast<double>(sampleCount - 1);
                }
                else
                {
                    active_ = false;
                    detachSample();
                    envelopeLevel_ = 0.0f;
                    break;
                }
            }
        }

        const auto index = static_cast<std::int64_t>(phase_);
        const float frac = static_cast<float>(phase_ - static_cast<double>(index));

        // Reading faster than 1x lifts the source spectrum, so anything above
        // the output Nyquist would fold back as an audible tone. The shared
        // sinc table band-limits the read for the rate actually being played;
        // taps wrap inside an active loop so the seam stays continuous, and
        // clamp at the ends so a one-shot never reads outside its buffer.
        const float position = vibratoDepthCents != 0.0f
            ? (effectiveRate > 1.0
                ? static_cast<float> (std::clamp (
                    (fixedLog2Rate + static_cast<double> (vibratoSemitones) / 12.0) * rateBracketScale,
                    0.0, static_cast<double> (BandLimitedInterpolator::kNumRateBrackets - 1)))
                : 0.0f)
            : fixedPosition;
        // Interior kernels never need edge clamps or loop modulo per tap.
        // Keep the guarded reader for the sample head, tail, and loop seams.
        const auto firstTap = index - BandLimitedInterpolator::kCentreTap;
        const auto lastTap = firstTap + BandLimitedInterpolator::kNumTaps - 1;
        const bool contiguous = firstTap >= 0 && lastTap < sampleCount
            && (!looping || (lastTap < loopEnd && (!hasLoopWrapped_ || firstTap >= loopStart)));
        const float interpolated = contiguous
            ? interpolator.interpolateRate (position, frac,
                [&] (int offset) noexcept { return sampleData[index + offset]; })
            : interpolator.interpolateRate (position, frac,
            [&] (int offset) noexcept
            {
                std::int64_t tapIndex = index + offset;

                if (looping)
                {
                    const std::int64_t loopLength = loopEnd - loopStart;
                    if (loopLength > 0)
                    {
                        if (tapIndex >= loopEnd || (hasLoopWrapped_ && tapIndex < loopStart))
                        {
                            auto relative = (tapIndex - loopStart) % loopLength;
                            if (relative < 0)
                                relative += loopLength;
                            tapIndex = loopStart + relative;
                        }
                    }
                }

                tapIndex = std::clamp<std::int64_t> (tapIndex, 0, sampleCount - 1);
                return sampleData[static_cast<std::size_t>(tapIndex)];
            });

        float sample = interpolated * velocityGain_ * env * attenuationGain_;

        if (filterRouting == 0) // Pre: filter before drive
            sample = filter_.process (sample);

        // Apply nonlinear drive based on curve ID
        const float currentDriveDb = driveIsSmoothing
            ? driveDbSmooth_.getNextValue()
            : steadyDriveDb;
        const float driveBlend = driveIsSmoothing
            ? driveBlendForDb (currentDriveDb)
            : steadyDriveBlend;
        if (driveBlend > 0.0f)
        {
            const float driveGain = driveIsSmoothing
                ? std::pow (10.0f, currentDriveDb / 20.0f)
                : steadyDriveGain;
            sample = drive_.process (sample, curveId, driveBlend, driveGain);
        }

        if (filterRouting != 0) // Post: filter after drive
            sample = filter_.process (sample);

        output[i] += sample;

        phase_ += effectiveRate;
        vibratoPhase_ += vibratoIncrement;
        if (vibratoPhase_ >= 2.0 * juce::MathConstants<double>::pi)
            vibratoPhase_ -= 2.0 * juce::MathConstants<double>::pi;

        // Wrap the loop: once the read position passes loopEnd_, continue from
        // loopStart_ keeping the fractional part, so the interpolation phase is
        // continuous across the wrap and the loop does not click.
        if (looping && phase_ >= static_cast<double>(loopEnd))
        {
            phase_ -= static_cast<double>(loopEnd - loopStart);
            hasLoopWrapped_ = true;
        }

        envPhase_ += 1.0f / static_cast<float>(hostSampleRate);
    }
}

int VoicePool::activeVoiceCount() const noexcept
{
    return static_cast<int> (std::count_if (voices_.begin(), voices_.end(), [] (const Voice& voice)
    {
        return voice.isActive();
    }));
}

int VoicePool::voiceIndexForNote(int midiNote) const noexcept
{
    if (midiNote < 0 || midiNote >= 128)
        return -1;

    const auto limit = std::min (static_cast<std::size_t> (polyphony_), voices_.size());
    for (std::size_t index = 0; index < limit; ++index)
        if (voices_[index].isActive() && voices_[index].note() == midiNote)
            return static_cast<int> (index);
    return -1;
}

void VoicePool::setPolyphony(int numVoices) noexcept
{
    const int newLimit = juce::jlimit (1, static_cast<int>(voices_.size()), numVoices);
    if (newLimit >= polyphony_)
    {
        polyphony_ = newLimit;
        return;
    }

    // Slots outside a newly reduced limit are not rendered. Retire them now
    // so they cannot remain frozen, revive later, or retain MIDI state.
    for (std::size_t i = static_cast<std::size_t>(newLimit); i < voices_.size(); ++i)
    {
        voices_[i].retire();
    }
    if (leadVoiceIndex_ >= newLimit)
        leadVoiceIndex_ = -1;
    polyphony_ = newLimit;
}

void VoicePool::prepare(int maximumExpectedSamplesPerBlock)
{
    if (maximumExpectedSamplesPerBlock <= 0)
    {
        stereoScratch_.clear();
        return;
    }

    stereoScratch_.assign (static_cast<std::size_t> (maximumExpectedSamplesPerBlock), 0.0f);
}

void VoicePool::setSustainHeld(bool held) noexcept
{
    if (sustainHeld_ == held)
        return;
    sustainHeld_ = held;

    if (!sustainHeld_)
    {
        // Pedal released: flush every note whose note-off was deferred while
        // it was held.
        for (int note = 0; note < 128; ++note)
        {
            if (pendingRelease_[static_cast<std::size_t>(note)])
            {
                pendingRelease_[static_cast<std::size_t>(note)] = false;
                releaseNote (note);
            }
        }
    }
}

void VoicePool::setLegatoEnabled(bool enabled) noexcept
{
    legatoEnabled_ = enabled;
}

void VoicePool::startVoice(Voice& voice, const Sample* sample, int midiNote, float velocity,
                           const SF2Loader* sampleOwner) noexcept
{
    voice.start (sample, midiNote, velocity, sampleOwner);
    voice.setStartSequence (nextStartSequence_++);
}

void VoicePool::start(const Sample* sample, int midiNote, float velocity,
                      const SF2Loader* sampleOwner) noexcept
{
    if (sample == nullptr)
        return;

    const std::array<const Sample*, 1> samples { sample };
    start (std::span<const Sample* const> { samples.data(), samples.size() }, midiNote, velocity, sampleOwner);
}

void VoicePool::chokeExclusiveClass(std::uint8_t exclusiveClass) noexcept
{
    if (exclusiveClass == 0)
        return;

    for (std::size_t index = 0; index < voices_.size(); ++index)
    {
        Voice& voice = voices_[index];
        if (voice.isActive() && voice.exclusiveClass() == exclusiveClass)
        {
            // Fade rather than cut: an exclusiveClass choke fires on the new
            // note-on, which is usually the loudest point of the old one.
            voice.choke();
            if (leadVoiceIndex_ == static_cast<int> (index))
                leadVoiceIndex_ = -1;
        }
    }
}

void VoicePool::start(std::span<const Sample* const> samples, int midiNote, float velocity,
                      const SF2Loader* sampleOwner) noexcept
{
    if (midiNote < 0 || midiNote >= 128)
        return;

    std::array<const Sample*, maxLayersPerNote> layers {};
    std::size_t layerCount = 0;
    for (const Sample* sample : samples)
    {
        if (sample == nullptr)
            continue;
        if (layerCount == layers.size())
            break;
        layers[layerCount++] = sample;
    }
    if (layerCount == 0)
        return;

    // SF2 exclusiveClass is a group choke, not a note mapping. Choke a group
    // once before its new layers begin so a closed hi-hat reliably retires an
    // open hi-hat even when the preset uses several overlapping regions.
    std::array<std::uint8_t, maxLayersPerNote> classes {};
    std::size_t classCount = 0;
    for (std::size_t layer = 0; layer < layerCount; ++layer)
    {
        const auto exclusiveClass = layers[layer]->exclusiveClass;
        if (exclusiveClass == 0)
            continue;
        bool seen = false;
        for (std::size_t index = 0; index < classCount; ++index)
            seen = seen || classes[index] == exclusiveClass;
        if (!seen)
            classes[classCount++] = exclusiveClass;
    }
    for (std::size_t index = 0; index < classCount; ++index)
        chokeExclusiveClass (classes[index]);

    // A note-on always clears any pending deferred release for that same
    // note number: if it was released and re-pressed while the pedal was
    // still held, the pedal's earlier note-off must not later steal the
    // fresh attack out from under the new press.
    pendingRelease_[static_cast<std::size_t>(midiNote)] = false;

    const bool alreadyHeld = keyHeld_[static_cast<std::size_t>(midiNote)];
    if (!alreadyHeld)
    {
        keyHeld_[static_cast<std::size_t>(midiNote)] = true;
        ++heldKeyCount_;
    }

    const auto limit = std::min (static_cast<std::size_t> (polyphony_), voices_.size());

    // Legato retargets every layer of the current lead note. A one-layer
    // preset stays byte-for-byte on the previous path, while a stacked SF2
    // layer no longer leaves an old velocity/round-robin sample sounding.
    if (legatoEnabled_ && !alreadyHeld && heldKeyCount_ > 1
        && leadVoiceIndex_ >= 0 && static_cast<std::size_t>(leadVoiceIndex_) < voices_.size()
        && voices_[static_cast<std::size_t>(leadVoiceIndex_)].isActive())
    {
        const int previousNote = voices_[static_cast<std::size_t>(leadVoiceIndex_)].note();
        std::array<int, maxLayersPerNote> existing {};
        std::size_t existingCount = 0;
        for (std::size_t index = 0; index < limit; ++index)
            if (voices_[index].isActive() && voices_[index].note() == previousNote)
            {
                if (existingCount < existing.size())
                    existing[existingCount++] = static_cast<int> (index);
                else
                    voices_[index].retire();
            }

        int newLead = -1;
        for (std::size_t layer = 0; layer < layerCount; ++layer)
        {
            Voice* voice = layer < existingCount
                ? &voices_[static_cast<std::size_t>(existing[layer])]
                : findFreeVoice();
            if (voice == nullptr)
                break;
            if (layer < existingCount)
                voice->retarget (layers[layer], midiNote, sampleOwner);
            else
                startVoice (*voice, layers[layer], midiNote, velocity, sampleOwner);
            if (newLead < 0)
                newLead = static_cast<int> (voice - voices_.data());
        }
        for (std::size_t layer = layerCount; layer < existingCount; ++layer)
            voices_[static_cast<std::size_t>(existing[layer])].retire();
        leadVoiceIndex_ = newLead;
        return;
    }

    // Retrigger every existing layer of the key in place. This keeps repeated
    // presses bounded without collapsing an overlapping zone stack to one
    // voice.
    std::array<int, maxLayersPerNote> existing {};
    std::size_t existingCount = 0;
    for (std::size_t index = 0; index < limit; ++index)
        if (voices_[index].isActive() && voices_[index].note() == midiNote)
        {
            if (existingCount < existing.size())
                existing[existingCount++] = static_cast<int> (index);
            else
                voices_[index].retire();
        }

    int newLead = -1;
    for (std::size_t layer = 0; layer < layerCount; ++layer)
    {
        Voice* voice = layer < existingCount
            ? &voices_[static_cast<std::size_t>(existing[layer])]
            : findFreeVoice();
        if (voice == nullptr)
            break;
        startVoice (*voice, layers[layer], midiNote, velocity, sampleOwner);
        if (newLead < 0)
            newLead = static_cast<int> (voice - voices_.data());
    }
    for (std::size_t layer = layerCount; layer < existingCount; ++layer)
        voices_[static_cast<std::size_t>(existing[layer])].retire();
    leadVoiceIndex_ = newLead;
}

void VoicePool::stop(int midiNote) noexcept
{
    if (midiNote < 0 || midiNote >= 128)
        return;

    if (midiNote >= 0 && midiNote < 128)
    {
        if (keyHeld_[static_cast<std::size_t>(midiNote)])
        {
            keyHeld_[static_cast<std::size_t>(midiNote)] = false;
            if (heldKeyCount_ > 0)
                --heldKeyCount_;
        }
    }

    if (sustainHeld_)
    {
        // Defer the actual release until the pedal comes up; the voice keeps
        // sounding (and, per spec, remains eligible for the note's own
        // eventual release, not a hard cut).
        pendingRelease_[static_cast<std::size_t>(midiNote)] = true;
        return;
    }

    releaseNote (midiNote);
}

void VoicePool::releaseNote(int midiNote) noexcept
{
    if (midiNote < 0 || midiNote >= 128)
        return;

    const auto limit = std::min (static_cast<std::size_t> (polyphony_), voices_.size());
    for (std::size_t index = 0; index < limit; ++index)
        if (voices_[index].isActive() && voices_[index].note() == midiNote)
            voices_[index].stop();
}

void VoicePool::stopAll() noexcept
{
    for (auto& voice : voices_)
        voice.stop();
    keyHeld_.fill (false);
    heldKeyCount_ = 0;
    pendingRelease_.fill (false);
    leadVoiceIndex_ = -1;
}

void VoicePool::setBankTokenForNote(int midiNote, const BankToken& token) noexcept
{
    if (midiNote < 0 || midiNote >= 128)
        return;

    const auto limit = std::min (static_cast<std::size_t> (polyphony_), voices_.size());
    for (std::size_t index = 0; index < limit; ++index)
        if (voices_[index].isActive() && voices_[index].note() == midiNote)
            voices_[index].setBankToken (token);
}

Voice* VoicePool::findFreeVoice() noexcept
{
    const auto limit = std::min (static_cast<std::size_t>(polyphony_), voices_.size());

    // First pass: an entirely idle slot.
    for (std::size_t i = 0; i < limit; ++i)
        if (!voices_[i].isActive())
            return &voices_[i];

    const auto quieterThenOlder = [this] (std::size_t candidate, std::size_t incumbent)
    {
        const Voice& candidateVoice = voices_[candidate];
        const Voice& incumbentVoice = voices_[incumbent];
        const float candidateLevel = candidateVoice.envelopeLevel();
        const float incumbentLevel = incumbentVoice.envelopeLevel();
        return candidateLevel < incumbentLevel
            || (!(incumbentLevel < candidateLevel)
                && candidateVoice.startSequence() < incumbentVoice.startSequence());
    };

    // Second pass: sacrifice the quietest release tail first, breaking equal
    // envelope levels by attack age to keep behavior reproducible.
    std::size_t releaseVictim = limit;
    for (std::size_t i = 0; i < limit; ++i)
        if (voices_[i].isReleasing()
            && (releaseVictim == limit || quieterThenOlder (i, releaseVictim)))
            releaseVictim = i;
    if (releaseVictim != limit)
        return &voices_[releaseVictim];

    // Finally choose the quietest active voice. Prefer an equally quiet
    // unprotected voice over the legato lead or a physically held key, while
    // still allowing a materially quieter protected voice to be selected.
    std::size_t protectedVictim = limit;
    std::size_t unprotectedVictim = limit;
    for (std::size_t i = 0; i < limit; ++i)
    {
        if (!voices_[i].isActive())
            continue;
        std::size_t& victim = isProtectedFromStealing (i) ? protectedVictim : unprotectedVictim;
        if (victim == limit || quieterThenOlder (i, victim))
            victim = i;
    }

    if (unprotectedVictim == limit)
        return protectedVictim == limit ? nullptr : &voices_[protectedVictim];
    if (protectedVictim == limit
        || !(voices_[protectedVictim].envelopeLevel() < voices_[unprotectedVictim].envelopeLevel()))
        return &voices_[unprotectedVictim];
    return &voices_[protectedVictim];
}

bool VoicePool::isProtectedFromStealing(std::size_t voiceIndex) const noexcept
{
    if (static_cast<int>(voiceIndex) == leadVoiceIndex_)
        return true;

    const int note = voices_[voiceIndex].note();
    return note >= 0 && note < 128 && keyHeld_[static_cast<std::size_t>(note)];
}

void VoicePool::render(float* output, int numSamples, int hostSampleRate, float driveDb, float velToDriveDb,
                        int curveId, int filterRouting, float filterOffsetCents,
                        float attackMs, float decayMs, float sustainLevel, float releaseMs,
                        float pitchBendSemitones, float vibratoDepthCents) noexcept
{
    std::fill(output, output + numSamples, 0.0f);

    const auto limit = std::min (static_cast<std::size_t>(polyphony_), voices_.size());
    for (std::size_t i = 0; i < limit; ++i)
        if (voices_[i].isActive())
        {
            voices_[i].render(output, numSamples, hostSampleRate, driveDb, velToDriveDb,
                              curveId, filterRouting, filterOffsetCents,
                              attackMs, decayMs, sustainLevel, releaseMs,
                              pitchBendSemitones, vibratoDepthCents, *interpolator_);
        }
}

VoicePool::StereoGains VoicePool::stereoGainsForVoice (std::size_t voiceIndex, float width,
                                                       float samplePan) noexcept
{
    const float safeWidth = juce::jlimit (0.0f, 1.0f, width);
    const float position = kStereoVoicePositions[voiceIndex % kStereoVoicePositions.size()];
    const float zonePan = juce::jlimit (-1.0f, 1.0f,
                                        std::isfinite (samplePan) ? samplePan : 0.0f);
    // Zone pan claims the image first. The synthetic voice spread fills only
    // the remaining space, so a SoundFont's hard-panned zone remains hard
    // panned while a centred zone preserves the established stereo layout.
    const float pan = juce::jlimit (0.0f, 1.0f,
                                    0.5f + zonePan * 0.5f
                                    + position * safeWidth * (1.0f - std::abs (zonePan)));

    return { kEqualPowerUnityScale * std::sin ((1.0f - pan) * kEqualPowerHalfPi),
             kEqualPowerUnityScale * std::sin (pan * kEqualPowerHalfPi) };
}

void VoicePool::renderStereo(float* outputLeft, float* outputRight, int numSamples,
                             int hostSampleRate, float driveDb, float velToDriveDb,
                             int curveId, int filterRouting, float filterOffsetCents,
                             float attackMs, float decayMs, float sustainLevel, float releaseMs,
                             float pitchBendSemitones, float vibratoDepthCents,
                             float stereoWidth) noexcept
{
    if (outputLeft == nullptr || numSamples <= 0)
        return;

    // A mono host, an unprepared pool, or an unexpectedly oversized host block
    // must remain safe. The fallback deliberately preserves the old dual-mono
    // behaviour instead of allocating in the real-time path. A zero or negative
    // width is not a fallback case: it is a normal render whose every lane
    // collapses to the centre, so the two paths stay continuous at the origin.
    if (outputRight == nullptr || outputRight == outputLeft
        || stereoScratch_.size() < static_cast<std::size_t> (numSamples)
        || ! std::isfinite (stereoWidth))
    {
        render (outputLeft, numSamples, hostSampleRate, driveDb, velToDriveDb,
                curveId, filterRouting, filterOffsetCents,
                attackMs, decayMs, sustainLevel, releaseMs,
                pitchBendSemitones, vibratoDepthCents);
        if (outputRight != nullptr && outputRight != outputLeft)
            juce::FloatVectorOperations::copy (outputRight, outputLeft, numSamples);
        return;
    }

    std::fill (outputLeft, outputLeft + numSamples, 0.0f);
    std::fill (outputRight, outputRight + numSamples, 0.0f);

    const auto limit = std::min (static_cast<std::size_t> (polyphony_), voices_.size());
    for (std::size_t i = 0; i < limit; ++i)
        if (voices_[i].isActive())
        {
            auto* scratch = stereoScratch_.data();
            std::fill (scratch, scratch + numSamples, 0.0f);
            voices_[i].render (scratch, numSamples, hostSampleRate, driveDb, velToDriveDb,
                               curveId, filterRouting, filterOffsetCents,
                               attackMs, decayMs, sustainLevel, releaseMs,
                               pitchBendSemitones, vibratoDepthCents, *interpolator_);

            const auto gains = stereoGainsForVoice (i, stereoWidth, voices_[i].samplePan());
            juce::FloatVectorOperations::addWithMultiply (outputLeft, scratch, gains.left, numSamples);
            juce::FloatVectorOperations::addWithMultiply (outputRight, scratch, gains.right, numSamples);
        }
}

} // namespace aod
