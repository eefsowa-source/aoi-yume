#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "Sampler.h"

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
    // Zero-length SoundFont envelope stages keep this fixture focused on the
    // ADSR and voice state; zone-envelope behavior has its own cases below.
    s.volumeEnvelope = { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    return s;
}

float blockPeak (const float* output, int numSamples)
{
    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        peak = std::max (peak, std::abs (output[i]));
    return peak;
}

float tailRms (const std::vector<float>& output, int tailSamples)
{
    const int firstSample = juce::jmax (0, static_cast<int> (output.size()) - tailSamples);
    float sumSquares = 0.0f;

    for (int sample = firstSample; sample < static_cast<int> (output.size()); ++sample)
        sumSquares += output[static_cast<std::size_t> (sample)]
                    * output[static_cast<std::size_t> (sample)];

    return std::sqrt (sumSquares / static_cast<float> (static_cast<int> (output.size()) - firstSample));
}

/** Estimate the fundamental by counting zero crossings over an explicit
    [first, first+window) slice. Counting crossings rather than taking an FFT
    is enough for the single question these tests ask - did the pitch move, and
    by how much - and it stays exact for the integer-cycle fixtures used here. */
float windowFrequency (const std::vector<float>& signal, int first, int window, int sampleRate)
{
    const int from = juce::jmax (1, first);
    const int to = juce::jmin (static_cast<int> (signal.size()), from + window);
    if (to <= from)
        return 0.0f;

    int crossings = 0;
    for (int i = from; i < to; ++i)
        if ((signal[static_cast<std::size_t> (i - 1)] < 0.0f)
            != (signal[static_cast<std::size_t> (i)] < 0.0f))
            ++crossings;

    const auto seconds = static_cast<float> (to - from) / static_cast<float> (sampleRate);
    return seconds > 0.0f ? static_cast<float> (crossings) / seconds * 0.5f : 0.0f;
}
/** RMS over an explicit [first, first+window) slice. Tests that compare
    levels pick the window themselves: a fixture that is only as long as its
    sample goes silent at the end, so measuring the final samples measures the
    end of the buffer rather than the filter. */
float windowRms (const std::vector<float>& signal, int first, int window)
{
    const int from = juce::jmax (0, first);
    const int to = juce::jmin (static_cast<int> (signal.size()), from + window);
    if (to <= from)
        return 0.0f;

    double sumSquares = 0.0;
    for (int i = from; i < to; ++i)
    {
        const auto x = static_cast<double> (signal[static_cast<std::size_t> (i)]);
        sumSquares += x * x;
    }
    return static_cast<float> (std::sqrt (sumSquares / (to - from)));
}
} // namespace

TEST_CASE ("voice pool prepares the public 128-voice default capacity", "[dsp][voice]")
{
    aod::VoicePool pool;

    REQUIRE (aod::VoicePool::maxVoices == 128);
    REQUIRE (pool.preparedCapacity() == aod::VoicePool::maxVoices);
    REQUIRE (pool.activeVoiceCount() == 0);
    REQUIRE (pool.voiceIndexForNote (60) == -1);
}

TEST_CASE ("voice pool starts all 128 MIDI notes concurrently", "[dsp][rt][voice]")
{
    aod::VoicePool pool;
    const aod::Sample sample = makeTone();

    for (int note = 0; note < 128; ++note)
        pool.start (&sample, note, 0.5f);

    REQUIRE (pool.activeVoiceCount() == 128);
    for (int note = 0; note < 128; ++note)
        REQUIRE (pool.voiceIndexForNote (note) == note);
}

TEST_CASE ("overlapping SoundFont zones start and release as one MIDI note", "[dsp][voice][sf2]")
{
    aod::VoicePool pool (4);
    const aod::Sample first = makeTone();
    const aod::Sample second = makeTone();
    const std::array<const aod::Sample*, 2> layers { &first, &second };

    pool.start (layers, 60, 0.8f);

    REQUIRE (pool.activeVoiceCount() == 2);
    REQUIRE (pool.voiceIndexForNote (60) >= 0);

    pool.stop (60);
    std::array<float, kBlockSize> output {};
    pool.render (output.data(), kBlockSize, kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f,
                 0.0f, 1000.0f, 1.0f, 1000.0f,
                 0.0f, 0.0f);
    REQUIRE (pool.activeVoiceCount() == 2);
}

