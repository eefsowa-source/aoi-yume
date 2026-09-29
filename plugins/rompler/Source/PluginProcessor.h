#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cstdint>
#include <tuple>
#include <functional>

#include "Parameters.h"
#include "Sampler.h"
#include "SF2Loader.h"
#include "BusProcessor.h"
#include "DynamicsProcessor.h"
#include "OutputSafetyProcessor.h"
#include "FxProcessor.h"
#include "PresetModel.h"
#include "RealtimeQueue.h"

namespace aod
{

enum class PresetApplyStatus
{
    applied,
    soundFontMissing,
    soundFontLoadFailed
};

class RomplerProcessor final : public juce::AudioProcessor, private juce::Timer
{
public:
    RomplerProcessor();
    ~RomplerProcessor() override;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    [[nodiscard]] juce::AudioProcessorValueTreeState& getValueTreeState() noexcept { return apvts_; }

    // Test-only observability for the audio-thread-to-message-thread mailbox.
    [[nodiscard]] float getRealtimeFilterOffsetForTesting() const noexcept
    {
        return realtimeFilterOffsetCents_.load (std::memory_order_relaxed);
    }
    [[nodiscard]] float getRealtimeBusCutoffForTesting() const noexcept
    {
        return realtimeBusCutoffHz_.load (std::memory_order_relaxed);
    }
    [[nodiscard]] int getPendingLatencyForTesting() const noexcept
    {
        return pendingLatencySamples_.load (std::memory_order_relaxed);
    }
    /** Test-only: synchronously drain the message-thread timer mailbox. */
    void drainDeferredWorkForTesting() { timerCallback(); }
    /** Test-only: voices currently rendering inside the pool. */
    [[nodiscard]] int getActiveVoiceCountForTesting() const noexcept
    {
        return voicePool_ ? voicePool_->activeVoiceCount() : 0;
    }
    /** Test-only: observe loader reuse and generation changes across prepares. */
    [[nodiscard]] const SF2Loader* getBankLoaderForTesting (int bankSlot) const noexcept
    {
        return bankSlot >= 0 && bankSlot < maxBanks
            ? sf2Loaders_[static_cast<std::size_t> (bankSlot)].get() : nullptr;
    }
    [[nodiscard]] std::uint32_t getBankGenerationForTesting (int bankSlot) const noexcept
    {
        return bankSlot >= 0 && bankSlot < maxBanks
            ? bankGeneration_[static_cast<std::size_t> (bankSlot)].load (std::memory_order_relaxed) : 0;
    }
    /**
        Test-only seam for interleaving a new MIDI CC after timerCallback()
        snapshots a controller mailbox but before it touches APVTS.
    */
    void setBeforeControllerMirrorForTesting (std::function<void()> hook)
    {
        beforeControllerMirrorForTesting_ = std::move (hook);
    }

    /** Capture the audible preset state; engine budget parameters are excluded. */
    [[nodiscard]] PresetDocument capturePreset() const;

    /** Apply a preset on the message thread in SoundFont -> bank -> program -> parameters order. */
    enum class ApplyStatus { ok, soundFontMissing, soundFontLoadFailed };
    [[nodiscard]] ApplyStatus applyPreset (const PresetDocument& document, const juce::File& resolvedSoundFont);
    [[nodiscard]] ApplyStatus applyPreset (const PresetDocument& document);
    void requestQuickSlot (int slot) noexcept;
    std::function<void (int)> onQuickSlotRequested;

    /** Peak magnitude of the most recently rendered block, for the UI meter. */
    [[nodiscard]] float getLastPeak() const noexcept { return lastPeak_.load (std::memory_order_relaxed); }

    /**
        Bitmask of MIDI notes with at least one sounding voice, split into two
        words: bits 0-63 of @p lo cover notes 0-63, @p hi covers 64-127.
        Published by the audio thread once per rendered range, so a note stays
        lit for its release tail and layers count once. Relaxed read for the
        UI timer only.
    */
    void getActiveNotes (std::uint64_t& lo, std::uint64_t& hi) const noexcept
    {
        lo = activeNotesLo_.load (std::memory_order_relaxed);
        hi = activeNotesHi_.load (std::memory_order_relaxed);
    }

    /** Linked compressor gain reduction in positive dB, for the UI GR meter. */
    [[nodiscard]] float getLastCompressionReductionDb() const noexcept
    {
        return dynamicsProcessor_.getLastGainReductionDb();
    }

    /**
        Loads a SoundFont from disk and, on success, selects its first preset.

        Safe to call from the message thread only: it allocates and does file
        I/O. processBlock() picks up the new bank through a released/acquired
        atomic pointer swap, never touching the old loader while the audio
        thread might still be reading it.
    */
    void loadSoundFont (const juce::File& file);

