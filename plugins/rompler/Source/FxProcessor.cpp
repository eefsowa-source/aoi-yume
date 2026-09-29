#include "FxProcessor.h"

#include <algorithm>
#include <cmath>

namespace aod
{

void FxProcessor::prepare (double sampleRate, int maximumBlockSize, int numChannels)
{
    sampleRate_ = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (maximumBlockSize);
    spec.numChannels      = static_cast<juce::uint32> (numChannels);

    chorus_.prepare (spec);
    reverb_.prepare (spec);
    const auto maxDelaySamples = static_cast<std::size_t> (std::ceil (sampleRate_ * 4.0)) + 1u;
    delayLeft_.assign (maxDelaySamples, 0.0f);
    delayRight_.assign (maxDelaySamples, 0.0f);
    delaySamples_ = 1;
    reset();
    prepared_ = true;
}

void FxProcessor::reset()
{
    chorus_.reset();
    reverb_.reset();
    std::fill (delayLeft_.begin(), delayLeft_.end(), 0.0f);
    std::fill (delayRight_.begin(), delayRight_.end(), 0.0f);
    delayWriteIndex_ = 0;
}

void FxProcessor::process (juce::AudioBuffer<float>& buffer,
                           float chorusRateHz, float chorusDepth, float chorusMix,
                           float reverbRoom, float reverbDamp, float reverbMix,
                           float delayMix, float delayFeedback, float bpm) noexcept
{
    if (!prepared_ || buffer.getNumSamples() == 0)
        return;

    juce::dsp::AudioBlock<float> block (buffer);
    juce::dsp::ProcessContextReplacing<float> context (block);

    chorus_.setRate (chorusRateHz);
    chorus_.setDepth (chorusDepth);
    chorus_.setMix (chorusMix);
    chorus_.process (context);

    auto params = reverb_.getParameters();
    params.roomSize   = reverbRoom;
    params.damping    = reverbDamp;
    params.wetLevel   = reverbMix;
    params.dryLevel   = 1.0f - reverbMix;
    params.width      = 1.0f;
    params.freezeMode = 0.0f;
    reverb_.setParameters (params);
    reverb_.process (context);

    const float safeBpm = std::isfinite (bpm) && bpm > 0.0f ? bpm : 120.0f;
    const int maxDelay = static_cast<int> (delayLeft_.size());
    if (maxDelay <= 1)
        return;

    // Fixed dotted-eighth sync: 1/8D = three sixteenth notes = 45/BPM sec.
    const double requestedDelaySamples = sampleRate_ * 45.0 / static_cast<double> (safeBpm);
    const double boundedDelaySamples = std::clamp (requestedDelaySamples, 1.0,
                                                   static_cast<double> (maxDelay - 1));
    const int requestedDelay = static_cast<int> (std::lround (boundedDelaySamples));
    delaySamples_ = requestedDelay;
    const float wet = juce::jlimit (0.0f, 1.0f, std::isfinite (delayMix) ? delayMix : 0.0f);
    const float feedback = juce::jlimit (0.0f, 0.95f, std::isfinite (delayFeedback) ? delayFeedback : 0.35f);
    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const int readIndex = (delayWriteIndex_ - delaySamples_ + maxDelay) % maxDelay;
        const float inputL = std::isfinite (left[sample]) ? left[sample] : 0.0f;
        const float inputR = right != nullptr && std::isfinite (right[sample]) ? right[sample] : inputL;
        const float delayedL = delayLeft_[static_cast<std::size_t> (readIndex)];
        const float delayedR = delayRight_[static_cast<std::size_t> (readIndex)];

        // Cross-feed the delay lines so repeats alternate left/right.
        delayLeft_[static_cast<std::size_t> (delayWriteIndex_)] = inputL + delayedR * feedback;
        delayRight_[static_cast<std::size_t> (delayWriteIndex_)] = inputR + delayedL * feedback;
        left[sample] = inputL * (1.0f - wet) + delayedR * wet;
        if (right != nullptr)
            right[sample] = inputR * (1.0f - wet) + delayedL * wet;
        delayWriteIndex_ = (delayWriteIndex_ + 1) % maxDelay;
    }
}

} // namespace aod
