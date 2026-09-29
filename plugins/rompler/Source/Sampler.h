#pragma once

#include "BandLimitedInterpolator.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <x10/instrument/RegionIndex.h>
#include <x10/dsp/nonlinear/Curves.h>
#include <x10/dsp/nonlinear/Adaa1.h>
#include <x10/dsp/filter/TptSvf.h>
#include <x10/dsp/envelope/Adsr.h>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace aod
{
class SF2Loader;

/**
    Lightweight bank-generation token passed by value to Voice.
    Allows deferred cleanup of retired SoundFont instances without
    holding reference-counted pointers in the audio thread.
*/
struct BankToken
{
    int bankSlot = 0;
    std::uint32_t generation = 0;
    std::uint64_t bankId = 0;
    
    [[nodiscard]] bool operator==(const BankToken& other) const noexcept
    {
        return bankSlot == other.bankSlot 
            && generation == other.generation
            && bankId == other.bankId;
    }
};


struct Sample
{
    std::vector<float> data;
    int sampleRate = 48000;
    int loopStart = 0;
    int loopEnd = 0;
    bool loopEnabled = false;
    // SoundFont amplitude generators are resolved at load time.  Keeping the
    // region data next to the decoded PCM lets the audio thread apply it using
    // only the Sample pointer it already owns.
    x10::instrument::Envelope volumeEnvelope {};
    float attenuationDb = 0.0f;
    float pan = 0.0f;
    std::uint8_t exclusiveClass = 0;
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
    /**
        Per-voice nonlinear drive with first-order antiderivative antialiasing.

        A separate type rather than an inline formula so the alias gate can
        measure the stage the audio path actually uses instead of a copy of it.
        Each curve keeps its own ADAA state, because a cached antiderivative value
        is only meaningful to the curve that produced it; entering another curve
        with stale state would be a mismatched F1 and can spike.

        Cost is one antiderivative evaluation per sample where the previous code
        evaluated the curve itself. logCosh is two library calls, so the CPU
        matrix is recorded next to this change rather than assumed.
    */
    class Drive
    {
    public:
        /**
            Selects antiderivative antialiasing.

            Off by default because it changes the sound of every patch with
            DRIVE above zero: first-order ADAA evaluates a two-point average in
            the small-signal limit, which measured 11.7 dB down at 20 kHz. The
            alias gain and that cost are both pinned by tests, so enabling this
            is a listening decision rather than a hidden default change.
        */
        void setAntialiasing (bool shouldAntialias) noexcept { antialias_ = shouldAntialias; }
        [[nodiscard]] bool antialiasing() const noexcept { return antialias_; }

        /** @returns @p x blended with the driven signal by @p blend (0 = dry). */
        [[nodiscard]] float process (float x, int curveId, float blend, float gain) noexcept;
        void reset() noexcept;

    private:
        [[nodiscard]] float curveValue (float driven, int curveId) noexcept;
        [[nodiscard]] static float directValue (float driven, int curveId) noexcept;

        x10::dsp::Adaa1<x10::dsp::curves::Tanh> tanh_;
        x10::dsp::Adaa1<x10::dsp::curves::Tube> tube_;
        x10::dsp::Adaa1<x10::dsp::curves::Transformer> transformer_;
        int lastCurveId_ = -1;
        bool antialias_ = false;
    };

    ~Voice();
    void start(const Sample* sample, int midiNote, float velocity,
               const SF2Loader* sampleOwner = nullptr) noexcept;
    /**
        Legato retarget: changes the sounding note/pitch of an already-active
        voice without resetting the envelope, phase or loop state. Used when
        CC65 (legato) is held and a new key is pressed while another is still
        down, so the pitch glides on the same voice instead of a fresh attack.
        No-op if the voice is not currently active.
    */
    void retarget(const Sample* sample, int midiNote,
                  const SF2Loader* sampleOwner = nullptr) noexcept;
    /**
        SoundFont exclusiveClass choke: fades this voice out quickly instead of
        cutting it, so the retrigger cannot click. No-op when not active.
    */
    void choke() noexcept;
    /** Immediately retires the slot and clears its current note ownership. */
    void retire() noexcept;
    /** Begins the release phase; the voice deactivates once the ADSR fades to zero. */
    void stop() noexcept;
    [[nodiscard]] bool isActive() const noexcept { return active_; }
    /** True while in the Release stage, before the slot is retired. */
    [[nodiscard]] bool isReleasing() const noexcept;
    /** The note this voice is currently sounding (or -1 once it has no note). */
    [[nodiscard]] int note() const noexcept { return midiNote_; }
    /** How far the envelope has run; used to pick the oldest voice when stealing. */
    [[nodiscard]] float envPhase() const noexcept { return envPhase_; }
    /** Current ADSR level, updated per rendered sample for deterministic stealing. */
    [[nodiscard]] float envelopeLevel() const noexcept { return envelopeLevel_; }
    /** SoundFont pan for this zone, in the normalized [-1, 1] convention. */
    [[nodiscard]] float samplePan() const noexcept { return sample_ != nullptr ? sample_->pan : 0.0f; }
    [[nodiscard]] std::uint8_t exclusiveClass() const noexcept
    {
        return sample_ != nullptr ? sample_->exclusiveClass : 0;
    }
    /** Monotonic sequence assigned by VoicePool on every fresh attack. */
    [[nodiscard]] std::uint64_t startSequence() const noexcept { return startSequence_; }
    void setStartSequence(std::uint64_t sequence) noexcept { startSequence_ = sequence; }
    
    /** Bank token at the time voice was started; used for deferred cleanup. */
    [[nodiscard]] BankToken bankToken() const noexcept { return bankToken_; }
    void setBankToken(const BankToken& token) noexcept { bankToken_ = token; }

    /** sustainLevel is normalized: 0.0 is silence and 1.0 is unity gain. */
    void render(float* output, int numSamples, int hostSampleRate, float driveDb, float velToDriveDb,
                int curveId, int filterRouting, float filterOffsetCents,
                float attackMs, float decayMs, float sustainLevel, float releaseMs,
                float pitchBendSemitones, float vibratoDepthCents,
                const BandLimitedInterpolator& interpolator) noexcept;

private:
    /**
        Per-zone amplitude shape from the SoundFont volume envelope.

        This covers delay/attack/hold/decay/sustain only. The zone's release
        time is not an independent note-off fade: it is handed to the voice's
        ADSR as a lower bound (Voice::zoneReleaseMs_) so the SoundFont's stated
        release is honoured while the UI release knob keeps its exponential
        shape and can still lengthen the tail. The same value also drives the
        Release stage, which only exclusiveClass choking enters.
    */
    class VolumeEnvelope
    {
    public:
        enum class Stage : std::uint8_t { Idle, Delay, Attack, Hold, Decay, Sustain, Release };

        void reset (const x10::instrument::Envelope& parameters) noexcept;
        void setParameters (const x10::instrument::Envelope& parameters) noexcept;
        void prepare (int sampleRate) noexcept;
        /** Fades from the current level to silence; used to choke a zone. */
        void startRelease (float seconds) noexcept;
        [[nodiscard]] float tick() noexcept;
        [[nodiscard]] bool isActive() const noexcept { return stage_ != Stage::Idle; }
        [[nodiscard]] bool isReleasing() const noexcept { return stage_ == Stage::Release; }

    private:
        void enter (Stage stage) noexcept;
        [[nodiscard]] int durationSamples (float seconds) const noexcept;

        x10::instrument::Envelope parameters_ {};
        Stage stage_ = Stage::Idle;
        float releaseSeconds_ = 0.0f;
        int sampleRate_ = 0;
        int samplesRemaining_ = 0;
        float level_ = 0.0f;
        float increment_ = 0.0f;
    };

    const Sample* sample_ = nullptr;
    const SF2Loader* sampleOwner_ = nullptr;
    double phase_ = 0.0;
    float velocity_ = 0.0f;
    /** SoundFont velocity curve, applied instead of a linear multiply. */
    float velocityGain_ = 1.0f;
    bool active_ = false;
    juce::SmoothedValue<float> driveDbSmooth_;
    bool driveNeedsReset_ = true;
    std::uint32_t steadyDriveDbBits_ = 0;
    float steadyDriveBlend_ = 0.0f;
    float steadyDriveGain_ = 1.0f;
    bool steadyDriveCacheValid_ = false;
    int midiNote_ = -1;
    float envPhase_ = 0.0f;
    float envelopeLevel_ = 0.0f;
    float attenuationGain_ = 1.0f;
    // SoundFont release for the current zone, in milliseconds. Used as a floor
    // for the ADSR release so the UI knob can extend but not shorten below it.
    float zoneReleaseMs_ = 0.0f;
    BankToken bankToken_;
    std::uint64_t startSequence_ = 0;

    // Per-voice vibrato LFO phase, in radians. Advances at a fixed musical
    // rate (see kVibratoRateHz in Sampler.cpp) independent of pitch bend;
    // reset on start() so every fresh attack begins at a consistent phase.
    double vibratoPhase_ = 0.0;

    x10::dsp::Adsr adsr_;
    VolumeEnvelope volumeEnvelope_;
    Drive drive_;
    // Bit-pattern hash of the last pushed envelope parameter block; see render()
    // for why we must not re-push identical values every block.
    std::uint32_t envParamHash_ = 0;

    // Loop state: while looping (not in Release), phase_ wraps back to
    // loopStart_ once it passes loopEnd_. Cleared by start() so a retriggered
    // voice always begins from the sample head.
    int loopStart_ = 0;
    int loopEnd_ = 0;
    bool loopEnabled_ = false;
    // Before the first wrap the sample head must remain readable. Once a
    // voice has crossed the loop seam, taps below loopStart_ are allowed to
    // wrap to the loop tail for a continuous band-limited read.
    bool hasLoopWrapped_ = false;

    // Playback rate in source frames per output sample. 1.0 plays the sample at
    // its recorded pitch; a higher MIDI note advances faster, a lower one
    // slower. Computed once at start()/retarget() from the note, rootKey and
    // tunings. Pitch bend and vibrato are applied as an additional per-sample
    // multiplier inside render(), never baked into this cached rate, so a
    // live wheel/CC1 change takes effect immediately without a fresh attack.
    double playRate_ = 1.0;

    [[nodiscard]] double computePlayRate(const Sample* sample, int midiNote) const noexcept;

    x10::dsp::TptSvf filter_;
    bool filterNeedsPrepare_ = true;
    int filterSampleRate_ = 0;
    const Sample* filterParameterSample_ = nullptr;
    std::uint32_t filterParameterOffsetBits_ = 0;
    bool filterParametersCached_ = false;

    void bindSample(const Sample* sample, const SF2Loader* sampleOwner) noexcept;
    void detachSample() noexcept;
};

class VoicePool
{
public:
    static constexpr int maxVoices = 128;
    static constexpr std::size_t maxLayersPerNote = 8;

    explicit VoicePool(int numVoices = maxVoices)
        : voices_(static_cast<std::size_t>(juce::jmax (1, numVoices)))
        // Construct the immutable sinc table before any render call. Its
        // function-local static initialisation can take a lock, so it must not
        // first occur on the audio thread.
        , interpolator_(&BandLimitedInterpolator::shared())
        , polyphony_(static_cast<int>(voices_.size()))
    {}

    [[nodiscard]] int activeVoiceCount() const noexcept;
    [[nodiscard]] int voiceIndexForNote(int midiNote) const noexcept;
    [[nodiscard]] int preparedCapacity() const noexcept { return static_cast<int>(voices_.size()); }
    
    /** Provide direct access to a voice by index for setting bankToken; audio-thread only. */
    [[nodiscard]] Voice* getVoiceAtIndex(int voiceIndex) noexcept
    {
        if (voiceIndex < 0 || voiceIndex >= static_cast<int>(voices_.size()))
            return nullptr;
        return &voices_[static_cast<std::size_t>(voiceIndex)];
    }

    /** Caps the number of concurrently playing voices. Call from the audio thread. */
    void setPolyphony(int numVoices) noexcept;

    /**
        Reserve the scratch buffer used by renderStereo(). This must run before
        the host starts calling processBlock(); renderStereo() never grows the
        buffer on the audio thread.
    */
    void prepare(int maximumExpectedSamplesPerBlock);

    void start(const Sample* sample, int midiNote, float velocity,
               const SF2Loader* sampleOwner = nullptr) noexcept;
    /** Starts every matching SF2 zone for one MIDI note without allocating. */
    void start(std::span<const Sample* const> samples, int midiNote, float velocity,
               const SF2Loader* sampleOwner = nullptr) noexcept;
    void stop(int midiNote) noexcept;
    void stopAll() noexcept;
    /** Assign the current bank token to every layer of one started note. */
    void setBankTokenForNote (int midiNote, const BankToken& token) noexcept;

    /**
        CC64 sustain pedal state. While held, note-offs are deferred (the
        voice keeps sounding) instead of releasing immediately; releasing the
        pedal flushes every deferred note-off. Call from the audio thread only.
    */
    void setSustainHeld(bool held) noexcept;

    /**
        CC65 legato state. While enabled and at least one key is already held,
        a new note-on retargets the currently sounding lead voice instead of
        triggering a fresh envelope. Call from the audio thread only.
    */
    void setLegatoEnabled(bool enabled) noexcept;

    /** sustainLevel is normalized: 0.0 is silence and 1.0 is unity gain. */
    void render(float* output, int numSamples, int hostSampleRate, float driveDb, float velToDriveDb,
                int curveId, int filterRouting, float filterOffsetCents,
                float attackMs, float decayMs, float sustainLevel, float releaseMs,
                float pitchBendSemitones, float vibratoDepthCents) noexcept;

    /**
        Per-lane gains of the synthetic stereo spread.

        Every lane returns the same total power (left^2 + right^2) as the
        legacy dual-mono signal, and lane 0 is centred at unity in both
        channels. A zero or negative width therefore collapses all lanes to
        that centre. Non-finite widths are clamped away by the caller.
    */
    struct StereoGains
    {
        float left = 1.0f;
        float right = 1.0f;
    };

    [[nodiscard]] static StereoGains stereoGainsForVoice(std::size_t voiceIndex, float width,
                                                          float samplePan = 0.0f) noexcept;

    /**
        Render the voices into independent L/R accumulators. Each active voice
        is rendered once into a prepared scratch buffer and then summed into
        both channels with its per-lane equal-power gain. A width of zero
        reproduces the legacy mono render copied to both channels. A mono host,
        an unprepared pool, an oversized host block, or a non-finite width
        falls back to that same dual-mono path without allocating.
    */
    void renderStereo(float* outputLeft, float* outputRight, int numSamples,
                      int hostSampleRate, float driveDb, float velToDriveDb,
                      int curveId, int filterRouting, float filterOffsetCents,
                      float attackMs, float decayMs, float sustainLevel, float releaseMs,
                      float pitchBendSemitones, float vibratoDepthCents,
                      float stereoWidth) noexcept;

private:
    std::vector<Voice> voices_;
    std::vector<float> stereoScratch_;
    const BandLimitedInterpolator* interpolator_ = nullptr;
    int polyphony_ = 0;
    std::uint64_t nextStartSequence_ = 0;

    bool sustainHeld_ = false;
    bool legatoEnabled_ = false;
    // Keys currently physically held down (independent of sustain pedal),
    // used to decide whether a legato retarget is possible.
    std::array<bool, 128> keyHeld_ {};
    int heldKeyCount_ = 0;
    // Notes whose note-off was deferred because the sustain pedal was held.
    std::array<bool, 128> pendingRelease_ {};
    // Index of the most recently triggered/retargeted voice, used as the
    // legato "lead voice" target for the next retarget.
    int leadVoiceIndex_ = -1;

    void releaseNote(int midiNote) noexcept;
    void startVoice(Voice& voice, const Sample* sample, int midiNote, float velocity,
                    const SF2Loader* sampleOwner) noexcept;
    void chokeExclusiveClass (std::uint8_t exclusiveClass) noexcept;
    [[nodiscard]] bool isProtectedFromStealing(std::size_t voiceIndex) const noexcept;

    [[nodiscard]] Voice* findFreeVoice() noexcept;
};

} // namespace aod
