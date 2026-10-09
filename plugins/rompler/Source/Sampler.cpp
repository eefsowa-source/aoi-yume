#include "Sampler.h"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace aod
{

void Voice::start(const Sample* sample, int midiNote, float velocity) noexcept
{
    sample_ = sample;
    velocity_ = velocity;
    midiNote_ = midiNote;
    phase_ = 0.0;
    envPhase_ = 0.0f;
    active_ = true;
    filterNeedsPrepare_ = true;

    // Copy loop points at start(): render() must not read through a sample
    // pointer that may belong to a retired loader once this voice is retriggered
    // against a newer one. Keeping the loop state here makes the audio thread
    // self-contained for the voice's lifetime.
    loopStart_ = sample->loopStart;
    loopEnd_   = sample->loopEnd;
    loopEnabled_ = sample->loopEnabled && loopEnd_ > loopStart_ + 1;

    // Pitch: the sample is recorded at rootKey. A note played N semitones above
    // rootKey must advance N semitones faster (pitch ratio 2^(N/12)); the
    // region's per-key scale (usually 100 cents/key) and constant tune offset
    // are folded in so a scale of 0 pins every note to the root pitch.
    const double semitones = static_cast<double> (midiNote - sample->rootKey)
        * static_cast<double> (sample->scaleTuningCentsPerKey) / 100.0
        + static_cast<double> (sample->tuneCents) / 100.0;
    playRate_ = std::pow (2.0, semitones / 12.0);

    adsr_.noteOn();
}

void Voice::stop() noexcept
{
    if (!active_)
        return;
    adsr_.noteOff();
}

bool Voice::isReleasing() const noexcept
{
    return active_ && adsr_.stage() == x10::dsp::Adsr::Stage::Release;
}

void Voice::render(float* output, int numSamples, int hostSampleRate, float driveDb, float velToDriveDb,
                    int curveId, int filterRouting, float filterOffsetCents,
                    float attackMs, float decayMs, float sustainLevel, float releaseMs) noexcept
{
    if (!active_ || sample_ == nullptr || sample_->data.empty())
        return;

    if (filterNeedsPrepare_ || filterSampleRate_ != hostSampleRate)
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

    // Push ADSR parameters only on change: the setters recompute the current
    // stage's ramp even for identical values, which would restart a Decay or
    // Release fade from the current level every block.
    const float envParams[] = { attackMs, decayMs, sustainLevel, releaseMs };
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
        adsr_.setSustainLevel (sustainLevel);
        adsr_.setReleaseSec (releaseMs * 0.001f);
    }

    const float cutoffHz = std::clamp (
        sample_->filterCutoffHz * std::pow (2.0f, filterOffsetCents / 1200.0f),
        20.0f, static_cast<float> (hostSampleRate) * 0.49f);
    const float q = std::pow (10.0f, sample_->filterResonanceDb / 20.0f) * 0.7071068f;
    filter_.setCutoff (cutoffHz, q);

    const float* sampleData = sample_->data.data();
    const auto sampleCount = static_cast<std::int64_t>(sample_->data.size());

    // Velocity shapes the drive amount: velToDriveDb at 0% is neutral, +100%
    // makes hard hits drive harder and -100% does the inverse. This is an
    // additional dB offset centred so a velocity of 127 (1.0) is the reference.
    const float velDriveDb = driveDb + velToDriveDb * (velocity_ - 1.0f);
    const float driveGain = std::pow (10.0f, velDriveDb / 20.0f);

    // Loop points as sample-frame indices into sampleData. While looping, phase_
    // wraps from loopEnd_ back to loopStart_ so sustained notes never run off
    // the end of the sample; during Release the loop is ignored and the tail
    // plays out so the ADSR release has real data to fade.
    const bool inRelease = adsr_.stage() == x10::dsp::Adsr::Stage::Release;
    const bool looping = loopEnabled_ && !inRelease;
    const auto loopStart = static_cast<std::int64_t>(loopStart_);
    const auto loopEnd = static_cast<std::int64_t>(loopEnd_);

    for (int i = 0; i < numSamples; ++i)
    {
        const float env = adsr_.tick();
        if (env <= 0.0f && !adsr_.isActive())
        {
            active_ = false;
            break;
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
                    break;
                }
            }
        }

        const auto index = static_cast<std::int64_t>(phase_);
        const float frac = static_cast<float>(phase_ - static_cast<double>(index));

        // 4-point Lagrange interpolation. Linear interpolation images badly
        // above ~+7 semitones; the extra two taps cost little and stay exact
        // at integer phases. Taps inside the loop wrap so a sustained note
        // reads continuous data; taps before the loop and past the end clamp.
        const auto tapIndex = [&] (std::int64_t tap) noexcept
        {
            if (looping && tap >= loopEnd)
                tap = loopStart + (tap - loopEnd);
            return static_cast<std::size_t> (std::clamp (tap, std::int64_t { 0 }, sampleCount - 1));
        };
        const float sm1 = sampleData[tapIndex (index - 1)];
        const float s0  = sampleData[tapIndex (index)];
        const float s1  = sampleData[tapIndex (index + 1)];
        const float s2  = sampleData[tapIndex (index + 2)];
        const float interpolated =
            s0 + 0.5f * frac * (s1 - sm1
                + frac * (2.0f * sm1 - 5.0f * s0 + 4.0f * s1 - s2
                + frac * (3.0f * (s0 - s1) + s2 - sm1)));

        float sample = interpolated * velocity_ * env;

        if (filterRouting == 0) // Pre: filter before drive
            sample = filter_.process (sample);

        // Apply nonlinear drive based on curve ID
        const float driven = driveGain * sample;
        if (curveId == 1)
            sample = x10::dsp::curves::Tube::f (driven) / driveGain;
        else if (curveId == 2)
            sample = x10::dsp::curves::Transformer::f (driven) / driveGain;
        else // curveId == 0 or default
            sample = x10::dsp::curves::Tanh::f (driven) / driveGain;

        if (filterRouting != 0) // Post: filter after drive
            sample = filter_.process (sample);

        output[i] += sample;

        phase_ += playRate_;

        // Wrap the loop: once the read position passes loopEnd_, continue from
        // loopStart_ keeping the fractional part, so the interpolation phase is
        // continuous across the wrap and the loop does not click.
        if (looping && phase_ >= static_cast<double>(loopEnd))
            phase_ -= static_cast<double>(loopEnd - loopStart);

        envPhase_ += 1.0f / static_cast<float>(hostSampleRate);
    }
}