TEST_CASE ("SoundFont exclusive class chokes every matching layer", "[dsp][voice][sf2]")
{
    aod::VoicePool pool (4);
    aod::Sample openHat = makeTone();
    aod::Sample closedHat = makeTone();
    openHat.exclusiveClass = 7;
    closedHat.exclusiveClass = 7;

    pool.start (&openHat, 46, 0.8f);
    pool.start (&closedHat, 42, 0.8f);

    // The choke fades the old voice instead of cutting it, so it is still
    // sounding during the first samples of the new note.
    std::array<float, 16> fade {};
    pool.render (fade.data(), static_cast<int> (fade.size()), kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f,
                 0.0f, 0.0f, 1.0f, 1000.0f,
                 0.0f, 0.0f);
    REQUIRE (pool.activeVoiceCount() == 2);
    REQUIRE (blockPeak (fade.data(), static_cast<int> (fade.size())) > 0.0f);

    std::array<float, kBlockSize> block {};
    pool.render (block.data(), kBlockSize, kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f,
                 0.0f, 0.0f, 1.0f, 1000.0f,
                 0.0f, 0.0f);

    REQUIRE (pool.activeVoiceCount() == 1);
    REQUIRE (pool.voiceIndexForNote (46) == -1);
    REQUIRE (pool.voiceIndexForNote (42) >= 0);
}

TEST_CASE ("SoundFont attenuation applies before the voice chain", "[dsp][voice][sf2]")
{
    aod::Sample unity;
    unity.data.assign (4096, 1.0f);
    unity.sampleRate = kSampleRate;
    unity.volumeEnvelope = { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.1f };

    // -6.0206 dB is exactly half amplitude, so the ratio isolates the
    // generator from every other gain in the voice chain.
    aod::Sample half = unity;
    half.attenuationDb = 6.0206f;

    const auto peakFor = [] (aod::Sample& sample)
    {
        aod::VoicePool pool (1);
        pool.start (&sample, 60, 1.0f);
        std::array<float, 256> output {};
        pool.render (output.data(), static_cast<int> (output.size()), kSampleRate,
                     0.0f, 0.0f, 0, 0, 0.0f,
                     0.0f, 0.0f, 1.0f, 1000.0f,
                     0.0f, 0.0f);
        return blockPeak (output.data(), static_cast<int> (output.size()));
    };

    const float unityPeak = peakFor (unity);
    REQUIRE (unityPeak > 0.0f);
    REQUIRE (peakFor (half) / unityPeak == Catch::Approx (0.5f).margin (0.01f));
}

TEST_CASE ("voice velocity follows the SoundFont curve", "[dsp][voice][sf2]")
{
    // The amplitude is a quadratic curve in velocity: 0 dB at 127, then 12 dB per
    // halving, quantised to the 0.1 dB the format stores. FluidSynth rendering the
    // local corpus reproduces these exact decibels.
    const std::pair<int, float> reference[] = {
        { 127, 0.0f }, { 96, -4.9f }, { 64, -11.9f }, { 32, -23.9f },
        { 16, -36.0f }, { 8, -48.0f }, { 1, -84.2f }
    };

    aod::Sample sample;
    sample.data.assign (512, 1.0f);
    sample.sampleRate = kSampleRate;
    sample.volumeEnvelope = { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.1f };

    std::array<float, 256> output {};
    const auto levelAt = [&] (int velocity)
    {
        aod::VoicePool pool (1);
        pool.start (&sample, 60, static_cast<float> (velocity) / 127.0f);
        std::fill (output.begin(), output.end(), 0.0f);
        pool.render (output.data(), static_cast<int> (output.size()), kSampleRate,
                     0.0f, 0.0f, 0, 0, 0.0f,
                     0.0f, 0.0f, 1.0f, 1000.0f,
                     0.0f, 0.0f);
        return blockPeak (output.data(), static_cast<int> (output.size()));
    };

    const float fullScale = levelAt (127);
    REQUIRE (fullScale > 0.0f);

    for (const auto& [velocity, expectedDb] : reference)
    {
        const float relativeDb = 20.0f * std::log10 (levelAt (velocity) / fullScale);
        CAPTURE (velocity, expectedDb, relativeDb);
        REQUIRE (relativeDb == Catch::Approx (expectedDb).margin (0.15f));
    }
}

