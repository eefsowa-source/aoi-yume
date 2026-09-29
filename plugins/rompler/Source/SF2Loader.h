#pragma once

#include "Sampler.h"
#include <x10/sf2/Sf2Reader.h>
#include <x10/sf2/Sf2Flattener.h>
#include <x10/instrument/RegionIndex.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aod
{

class SF2Loader
{
public:
    // Eight covers conventional velocity/round-robin stacks while keeping
    // every note-on's working set fixed and stack allocated.
    static constexpr std::size_t maxMatchingSamples = VoicePool::maxLayersPerNote;
    explicit SF2Loader(int hostSampleRate) : hostSampleRate_(hostSampleRate) {}

    bool loadFile(const juce::File& file);

    /**
        Return an already decoded bank when the file identity and host rate
        match. Loading and cache eviction happen off the audio thread only.
        Callers retain shared ownership while voices may reference its samples.
    */
    [[nodiscard]] static std::shared_ptr<const SF2Loader> loadCached (const juce::File& file,
                                                                       int hostSampleRate);

    [[nodiscard]] int hostSampleRate() const noexcept { return hostSampleRate_; }

    /** Audio-thread voice lease counters used to retire sample storage safely. */
    void retainVoiceSample() const noexcept
    {
        voiceSampleReferences_.fetch_add (1, std::memory_order_relaxed);
    }
    void releaseVoiceSample() const noexcept
    {
        voiceSampleReferences_.fetch_sub (1, std::memory_order_release);
    }
    [[nodiscard]] bool hasVoiceSampleReferences() const noexcept
    {
        return voiceSampleReferences_.load (std::memory_order_acquire) != 0;
    }

    /** Approximate bytes held by decoded sample buffers. */
    [[nodiscard]] std::size_t sampleStorageBytes() const noexcept;

    /**
        Writes the zone samples matching @p key and @p velocity into @p out and
        returns how many zones resolved.  Only the first out.size() pointers are
        written, so a return value larger than out.size() tells the caller that
        zones were dropped and it can retry with a wider buffer.  The caller
        supplies fixed storage, which keeps this safe on the audio thread.  A
        bank with more than maxMatchingSamples overlapping zones for one note
        cannot expose the extras.
    */
    [[nodiscard]] std::size_t getSamples(int bank, int program, int key, int velocity,
                                         std::span<const Sample*> out) const noexcept;

    /** Returns the first matching sample for single-zone callers. */
    [[nodiscard]] const Sample* getSample(int bank, int program, int key, int velocity) const noexcept;

    /** (bank, program) of preset 0 in load order, or {0, 0} if nothing loaded. */
    [[nodiscard]] std::pair<int, int> firstPresetProgram() const noexcept;

    [[nodiscard]] int presetCount() const noexcept;
    [[nodiscard]] juce::String presetName (int presetIndex) const noexcept;
    [[nodiscard]] std::pair<int, int> presetBankProgram (int presetIndex) const noexcept;

private:
    int hostSampleRate_;
    mutable std::atomic<int> voiceSampleReferences_ { 0 };
    std::unique_ptr<x10::instrument::RegionIndex> regionIndex_;
    std::unordered_map<const x10::instrument::Region*, Sample> samples_;

    void resampleToHostRate(Sample& sample);
};

} // namespace aod
