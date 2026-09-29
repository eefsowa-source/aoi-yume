#include "DynamicsProcessor.h"

#include <algorithm>
#include <cmath>

namespace aod
{
namespace
{
constexpr float kDetectorFloorDb = -120.0f;
constexpr float kMinimumRatio = 1.0f;
constexpr float kMaximumRatio = 100.0f;
constexpr float kMinimumTimeMs = 0.0f;
constexpr float kMaximumTimeMs = 10000.0f;
constexpr float kMinimumMakeupDb = -60.0f;
constexpr float kMaximumMakeupDb = 60.0f;
constexpr float kEvenHarmonicAmount = 0.18f;
constexpr float kEvenHarmonicDcCoefficientAtReferenceRate = 0.9995f;
constexpr double kEvenHarmonicDcReferenceSampleRate = 48000.0;

float clampFinite (float value, float minimum, float maximum, float fallback) noexcept
{
    if (!std::isfinite (value))
        return fallback;

    return juce::jlimit (minimum, maximum, value);
}

float finiteSample (float value) noexcept
{
    return std::isfinite (value) ? value : 0.0f;
}

float smoothingCoefficient (float timeMs, double sampleRate) noexcept
{
    if (timeMs <= 0.0f || sampleRate <= 0.0 || !std::isfinite (sampleRate))
        return 0.0f;

    const double timeSeconds = static_cast<double> (timeMs) * 0.001;
    const double exponent = -1.0 / (timeSeconds * sampleRate);
    return static_cast<float> (std::exp (exponent));
}
} // namespace

void DynamicsProcessor::prepare (double sampleRate, int maximumBlockSize, int numChannels)
{
    static_cast<void> (maximumBlockSize);

    sampleRate_ = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
    evenHarmonicDcCoefficient_ = static_cast<float> (std::pow (
        static_cast<double> (kEvenHarmonicDcCoefficientAtReferenceRate),
        kEvenHarmonicDcReferenceSampleRate / sampleRate_));
    preparedChannels_ = juce::jmax (0, numChannels);
    evenHarmonicDc_.assign (static_cast<std::size_t> (preparedChannels_), 0.0f);
    prepared_ = true;
    reset();
}

void DynamicsProcessor::reset()
{
    gainReductionDb_ = 0.0f;
    std::fill (evenHarmonicDc_.begin(), evenHarmonicDc_.end(), 0.0f);
    lastGainReductionDb_.store (0.0f, std::memory_order_relaxed);
}

float DynamicsProcessor::getLastGainReductionDb() const noexcept
{
    return lastGainReductionDb_.load (std::memory_order_relaxed);
}

void DynamicsProcessor::process (juce::AudioBuffer<float>& buffer,
                                 float thresholdDb, float ratio,
                                 float attackMs, float releaseMs,
                                 float makeupDb, float mixPercent) noexcept
{
    if (!prepared_ || buffer.getNumSamples() <= 0)
        return;

    const int numChannels = juce::jmin (preparedChannels_, buffer.getNumChannels());
    if (numChannels <= 0)
        return;

    const float wetMix = clampFinite (mixPercent, 0.0f, 100.0f, 100.0f) * 0.01f;
    if (wetMix <= 0.0f)
    {
        gainReductionDb_ = 0.0f;
        std::fill (evenHarmonicDc_.begin(), evenHarmonicDc_.end(), 0.0f);
        lastGainReductionDb_.store (0.0f, std::memory_order_relaxed);
        return;
    }

    const float safeThresholdDb = clampFinite (thresholdDb, kDetectorFloorDb, 24.0f, 0.0f);
    const float safeRatio = clampFinite (ratio, kMinimumRatio, kMaximumRatio, kMinimumRatio);
    const float safeAttackMs = clampFinite (attackMs, kMinimumTimeMs, kMaximumTimeMs, 0.0f);
    const float safeReleaseMs = clampFinite (releaseMs, kMinimumTimeMs, kMaximumTimeMs, 0.0f);
    const float safeMakeupDb = clampFinite (makeupDb, kMinimumMakeupDb, kMaximumMakeupDb, 0.0f);
    const float attackCoefficient = smoothingCoefficient (safeAttackMs, sampleRate_);
    const float releaseCoefficient = smoothingCoefficient (safeReleaseMs, sampleRate_);
    const float dryMix = 1.0f - wetMix;

    auto* left = buffer.getWritePointer (0);
    auto* right = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const float leftInput = finiteSample (left[sample]);
        const float rightInput = right != nullptr ? finiteSample (right[sample]) : 0.0f;
        const float detector = juce::jmax (std::abs (leftInput), std::abs (rightInput));
        const float inputDb = juce::Decibels::gainToDecibels (detector, kDetectorFloorDb);
        const float amountAboveThreshold = inputDb - safeThresholdDb;
        const float targetGainReductionDb = amountAboveThreshold > 0.0f
            ? amountAboveThreshold * ((1.0f / safeRatio) - 1.0f)
            : 0.0f;

        const float coefficient = targetGainReductionDb < gainReductionDb_
            ? attackCoefficient
            : releaseCoefficient;
        gainReductionDb_ = coefficient * gainReductionDb_
            + (1.0f - coefficient) * targetGainReductionDb;

        if (!std::isfinite (gainReductionDb_))
            gainReductionDb_ = 0.0f;

        const float compressedGain = juce::Decibels::decibelsToGain (gainReductionDb_ + safeMakeupDb);
        const float mixedGain = dryMix + wetMix * compressedGain;
        const float reductionAmount = juce::jlimit (0.0f, 1.0f, -gainReductionDb_ / 24.0f);
        // Increasing reduction progressively drives a deliberately asymmetric
        // polynomial stage. The squared term creates even harmonics; a slow
        // running mean removes its otherwise audible DC offset.
        const float harmonicAmount = kEvenHarmonicAmount * reductionAmount;

        const auto applyChannel = [&] (float input, int channel) noexcept
        {
            const float compressed = input * mixedGain;
            if (harmonicAmount <= 0.0f || channel < 0
                || channel >= static_cast<int> (evenHarmonicDc_.size()))
                return compressed;

            const float squared = compressed * compressed;
            auto& dc = evenHarmonicDc_[static_cast<std::size_t> (channel)];
            dc = evenHarmonicDcCoefficient_ * dc
                + (1.0f - evenHarmonicDcCoefficient_) * squared;
            return compressed + harmonicAmount * (squared - dc);
        };

        float leftOutput = applyChannel (leftInput, 0);
        if (!std::isfinite (leftOutput))
            leftOutput = 0.0f;
        left[sample] = leftOutput;

        if (right != nullptr)
        {
            float rightOutput = applyChannel (rightInput, 1);
            if (!std::isfinite (rightOutput))
                rightOutput = 0.0f;
            right[sample] = rightOutput;
        }

        for (int channel = 2; channel < numChannels; ++channel)
        {
            auto* data = buffer.getWritePointer (channel);
            float output = applyChannel (finiteSample (data[sample]), channel);
            if (!std::isfinite (output))
                output = 0.0f;
            data[sample] = output;
        }
    }

    lastGainReductionDb_.store (juce::jmax (0.0f, -gainReductionDb_), std::memory_order_relaxed);
}

} // namespace aod