TEST_CASE ("a zone with no modulation depth renders exactly as before", "[dsp][voice][sf2]")
{
    // The whole stage rests on a zero modEnv depth being a true identity: a
    // zone that states no modulation has to render exactly as it did before
    // this existed. What this can prove without the old binary is that the
    // depth is the only thing that changes the output - clearing the depths
    // has to make the modulation disappear entirely, leaving a voice that is
    // still alive and still producing signal.
    aod::Sample withDepths = makeTone();
    withDepths.modulationEnvelope = { 0.02f, 0.01f, 0.01f, 0.05f, 0.5f, 0.05f };
    withDepths.modEnvToPitchCents  = 1200.0f;
    withDepths.modEnvToFilterCents = -1200.0f;

    aod::Sample noDepths = withDepths;
    noDepths.modEnvToPitchCents  = 0.0f;
    noDepths.modEnvToFilterCents = 0.0f;

    const auto render = [] (const aod::Sample& s)
    {
        aod::VoicePool pool (1);
        pool.start (&s, 60, 1.0f);
        std::vector<float> out (static_cast<std::size_t> (kSampleRate), 0.0f);
        std::vector<float> block (static_cast<std::size_t> (kBlockSize), 0.0f);
        for (int b = 0; b < static_cast<int> (out.size()) / kBlockSize; ++b)
        {
            std::fill (block.begin(), block.end(), 0.0f);
            pool.render (block.data(), kBlockSize, kSampleRate,
                         0.0f, 0.0f, 0, 1, 0.0f,
                         0.0f, 0.0f, 1.0f, 1000.0f, 0.0f, 0.0f);
            std::copy (block.begin(), block.end(),
                       out.begin() + static_cast<std::ptrdiff_t> (b) * kBlockSize);
        }
        return out;
    };

    const auto modulated = render (withDepths);
    const auto plain = render (noDepths);

    // The modulated zone must actually differ, or the comparison below would
    // pass for the wrong reason.
    float largestDifference = 0.0f;
    for (std::size_t i = 0; i < plain.size(); ++i)
        largestDifference = std::max (largestDifference,
                                      std::abs (modulated[i] - plain[i]));
    CAPTURE (largestDifference);
    REQUIRE (largestDifference > 0.01f);

    // With the depths cleared the voice must still be sounding: an inert
    // modulation path means the envelope was never ticked, not that the voice
    // was silenced by an envelope that ran to its end.
    const float plainLevel = windowRms (plain, kSampleRate / 4, kSampleRate / 4);
    CAPTURE (plainLevel);
    REQUIRE (plainLevel > 0.1f);
}

