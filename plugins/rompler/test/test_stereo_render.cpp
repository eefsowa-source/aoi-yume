#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>

#include "Sampler.h"
#include "support/AllocationGuard.h"

namespace
{
constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 256;

aod::Sample makeTone()
{
    aod::Sample sample;
    sample.data.resize (static_cast<std::size_t> (kSampleRate));
    for (int i = 0; i < kSampleRate; ++i)
        sample.data[static_cast<std::size_t> (i)] =
            std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                      * static_cast<float> (i) / static_cast<float> (kSampleRate));
    sample.sampleRate = kSampleRate;
    return sample;
}

void renderArgs (aod::VoicePool& pool, float* output, int numSamples)
{
    pool.render (output, numSamples, kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f,
                 0.0f, 1000.0f, 1.0f, 1000.0f,
                 0.0f, 0.0f);
}

void renderStereoArgs (aod::VoicePool& pool, float* left, float* right,
                       int numSamples, float width)
{
    pool.renderStereo (left, right, numSamples, kSampleRate,
                       0.0f, 0.0f, 0, 0, 0.0f,
                       0.0f, 1000.0f, 1.0f, 1000.0f,
                       0.0f, 0.0f, width);
}
} // namespace

TEST_CASE ("stereo render width zero preserves the legacy dual-mono signal", "[dsp][stereo]")
{
    const auto sample = makeTone();
    aod::VoicePool mono (3), stereo (3);
    stereo.prepare (kBlockSize);
    for (const int note : { 60, 64, 67 })
    {
        mono.start (&sample, note, 0.8f);
        stereo.start (&sample, note, 0.8f);
    }

    std::array<float, kBlockSize> expected {};
    std::array<float, kBlockSize> left {};
    std::array<float, kBlockSize> right {};
    renderArgs (mono, expected.data(), kBlockSize);
    renderStereoArgs (stereo, left.data(), right.data(), kBlockSize, 0.0f);

    for (int i = 0; i < kBlockSize; ++i)
    {
        REQUIRE (std::abs (left[static_cast<std::size_t> (i)]
                           - expected[static_cast<std::size_t> (i)]) < 1.0e-6f);
        REQUIRE (std::abs (right[static_cast<std::size_t> (i)]
                           - expected[static_cast<std::size_t> (i)]) < 1.0e-6f);
    }
}

TEST_CASE ("stereo render spreads independent voice lanes", "[dsp][stereo]")
{
    const auto sample = makeTone();
    aod::VoicePool pool (3);
    pool.prepare (kBlockSize);
    for (const int note : { 60, 64, 67 })
        pool.start (&sample, note, 0.8f);

    std::array<float, kBlockSize> left {};
    std::array<float, kBlockSize> right {};
    renderStereoArgs (pool, left.data(), right.data(), kBlockSize, 0.75f);

    float maximumDifference = 0.0f;
    for (int i = 0; i < kBlockSize; ++i)
    {
        const auto index = static_cast<std::size_t> (i);
        REQUIRE (std::isfinite (left[index]));
        REQUIRE (std::isfinite (right[index]));
        maximumDifference = std::max (maximumDifference, std::abs (left[index] - right[index]));
    }

    REQUIRE (maximumDifference > 1.0e-4f);
}

TEST_CASE ("stereo render performs no audio-thread allocations after prepare", "[dsp][rt][stereo]")
{
    const auto sample = makeTone();
    aod::VoicePool pool (8);
    pool.prepare (kBlockSize);
    for (const int note : { 48, 52, 55, 60, 64, 67, 71, 76 })
        pool.start (&sample, note, 0.8f);

    std::array<float, kBlockSize> left {};
    std::array<float, kBlockSize> right {};
    renderStereoArgs (pool, left.data(), right.data(), kBlockSize, 0.75f);

    const x10::instrument::test::AllocationScope scope;
    for (int pass = 0; pass < 32; ++pass)
        renderStereoArgs (pool, left.data(), right.data(), kBlockSize, 0.75f);

    REQUIRE (scope.allocationsSoFar() == 0);
}

TEST_CASE ("stereo gain law is equal-power and left/right balanced across lanes", "[dsp][stereo]")
{
    constexpr float width = 0.6f;
    constexpr std::size_t laneCount = 9;

    float totalLeftPower = 0.0f;
    float totalRightPower = 0.0f;
    for (std::size_t lane = 0; lane < laneCount; ++lane)
    {
        const auto gains = aod::VoicePool::stereoGainsForVoice (lane, width);
        REQUIRE (std::isfinite (gains.left));
        REQUIRE (std::isfinite (gains.right));
        // Every lane must carry the power of the legacy dual-mono signal
        // (1^2 + 1^2); no slot may be louder or quieter by accident.
        REQUIRE (std::abs (gains.left * gains.left + gains.right * gains.right - 2.0f) < 1.0e-4f);
        totalLeftPower += gains.left * gains.left;
        totalRightPower += gains.right * gains.right;
    }

    // The lane positions mirror each other, so filling every lane leaves both
    // channels at the same power instead of drifting to one side.
    REQUIRE (std::abs (totalLeftPower - totalRightPower) < 1.0e-4f);

    // The centre lane stays dual-mono, which is what keeps the first voice of a
    // fresh phrase centred.
    const auto centre = aod::VoicePool::stereoGainsForVoice (0, width);
    REQUIRE (std::abs (centre.left - 1.0f) < 1.0e-6f);
    REQUIRE (std::abs (centre.right - 1.0f) < 1.0e-6f);
    REQUIRE (juce::exactlyEqual (centre.left, centre.right));

    // Slots past the lane table wrap back to the centre lane.
    const auto wrapped = aod::VoicePool::stereoGainsForVoice (laneCount, width);
    REQUIRE (juce::exactlyEqual (wrapped.left, centre.left));
    REQUIRE (juce::exactlyEqual (wrapped.right, centre.right));

    // Zero and negative widths collapse every lane onto that same centre, so
    // there is no level step between a collapsed image and a narrow one.
    for (std::size_t lane = 0; lane < laneCount; ++lane)
    {
        const auto collapsed = aod::VoicePool::stereoGainsForVoice (lane, 0.0f);
        REQUIRE (std::abs (collapsed.left - 1.0f) < 1.0e-6f);
        REQUIRE (std::abs (collapsed.right - 1.0f) < 1.0e-6f);

        const auto clamped = aod::VoicePool::stereoGainsForVoice (lane, -0.5f);
        REQUIRE (juce::exactlyEqual (clamped.left, collapsed.left));
        REQUIRE (juce::exactlyEqual (clamped.right, collapsed.right));
    }
}

