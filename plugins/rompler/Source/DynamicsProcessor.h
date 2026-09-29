#pragma once

#include <atomic>
#include <vector>

#include <juce_dsp/juce_dsp.h>

namespace aod
{

/**
    A zero-latency, feed-forward compressor for the master signal path.

    The detector is stereo linked: it follows the larger absolute value of
    the left and right inputs, then applies one gain value to both channels.
    This keeps a loud event on one side from shifting the stereo image. The
    processor owns no per-block storage, so process() is allocation-free.
*/
class DynamicsProcessor
{
public:
    void prepare (double sampleRate, int maximumBlockSize, int numChannels);
    void reset();

    /**
        Process one block in place.

        thresholdDb is the point above which gain reduction begins, ratio is
        the usual input:output ratio, and attackMs/releaseMs smooth the gain
        reduction envelope. makeupDb is applied to the compressed signal
        before it is blended with the dry signal. mixPercent is 0-100.
    */
    void process (juce::AudioBuffer<float>& buffer,
                  float thresholdDb, float ratio,
                  float attackMs, float releaseMs,
                  float makeupDb, float mixPercent) noexcept;

    /** Current linked gain reduction, reported as a positive dB value. */
    [[nodiscard]] float getLastGainReductionDb() const noexcept;

private:
    double sampleRate_ = 44100.0;
    int preparedChannels_ = 0;
    bool prepared_ = false;

    // Stored in dB as a non-positive value: 0 dB is unity and -6 dB is six
    // decibels of reduction. The atomic mirror is read by the editor thread.
    float gainReductionDb_ = 0.0f;
    std::atomic<float> lastGainReductionDb_ { 0.0f };
    float evenHarmonicDcCoefficient_ = 0.9995f;
    // Per-channel running mean of the squared signal used to remove the DC
    // component introduced by the asymmetric (even-harmonic) stage.
    std::vector<float> evenHarmonicDc_;
};

} // namespace aod