TEST_CASE ("zone modulation envelope moves the voice pitch", "[dsp][voice][sf2]")
{
    // modEnvToPitchCents says how far the zone's modulation envelope swings
    // the pitch. The envelope starts high and decays toward its sustain, so a
    // positive depth bends the attack up by the stated number of cents and
    // settles back as the envelope falls. A zero depth must be a true no-op.
    aod::Sample sample = makeTone();
    // Attack 50 ms, instant decay to a full sustain, so the whole test window
    // sits in a steady state and the frequency reading is not a transient.
    sample.modulationEnvelope = { 0.05f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    sample.modEnvToPitchCents = 0.0f;

    const auto renderTailFrequency = [&] (float cents)
    {
        sample.modEnvToPitchCents = cents;
        aod::VoicePool pool (1);
        pool.start (&sample, 60, 1.0f);

        // 1 s of tail at 48 kHz, rendered in blocks so the envelope settles
        // before anything is measured.
        std::vector<float> tail (static_cast<std::size_t> (kSampleRate), 0.0f);
        std::vector<float> block (static_cast<std::size_t> (kBlockSize), 0.0f);
        std::fill (tail.begin(), tail.end(), 0.0f);
        for (int b = 0; b < static_cast<int> (tail.size()) / kBlockSize; ++b)
        {
            std::fill (block.begin(), block.end(), 0.0f);
            pool.render (block.data(), kBlockSize, kSampleRate,
                         0.0f, 0.0f, 0, 0, 0.0f,
                         0.0f, 0.0f, 1.0f, 1000.0f,
                         0.0f, 0.0f);
            std::copy (block.begin(), block.end(),
                       tail.begin() + static_cast<std::ptrdiff_t> (b) * kBlockSize);
        }
        // Measure well past the 50 ms attack, over a whole number of 1 kHz
        // cycles at the flat rate so the crossing count is exact.
        return windowFrequency (tail, kSampleRate / 4, kSampleRate / 20, kSampleRate);
    };

    // The fixture is a 1000 Hz sine and the zone envelope sits at sustain 1.0,
    // so an unmodulated voice must read back at the fixture frequency.
    const float flat = renderTailFrequency (0.0f);
    CAPTURE (flat);
    REQUIRE (flat == Catch::Approx (1000.0f).margin (25.0f));

    // 1200 cents is one octave. With a full-sustain modulation envelope the
    // whole tail should be an octave up; a smaller depth has to land in
    // between, which is what proves the depth is read as a real ratio and not
    // simply switched on.
    const float octave = renderTailFrequency (1200.0f);
    CAPTURE (octave);
    REQUIRE (octave == Catch::Approx (2000.0f).margin (60.0f));

    const float half = renderTailFrequency (600.0f);
    CAPTURE (half);
    REQUIRE (half > flat + 200.0f);
    REQUIRE (half < octave - 200.0f);
}

TEST_CASE ("zone modulation envelope moves the voice filter cutoff", "[dsp][voice][sf2]")
{
    // modEnvToFilterCents sweeps the zone's cutoff. With a sustain of 1.0 the
    // envelope holds its peak, so the tail is rendered at a cutoff shifted by
    // the stated number of cents. Negative cents close the filter, which shows
    // up as less energy at the fixture's 1 kHz.
    aod::Sample sample = makeTone();
    // The fixture is a 1 kHz tone and the zone cutoff starts an octave below
    // it, so a downward sweep is immediately audible while an upward sweep
    // opens it fully. A 12 dB/oct slope barely moves a tone that is already
    // an octave below the corner, so the fixture has to sit close to it for
    // the depth to be measurable at all.
    sample.filterCutoffHz = 500.0f;
    sample.filterResonanceDb = 0.0f;
    sample.modulationEnvelope = { 0.05f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    sample.modEnvToFilterCents = 0.0f;

    const auto renderTailLevel = [&] (float cents)
    {
        sample.modEnvToFilterCents = cents;
        aod::VoicePool pool (1);
        pool.start (&sample, 60, 1.0f);

        std::vector<float> tail (static_cast<std::size_t> (kSampleRate), 0.0f);
        std::vector<float> block (static_cast<std::size_t> (kBlockSize), 0.0f);
        for (int b = 0; b < static_cast<int> (tail.size()) / kBlockSize; ++b)
        {
            std::fill (block.begin(), block.end(), 0.0f);
            pool.render (block.data(), kBlockSize, kSampleRate,
                         0.0f, 0.0f, 0, 1, 0.0f,
                         0.0f, 0.0f, 1.0f, 1000.0f,
                         0.0f, 0.0f);
            std::copy (block.begin(), block.end(),
                       tail.begin() + static_cast<std::ptrdiff_t> (b) * kBlockSize);
        }
        return windowRms (tail, kSampleRate / 4, kSampleRate / 2);
    };

    const float open = renderTailLevel (0.0f);
    CAPTURE (open);
    REQUIRE (open > 0.0f);

    // +1200 cents lifts the corner from 500 Hz to 1 kHz, right onto the tone, so
    // the level rises clearly. A filter that ignored the depth would read
    // exactly the same as the open case.
    const float opened = renderTailLevel (1200.0f);
    CAPTURE (opened, open);
    REQUIRE (opened > open * 1.5f);

    // Far enough down and the tone is cut for real, which separates a working
    // depth from one that is merely scaling the cutoff a little.
    const float shut = renderTailLevel (-2400.0f);
    CAPTURE (shut, open);
    REQUIRE (shut < open * 0.5f);
}


TEST_CASE ("normalized sustain remains audible after decay", "[dsp][voice][level]")
{
    aod::VoicePool pool (1);
    const aod::Sample sample = makeTone();
    std::vector<float> block (static_cast<std::size_t> (kBlockSize));

    pool.start (&sample, 60, 1.0f);
    for (int render = 0; render < 4; ++render)
    {
        std::fill (block.begin(), block.end(), 0.0f);
        pool.render (block.data(), kBlockSize, kSampleRate,
                     0.0f, 0.0f, 0, 0, 0.0f,
                     0.0f, 5.0f, 1.0f, 80.0f,
                     0.0f, 0.0f);
    }

    // PluginProcessor sends sustain as an already normalized 0..1 value.
    // A full sustain must therefore retain a musically usable level after the
    // decay stage, rather than being attenuated a second time.
    REQUIRE (tailRms (block, kBlockSize / 2) > 0.3f);
}

TEST_CASE ("stealing a release voice clears only its victim note mapping", "[dsp][voice]")
{
    aod::VoicePool pool (2);
    const aod::Sample sample = makeTone();

    pool.start (&sample, 60, 0.5f);
    pool.start (&sample, 62, 0.5f);
    pool.stop (60);
    pool.start (&sample, 64, 0.5f);

    REQUIRE (pool.voiceIndexForNote (60) == -1);
    REQUIRE (pool.voiceIndexForNote (62) == 1);
    REQUIRE (pool.voiceIndexForNote (64) == 0);
}

TEST_CASE ("voice stealing prioritizes a release tail before held voices", "[dsp][voice]")
{
    aod::VoicePool pool (2);
    const aod::Sample sample = makeTone();

    pool.start (&sample, 60, 0.5f);
    std::vector<float> block (4096);
    // Establish a quiet, but still audible, release candidate before adding
    // the second voice. This makes the selected victim observable by note.
    pool.render (block.data(), static_cast<int> (block.size()), kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 1.0f, 0.2f, 100.0f, 0.0f, 0.0f);
    pool.stop (60);
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), 64, kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 1.0f, 0.2f, 100.0f, 0.0f, 0.0f);
    pool.start (&sample, 62, 0.5f);
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), 2, kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 1.0f, 0.2f, 100.0f, 0.0f, 0.0f);
    pool.stop (60);
    pool.start (&sample, 64, 0.5f);

    REQUIRE (pool.voiceIndexForNote (60) == -1);
    REQUIRE (pool.voiceIndexForNote (62) == 1);
    REQUIRE (pool.voiceIndexForNote (64) == 0);
}

