#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace x10::dsp
{

/**
    Per-sample DAHDSR envelope with linear or exponential segments.

    Call tick() once per sample to advance and obtain the current level [0..1].
    Set parameters in seconds via the setters; they are latched immediately and
    applied on the next tick. The envelope does not depend on sample rate at
    parameter-set time: tick() internally converts seconds to per-sample
    increments or countdowns using the rate set by prepare().

    Lifecycle:
        reset()       → Idle (level 0)
        noteOn()      → Delay (if > 0) → Attack (ramp from current level to 1)
        tick() × N    → Attack → Hold (if > 0) → Decay → Sustain
        noteOff()     → Release (ramp from current level toward 0)
        tick() × N    → Release → Idle (level 0, isActive() == false)
        tick()        → returns 0.0 while Idle (no-op)

    Calling noteOn() while in any active stage re-arms from the current level
    (legato retrigger). Calling noteOff() while already in Release or Idle is a
    no-op.

    The two extra stages (Delay before Attack, Hold after Attack) stay zero for
    a plain ADSR and cost nothing. They exist because SoundFont volume
    envelopes are DAHDSR, so per-zone envelopes cannot be expressed otherwise.

    Tail curve: with setExponentialTails(false) every ramp is linear, matching
    the original four-stage behaviour exactly. With true, Decay and Release
    become exponential in amplitude - linear in dB, the curve SoundFont volume
    envelopes are specified with. An exponential stage is still bounded by its
    declared duration: at the end of the segment the level snaps the remaining
    sub-perceptual distance to the target, so Release still reaches Idle in
    exactly releaseSeconds.
*/
class Adsr
{
public:
    enum class Stage : uint8_t { Idle, Delay, Attack, Hold, Decay, Sustain, Release };

    /** Reset to Idle with zero level. Safe to call at any time. */
    void reset() noexcept
    {
        stage_ = Stage::Idle;
        level_ = 0.0f;
        increment_ = 0.0f;
        stageRemaining_ = 0;
    }

    /** Begin the sequence from the current level: Delay (if any) then Attack. */
    void noteOn() noexcept
    {
        stage_ = Stage::Delay;
        enterStage();
    }

    /** Transition to Release from the current level. No-op if already Idle or in Release. */
    void noteOff() noexcept
    {
        if (stage_ == Stage::Idle || stage_ == Stage::Release)
            return;
        stage_ = Stage::Release;
        enterStage();
    }

    /**
        Advance one sample and return the current level.
        While Idle, returns 0.0 and does nothing.
    */
    [[nodiscard]] float tick() noexcept
    {
        if (stage_ == Stage::Idle)
            return 0.0f;

        switch (stage_)
        {
            case Stage::Delay:
            case Stage::Hold:
                if (--stageRemaining_ <= 0)
                {
                    stage_ = (stage_ == Stage::Delay) ? Stage::Attack : Stage::Decay;
                    enterStage();
                }
                break;

            case Stage::Attack:
                level_ += increment_;
                if (level_ >= 1.0f)
                {
                    level_ = 1.0f;
                    stage_ = Stage::Hold;
                    enterStage();
                }
                break;

            case Stage::Decay:
                if (exponentialTails_)
                {
                    level_ *= expCoeff_;
                    if (--stageRemaining_ <= 0)
                    {
                        level_ = sustainLevel_;
                        stage_ = Stage::Sustain;
                        increment_ = 0.0f;
                    }
                    break;
                }
                level_ += increment_;
                if ((increment_ < 0.0f && level_ <= sustainLevel_)
                    || (increment_ > 0.0f && level_ >= sustainLevel_))
                {
                    level_ = sustainLevel_;
                    stage_ = Stage::Sustain;
                    increment_ = 0.0f;
                }
                break;

            case Stage::Sustain:
                // Level is held at sustainLevel_ until noteOff().
                break;

            case Stage::Release:
                if (exponentialTails_)
                {
                    level_ *= expCoeff_;
                    if (--stageRemaining_ <= 0)
                    {
                        level_ = 0.0f;
                        stage_ = Stage::Idle;
                        increment_ = 0.0f;
                    }
                    break;
                }
                level_ += increment_;
                if ((increment_ < 0.0f && level_ <= 0.0f)
                    || (increment_ > 0.0f && level_ >= 0.0f))
                {
                    level_ = 0.0f;
                    stage_ = Stage::Idle;
                    increment_ = 0.0f;
                }
                break;

            case Stage::Idle:
                break;
        }

        return level_;
    }

    void setDelaySec (float sec) noexcept
    {
        delaySec_ = std::max (sec, 0.0f);
        if (stage_ == Stage::Delay)
            configureStage();
    }

    void setAttackSec (float sec) noexcept
    {
        attackSec_ = std::max (sec, 0.0f);
        if (stage_ == Stage::Attack)
            configureStage();
    }

    void setHoldSec (float sec) noexcept
    {
        holdSec_ = std::max (sec, 0.0f);
        if (stage_ == Stage::Hold)
            configureStage();
    }

    void setDecaySec (float sec) noexcept
    {
        decaySec_ = std::max (sec, 0.0f);
        if (stage_ == Stage::Decay)
            configureStage();
    }

    void setSustainLevel (float level) noexcept
    {
        sustainLevel_ = std::clamp (level, 0.0f, 1.0f);
        if (stage_ == Stage::Decay || stage_ == Stage::Sustain)
            configureStage();
    }

    void setReleaseSec (float sec) noexcept
    {
        releaseSec_ = std::max (sec, 0.0f);
        if (stage_ == Stage::Release)
            configureStage();
    }

    /**
        Switches Decay and Release between linear ramps and exponential
        (amplitude-domain, i.e. linear-in-dB) curves. Attack and the timed
        Delay/Hold stages are unaffected. Mid-stage changes re-time the curve
        the same way the time setters do.
    */
    void setExponentialTails (bool enabled) noexcept
    {
        if (exponentialTails_ == enabled)
            return;
        exponentialTails_ = enabled;
        if (stage_ == Stage::Decay || stage_ == Stage::Release)
            configureStage();
    }

    [[nodiscard]] bool exponentialTails() const noexcept { return exponentialTails_; }

    /** Must be called once before the first tick with the current sample rate. */
    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = std::max (sampleRate, 1.0);
        configureStage();
    }

    [[nodiscard]] Stage stage() const noexcept { return stage_; }
    [[nodiscard]] bool isActive() const noexcept { return stage_ != Stage::Idle; }
    [[nodiscard]] float currentLevel() const noexcept { return level_; }

private:
    // Amplitude floor used as the geometric end point when an exponential
    // stage decays toward silence. -80 dB is below audibility and the stage
    // snaps the last step to the exact target anyway.
    static constexpr float kSilenceLevel = 1.0e-4f;

    /**
        Configures the per-sample mechanism for the current stage, then loops
        to consume zero-length stages in one call, so e.g. a zero Delay falls
        straight through to Attack.

        Linear stages keep the historic increment scheme, where a mid-stage
        parameter change re-times the ramp over the full duration from the
        current level (deliberately stretching rather than restarting). Delay
        and Hold count samples down while the level stays put. Exponential
        stages multiply the level toward the target by a per-sample
        coefficient and count samples to the hard end point.
    */
    void enterStage() noexcept
    {
        for (;;)
        {
            const float durationSec =
                (stage_ == Stage::Delay)   ? delaySec_
              : (stage_ == Stage::Attack)  ? attackSec_
              : (stage_ == Stage::Hold)    ? holdSec_
              : (stage_ == Stage::Decay)   ? decaySec_
              : (stage_ == Stage::Release) ? releaseSec_
                                           : 0.0f;

            const bool linearRamp = stage_ == Stage::Attack
                || (stage_ == Stage::Decay && ! exponentialTails_)
                || (stage_ == Stage::Release && ! exponentialTails_);
            const bool exponential = (stage_ == Stage::Decay || stage_ == Stage::Release)
                                     && exponentialTails_;

            if (stage_ == Stage::Idle || stage_ == Stage::Sustain)
                return; // These stages hold the current level until re-armed.

            if (durationSec <= 0.0f)
            {
                // Zero-length stage: adopt the end level and move on.
                switch (stage_)
                {
                    case Stage::Delay:   stage_ = Stage::Attack;  continue;
                    case Stage::Attack:  level_ = 1.0f;          stage_ = Stage::Hold;    continue;
                    case Stage::Hold:    stage_ = Stage::Decay;  continue;
                    case Stage::Decay:   level_ = sustainLevel_; stage_ = Stage::Sustain; return;
                    case Stage::Release: level_ = 0.0f;          stage_ = Stage::Idle;    return;
                    // These are handled before the duration calculation, but
                    // spelling them out keeps strict switch-enum builds
                    // exhaustive when the compiler cannot prove that path.
                    case Stage::Idle:
                    case Stage::Sustain:
                        return;
                }
            }

            stageRemaining_ = static_cast<std::int64_t> (std::max (
                sampleRate_ * static_cast<double> (durationSec), 1.0));

            if (linearRamp)
            {
                const float targetVal = (stage_ == Stage::Attack) ? 1.0f
                                      : (stage_ == Stage::Decay)  ? sustainLevel_
                                      : 0.0f;
                increment_ = (targetVal - level_)
                             / static_cast<float> (stageRemaining_);
            }
            else if (exponential)
            {
                const float target = (stage_ == Stage::Decay) ? sustainLevel_ : 0.0f;
                increment_ = 0.0f;
                const float from = std::max (level_, kSilenceLevel);
                const float to   = std::max (target, kSilenceLevel);
                if (from <= to && target <= level_)
                {
                    // Nothing to decay toward (silent or already at/below a
                    // silent target): adopt the target immediately.
                    level_ = target;
                    stage_ = (stage_ == Stage::Decay) ? Stage::Sustain : Stage::Idle;
                    return;
                }
                expCoeff_ = static_cast<float> (
                    std::exp (std::log (static_cast<double> (to) / static_cast<double> (from))
                              / static_cast<double> (stageRemaining_)));
            }
            return;
        }
    }

    /**
        Re-time the current stage after a parameter change, preserving the
        historic behaviour of stretching the segment over the full duration
        from the current level rather than restarting it.
    */
    void configureStage() noexcept
    {
        enterStage();
    }

    double sampleRate_ = 48000.0;
    Stage  stage_      = Stage::Idle;
    float  level_      = 0.0f;
    float  increment_  = 0.0f;

    std::int64_t stageRemaining_ = 0;
    float  expCoeff_        = 1.0f;
    bool   exponentialTails_ = false;

    float  delaySec_     = 0.0f;
    float  attackSec_    = 0.01f;
    float  holdSec_      = 0.0f;
    float  decaySec_     = 0.3f;
    float  sustainLevel_ = 0.7f;
    float  releaseSec_   = 0.08f;
};

} // namespace x10::dsp
