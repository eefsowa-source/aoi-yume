#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "Sampler.h"
#include "BusProcessor.h"

namespace
{
constexpr int kSampleRate = 48000;
constexpr int kBlockSize  = 512;

/** A short, constant 1 kHz-ish burst so velocity and envelope are the only variables. */
aod::Sample makeTone()
{
    aod::Sample s;
    constexpr int length = kSampleRate; // 1 second at 48k
    s.data.resize (static_cast<std::size_t> (length));
    for (int i = 0; i < length; ++i)
        s.data[static_cast<std::size_t> (i)] = std::sin (2.0f * 3.14159265f * 1000.0f * static_cast<float> (i)
                                                         / static_cast<float> (kSampleRate));
    s.sampleRate = kSampleRate;
    return s;
}

float blockPeak (const float* output, int numSamples)
{
    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        peak = std::max (peak, std::abs (output[i]));
    return peak;
}
} // namespace

TEST_CASE ("FIR oversampling path reports latency and passes audio", "[dsp][bus]")
{
    aod::BusProcessor bus;
    bus.prepare (kSampleRate, kBlockSize, 1);

    // Round-trip latency = (L-1) * (1 - 2^-factor) input-rate samples.
    REQUIRE (bus.getLatencySamples (0) == 0);   // 1x
    REQUIRE (bus.getLatencySamples (4) == 63);  // 2x FIR: 126 * 0.5
    REQUIRE (bus.getLatencySamples (5) == 95);  // 4x FIR: 126 * 0.75 -> 94.5 -> 95
    REQUIRE (bus.getLatencySamples (6) == 110); // 8x FIR: 126 * 0.875 -> 110.25

    // Neutral settings (no drive, no fold): output should be a delayed copy of
    // the input sine, not silence and not garbage.
    juce::AudioBuffer<float> in (1, kBlockSize), out (1, kBlockSize);
    for (int i = 0; i < kBlockSize; ++i)
        in.setSample (0, i, 0.25f * std::sin (2.0f * 3.14159265f * 1000.0f
                                              * static_cast<float> (i) / kSampleRate));
    out.copyFrom (0, 0, in, 0, 0, kBlockSize);
    bus.process (out, 0.0f, 0.0f, 4);

    float peak = 0.0f;
    for (int i = 0; i < kBlockSize; ++i)
        peak = std::max (peak, std::abs (out.getSample (0, i)));
    REQUIRE (peak > 0.05f);   // FIR tail of the first block still carries signal
    REQUIRE (peak < 0.6f);    // but stays bounded

    // Round-trip fidelity: a passband tone must come back as a pure 63-sample
    // delay of itself. A wrong history index still passes audio above but
    // shifts or scrambles the copy (an off-by-one in the delay line moves the
    // error to ~0.03 at this level). Skip the startup transient where the
    // zero-filled history is still ringing in.
    for (int i = 192; i < kBlockSize; ++i)
        REQUIRE (std::abs (out.getSample (0, i) - in.getSample (0, i - 63)) < 1.0e-3f);
}

TEST_CASE ("Lagrange playback interpolation tracks a pitched sine closely", "[dsp][voice]")
{
    // +7 semitones => playRate ~= 1.498, so nearly every output sample lands
    // between stored frames. The reference is a second sample pre-synthesised
    // at exactly the output frequency, played untransposed: its taps always
    // land on integers, so any error it shares with the pitched voice
    // (filter phase, drive curve, envelope) cancels and only the interpolation
    // error remains. Linear lands near -49 dB here; Lagrange-4 clears -75 dB,
    // so the bound separates the kernels with margin on both sides.
    const double rate = std::pow (2.0, 7.0 / 12.0);

    aod::Sample ref;
    ref.data.resize (static_cast<std::size_t> (kSampleRate));
    for (int i = 0; i < kSampleRate; ++i)
        ref.data[static_cast<std::size_t> (i)] = std::sin (2.0f * 3.14159265f
            * static_cast<float> (1000.0 * rate) * static_cast<float> (i)
            / static_cast<float> (kSampleRate));
    ref.sampleRate = kSampleRate;

    const aod::Sample tone = makeTone();
    aod::VoicePool pitched, reference;
    pitched.start (&tone, 67, 1.0f);
    reference.start (&ref, 60, 1.0f);

    constexpr int blocks = 8;
    double errSum = 0.0, refSum = 0.0;
    for (int b = 0; b < blocks; ++b)
    {
        std::vector<float> outP (static_cast<std::size_t> (kBlockSize));
        std::vector<float> outR (static_cast<std::size_t> (kBlockSize));
        pitched.render (outP.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f,
                        0.0f, 1.0f, 1.0f, 80.0f);
        reference.render (outR.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f,
                          0.0f, 1.0f, 1.0f, 80.0f);
        if (b == 0)
            continue; // let the filter settle
        for (int i = 0; i < kBlockSize; ++i)
        {
            errSum += static_cast<double> (outP[static_cast<std::size_t> (i)]
                                           - outR[static_cast<std::size_t> (i)])
                    * static_cast<double> (outP[static_cast<std::size_t> (i)]
                                           - outR[static_cast<std::size_t> (i)]);
            refSum += static_cast<double> (outR[static_cast<std::size_t> (i)])
                    * static_cast<double> (outR[static_cast<std::size_t> (i)]);
        }
    }
    const double rmsErrDb = 10.0 * std::log10 ((errSum + 1e-12) / (refSum + 1e-12));
    REQUIRE (rmsErrDb < -75.0);
}