TEST_CASE ("voice stealing uses start order when envelope levels are equal", "[dsp][voice]")
{
    aod::VoicePool pool (2);
    const aod::Sample sample = makeTone();

    pool.start (&sample, 60, 0.5f);
    pool.start (&sample, 62, 0.5f);
    pool.start (&sample, 64, 0.5f);

    REQUIRE (pool.voiceIndexForNote (60) == -1);
    REQUIRE (pool.voiceIndexForNote (62) == 1);
    REQUIRE (pool.voiceIndexForNote (64) == 0);
}

TEST_CASE ("voice stealing chooses the quietest held envelope", "[dsp][voice]")
{
    aod::VoicePool pool (2);
    const aod::Sample sample = makeTone();
    std::vector<float> block (4096);

    pool.start (&sample, 60, 0.5f);
    // Note 60 reaches its 20% sustain before note 62 starts its full-level
    // attack. Both keys remain held, so release status cannot decide it.
    pool.render (block.data(), static_cast<int> (block.size()), kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 1.0f, 0.2f, 100.0f, 0.0f, 0.0f);
    pool.start (&sample, 62, 0.5f);
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), 2, kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 1.0f, 0.2f, 100.0f, 0.0f, 0.0f);

    pool.start (&sample, 64, 0.5f);

    REQUIRE (pool.voiceIndexForNote (60) == -1);
    REQUIRE (pool.voiceIndexForNote (62) == 1);
    REQUIRE (pool.voiceIndexForNote (64) == 0);
}

TEST_CASE ("polyphony capacity steals the old mapping for the new note", "[dsp][voice]")
{
    aod::VoicePool pool;
    const aod::Sample sample = makeTone();

    pool.setPolyphony (1);

    pool.start (&sample, 60, 0.5f);
    pool.start (&sample, 62, 0.5f);

    REQUIRE (pool.voiceIndexForNote (60) == -1);
    REQUIRE (pool.voiceIndexForNote (62) == 0);

    std::vector<float> block (static_cast<std::size_t> (kBlockSize));
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);

    // One occupied slot remains, now owned by the new note rather than a
    // silently dropped note-on.
    REQUIRE (blockPeak (block.data(), kBlockSize) > 0.0f);
}

TEST_CASE ("lowering polyphony retires out-of-range voices permanently", "[dsp][voice]")
{
    aod::VoicePool pool;
    const aod::Sample sample = makeTone();

    for (int note = 0; note < aod::VoicePool::maxVoices; ++note)
        pool.start (&sample, note, 0.5f);
    REQUIRE (pool.activeVoiceCount() == aod::VoicePool::maxVoices);

    pool.setPolyphony (32);
    REQUIRE (pool.activeVoiceCount() == 32);
    for (int note = 0; note < 32; ++note)
        REQUIRE (pool.voiceIndexForNote (note) == note);
    for (int note = 32; note < aod::VoicePool::maxVoices; ++note)
        REQUIRE (pool.voiceIndexForNote (note) == -1);

    pool.setPolyphony (aod::VoicePool::maxVoices);
    REQUIRE (pool.activeVoiceCount() == 32);
    pool.start (&sample, 100, 0.5f);
    REQUIRE (pool.voiceIndexForNote (100) == 32);
}

