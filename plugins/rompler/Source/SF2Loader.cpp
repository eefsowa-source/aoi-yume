#include "SF2Loader.h"
#include <algorithm>
#include <cstring>

namespace aod
{

namespace
{

/** Converts a little-endian int16 PCM range [start, end) frames into float [-1, 1]. */
std::vector<float> convertPcmRange(std::span<const std::byte> sampleData,
                                    std::uint32_t start, std::uint32_t end)
{
    const std::size_t frameCount = sampleData.size() / 2;
    if (start >= frameCount || end > frameCount || start >= end)
        return {};

    std::vector<float> out(end - start);
    const auto* bytes = sampleData.data();

    for (std::uint32_t i = start; i < end; ++i)
    {
        std::int16_t sample16;
        std::memcpy(&sample16, bytes + (static_cast<std::size_t>(i) * 2), sizeof(sample16));
        out[i - start] = static_cast<float>(sample16) / 32768.0f;
    }

    return out;
}

} // namespace

bool SF2Loader::loadFile(const juce::File& file)
{
    std::vector<std::byte> fileBytes;
    x10::sf2::RawBank rawBank;

    const auto error = x10::sf2::readFile(file.getFullPathName().toStdString(), fileBytes, rawBank);
    if (error != x10::sf2::Sf2Error::ok)
        return false;

    std::vector<x10::instrument::Preset> presets = x10::sf2::flatten(rawBank);

    samples_.clear();

    for (const auto& preset : presets)
    {
        for (const auto& region : preset.regions)
        {
            Sample sample;
            sample.sampleRate = static_cast<int>(region.sampleRateHz);
            sample.data = convertPcmRange(rawBank.sampleData, region.start, region.end);
            sample.loopStart = static_cast<int>(region.loopStart > region.start
                                                     ? region.loopStart - region.start : 0);
            sample.loopEnd = static_cast<int>(region.loopEnd > region.start
                                                   ? region.loopEnd - region.start : 0);
            // Loop points may exceed the loaded playback range in malformed
            // files (endLoop past end). Clamp them so the loop wraps inside the
            // buffer and a release tail never reads out of bounds.
            if (sample.data.size() >= 2)
            {
                const auto maxFrame = static_cast<int> (sample.data.size() - 1);
                sample.loopEnd = std::min (sample.loopEnd, maxFrame);
                sample.loopStart = std::min (sample.loopStart, maxFrame - 1);
            }
            sample.loopEnabled = region.loopMode != x10::instrument::LoopMode::none;
            sample.filterCutoffHz = region.filterCutoffHz;
            sample.filterResonanceDb = region.filterResonanceDb;
            sample.rootKey = region.rootKey;
            sample.tuneCents = region.tuneCents;
            sample.scaleTuningCentsPerKey = region.scaleTuningCentsPerKey;

            resampleToHostRate(sample);

            samples_.emplace(&region, std::move(sample));
        }
    }

    // Pointers into presets[].regions stay valid: RegionIndex moves the
    // vector<Preset> buffer, it does not relocate individual elements.
    regionIndex_ = std::make_unique<x10::instrument::RegionIndex>(std::move(presets));

    return true;
}

Sample* SF2Loader::getSample(int bank, int program, int key, int velocity) noexcept
{
    if (!regionIndex_)
        return nullptr;

    std::array<const x10::instrument::Region*, 1> matches {};
    const std::size_t matchCount = regionIndex_->match(
        static_cast<std::uint16_t>(bank), static_cast<std::uint16_t>(program),
        key, velocity, matches);

    if (matchCount == 0)
        return nullptr;

    auto it = samples_.find(matches[0]);
    return it != samples_.end() ? &it->second : nullptr;
}

std::pair<int, int> SF2Loader::firstPresetProgram() const noexcept
{
    if (!regionIndex_ || regionIndex_->presetCount() == 0)
        return { 0, 0 };

    const auto& preset = regionIndex_->presetAt(0);
    return { preset.bank, preset.program };
}

int SF2Loader::presetCount() const noexcept
{
    return regionIndex_ ? static_cast<int>(regionIndex_->presetCount()) : 0;
}

juce::String SF2Loader::presetName(int presetIndex) const noexcept
{
    if (!regionIndex_ || presetIndex < 0 || presetIndex >= presetCount())
        return {};

    return regionIndex_->presetAt(static_cast<std::size_t>(presetIndex)).name;
}

std::pair<int, int> SF2Loader::presetBankProgram(int presetIndex) const noexcept
{
    if (!regionIndex_ || presetIndex < 0 || presetIndex >= presetCount())
        return { 0, 0 };

    const auto& preset = regionIndex_->presetAt(static_cast<std::size_t>(presetIndex));
    return { preset.bank, preset.program };
}

void SF2Loader::resampleToHostRate(Sample& sample)
{
    if (sample.sampleRate == hostSampleRate_ || sample.data.empty())
        return;

    const float ratio = static_cast<float>(hostSampleRate_) / static_cast<float>(sample.sampleRate);
    const auto newSize = static_cast<std::size_t>(static_cast<float>(sample.data.size()) * ratio);
    if (newSize == 0)
        return;

    std::vector<float> resampled(newSize);

    for (std::size_t i = 0; i < newSize; ++i)
    {
        const float phase = static_cast<float>(i) / ratio;
        const auto index = static_cast<std::size_t>(phase);

        if (index >= sample.data.size() - 1)
        {
            resampled[i] = sample.data.back();
        }
        else
        {
            // Same 4-point Lagrange as the playback path so a font's high end
            // survives the host-rate conversion instead of being smoothed.
            const float frac = phase - static_cast<float>(index);
            const auto tap = [&] (std::int64_t j) noexcept
            {
                j = std::clamp (j, std::int64_t { 0 }, static_cast<std::int64_t> (sample.data.size()) - 1);
                return sample.data[static_cast<std::size_t> (j)];
            };
            const float sm1 = tap (static_cast<std::int64_t> (index) - 1);
            const float s0  = tap (static_cast<std::int64_t> (index));
            const float s1  = tap (static_cast<std::int64_t> (index) + 1);
            const float s2  = tap (static_cast<std::int64_t> (index) + 2);
            resampled[i] = s0 + 0.5f * frac * (s1 - sm1
                + frac * (2.0f * sm1 - 5.0f * s0 + 4.0f * s1 - s2
                + frac * (3.0f * (s0 - s1) + s2 - sm1)));
        }
    }

    sample.data = std::move(resampled);
    sample.sampleRate = hostSampleRate_;

    // Loop points were clamped against the *source* buffer in loadFile(); the
    // resampled buffer is a different length, so rescale and re-clamp or the
    // voice loop wraps outside the data (out-of-bounds reads when the host
    // rate is lower than the source rate).
    sample.loopStart = static_cast<int> (static_cast<float> (sample.loopStart) * ratio);
    sample.loopEnd   = static_cast<int> (static_cast<float> (sample.loopEnd) * ratio);
    if (sample.data.size() >= 2)
    {
        const auto maxFrame = static_cast<int> (sample.data.size() - 1);
        sample.loopEnd   = std::min (sample.loopEnd, maxFrame);
        sample.loopStart = std::min (sample.loopStart, maxFrame - 1);
    }
}

} // namespace aod