    /** Loads a SoundFont into a specific bank slot (0..maxBanks-1). */
    void loadSoundFont (const juce::File& file, int bankSlot);

    /** Removes the SoundFont loaded in the given bank slot. */
    void removeBank (int bankSlot);

    /** Switches the active bank slot, updating activeLoader_ for the audio thread. */
    void switchBank (int bankSlot);

    [[nodiscard]] static constexpr int getMaxBanks() noexcept { return maxBanks; }
    [[nodiscard]] static juce::String canonicalBundledSoundFontFileName (const juce::String& fileName);
    [[nodiscard]] int getActiveBankSlot() const noexcept { return activeBankSlot_.load (std::memory_order_relaxed); }
    [[nodiscard]] bool isBankLoaded (int bankSlot) const noexcept;
    [[nodiscard]] juce::String getBankName (int bankSlot) const noexcept
    {
        return juce::File (bankNames_[static_cast<std::size_t> (bankSlot)]).getFileNameWithoutExtension();
    }
    [[nodiscard]] const juce::String& getBankPath (int bankSlot) const noexcept { return bankNames_[static_cast<std::size_t> (bankSlot)]; }

    /**
        Loads the SoundFont bundled inside the plugin bundle's Contents/Resources
        directory, if one is present. Offered from the message-thread drain that
        services work deferred out of prepareToPlay(), so the plugin starts
        already usable without a manual Load step. No-op when slot 0 already
        holds a bank (e.g. a session restored between prepare and the drain) or
        when the bundle has no .sf2 resource (release builds without the
        bundled font, the ui_shot tool, the standalone build, or local dev).
    */
    void loadBundledSoundFont();

    [[nodiscard]] juce::String getLoadedFileName() const noexcept
    {
        const int slot = activeBankSlot_.load (std::memory_order_relaxed);
        return getBankName (slot);
    }

    [[nodiscard]] int getPresetCount() const noexcept;
    [[nodiscard]] juce::String getPresetName (int presetIndex) const noexcept;
    [[nodiscard]] std::pair<int, int> getPresetBankProgram (int presetIndex) const noexcept;

    /** Selects which (bank, program) note-on resolves against. Message-thread only. */
    void selectPreset (int bank, int program) noexcept;

    /** Current (bank, program) pair, for the UI readout. Message-thread safe. */
    [[nodiscard]] std::pair<int, int> getCurrentBankProgram() const noexcept
    {
        return { currentBank_.load (std::memory_order_relaxed),
                 currentProgram_.load (std::memory_order_relaxed) };
    }

    /**
        Queues a note-on/off for the audio thread. Called from the message thread
        (UI keyboard, computer keyboard); drained inside processBlock() so it is
        sample-accurate and allocation-free on the audio thread.
    */
    void postNote (int note, bool on, int velocity = 100);

    /**
        Returns and clears whether a UI-note queue overflow was recovered on
        the audio thread. Call from the message thread to show non-modal UI.
    */
    [[nodiscard]] bool consumeUiNoteQueueOverflow() noexcept;

    // ------------------------------------------------------------------
    // MIDI performance controls (pitch wheel, mod wheel / vibrato, sustain,
    // legato). Host MIDI updates these directly from the audio thread; the
    // UI wheel widgets call the "post" setters from the message thread so
    // dragging them behaves identically to a live MIDI controller. Both
    // paths write the same lock-free atomics that processBlock() reads, so
    // there is exactly one source of truth for the DSP either way.
    // ------------------------------------------------------------------

    /** UI pitch-wheel drag: value in [-1, 1], springs back to 0 on release. */
    void postPitchWheel (float normalizedValue) noexcept;
    /** UI mod-wheel drag: value in [0, 1], holds its position. */
    void postModWheel (float normalizedValue) noexcept;

    /** Current pitch bend, normalized to [-1, 1], for the UI wheel display. */
    [[nodiscard]] float getPitchWheelNormalized() const noexcept
    {
        return pitchBendSemitones_.load (std::memory_order_relaxed) / kPitchBendRangeSemitones;
    }
    /** Current mod wheel / vibrato depth, normalized to [0, 1], for the UI display. */
    [[nodiscard]] float getModWheelNormalized() const noexcept
    {
        return modWheelValue_.load (std::memory_order_relaxed);
    }
    [[nodiscard]] bool isSustainHeld() const noexcept
    {
        return sustainHeld_.load (std::memory_order_relaxed);
    }
    [[nodiscard]] bool isLegatoEnabled() const noexcept
    {
        return legatoEnabled_.load (std::memory_order_relaxed);
    }