void VoicePool::setPolyphony(int numVoices) noexcept
{
    polyphony_ = juce::jlimit (1, static_cast<int>(voices_.size()), numVoices);
}

void VoicePool::start(std::span<const Sample*> samples, int midiNote, float velocity) noexcept
{
    if (midiNote < 0 || midiNote >= 128 || samples.empty())
        return;

    // Drop null layers up front so a mid-span hole cannot strand a reusable
    // voice ringing a stale sample.
    std::array<const Sample*, maxVoices> layers {};
    std::size_t layerCount = 0;
    for (auto* s : samples)
        if (s != nullptr && layerCount < maxVoices)
            layers[layerCount++] = s;
    if (layerCount == 0)
        return;

    // Retrigger: voices already sounding this note are reused first, so
    // re-pressing a layered note replaces its own voices instead of stacking
    // a new set underneath the still-ringing old one.
    std::array<Voice*, maxVoices> reusable {};
    std::size_t reusableCount = 0;
    for (auto& voice : voices_)
        if (voice.isActive() && voice.note() == midiNote && reusableCount < maxVoices)
            reusable[reusableCount++] = &voice;

    // A note-off scan releases every voice of the note, so surplus reused
    // voices from a wider old layer set must be released explicitly.
    for (std::size_t i = layerCount; i < reusableCount; ++i)
        reusable[i]->stop();

    for (std::size_t layer = 0; layer < layerCount; ++layer)
    {
        Voice* voice = layer < reusableCount ? reusable[layer] : findFreeVoice();
        if (voice == nullptr)
            return;
        voice->start (layers[layer], midiNote, velocity);
    }
}

void VoicePool::stop(int midiNote) noexcept
{
    if (midiNote < 0 || midiNote >= 128)
        return;

    // Every voice matching the note releases: velocity layers each own a
    // voice, and recycled slots keep their current note so a stale index can
    // never release the wrong sound.
    for (auto& voice : voices_)
        if (voice.note() == midiNote)
            voice.stop();
}

void VoicePool::stopAll() noexcept
{
    for (auto& voice : voices_)
        voice.stop();
}

Voice* VoicePool::findFreeVoice() noexcept
{
    const auto limit = std::min (static_cast<std::size_t>(polyphony_), voices_.size());

    // First pass: an entirely idle slot.
    for (std::size_t i = 0; i < limit; ++i)
        if (!voices_[i].isActive())
            return &voices_[i];

    // Second pass: a slot still rendering its release tail. Reallocating it is
    // preferable to silently dropping the new note, and re-triggering merely
    // overrides the fade with the fresh attack.
    for (std::size_t i = 0; i < limit; ++i)
        if (voices_[i].isReleasing())
            return &voices_[i];

    // Third pass: the whole pool is busy with sustained notes. Steal the
    // *oldest* active voice so the new note is never silently dropped; the
    // oldest has decayed the furthest, so it is the least audible victim.
    // (Voice::start() rewrites all state, so the steal is click-free apart
    // from the natural note cut.)
    std::size_t oldest = 0;
    float oldestPhase = -1.0f;
    for (std::size_t i = 0; i < limit; ++i)
        if (voices_[i].isActive() && voices_[i].envPhase() > oldestPhase)
        {
            oldest = i;
            oldestPhase = voices_[i].envPhase();
        }
    return &voices_[oldest];
}

void VoicePool::render(float* output, int numSamples, int hostSampleRate, float driveDb, float velToDriveDb,
                        int curveId, int filterRouting, float filterOffsetCents,
                        float attackMs, float decayMs, float sustainLevel, float releaseMs) noexcept
{
    std::fill(output, output + numSamples, 0.0f);

    const auto limit = std::min (static_cast<std::size_t>(polyphony_), voices_.size());
    for (std::size_t i = 0; i < limit; ++i)
        if (voices_[i].isActive())
            voices_[i].render(output, numSamples, hostSampleRate, driveDb, velToDriveDb,
                              curveId, filterRouting, filterOffsetCents,
                              attackMs, decayMs, sustainLevel, releaseMs);
}

} // namespace aod