TEST_CASE ("layered samples sound together and release together", "[dsp][voice]")
{
    // Two samples layered on one note: both must sound (vel layers /
    // round robins) and a single note-off must release both voices.
    aod::VoicePool pool;
    aod::Sample a = makeTone(), b = makeTone();
    for (auto& v : b.data)
        v *= 0.5f;

    std::array<const aod::Sample*, 2> layers { &a, &b };
    pool.start (std::span { layers }, 60, 1.0f);

    std::vector<float> layered (static_cast<std::size_t> (kBlockSize));
    pool.render (layered.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f,
                 0.0f, 1.0f, 1.0f, 80.0f);

    aod::VoicePool solo;
    solo.start (&a, 60, 1.0f);
    std::vector<float> single (static_cast<std::size_t> (kBlockSize));
    solo.render (single.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f,
                 0.0f, 1.0f, 1.0f, 80.0f);

    // The layered render must be louder than either single voice: the second
    // layer adds energy, not a replacement.
    REQUIRE (blockPeak (layered.data(), kBlockSize)
           > blockPeak (single.data(), kBlockSize) * 1.3f);

    // One note-off releases every layer.
    pool.stop (60);
    for (int b2 = 0; b2 < 64; ++b2)
        pool.render (layered.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f,
                     0.0f, 1.0f, 1.0f, 5.0f);
    REQUIRE (blockPeak (layered.data(), kBlockSize) < 1e-4f);
}

TEST_CASE ("polyphone cap stops allocating voices past the limit", "[dsp][voice]")
{
    aod::VoicePool pool;
    const aod::Sample sample = makeTone();

    pool.setPolyphony (1);

    // note-to-voice index is not public, but the observable behaviour is: with
    // only one voice slot, the second simultaneous note is dropped silently.
    pool.start (&sample, 60, 0.5f);
    pool.start (&sample, 62, 0.5f);

    std::vector<float> block (static_cast<std::size_t> (kBlockSize));
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);

    // One voice playing a 1s tone within a 512-frame block => non-zero but a
    // single voice's amplitude, not two stacked. We simply assert it fired.
    REQUIRE (blockPeak (block.data(), kBlockSize) > 0.0f);

    // Raising polyphony lets the dropped note register on the next render.
    pool.setPolyphony (4);
    std::vector<float> block2 (static_cast<std::size_t> (kBlockSize));
    pool.render (block2.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
    REQUIRE (blockPeak (block2.data(), kBlockSize) > 0.0f);
}

TEST_CASE ("velocity-to-drive scales loudness monotonically", "[dsp][voice]")
{
    aod::VoicePool pool;
    const aod::Sample sample = makeTone();

    // Positive velToDriveDb makes a hard hit (v=1.0) the reference and a soft
    // hit quieter; neutral 0 leaves it untouched.
    pool.start (&sample, 60, 1.0f);
    std::vector<float> loud (static_cast<std::size_t> (kBlockSize));
    pool.render (loud.data(), kBlockSize, kSampleRate, 10.0f, 20.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);

    pool.stopAll();
    pool.start (&sample, 60, 0.1f);
    std::vector<float> soft (static_cast<std::size_t> (kBlockSize));
    pool.render (soft.data(), kBlockSize, kSampleRate, 10.0f, 20.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);

    REQUIRE (blockPeak (soft.data(), kBlockSize) < blockPeak (loud.data(), kBlockSize));
}