    /**
        Directory bundled inside the plugin's own package that holds the
        packaged SoundFonts (Contents/Resources/SoundFonts on macOS). Used as
        the FileChooser's default browsing location so a user picking a new
        bank naturally lands on the bundled set first. Returns an invalid
        (non-existent) File outside of a macOS bundle build.
    */
    [[nodiscard]] juce::File getBundledSoundFontsDirectory() const noexcept;

private:
    enum PendingAsyncFlag : unsigned int
    {
        mirrorFilterOffset = 1u << 0,
        mirrorBusCutoff = 1u << 1,
        mirrorLatency = 1u << 2,
        quickSlot = 1u << 3,
        mirrorLegato = 1u << 4,
        reloadBanksForRate = 1u << 5,
        offerBundledFont = 1u << 6
    };

    // The processor is constructed and destroyed on JUCE's message thread.
    // This timer owns all host/UI work deferred from processBlock().
    void timerCallback() override;

    /**
        Message-thread drain for the bank work prepareToPlay() cannot do on
        its own thread: re-decode every loaded bank whose loader is still at
        the previous host rate, then offer the bundled SoundFont. The flag
        re-arms on every prepare, so the loop converges all slots to the most
        recently requested rate.
    */
    void reloadBanksForPreparedRate();

    // Drain and destroy retired SoundFont instances.
    // Safe to call from message thread only; ensures no voice is still
    // referencing samples from the oldest retired loader.
    void drainRetiredLoaders() noexcept;
    void queueAsyncFlag (unsigned int flag) noexcept;
    void mirrorNormalizedParameter (const char* parameterId, float normalizedValue);
    std::atomic<unsigned int> pendingAsyncFlags_ { 0 };
    std::atomic<int> pendingQuickSlot_ { 0 };
    std::atomic<int> pendingBankReloadRate_ { 0 };
    std::atomic<float> pendingFilterOffsetNormalized_ { 0.5f };
    std::atomic<float> pendingBusCutoffNormalized_ { 1.0f };
    std::atomic<float> pendingLegatoNormalized_ { 0.0f };
    // The audio thread uses each generation as a seqlock: odd while it writes
    // the snapshot, even when stable. A captured MIDI CC is authoritative
    // until its deferred APVTS mirror is sent; JUCE's public parameter API has
    // no atomic compare-and-set against concurrent host automation.
    std::atomic<std::uint64_t> filterOffsetCcGeneration_ { 0 };
    std::atomic<std::uint64_t> filterOffsetMirroredGeneration_ { 0 };
    std::atomic<std::uint64_t> busCutoffCcGeneration_ { 0 };
    std::atomic<std::uint64_t> busCutoffMirroredGeneration_ { 0 };
    std::atomic<std::uint64_t> legatoCcGeneration_ { 0 };
    std::atomic<std::uint64_t> legatoMirroredGeneration_ { 0 };
    static constexpr int maxControllerMirrorAttempts = 4;
    std::function<void()> beforeControllerMirrorForTesting_;
    std::atomic<int> pendingLatencySamples_ { -1 };
    /** Cap on buffered message-thread note events; see postNote(). */
    static constexpr std::size_t maxQueuedNotes = 256;
    struct UiNoteEvent
    {
        int note;
        int velocity;
        bool noteOn;
    };
    struct BlockParameters
    {
        float driveDb = 0.0f;
        int curveId = 0;
        float velToDriveDb = 0.0f;
        int filterRouting = 0;
        float filterOffsetCents = 0.0f;
        int polyphony = VoicePool::maxVoices;
        float attackMs = 0.0f;
        float decayMs = 0.0f;
        // Voice and ADSR contracts use a normalized 0..1 sustain level.
        float sustainLevel = 1.0f;
        float releaseMs = 0.0f;
        float pitchBendSemitones = 0.0f;
        float vibratoDepthCents = 0.0f;
        float ccGain = 1.0f;
        float pan = 0.5f;
        float tapeDrivePercent = 0.0f;
        float foldPercent = 0.0f;
        float filterCutoffHz = 20000.0f;
        float filterResonancePercent = 0.0f;
        float compThreshold = -18.0f;
        float compRatio = 3.0f;
        float compAttack = 15.0f;
        float compRelease = 180.0f;
        float compMakeup = 0.0f;
        float compMix = 0.0f;
        float chorusRate = 1.0f;
        float chorusDepth = 0.3f;
        float chorusMix = 0.25f;
        float reverbRoom = 0.4f;
        float reverbDamp = 0.5f;
        float reverbMix = 0.2f;
        float delayMix = 0.0f;
        float delayFeedback = 0.35f;
        float bpm = 120.0f;
        float outputGain = 1.0f;
    };

