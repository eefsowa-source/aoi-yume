#include "SF2Loader.h"
#include "BandLimitedInterpolator.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>

namespace aod
{

namespace
{

struct CachedBank
{
    juce::String path;
    juce::uint64 fileIdentifier = 0;
    juce::int64 size = 0;
    juce::int64 modifiedMilliseconds = 0;
    juce::int64 createdMilliseconds = 0;
    int sampleRate = 0;
    std::size_t sampleBytes = 0;
    std::shared_ptr<const SF2Loader> loader;
};

// pluginval revisits the same rates across many prepare/release cycles and
// processor instances. Keep the cache bounded by both entry count and decoded
// sample storage so a few unusually large banks cannot pin several GB in a
// long-running host. Map and allocator overhead is not included in this budget.
constexpr std::size_t kMaxCachedBanks = 4;
constexpr std::size_t kMaxCachedSampleBytes = 512u * 1024u * 1024u;
std::mutex cacheMutex;
std::list<CachedBank> cachedBanks;
std::size_t cachedSampleBytes = 0;

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

std::shared_ptr<const SF2Loader> SF2Loader::loadCached (const juce::File& file, int hostSampleRate)
{
    if (! file.existsAsFile())
        return {};

    const auto path = file.getFullPathName();
    const auto fileIdentifier = file.getFileIdentifier();
    const auto size = file.getSize();
    const auto modifiedMilliseconds = file.getLastModificationTime().toMilliseconds();
    const auto createdMilliseconds = file.getCreationTime().toMilliseconds();

    // Keep the lock through a miss so simultaneous prepare calls cannot parse
    // and resample the same large bank twice. This function is never called by
    // processBlock or its descendants.
    const std::lock_guard<std::mutex> lock (cacheMutex);
    for (auto it = cachedBanks.begin(); it != cachedBanks.end(); ++it)
    {
        if (it->path == path && it->fileIdentifier == fileIdentifier
            && it->size == size && it->createdMilliseconds == createdMilliseconds
            && it->modifiedMilliseconds == modifiedMilliseconds
            && it->sampleRate == hostSampleRate)
        {
            cachedBanks.splice (cachedBanks.begin(), cachedBanks, it);
            return cachedBanks.front().loader;
        }
    }

    auto loader = std::make_shared<SF2Loader> (hostSampleRate);
    if (! loader->loadFile (file))
        return {};

    const auto sampleBytes = loader->sampleStorageBytes();
    if (sampleBytes > kMaxCachedSampleBytes)
        return loader;

    cachedBanks.push_front ({ path, fileIdentifier, size, modifiedMilliseconds,
                              createdMilliseconds, hostSampleRate, sampleBytes, loader });
    cachedSampleBytes += sampleBytes;
    while (! cachedBanks.empty()
           && (cachedBanks.size() > kMaxCachedBanks || cachedSampleBytes > kMaxCachedSampleBytes))
    {
        cachedSampleBytes -= cachedBanks.back().sampleBytes;
        cachedBanks.pop_back();
    }
    return loader;
}

std::size_t SF2Loader::sampleStorageBytes() const noexcept
{
    std::size_t bytes = 0;
    constexpr auto maxBytes = std::numeric_limits<std::size_t>::max();
    for (const auto& entry : samples_)
    {
        const auto capacity = entry.second.data.capacity();
        if (capacity > (maxBytes - bytes) / sizeof (float))
            return maxBytes;
        bytes += capacity * sizeof (float);
    }
    return bytes;
}

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
            sample.volumeEnvelope = region.volumeEnvelope;
            // The modulation envelope and its two depth generators. Both
            // depths are in cents, and both are zero for a zone that states no
            // modulation, which is what keeps the voice's second-envelope path
            // inert for the majority of presets.
            sample.modulationEnvelope = region.modulationEnvelope;
            sample.modEnvToPitchCents  = region.modEnvToPitchCents;
            sample.modEnvToFilterCents = region.modEnvToFilterCents;
            sample.attenuationDb = region.attenuationDb;
            sample.pan = region.pan;
            sample.exclusiveClass = region.exclusiveClass;
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

std::size_t SF2Loader::getSamples(int bank, int program, int key, int velocity,
                                  std::span<const Sample*> out) const noexcept
{
    if (!regionIndex_)
        return 0;

    std::array<const x10::instrument::Region*, maxMatchingSamples> matches {};
    const std::size_t matchCount = regionIndex_->match(
        static_cast<std::uint16_t>(bank), static_cast<std::uint16_t>(program),
        key, velocity, matches);

    // Report every resolved zone so a caller whose buffer is smaller than the
    // result can tell that zones were dropped, matching RegionIndex::match.
    const auto examined = std::min (matchCount, matches.size());
    std::size_t resolved = 0;
    for (std::size_t index = 0; index < examined; ++index)
        if (const auto it = samples_.find (matches[index]); it != samples_.end())
        {
            if (resolved < out.size())
                out[resolved] = &it->second;
            ++resolved;
        }

    return resolved;
}

const Sample* SF2Loader::getSample(int bank, int program, int key, int velocity) const noexcept
{
    std::array<const Sample*, 1> match {};
    return getSamples (bank, program, key, velocity, match) != 0 ? match.front() : nullptr;
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

    // Rate conversion happens once, on the loading thread, so it uses the same
    // band-limited kernel the voices use rather than a linear approximation.
    // Downward conversion also needs the source band-limited first, otherwise
    // content above the new Nyquist folds into the stored PCM permanently.
    const auto& interpolator = BandLimitedInterpolator::shared();
    const double sourceStep = 1.0 / static_cast<double>(ratio);
    const float position = interpolator.positionForRate(sourceStep);
    const auto sourceCount = static_cast<std::int64_t>(sample.data.size());

    for (std::size_t i = 0; i < newSize; ++i)
    {
        const double phase = static_cast<double>(i) * sourceStep;
        const auto index = static_cast<std::int64_t>(phase);
        const auto frac = static_cast<float>(phase - static_cast<double>(index));

        resampled[i] = interpolator.interpolateRate(position, frac,
            [&](int offset) noexcept
            {
                const auto tapIndex = std::clamp<std::int64_t>(index + offset, 0, sourceCount - 1);
                return sample.data[static_cast<std::size_t>(tapIndex)];
            });
    }

    sample.data = std::move(resampled);
    sample.sampleRate = hostSampleRate_;

    // Loop points are stored in frames, so they have to follow the new rate or
    // a converted sample loops at the wrong place.
    const auto scaleStartPoint = [&](int point)
    {
        const auto scaled = static_cast<std::int64_t>(std::llround(static_cast<double>(point) * static_cast<double>(ratio)));
        return static_cast<int>(std::clamp<std::int64_t>(scaled, 0, static_cast<std::int64_t>(newSize) - 1));
    };
    // loopEnd is an exclusive boundary in the playback engine. It is allowed
    // to equal newSize, unlike a start point that must address a frame.
    const auto scaleEndPoint = [&](int point)
    {
        const auto scaled = static_cast<std::int64_t>(std::llround(static_cast<double>(point) * static_cast<double>(ratio)));
        return static_cast<int>(std::clamp<std::int64_t>(scaled, 0, static_cast<std::int64_t>(newSize)));
    };
    sample.loopStart = scaleStartPoint(sample.loopStart);
    sample.loopEnd = scaleEndPoint(sample.loopEnd);
}

} // namespace aod