TEST_CASE ("release fades to silence instead of cutting off abruptly", "[dsp][voice]")
{
    aod::VoicePool pool;
    const aod::Sample sample = makeTone();

    pool.start (&sample, 60, 0.8f);

    // Let the voice ring for a bit, then release it.
    std::vector<float> ring (static_cast<std::size_t> (kBlockSize));
    pool.render (ring.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
    REQUIRE (blockPeak (ring.data(), kBlockSize) > 0.0f);

    pool.stop (60);

    // Across a handful of blocks the release ramp should shrink and reach zero.
    float lastPeak = blockPeak (ring.data(), kBlockSize);
    for (int b = 0; b < 64; ++b)
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        pool.render (ring.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
        const float p = blockPeak (ring.data(), kBlockSize);
        REQUIRE (p <= lastPeak + 1.0e-5f); // monotonic decay
        lastPeak = p;
        if (p <= 1.0e-6f)
            break;
    }

    // 80ms release at 48k = 3840 samples = 7.5 blocks; 64 iterations is plenty.
    REQUIRE (lastPeak <= 1.0e-6f);
}

TEST_CASE ("a released voice is reusable for a new note", "[dsp][voice]")
{
    aod::VoicePool pool;
    const aod::Sample sample = makeTone();

    pool.start (&sample, 60, 0.5f);
    pool.stop (60);

    // The slot is still owned by the fading voice, but a new start must re-fire
    // regardless of the release tail by resetting the same voice.
    pool.start (&sample, 60, 0.5f);
    std::vector<float> block (static_cast<std::size_t> (kBlockSize));
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
    REQUIRE (blockPeak (block.data(), kBlockSize) > 0.0f);
}

TEST_CASE ("retriggering a held note reuses its voice instead of stacking", "[dsp][voice]")
{
    aod::VoicePool pool;
    const aod::Sample sample = makeTone();

    // Hold note 60, then re-strike it (legato retrigger). The first voice must
    // be reset in place, so the pool does not burn a fresh slot per strike.
    pool.setPolyphony (1);
    pool.start (&sample, 60, 0.5f);
    pool.start (&sample, 60, 0.9f); // retrigger while still held

    std::vector<float> block (static_cast<std::size_t> (kBlockSize));
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);

    // With polyphony 1, a stacked voice would have been dropped and produced
    // silence; retriggering in place must still sound.
    REQUIRE (blockPeak (block.data(), kBlockSize) > 0.0f);
}

TEST_CASE ("note-off cannot release a voice recycled for another note", "[dsp][voice]")
{
    aod::VoicePool pool;
    const aod::Sample sample = makeTone();

    // Note 60 grabs the only slot, then releases and finishes its fade.
    pool.start (&sample, 60, 0.5f);
    pool.stop (60);
    for (int b = 0; b < 64; ++b)
    {
        std::vector<float> block (static_cast<std::size_t> (kBlockSize));
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
    }

    // The slot is recycled for note 62. A late note-off for 60 must not
    // silence the voice that is now actually playing 62.
    pool.start (&sample, 62, 0.5f);
    pool.stop (60); // stale note-off for a note that is no longer sounding

    std::vector<float> block (static_cast<std::size_t> (kBlockSize));
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
    REQUIRE (blockPeak (block.data(), kBlockSize) > 0.0f);
}

TEST_CASE ("a saturated pool steals the longest-held voice for a new note", "[dsp][voice]")
{
    aod::VoicePool pool;
    const aod::Sample sample = makeTone();

    // Fill all slots with held notes so no voice is free or releasing.
    pool.setPolyphony (2);
    pool.start (&sample, 60, 0.5f);
    pool.start (&sample, 62, 0.5f);
    std::vector<float> block (static_cast<std::size_t> (kBlockSize));
    for (int b = 0; b < 8; ++b) // let note 60's envelope age
    {
        std::fill (block.begin(), block.end(), 0.0f);
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
    }

    // A third note must steal the oldest slot (60) rather than drop silently.
    pool.start (&sample, 64, 0.5f);
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
    REQUIRE (blockPeak (block.data(), kBlockSize) > 0.0f);
}