    void dispatchMidiMessage (const juce::MidiMessage&, const SF2Loader&) noexcept;
    void dispatchUiNote (const UiNoteEvent&, const SF2Loader&) noexcept;
    void renderRange (juce::AudioBuffer<float>&, int start, int count) noexcept;
    void syncBlockParameters() noexcept;
    static constexpr int maxBanks = 4;
    /** Total pitch-wheel swing in each direction, per the design spec. */
    static constexpr float kPitchBendRangeSemitones = 2.0f;
    /** Vibrato depth at full mod-wheel deflection (CC1 = 127). */
    static constexpr float kMaxVibratoDepthCents = 50.0f;

    juce::AudioProcessorValueTreeState apvts_;

    // Multi-bank SoundFont storage. Each slot holds its own SF2Loader.
    // activeLoader_ always points to the active slot's loader for the audio thread.
    std::array<std::shared_ptr<const SF2Loader>, maxBanks> sf2Loaders_;
    std::array<juce::String, maxBanks> bankNames_;
    std::atomic<int> activeBankSlot_ { 0 };
    std::atomic<const SF2Loader*> activeLoader_ { nullptr };
    std::atomic<std::uint32_t> audioBlocksInFlight_ { 0 };
    std::vector<std::shared_ptr<const SF2Loader>> retiredLoaders_;

    // Bank generation counters: incremented on each successful loadSoundFont()
    std::array<std::atomic<std::uint32_t>, maxBanks> bankGeneration_ {};
    std::array<std::uint64_t, maxBanks> bankFileHash_ {};

    std::unique_ptr<VoicePool> voicePool_;
    BusProcessor busProcessor_;
    DynamicsProcessor dynamicsProcessor_;
    OutputSafetyProcessor outputSafetyProcessor_;
    FxProcessor fxProcessor_;
    BlockParameters blockParameters_;
    int cachedOsFactorIndex_ = 2;
    double sampleRate_ = 48000.0;
    std::atomic<int> currentBank_ { 0 };
    std::atomic<int> currentProgram_ { 0 };
    std::atomic<float> lastPeak_ { 0.0f };
    std::atomic<std::uint64_t> activeNotesLo_ { 0 };
    std::atomic<std::uint64_t> activeNotesHi_ { 0 };

    /** Set once the bundled font has been offered up; see loadBundledSoundFont().
        Written by releaseResources() on the host's teardown thread and by the
        message-thread drain, so it stays atomic. */
    std::atomic<bool> bundledFontLoaded_ { false };

    // Message-thread -> audio-thread note events. Bounded and lock-free; if
    // the host is not running we drop rather than grow unbounded.
    SpscQueue<UiNoteEvent, maxQueuedNotes + 1> noteQueue_;
    std::atomic<bool> noteQueueOverflowed_ { false };
    std::atomic<bool> uiNoteQueueOverflowDiagnostic_ { false };

    // Shared MIDI-performance state (see the postXxx()/getXxx() block above).
    // Written by processBlock() from host MIDI, and by the UI wheel widgets
    // from the message thread; read by processBlock() every block and by the
    // editor for wheel repaint polling. No allocation/locking on either path.
    std::atomic<float> pitchBendSemitones_ { 0.0f };
    std::atomic<float> modWheelValue_ { 0.0f };
    std::atomic<bool> sustainHeld_ { false };
    std::atomic<bool> legatoEnabled_ { false };

    // CC7 (master volume), CC11 (expression) and CC10 (pan). Persist across
    // blocks like real hardware controllers; audio-thread only, so plain
    // members rather than atomics are sufficient (the UI does not read or
    // drive these directly, only host MIDI does).
    float masterVolumeCc7_ = 1.0f;
    float expressionCc11_ = 1.0f;
    float panCc10_ = 0.5f;

    // CC71/74 update these immediately in the audio callback. Until their
    // deferred APVTS mirrors land, syncBlockParameters keeps these values
    // instead of reapplying the stale host-side parameter value.
    std::atomic<float> realtimeFilterOffsetCents_ { 0.0f };
    std::atomic<float> realtimeBusCutoffHz_ { 20000.0f };

    // Mirrors the voiceLegato APVTS param at the block boundary, the same
    // pattern as cachedOsFactorIndex_: a UI/automation change to the param is
    // applied to legatoEnabled_ once per block. A live CC65 message updates
    // legatoEnabled_ and this cache immediately, then queues the deferred
    // APVTS mirror through the same mailbox as CC71/74 — until the mirror
    // lands, syncBlockParameters() leaves the controller's value alone
    // instead of restoring the stale parameter, so CC65 persists.
    int cachedLegatoParamValue_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RomplerProcessor)
};

} // namespace aod
