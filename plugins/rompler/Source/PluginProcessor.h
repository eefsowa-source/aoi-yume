#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <queue>
#include <mutex>
#include <tuple>

#include "Parameters.h"
#include "Sampler.h"
#include "SF2Loader.h"
#include "BusProcessor.h"
#include "FxProcessor.h"

namespace aod
{

class RomplerProcessor final : public juce::AudioProcessor
{
public:
    RomplerProcessor();
    ~RomplerProcessor() override = default;

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

    /** Peak magnitude of the most recently rendered block, for the UI meter. */
    [[nodiscard]] float getLastPeak() const noexcept { return lastPeak_.load (std::memory_order_relaxed); }

    /**
        Loads a SoundFont from disk and, on success, selects its first preset.

        Safe to call from the message thread only: it allocates and does file
        I/O. processBlock() picks up the new bank through a released/acquired
        atomic pointer swap, never touching the old unique_ptr while the audio
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
    [[nodiscard]] int getActiveBankSlot() const noexcept { return activeBankSlot_.load (std::memory_order_relaxed); }
    [[nodiscard]] bool isBankLoaded (int bankSlot) const noexcept;
    [[nodiscard]] const juce::String& getBankName (int bankSlot) const noexcept { return bankNames_[static_cast<std::size_t> (bankSlot)]; }

    /**
        Loads the SoundFont bundled inside the plugin bundle's Contents/Resources
        directory, if one is present. Called from prepareToPlay() on the message
        thread so the plugin starts already usable without a manual Load step.
        No-op when the bundle has no .sf2 resource (e.g. release builds without
        the bundled font, the ui_shot tool, the standalone build, or local dev).
    */
    void loadBundledSoundFont();

    [[nodiscard]] juce::String getLoadedFileName() const noexcept
    {
        const int slot = activeBankSlot_.load (std::memory_order_relaxed);
        return bankNames_[static_cast<std::size_t> (slot)];
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

private:
    /** Cap on buffered message-thread note events; see postNote(). */
    static constexpr std::size_t maxQueuedNotes = 256;
    static constexpr int maxBanks = 4;

    juce::AudioProcessorValueTreeState apvts_;

    // Multi-bank SoundFont storage. Each slot holds its own SF2Loader.
    // activeLoader_ always points to the active slot's loader for the audio thread.
    std::array<std::unique_ptr<SF2Loader>, maxBanks> sf2Loaders_;
    std::array<juce::String, maxBanks> bankNames_;
    std::atomic<int> activeBankSlot_ { 0 };
    std::atomic<SF2Loader*> activeLoader_ { nullptr };
    std::vector<std::unique_ptr<SF2Loader>> retiredLoaders_;

    std::unique_ptr<VoicePool> voicePool_;
    BusProcessor busProcessor_;
    FxProcessor fxProcessor_;
    int cachedOsFactorIndex_ = 2;
    double sampleRate_ = 48000.0;
    std::atomic<int> currentBank_ { 0 };
    std::atomic<int> currentProgram_ { 0 };
    std::atomic<float> lastPeak_ { 0.0f };

    /** Set once the bundled font has been offered up; see loadBundledSoundFont(). */
    bool bundledFontLoaded_ = false;

    // Message-thread -> audio-thread note events. Bounded; if the host is not
    // running we drop rather than grow unbounded.
    std::queue<std::tuple<int, bool, int>> noteQueue_;
    std::mutex noteQueueMutex_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RomplerProcessor)
};

} // namespace aod