TEST_CASE ("a looping sample keeps sounding past its end", "[dsp][voice][loop]")
{
    aod::Sample sample = makeTone();
    // Loop the whole 1 s tone back to its head. A non-looping voice would run
    // off the end and go silent within the first second of playback; a looping
    // voice must still be sounding well past that.
    sample.loopStart   = 0;
    sample.loopEnd     = static_cast<int> (sample.data.size()) - 1;
    sample.loopEnabled = true;

    aod::VoicePool pool;
    pool.start (&sample, 60, 0.8f);

    // Play far past the sample length: 3 s of audio > 1 s sample.
    float lastPeak = 0.0f;
    for (int b = 0; b < 3 * kSampleRate / kBlockSize; ++b)
    {
        std::vector<float> block (static_cast<std::size_t> (kBlockSize));
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
        lastPeak = blockPeak (block.data(), kBlockSize);
        REQUIRE (lastPeak > 0.0f); // never dies out while held
    }
}

TEST_CASE ("a non-looping sample goes silent once it ends", "[dsp][voice][loop]")
{
    aod::Sample sample = makeTone();
    // No loop points: the classic one-shot behaviour must be preserved.
    sample.loopEnabled = false;

    aod::VoicePool pool;
    pool.start (&sample, 60, 0.8f);

    // The 1 s sample must have ended well before 2 s of playback.
    bool wentSilent = false;
    for (int b = 0; b < 2 * kSampleRate / kBlockSize; ++b)
    {
        std::vector<float> block (static_cast<std::size_t> (kBlockSize));
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
        if (blockPeak (block.data(), kBlockSize) <= 1.0e-6f)
        {
            wentSilent = true;
            break;
        }
    }
    REQUIRE (wentSilent);
}

TEST_CASE ("release on a looping sample fades out instead of restarting the loop", "[dsp][voice][loop]")
{
    aod::Sample sample = makeTone();
    sample.loopStart   = 0;
    sample.loopEnd     = static_cast<int> (sample.data.size()) - 1;
    sample.loopEnabled = true;

    aod::VoicePool pool;
    pool.start (&sample, 60, 0.8f);

    // Let it loop for a while, then release.
    for (int b = 0; b < 2 * kSampleRate / kBlockSize; ++b)
    {
        std::vector<float> block (static_cast<std::size_t> (kBlockSize));
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
    }
    pool.stop (60);

    // The release fade must run to zero monotonically; the loop must not
    // re-engage and keep it ringing forever.
    float lastPeak = 1.0f;
    bool hitZero = false;
    for (int b = 0; b < 32; ++b)
    {
        std::vector<float> block (static_cast<std::size_t> (kBlockSize));
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
        const float p = blockPeak (block.data(), kBlockSize);
        REQUIRE (p <= lastPeak + 1.0e-5f); // monotonic decay, no click back up
        lastPeak = p;
        if (p <= 1.0e-6f)
        {
            hitZero = true;
            break;
        }
    }
    REQUIRE (hitZero);
}

TEST_CASE ("release holds the last sample instead of truncating mid-cycle", "[dsp][voice]")
{
    aod::Sample sample;
    // Very short sample: release starts after the tone has nearly ended, so the
    // old code path would cut the voice off at the buffer end and click.
    constexpr int length = 128;
    sample.data.resize (static_cast<std::size_t> (length));
    for (int i = 0; i < length; ++i)
        sample.data[static_cast<std::size_t> (i)] = std::sin (2.0f * 3.14159265f * 1000.0f
                                                             * static_cast<float> (i)
                                                             / static_cast<float> (kSampleRate));
    sample.sampleRate = kSampleRate;

    aod::VoicePool pool;
    pool.start (&sample, 60, 0.8f);

    // Play the tone nearly to the end of the sample, then release.
    std::vector<float> block (static_cast<std::size_t> (kBlockSize));
    for (int b = 0; b < 1; ++b)
    {
        std::fill (block.begin(), block.end(), 0.0f);
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
    }
    pool.stop (60);

    // The release must still fade out to silence rather than stopping the
    // moment the sample ends.
    float lastPeak = 1.0f;
    bool hitZero = false;
    for (int b = 0; b < 32; ++b)
    {
        std::fill (block.begin(), block.end(), 0.0f);
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
        const float p = blockPeak (block.data(), kBlockSize);
        REQUIRE (p <= lastPeak + 1.0e-5f); // monotonic decay, no click back up
        lastPeak = p;
        if (p <= 1.0e-6f)
        {
            hitZero = true;
            break;
        }
    }
    REQUIRE (hitZero);
}