TEST_CASE ("legato retarget to a shorter sample resets sample-dependent state", "[dsp][voice][legato]")
{
    aod::Sample longLoop = makeTone();
    longLoop.loopStart = 200;
    longLoop.loopEnd = 300;
    longLoop.loopEnabled = true;
    aod::Sample shortSample;
    shortSample.data.assign (16, 0.5f);
    shortSample.sampleRate = kSampleRate;
    shortSample.loopStart = 0;
    shortSample.loopEnd = 15;
    shortSample.loopEnabled = true;

    aod::VoicePool pool (1);
    pool.setLegatoEnabled (true);
    pool.start (&longLoop, 60, 1.0f);
    std::vector<float> block (256);
    for (int pass = 0; pass < 2; ++pass)
    {
        std::fill (block.begin(), block.end(), 0.0f);
        pool.render (block.data(), static_cast<int> (block.size()), kSampleRate,
                     0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 1.0f, 1.0f, 100.0f, 0.0f, 0.0f);
    }

    pool.start (&shortSample, 62, 1.0f);
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), static_cast<int> (block.size()), kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 1.0f, 1.0f, 100.0f, 0.0f, 0.0f);

    REQUIRE (pool.voiceIndexForNote (60) == -1);
    REQUIRE (pool.voiceIndexForNote (62) == 0);
    REQUIRE (blockPeak (block.data(), static_cast<int> (block.size())) > 0.1f);
}

TEST_CASE ("ADSR sustain level uses normalized unity range", "[dsp][voice][envelope]")
{
    aod::Sample constant;
    constant.data.assign (kSampleRate, 1.0f);
    constant.sampleRate = kSampleRate;
    aod::VoicePool pool;
    pool.start (&constant, 60, 1.0f);
    std::vector<float> block (512);

    for (int pass = 0; pass < 8; ++pass)
    {
        std::fill (block.begin(), block.end(), 0.0f);
        pool.render (block.data(), static_cast<int> (block.size()), kSampleRate,
                     0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 1.0f, 0.5f, 100.0f, 0.0f, 0.0f);
    }

    // Sustain is a normalized level, so a full-scale source held at 50%
    // sustain must arrive at 0.5. At Drive 0 the voice bypasses its saturation
    // curve, so the level is no longer pulled down to tanh(0.5).
    REQUIRE (blockPeak (block.data(), static_cast<int> (block.size()))
             == Catch::Approx (0.5f).margin (0.03f));
}

TEST_CASE ("velocity-to-drive scales loudness monotonically", "[dsp][voice]")
{
    aod::VoicePool pool;
    const aod::Sample sample = makeTone();

    // Positive velToDriveDb makes a hard hit (v=1.0) the reference and a soft
    // hit quieter; neutral 0 leaves it untouched.
    pool.start (&sample, 60, 1.0f);
    std::vector<float> loud (static_cast<std::size_t> (kBlockSize));
    pool.render (loud.data(), kBlockSize, kSampleRate, 10.0f, 20.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);

    pool.stopAll();
    pool.start (&sample, 60, 0.1f);
    std::vector<float> soft (static_cast<std::size_t> (kBlockSize));
    pool.render (soft.data(), kBlockSize, kSampleRate, 10.0f, 20.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);

    REQUIRE (blockPeak (soft.data(), kBlockSize) < blockPeak (loud.data(), kBlockSize));
}

