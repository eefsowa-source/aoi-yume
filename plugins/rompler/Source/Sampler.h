#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <x10/instrument/RegionIndex.h>
#include <x10/dsp/nonlinear/Curves.h>
#include <x10/dsp/filter/TptSvf.h>
#include <x10/dsp/envelope/Adsr.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace aod
{

struct Sample
{
    std::vector<float> data;
    int sampleRate = 48000;
    int loopStart = 0;
    int loopEnd = 0;
    bool loopEnabled = false;
    float filterCutoffHz = 19912.13f;
    float filterResonanceDb = 0.0f;
    // Pitch mapping: the sample plays back untransposed when the played MIDI
    // note equals rootKey. tuneCents is a constant offset; scaleTuningCentsPerKey
    // is the per-key pitch step (100 = normal chromatic, 0 = pinned to rootKey).
    float rootKey = 60.0f;
    float tuneCents = 0.0f;
    float scaleTuningCentsPerKey = 100.0f;
};

class Voice
{
public:
    void start(const Sample* sample, int midiNote, float velocity) noexcept;
    /** Begins the release phase; the voice deactivates once the ADSR fades to zero. */
    void stop() noexcept;
    [[nodiscard]] bool isActive() const noexcept { return active_; }
    /** True while in the Release stage, before the slot is retired. */
    [[nodiscard]] bool isReleasing() const noexcept;
    /** The note this voice is currently sounding (or -1 once it has no note). */
    [[nodiscard]] int note() const noexcept { return midiNote_; }
    /** How far the envelope has run; used to pick the oldest voice when stealing. */
    [[nodiscard]] float envPhase() const noexcept { return envPhase_; }

    void render(float* output, int numSamples, int hostSampleRate, float driveDb, float velToDriveDb,
                int curveId, int filterRouting, float filterOffsetCents,
                float attackMs, float decayMs, float sustainLevel, float releaseMs) noexcept;

private:
    const Sample* sample_ = nullptr;
    double phase_ = 0.0;
    float velocity_ = 0.0f;
    bool active_ = false;
    int midiNote_ = -1;
    float envPhase_ = 0.0f;

    x10::dsp::Adsr adsr_;
    // Bit-pattern hash of the last pushed envelope parameter block; see render()
    // for why we must not re-push identical values every block.
    std::uint32_t envParamHash_ = 0;

    // Loop state: while looping (not in Release), phase_ wraps back to
    // loopStart_ once it passes loopEnd_. Cleared by start() so a retriggered
    // voice always begins from the sample head.
    int loopStart_ = 0;
    int loopEnd_ = 0;
    bool loopEnabled_ = false;

    // Playback rate in source frames per output sample. 1.0 plays the sample at
    // its recorded pitch; a higher MIDI note advances faster, a lower one
    // slower. Computed once at start() from the note, rootKey and tunings.
    double playRate_ = 1.0;

    x10::dsp::TptSvf filter_;
    bool filterNeedsPrepare_ = true;
    int filterSampleRate_ = 0;
};

class VoicePool
{
public:
    static constexpr int maxVoices = 32;

    explicit VoicePool(int numVoices = maxVoices) : voices_(static_cast<std::size_t>(numVoices)) {}

    /** Caps the number of concurrently playing voices. Call from the audio thread. */
    void setPolyphony(int numVoices) noexcept;

    void start(const Sample* sample, int midiNote, float velocity) noexcept;
    void stop(int midiNote) noexcept;
    void stopAll() noexcept;

    void render(float* output, int numSamples, int hostSampleRate, float driveDb, float velToDriveDb,
                int curveId, int filterRouting, float filterOffsetCents,
                float attackMs, float decayMs, float sustainLevel, float releaseMs) noexcept;

private:
    std::vector<Voice> voices_;
    std::array<int, 128> noteToVoice_ {};
    int polyphony_ = static_cast<int>(voices_.size());

    [[nodiscard]] Voice* findFreeVoice() noexcept;
};

} // namespace aod
