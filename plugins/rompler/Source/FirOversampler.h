#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>

#include "FirHalfbandCoeffs.h"

namespace aod
{

/**
    Linear-phase oversampler built on a least-squares halfband FIR
    (tools/design_halfband.py). JUCE's dsp::Oversampling only ships its own
    filters, so the FIR path lives here.

    The halfband's odd taps are all zero except the centre, which splits each
    2x stage into a 64-tap FIR (even phase) plus an integer delay (odd phase).
    2x/4x/8x use one to three cascaded stages, selected at block time; all
    buffers are allocated in prepare() so the audio path never allocates.

    Latency per stage s (input rate 2^s) is (L-1)/2 = 63 samples of that rate
    each way, i.e. 126 / 2^s input-rate samples round trip. Reported latency
    is the sum over active stages, matching how dsp::Oversampling reports
    getLatencyInSamples().
*/
class FirOversampler
{
public:
    /** factorIndex: 1=2x, 2=4x, 3=8x. */
    void prepare (int maximumBlockSize, int numChannels)
    {
        stages_.clear();
        for (int s = 0; s < kMaxStages; ++s)
        {
            Stage st;
            const int levelSize = maximumBlockSize << (s + 1);
            st.level.setSize (numChannels, levelSize, false, true, true);
            st.upHist.assign (static_cast<std::size_t> (numChannels), {});
            st.downEven.assign (static_cast<std::size_t> (numChannels), {});
            st.downOdd.assign (static_cast<std::size_t> (numChannels), {});
            st.pos.assign (static_cast<std::size_t> (numChannels), 0);
            stages_.push_back (std::move (st));
        }
    }

    void reset() noexcept
    {
        for (auto& st : stages_)
            for (std::size_t ch = 0; ch < st.upHist.size(); ++ch)
            {
                st.upHist[ch].fill (0.0f);
                st.downEven[ch].fill (0.0f);
                st.downOdd[ch].fill (0.0f);
                st.pos[ch] = 0;
            }
    }

    /** Round-trip latency in input-rate samples for 1..3 stages. */
    static float getLatencyInSamples (int factorIndex) noexcept
    {
        return static_cast<float> (halfband::kTaps - 1)
             * (1.0f - std::pow (0.5f, static_cast<float> (factorIndex)));
    }

    /** Returns a block at 2^factorIndex times the input rate. */
    juce::dsp::AudioBlock<float> processSamplesUp (const juce::dsp::AudioBlock<float>& block,
                                                   int factorIndex) noexcept
    {
        activeFactor_ = juce::jlimit (1, kMaxStages, factorIndex);

        const auto* source = &block;
        for (int s = 0; s < activeFactor_; ++s)
        {
            upStage (stages_[static_cast<std::size_t> (s)], *source);
            source = &stages_[static_cast<std::size_t> (s)].levelBlock;
        }
        return stages_[static_cast<std::size_t> (activeFactor_ - 1)].levelBlock;
    }

    /** Decimates the top-level block back into the original-rate block. */
    void processSamplesDown (const juce::dsp::AudioBlock<float>& block) noexcept
    {
        for (int s = activeFactor_ - 1; s > 0; --s)
            downStage (stages_[static_cast<std::size_t> (s)],
                       stages_[static_cast<std::size_t> (s - 1)].levelBlock);
        downStage (stages_[0], block);
    }

private:
    static constexpr int kMaxStages = 3;

    struct Stage
    {
        juce::AudioBuffer<float> level;
        juce::dsp::AudioBlock<float> levelBlock;  // rebound lazily in upStage
        std::vector<std::array<float, halfband::kEvenTaps>> upHist;
        std::vector<std::array<float, halfband::kEvenTaps>> downEven;
        std::vector<std::array<float, halfband::kEvenTaps>> downOdd;
        std::vector<int> pos;
    };

    void upStage (Stage& st, const juce::dsp::AudioBlock<float>& in) noexcept
    {
        const auto numSamples = in.getNumSamples();
        const auto numChannels = juce::jmin (in.getNumChannels(),
                                             static_cast<std::size_t> (st.level.getNumChannels()));
        st.levelBlock = juce::dsp::AudioBlock<float> (st.level).getSubBlock (0, numSamples * 2);

        for (std::size_t ch = 0; ch < numChannels; ++ch)
        {
            const auto* src = in.getChannelPointer (ch);
            auto* dst = st.levelBlock.getChannelPointer (ch);
            auto& hist = st.upHist[ch];
            int pos = st.pos[ch];

            for (std::size_t n = 0; n < numSamples; ++n)
            {
                hist[static_cast<std::size_t> (pos)] = src[n];
                pos = (pos + 1) & 63;

                float even = 0.0f;
                for (int j = 0; j < halfband::kEvenTaps; ++j)
                    even += halfband::kEven[static_cast<std::size_t> (j)]
                          * hist[static_cast<std::size_t> ((pos - 1 - j) & 63)];

                // Polyphase: y[2n] is the even-tap convolution (x2 gain folded
                // in), y[2n+1] is the centre tap = a pure 31-sample delay.
                dst[n * 2]     = 2.0f * even;
                dst[n * 2 + 1] = hist[static_cast<std::size_t> ((pos - 32) & 63)];
            }
            st.pos[ch] = pos;
        }
    }

    void downStage (Stage& st, const juce::dsp::AudioBlock<float>& out) noexcept
    {
        // st.level holds the 2x-rate block; out is the input-rate block.
        const auto numSamples = out.getNumSamples();
        const auto numChannels = juce::jmin (out.getNumChannels(),
                                             static_cast<std::size_t> (st.level.getNumChannels()));

        for (std::size_t ch = 0; ch < numChannels; ++ch)
        {
            const auto* src = st.level.getReadPointer (static_cast<int> (ch));
            auto* dst = out.getChannelPointer (ch);
            auto& ev = st.downEven[ch];
            auto& od = st.downOdd[ch];
            int pos = st.pos[ch];

            for (std::size_t n = 0; n < numSamples; ++n)
            {
                ev[static_cast<std::size_t> (pos)] = src[n * 2];
                od[static_cast<std::size_t> (pos)] = src[n * 2 + 1];
                pos = (pos + 1) & 63;

                float even = 0.0f;
                for (int j = 0; j < halfband::kEvenTaps; ++j)
                    even += halfband::kEven[static_cast<std::size_t> (j)]
                          * ev[static_cast<std::size_t> ((pos - 1 - j) & 63)];

                // Odd phase reduces to h[63] = 0.5 acting on w[2n-63], which is
                // the odd sample 32 positions back in odd-sample indexing.
                dst[n] = even + 0.5f * od[static_cast<std::size_t> ((pos - 33) & 63)];
            }
            st.pos[ch] = pos;
        }
    }

    std::vector<Stage> stages_;
    int activeFactor_ = 1;
};

} // namespace aod