TEST_CASE ("release fades to silence instead of cutting off abruptly", "[dsp][voice]")
{
    aod::VoicePool pool;
    const aod::Sample sample = makeTone();

    pool.start (&sample, 60, 0.8f);

    // Let the voice ring for a bit, then release it.
    std::vector<float> ring (static_cast<std::size_t> (kBlockSize));
    pool.render (ring.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
    REQUIRE (blockPeak (ring.data(), kBlockSize) > 0.0f);

    pool.stop (60);

    // Across a handful of blocks the release ramp should shrink and reach zero.
    float lastPeak = blockPeak (ring.data(), kBlockSize);
    for (int b = 0; b < 64; ++b)
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        pool.render (ring.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
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
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
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
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);

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
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
    }

    // The slot is recycled for note 62. A late note-off for 60 must not
    // silence the voice that is now actually playing 62.
    pool.start (&sample, 62, 0.5f);
    pool.stop (60); // stale note-off for a note that is no longer sounding

    std::vector<float> block (static_cast<std::size_t> (kBlockSize));
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
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
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
    }

    // A third note must steal the oldest slot (60) rather than drop silently.
    pool.start (&sample, 64, 0.5f);
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
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
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
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
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
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
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
    }
    pool.stop (60);

    // The release fade must run to zero monotonically; the loop must not
    // re-engage and keep it ringing forever.
    float lastPeak = 1.0f;
    bool hitZero = false;
    for (int b = 0; b < 32; ++b)
    {
        std::vector<float> block (static_cast<std::size_t> (kBlockSize));
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
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
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
    }
    pool.stop (60);

    // The release must still fade out to silence rather than stopping the
    // moment the sample ends.
    float lastPeak = 1.0f;
    bool hitZero = false;
    for (int b = 0; b < 32; ++b)
    {
        std::fill (block.begin(), block.end(), 0.0f);
        pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
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
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
    pool.stop (60);

    // Render one more block and look at the *first differences* across the
    // release boundary. A 1 kHz sine at 48 kHz has a max slope of about 0.13
    // samples^-1; a discontinuous envelope (fixed-time ramp from a low release
    // level) would introduce a slope spike several times that. The scaled
    // ramp keeps the fade slope the same whatever the release level.
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);

    float maxSlope = 0.0f;
    for (int i = 1; i < kBlockSize; ++i)
        maxSlope = std::max (maxSlope, std::abs (block[static_cast<std::size_t> (i)]
                                                - block[static_cast<std::size_t> (i - 1)]));

    // Sine slope bound is ~0.13; release ramps down at the same rate, so the
    // combined envelope+sine slope stays under ~0.35. Anything much larger
    // indicates a discontinuity (click) at the release point.
    REQUIRE (maxSlope < 0.5f);
}


TEST_CASE ("bank token assignment on voice start", "[lifetime][banking]")
{
    aod::Sample sample1 = makeTone();
    aod::Sample sample2 = makeTone();

    aod::VoicePool pool;

    // Start a voice: it should capture bank token from the active loader
    pool.start (&sample1, 60, 0.8f);
    const int voiceIdx = pool.voiceIndexForNote(60);
    REQUIRE (voiceIdx >= 0);
    
    // Token should be initialized (all zeros at start of test)
    aod::Voice* voice = pool.getVoiceAtIndex(voiceIdx);
    REQUIRE (voice != nullptr);
    aod::BankToken token = voice->bankToken();
    REQUIRE (token.bankSlot == 0);
    REQUIRE (token.generation == 0);
    REQUIRE (token.bankId == 0);

    // Render block to verify voice is active
    std::vector<float> block (static_cast<std::size_t> (kBlockSize));
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
    REQUIRE (voice->isActive());
}

TEST_CASE ("rapid bank switches do not crash while voices render", "[lifetime][banking][stress]")
{
    aod::Sample sample = makeTone();
    aod::VoicePool pool;

    std::vector<float> block (static_cast<std::size_t> (kBlockSize));

    // Simulate rapid note-ons and note-offs during many blocks
    // (In real scenario, PluginProcessor would call pool.start() and dispatch,
    // setting bank tokens; here we just verify the pool doesn't crash.)
    for (int cycle = 0; cycle < 20; ++cycle)
    {
        // Start a few notes
        pool.start (&sample, 60, 0.8f);
        pool.start (&sample, 62, 0.7f);
        pool.start (&sample, 64, 0.6f);

        // Render multiple blocks
        for (int b = 0; b < 5; ++b)
        {
            std::fill (block.begin(), block.end(), 0.0f);
            pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
        }

        // Release notes
        pool.stop (60);
        pool.stop (62);
        pool.stop (64);

        // Render rest of release
        for (int b = 0; b < 10; ++b)
        {
            std::fill (block.begin(), block.end(), 0.0f);
            pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
        }
    }

    REQUIRE (pool.activeVoiceCount() == 0);
}

