#include "BusProcessor.h"
#include <cmath>

namespace aod
{

void BusProcessor::prepare (double sampleRate, int maximumBlockSize, int numChannels)
{
    for (int factor = 0; factor < numFactors; ++factor)
    {
        oversamplers_[static_cast<std::size_t> (factor)] = std::make_unique<juce::dsp::Oversampling<float>> (
            static_cast<std::size_t> (numChannels), static_cast<std::size_t> (factor),
            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR);
        oversamplers_[static_cast<std::size_t> (factor)]->initProcessing (static_cast<std::size_t> (maximumBlockSize));
    }

    firOversampler_.prepare (maximumBlockSize, numChannels);

    dcBlockers_.assign (static_cast<std::size_t> (numChannels), x10::dsp::DCBlocker{});
    for (auto& blocker : dcBlockers_)
        blocker.prepare (sampleRate);
}

int BusProcessor::getLatencySamples (int osFactorIndex) const noexcept
{
    osFactorIndex = juce::jlimit (0, numChoices - 1, osFactorIndex);
    if (osFactorIndex >= numFactors)
        return static_cast<int> (std::lround (FirOversampler::getLatencyInSamples (osFactorIndex - numFactors + 1)));
    const auto& os = oversamplers_[static_cast<std::size_t> (osFactorIndex)];
    return os ? static_cast<int> (std::lround (os->getLatencyInSamples())) : 0;
}

void BusProcessor::reset()
{
    for (auto& os : oversamplers_)
        if (os)
            os->reset();
    firOversampler_.reset();
    for (auto& blocker : dcBlockers_)
        blocker.reset();
}

float BusProcessor::foldSample (float x, float amount) noexcept
{
    if (amount <= 0.0f)
        return x;

    const float k = 1.0f + amount * 4.0f;
    return std::sin (x * k * juce::MathConstants<float>::halfPi);
}

void BusProcessor::process (juce::AudioBuffer<float>& buffer, float tapeDrivePercent, float foldPercent,
                             int osFactorIndex) noexcept
{
    osFactorIndex = juce::jlimit (0, numChoices - 1, osFactorIndex);

    juce::dsp::AudioBlock<float> block (buffer);
    const bool fir = osFactorIndex >= numFactors;
    auto oversampledBlock = fir
        ? firOversampler_.processSamplesUp (block, osFactorIndex - numFactors + 1)
        : oversamplers_[static_cast<std::size_t> (osFactorIndex)]->processSamplesUp (block);

    const float driveAmount = tapeDrivePercent / 100.0f;
    const float driveGain = 1.0f + driveAmount * 4.0f;
    const float foldAmount = foldPercent / 100.0f;

    for (std::size_t ch = 0; ch < oversampledBlock.getNumChannels(); ++ch)
    {
        auto* data = oversampledBlock.getChannelPointer (ch);
        auto& dcBlocker = dcBlockers_[ch < dcBlockers_.size() ? ch : 0];

        for (std::size_t i = 0; i < oversampledBlock.getNumSamples(); ++i)
        {
            float sample = data[i];

            if (driveAmount > 0.0f)
            {
                sample = x10::dsp::curves::Tanh::f (sample * driveGain) / driveGain;
                sample = dcBlocker.process (sample);
            }

            sample = foldSample (sample, foldAmount);

            data[i] = sample;
        }
    }

    if (fir)
        firOversampler_.processSamplesDown (block);
    else
        oversamplers_[static_cast<std::size_t> (osFactorIndex)]->processSamplesDown (block);
}

} // namespace aod
