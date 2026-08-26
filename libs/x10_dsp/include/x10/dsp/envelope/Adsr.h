#pragma once

#include <algorithm>
#include <cmath>

namespace x10::dsp
{

/**
    Per-sample ADSR envelope with linear segments.

    Call tick() once per sample to advance and obtain the current level [0..1].
    Set parameters in seconds via the setters; they are latched immediately and
    applied on the next tick. The envelope does not depend on sample rate at
    parameter-set time: tick() internally converts seconds to a per-sample
    increment from the rate set by prepare().

    Lifecycle:
        reset()       → Idle (level 0)
        noteOn()      → Attack (ramp from current level toward 1.0)
        tick() × N    → Attack → Decay → Sustain
        noteOff()     → Release (ramp from current level toward 0)
        tick() × N    → Release → Idle (level 0, isActive() == false)
        tick()        → returns 0.0 while Idle (no-op)

    Calling noteOn() while in Attack or Sustain re-arms from the current level
    (legato retrigger). Calling noteOff() while already in Release is a no-op.
*/
class Adsr
{
public:
    enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };

    /** Reset to Idle with zero level. Safe to call at any time. */
    void reset() noexcept
    {
        stage_ = Stage::Idle;
        level_ = 0.0f;
        increment_ = 0.0f;
        target_ = 0.0f;
    }

    /** Begin the Attack phase from the current level. */
    void noteOn() noexcept
    {
        stage_ = Stage::Attack;
        target_ = 1.0f;
        computeIncrement();
    }

    /** Transition to Release from the current level. No-op if already Idle or in Release. */
    void noteOff() noexcept
    {
        if (stage_ == Stage::Idle || stage_ == Stage::Release)
            return;
        stage_ = Stage::Release;
        target_ = 0.0f;
        computeIncrement();
    }

    /**
        Advance one sample and return the current level.
        While Idle, returns 0.0 and does nothing.
    */
    [[nodiscard]] float tick() noexcept
    {
        if (stage_ == Stage::Idle)
            return 0.0f;

        level_ += increment_;

        switch (stage_)
        {
            case Stage::Attack:
                if (level_ >= 1.0f)
                {
                    level_ = 1.0f;
                    stage_ = Stage::Decay;
                    computeIncrement();
                }
                break;

            case Stage::Decay:
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

    void setAttackSec (float sec) noexcept
    {
        attackSec_ = std::max (sec, 0.0f);
        if (stage_ == Stage::Attack)
            computeIncrement();
    }

    void setDecaySec (float sec) noexcept
    {
        decaySec_ = std::max (sec, 0.0f);
        if (stage_ == Stage::Decay)
            computeIncrement();
    }

    void setSustainLevel (float level) noexcept
    {
        sustainLevel_ = std::clamp (level, 0.0f, 1.0f);
        if (stage_ == Stage::Decay || stage_ == Stage::Sustain)
            computeIncrement();
    }

    void setReleaseSec (float sec) noexcept
    {
        releaseSec_ = std::max (sec, 0.0f);
        if (stage_ == Stage::Release)
            computeIncrement();
    }

    /** Must be called once before the first tick with the current sample rate. */
    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = std::max (sampleRate, 1.0);
        computeIncrement();
    }

    [[nodiscard]] Stage stage() const noexcept { return stage_; }
    [[nodiscard]] bool isActive() const noexcept { return stage_ != Stage::Idle; }
    [[nodiscard]] float currentLevel() const noexcept { return level_; }

private:
    /**
        Compute the per-sample increment for the current stage.
        If the stage duration is zero, the level jumps immediately to the target.
    */
    void computeIncrement() noexcept
    {
        float durationSec = 0.0f;
        switch (stage_)
        {
            case Stage::Attack:  durationSec = attackSec_;  break;
            case Stage::Decay:   durationSec = decaySec_;   break;
            case Stage::Release: durationSec = releaseSec_;  break;
            case Stage::Idle:
            case Stage::Sustain: durationSec = 0.0f;         break;
        }

        if (durationSec <= 0.0f)
        {
            level_ = (stage_ == Stage::Attack)  ? 1.0f
                   : (stage_ == Stage::Decay)   ? sustainLevel_
                   : 0.0f;
            stage_ = (stage_ == Stage::Attack) ? Stage::Decay
                   : (stage_ == Stage::Decay)  ? Stage::Sustain
                   : Stage::Idle;
            increment_ = 0.0f;
            if (stage_ == Stage::Decay)
                computeIncrement();
            return;
        }

        const float samples = static_cast<float> (sampleRate_ * durationSec);
        const float targetVal = (stage_ == Stage::Attack) ? 1.0f
                              : (stage_ == Stage::Decay)  ? sustainLevel_
                              : 0.0f;
        increment_ = (targetVal - level_) / samples;
    }

    double sampleRate_ = 48000.0;
    Stage  stage_      = Stage::Idle;
    float  level_      = 0.0f;
    float  increment_  = 0.0f;
    float  target_     = 0.0f;

    float  attackSec_   = 0.01f;
    float  decaySec_    = 0.3f;
    float  sustainLevel_ = 0.7f;
    float  releaseSec_  = 0.08f;
};

} // namespace x10::dsp