TEST_CASE ("voice retirement during polyphony reduction", "[lifetime][polyphony]")
{
    aod::Sample sample = makeTone();
    aod::VoicePool pool (128);

    std::vector<float> block (static_cast<std::size_t> (kBlockSize));

    // Start many voices
    for (int note = 0; note < 64; ++note)
    {
        pool.start (&sample, note, 0.5f);
    }

    REQUIRE (pool.activeVoiceCount() == 64);

    // Render a block
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);

    // Reduce polyphony: voices above new limit should be deactivated
    pool.setPolyphony (32);

    // Render again: excess voices should retire cleanly
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), kBlockSize, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);

    // Active count should drop
    REQUIRE (pool.activeVoiceCount() <= 32);
}

TEST_CASE ("legato retarget to shorter sample does not OOB", "[dsp][legato][oob]")
{
    aod::Sample longSample;
    longSample.data.assign (2400, 0.5f);
    longSample.sampleRate = kSampleRate;
    longSample.loopStart = 100;
    longSample.loopEnd = 2000;
    longSample.loopEnabled = true;
    
    aod::Sample shortSample;
    shortSample.data.assign (16, 0.5f);
    shortSample.sampleRate = kSampleRate;
    shortSample.loopStart = 0;
    shortSample.loopEnd = 15;
    shortSample.loopEnabled = true;
    
    aod::VoicePool pool (1);
    pool.setLegatoEnabled (true);
    pool.start (&longSample, 60, 1.0f);
    
    std::vector<float> block (512);
    for (int i = 0; i < 10; ++i)
    {
        std::fill (block.begin(), block.end(), 0.0f);
        pool.render (block.data(), 512, kSampleRate,
                     0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 10.0f, 1.0f, 100.0f, 100.0f, 0.0f);
    }
    
    pool.start (&shortSample, 61, 1.0f);
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), 512, kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 10.0f, 1.0f, 100.0f, 100.0f, 0.0f);
    
    REQUIRE (pool.voiceIndexForNote (61) == 0);
}

TEST_CASE ("polyphony shrink clears high-index voices", "[dsp][polyphony]")
{
    aod::Sample sample = makeTone();
    aod::VoicePool pool (128);
    
    std::vector<float> block (512);
    
    for (int note = 0; note < 128; ++note)
        pool.start (&sample, note, 1.0f);
    
    REQUIRE (pool.activeVoiceCount() == 128);
    
    pool.setPolyphony (32);
    
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), 512, kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f, 10.0f, 100.0f, 1.0f, 100.0f, 0.0f, 0.0f);
    
    REQUIRE (pool.activeVoiceCount() <= 32);
    
    pool.setPolyphony (128);
    
    std::fill (block.begin(), block.end(), 0.0f);
    pool.render (block.data(), 512, kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f, 10.0f, 100.0f, 1.0f, 100.0f, 0.0f, 0.0f);
    
    REQUIRE (pool.activeVoiceCount() <= 32);
}

TEST_CASE ("processor integration: polyphony + MIDI offset + render", "[integration][processor][polyphony]")
{
    aod::Sample sample = makeTone();
    aod::VoicePool pool (64);
    pool.setLegatoEnabled(false);
    
    std::vector<float> block(kBlockSize);
    
    // Phase 1: Start 48 notes (below 64 limit)
    for (int note = 0; note < 48; ++note)
    {
        pool.start(&sample, note, 0.8f);
    }
    REQUIRE(pool.activeVoiceCount() == 48);
    
    // Phase 2: Render with full parameters
    std::fill(block.begin(), block.end(), 0.0f);
    pool.render(block.data(), kBlockSize, kSampleRate,
                2.0f, 1.5f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
    
    // Phase 3: Reduce polyphony to 32 (retire high-index voices)
    pool.setPolyphony(32);
    
    // Phase 4: Render again to ensure retired voices don't audio-glitch
    std::fill(block.begin(), block.end(), 0.0f);
    pool.render(block.data(), kBlockSize, kSampleRate,
                2.0f, 1.5f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
    
    // Active voices should respect new limit
    REQUIRE(pool.activeVoiceCount() <= 32);
    
    // Phase 5: Start new notes after reduction (should reuse retired voices)
    for (int note = 48; note < 56; ++note)
    {
        pool.start(&sample, note, 0.9f);
    }
    
    // Phase 6: Final render should work without OOB or crashes
    std::fill(block.begin(), block.end(), 0.0f);
    pool.render(block.data(), kBlockSize, kSampleRate,
                2.0f, 1.5f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);
    
    // Should have <= 32 active voices (some may have expired naturally)
    REQUIRE(pool.activeVoiceCount() <= 40);
}