TEST_CASE ("a single voice lane stays centred and dual-mono at full spread", "[dsp][stereo]")
{
    const auto sample = makeTone();
    aod::VoicePool pool (4);
    pool.prepare (kBlockSize);
    pool.start (&sample, 69, 0.8f);
    REQUIRE (pool.activeVoiceCount() == 1);

    std::array<float, kBlockSize> left {};
    std::array<float, kBlockSize> right {};
    renderStereoArgs (pool, left.data(), right.data(), kBlockSize, 0.6f);

    float peak = 0.0f;
    for (int i = 0; i < kBlockSize; ++i)
    {
        const auto index = static_cast<std::size_t> (i);
        peak = std::max (peak, std::abs (left[index]));
        REQUIRE (juce::exactlyEqual (left[index], right[index]));
    }
    REQUIRE (peak > 0.0f);
}

TEST_CASE ("SoundFont zone pan is applied before the global stereo bus", "[dsp][stereo][sf2]")
{
    auto sample = makeTone();
    sample.pan = -1.0f;
    sample.volumeEnvelope = { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.1f };

    aod::VoicePool pool (1);
    pool.prepare (kBlockSize);
    pool.start (&sample, 69, 1.0f);

    std::array<float, kBlockSize> left {};
    std::array<float, kBlockSize> right {};
    renderStereoArgs (pool, left.data(), right.data(), kBlockSize, 0.6f);

    const auto peak = [] (const auto& channel)
    {
        float result = 0.0f;
        for (const float sampleValue : channel)
            result = std::max (result, std::abs (sampleValue));
        return result;
    };

    REQUIRE (peak (left) > 0.1f);
    REQUIRE (peak (right) < 1.0e-6f);
}

TEST_CASE ("a near-zero width stays continuous with the dual-mono signal", "[dsp][stereo]")
{
    const auto sample = makeTone();
    aod::VoicePool reference (3), narrow (3);
    narrow.prepare (kBlockSize);
    for (const int note : { 60, 64, 67 })
    {
        reference.start (&sample, note, 0.8f);
        narrow.start (&sample, note, 0.8f);
    }

    std::array<float, kBlockSize> dualMono {};
    std::array<float, kBlockSize> left {};
    std::array<float, kBlockSize> right {};
    renderArgs (reference, dualMono.data(), kBlockSize);
    renderStereoArgs (narrow, left.data(), right.data(), kBlockSize, 1.0e-5f);

    // A small width must not introduce a level step: before this policy the
    // non-centre lanes dropped about 3 dB as soon as width left zero.
    for (int i = 0; i < kBlockSize; ++i)
    {
        const auto index = static_cast<std::size_t> (i);
        REQUIRE (std::abs (left[index] - dualMono[index]) < 1.0e-4f);
        REQUIRE (std::abs (right[index] - dualMono[index]) < 1.0e-4f);
    }
}

TEST_CASE ("a mono host and an oversized block fall back to dual-mono", "[dsp][rt][stereo]")
{
    const auto sample = makeTone();
    aod::VoicePool referencePool (3), hostPool (3);
    referencePool.prepare (kBlockSize);
    hostPool.prepare (kBlockSize);
    for (const int note : { 60, 64, 67 })
    {
        referencePool.start (&sample, note, 0.8f);
        hostPool.start (&sample, note, 0.8f);
    }

    std::array<float, kBlockSize> dualMono {};
    renderArgs (referencePool, dualMono.data(), kBlockSize);

    // No right channel at all: the pool must reuse the mono path, not the
    // scratch buffer, and must not allocate while doing it.
    std::array<float, kBlockSize> monoHost {};
    const x10::instrument::test::AllocationScope scope;
    renderStereoArgs (hostPool, monoHost.data(), nullptr, kBlockSize, 0.6f);
    REQUIRE (scope.allocationsSoFar() == 0);
    for (int i = 0; i < kBlockSize; ++i)
        REQUIRE (juce::exactlyEqual (monoHost[static_cast<std::size_t> (i)],
                                     dualMono[static_cast<std::size_t> (i)]));

    // A host block larger than the prepared scratch buffer is equally safe.
    std::array<float, kBlockSize * 2> oversizedLeft {};
    std::array<float, kBlockSize * 2> oversizedRight {};
    renderStereoArgs (hostPool, oversizedLeft.data(), oversizedRight.data(), kBlockSize * 2, 0.6f);
    REQUIRE (scope.allocationsSoFar() == 0);
    for (int i = 0; i < kBlockSize * 2; ++i)
        REQUIRE (juce::exactlyEqual (oversizedLeft[static_cast<std::size_t> (i)],
                                     oversizedRight[static_cast<std::size_t> (i)]));
    REQUIRE (std::isfinite (oversizedLeft[0]));
}
