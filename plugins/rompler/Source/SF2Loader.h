#pragma once

#include "Sampler.h"
#include <x10/sf2/Sf2Reader.h>
#include <x10/sf2/Sf2Flattener.h>
#include <x10/instrument/RegionIndex.h>
#include <juce_core/juce_core.h>
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
    explicit SF2Loader(int hostSampleRate) : hostSampleRate_(hostSampleRate) {}

    bool loadFile(const juce::File& file);

    /** Max velocity layers/round robins resolved per note-on. */
    static constexpr int maxLayers = 8;

    /** Returns nullptr if no matching region/sample was found. */
    [[nodiscard]] const Sample* getSample(int bank, int program, int key, int velocity) noexcept;

    /**
        Fills @p out with every sample matching (bank, program, key, velocity)
        — velocity layers and round robins all sound together. Returns the
        subspan actually written. noexcept and allocation-free, safe on the
        audio thread.
    */
    [[nodiscard]] std::span<const Sample*> getSamples(int bank, int program, int key, int velocity,
                                                std::span<const Sample*> out) noexcept;

    /** (bank, program) of preset 0 in load order, or {0, 0} if nothing loaded. */
    [[nodiscard]] std::pair<int, int> firstPresetProgram() const noexcept;

    [[nodiscard]] int presetCount() const noexcept;
    [[nodiscard]] juce::String presetName (int presetIndex) const noexcept;
    [[nodiscard]] std::pair<int, int> presetBankProgram (int presetIndex) const noexcept;

private:
    int hostSampleRate_;
    std::unique_ptr<x10::instrument::RegionIndex> regionIndex_;
    std::unordered_map<const x10::instrument::Region*, Sample> samples_;

    void resampleToHostRate(Sample& sample);
};

} // namespace aod