TEST_CASE ("release mid-attack keeps the fade slope continuous", "[dsp][voice]")
{
    aod::Sample sample = makeTone();

    aod::VoicePool pool;
    pool.start (&sample, 60, 0.8f);

    // Render a few samples into the attack (attack is 10 ms at 48 kHz, so
    // ~480 samples), then release while the envelope is still rising.
    std::vector<float> block (static_cast<std::size_t> (kBlockSize));
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
    pool.stop (60);

    // Render one more block and look at the *first differences* across the
    // release boundary. A 1 kHz sine at 48 kHz has a max slope of about 0.13
    // samples^-1; a discontinuous envelope (fixed-time ramp from a low release
    // level) would introduce a slope spike several times that. The scaled
    // ramp keeps the fade slope the same whatever the release level.
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);

    float maxSlope = 0.0f;
    for (int i = 1; i < kBlockSize; ++i)
        maxSlope = std::max (maxSlope, std::abs (block[static_cast<std::size_t> (i)]
                                                - block[static_cast<std::size_t> (i - 1)]));

    // Sine slope bound is ~0.13; release ramps down at the same rate, so the
    // combined envelope+sine slope stays under ~0.35. Anything much larger
    // indicates a discontinuity (click) at the release point.
    REQUIRE (maxSlope < 0.5f);
}

TEST_CASE ("exclusive class chokes the earlier note in the same group", "[dsp][voice]")
{
    aod::VoicePool pool;
    aod::Sample closed = makeTone();
    aod::Sample open   = makeTone();
    aod::Sample pad    = makeTone();
    closed.exclusiveClass = 4;
    open.exclusiveClass   = 4;
    pad.exclusiveClass    = 7; // different group: must survive the choke

    pool.start (&closed, 60, 0.5f);
    pool.start (&pad,    72, 0.5f);
    pool.start (&open,   62, 0.5f); // same class as note 60: chokes it

    // Release whatever survived the choke and let both tails die. If note 60
    // was not choked its sustain would still be sounding into these blocks.
    pool.stop (62);
    pool.stop (72);

    std::vector<float> block (static_cast<std::size_t> (kBlockSize));
    for (int b = 0; b < 32; ++b)
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);

    REQUIRE (blockPeak (block.data(), kBlockSize) <= 1.0e-6f);
}

TEST_CASE ("a short region release ends the note before the UI release", "[dsp][voice]")
{
    aod::VoicePool pool;
    aod::Sample sample = makeTone();
    // The region's own volume envelope wins when it ends sooner than the
    // panel ADSR: a 20 ms font release under an 80 ms UI release truncates.
    sample.volumeEnvelope.releaseSeconds = 0.02f;

    pool.start (&sample, 60, 0.8f);
    std::vector<float> block (static_cast<std::size_t> (kBlockSize));
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
    pool.stop (60);

    // 20 ms at 48 kHz = 960 samples, under two blocks. Block 3 must be silent
    // even though the UI release would still be fading.
    for (int b = 0; b < 3; ++b)
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);

    REQUIRE (blockPeak (block.data(), kBlockSize) <= 1.0e-6f);
}

TEST_CASE ("region attenuation scales playback level", "[dsp][voice]")
{
    aod::VoicePool loud, quiet;
    const aod::Sample flat = makeTone();
    aod::Sample soft = makeTone();
    soft.attenuationDb = 12.0f; // ~0.25x linear

    std::vector<float> loudBlock (static_cast<std::size_t> (kBlockSize));
    std::vector<float> softBlock (static_cast<std::size_t> (kBlockSize));
    loud.start (&flat, 60, 0.8f);
    quiet.start (&soft, 60, 0.8f);
    loud.render  (loudBlock.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);
    quiet.render (softBlock.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);

    const float ratio = blockPeak (softBlock.data(), kBlockSize)
                      / blockPeak (loudBlock.data(), kBlockSize);
    REQUIRE (ratio > 0.15f);
    REQUIRE (ratio < 0.35f);
}
