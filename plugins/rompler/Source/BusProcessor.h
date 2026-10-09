#pragma once

#include <juce_dsp/juce_dsp.h>
#include <x10/dsp/nonlinear/Curves.h>
#include <x10/dsp/filter/DCBlocker.h>
#include <array>
#include <memory>

#include "FirOversampler.h"

namespace aod
{

/**
    Master bus nonlinear stage: tape-style saturation, then a wavefolder,
    both run inside an oversampled block to keep their aliasing under control.

    Oversampling factor is chosen per block from the Oversampling parameter.
    Choices 0-3 (1x/2x/4x/8x) run JUCE's halfband polyphase IIR; choices 4-6
    (2x/4x/8x FIR) run the linear-phase least-squares halfband in
    FirOversampler. All engines are built once in prepare() so switching
    factors mid-stream never allocates on the audio thread; only the host
    latency notification changes, and only when the factor actually changes.
*/
class BusProcessor
{
public:
    void prepare (double sampleRate, int maximumBlockSize, int numChannels);
    void reset();

    /**
        tapeDrivePercent, foldPercent: 0-100, straight from APVTS.
        osFactorIndex: 0=1x, 1=2x, 2=4x, 3=8x (IIR), 4=2x, 5=4x, 6=8x (FIR).

        The oversampling factor is fixed for the lifetime of a prepareToPlay
        session: reading it here would mean a possible mid-block change in
        latency, which can only be reported via updateHostDisplay() — a
        message-thread call that has no business on the audio thread. The
        processor reads the parameter once in prepareToPlay(), calls
        setLatencySamples() there, and passes the same index into every
        process() call until the next prepareToPlay().
    */
    void process (juce::AudioBuffer<float>& buffer, float tapeDrivePercent, float foldPercent,
                  int osFactorIndex) noexcept;

    [[nodiscard]] int getLatencySamples (int osFactorIndex) const noexcept;

private:
    static constexpr int numFactors = 4;       // IIR choices 0..3
    static constexpr int numChoices = 7;       // total Oversampling choices

    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, numFactors> oversamplers_;
    FirOversampler firOversampler_;
    std::vector<x10::dsp::DCBlocker> dcBlockers_;

    [[nodiscard]] static float foldSample (float x, float amount) noexcept;
};

} // namespace aod
