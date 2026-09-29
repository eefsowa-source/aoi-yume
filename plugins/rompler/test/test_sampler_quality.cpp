#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>
#include <chrono>
#include <cstdio>

#include "Sampler.h"
#include "support/AllocationGuard.h"

namespace
{
constexpr int kSampleRate = 48000;

/** Single-frequency source sample used to separate wanted pitch from folded images. */
aod::Sample makeSine (float frequencyHz, int lengthSamples = kSampleRate)
{
    aod::Sample sample;
    sample.data.resize (static_cast<std::size_t> (lengthSamples));
    for (int i = 0; i < lengthSamples; ++i)
        sample.data[static_cast<std::size_t> (i)] =
            std::sin (2.0f * juce::MathConstants<float>::pi * frequencyHz
                      * static_cast<float> (i) / static_cast<float> (kSampleRate));
    sample.sampleRate = kSampleRate;
    sample.rootKey = 60.0f;
    return sample;
}

std::vector<float> renderNote (const aod::Sample& sample, int midiNote, int numSamples)
{
    aod::VoicePool pool (1);
    pool.start (&sample, midiNote, 1.0f);

    std::vector<float> rendered (static_cast<std::size_t> (numSamples), 0.0f);
    pool.render (rendered.data(), numSamples, kSampleRate,
                 0.0f, 0.0f, 0, 0, 0.0f,
                 0.0f, 1000.0f, 1.0f, 1000.0f,
                 0.0f, 0.0f);
    return rendered;
}

/** Goertzel-style magnitude of one frequency, ignoring the envelope ramp at the start. */
float magnitudeAt (const std::vector<float>& signal, float frequencyHz)
{
    const int total = static_cast<int> (signal.size());
    const int first = total / 4;
    float cosine = 0.0f;
    float sine = 0.0f;

    for (int i = first; i < total; ++i)
    {
        const float phase = 2.0f * juce::MathConstants<float>::pi * frequencyHz
                          * static_cast<float> (i) / static_cast<float> (kSampleRate);
        cosine += signal[static_cast<std::size_t> (i)] * std::cos (phase);
        sine += signal[static_cast<std::size_t> (i)] * std::sin (phase);
    }

    return 2.0f * std::sqrt (cosine * cosine + sine * sine) / static_cast<float> (total - first);
}

float toDb (float magnitude)
{
    return 20.0f * std::log10 (std::max (magnitude, 1.0e-12f));
}
} // namespace

TEST_CASE ("sampler CPU deadline baseline", "[.][benchmark]")
{
    auto sample = makeSine (997.0f, 48000);
    sample.loopEnabled = true;
    sample.loopEnd = 48000;
    for (int rate : { 44100, 48000, 96000 })
        for (int blockSize : { 64, 512 })
            for (int voices : { 16, 32, 64, 128 })
                for (float drive : { 0.0f, 12.0f })
                {
                    aod::VoicePool pool (voices);
                    const int firstNote = voices == 128 ? 0 : (voices == 64 ? 24 : (voices == 32 ? 36 : 48));
                    for (int note = 0; note < voices; ++note)
                        pool.start (&sample, firstNote + note, 0.8f);
                    std::vector<float> block (static_cast<std::size_t> (blockSize));
                    std::array<double, 80> timings {};
                    for (int pass = -8; pass < 80; ++pass)
                    {
                        const auto begin = std::chrono::steady_clock::now();
                        pool.render (block.data(), blockSize, rate, drive, 0.0f, 0, 0, 0.0f,
                            0.0f, 1000.0f, 1.0f, 1000.0f, 0.0f, 12.0f);
                        const auto end = std::chrono::steady_clock::now();
                        if (pass >= 0)
                            timings[static_cast<std::size_t> (pass)] = std::chrono::duration<double, std::micro> (end - begin).count();
                    }
                    const double deadline = 1.0e6 * static_cast<double> (blockSize) / static_cast<double> (rate);
                    const auto missed = std::count_if (timings.begin(), timings.end(), [&](double value) { return value > deadline; });
                    std::sort (timings.begin(), timings.end());
                    std::printf ("CPU rate=%d block=%d voices=%d drive=%.0f median_us=%.2f p95_us=%.2f max_us=%.2f deadline_us=%.2f missed=%ld/80\n",
                        rate, blockSize, voices, static_cast<double> (drive), timings[40], timings[75], timings.back(), deadline, static_cast<long> (missed));
                    REQUIRE (pool.activeVoiceCount() == voices);
                }
}

TEST_CASE ("transposing a voice up an octave rejects folded images", "[dsp][voice][quality]")
{
    // 14 kHz played an octave above its root wants 28 kHz, which cannot exist
    // at 48 kHz. Linear interpolation folds it back to an audible 20 kHz tone.
    const aod::Sample sample = makeSine (14000.0f);
    const auto rendered = renderNote (sample, 72, 24000);

    const float aliasDb = toDb (magnitudeAt (rendered, 20000.0f));
    CAPTURE (aliasDb);
    REQUIRE (aliasDb < -55.0f);
}

TEST_CASE ("transposing a voice up an octave keeps the wanted partial", "[dsp][voice][quality]")
{
    // The alias limit must not be satisfied by muting the top end: a source
    // that stays inside the band after transposition has to survive intact.
    const aod::Sample sample = makeSine (8000.0f);
    const auto rendered = renderNote (sample, 72, 24000);

    const float partialDb = toDb (magnitudeAt (rendered, 16000.0f));
    CAPTURE (partialDb);
    REQUIRE (partialDb > -6.0f);
}

TEST_CASE ("zero drive leaves the sample undistorted", "[dsp][voice][quality]")
{
    // Aoi Clear is specified as a direct SF2 path at Drive 0, so the voice must
    // not push a neutral signal through its saturation curve.
    const aod::Sample sample = makeSine (1000.0f);
    const auto rendered = renderNote (sample, 60, 24000);

    const float fundamentalDb = toDb (magnitudeAt (rendered, 1000.0f));
    const float thirdHarmonicDb = toDb (magnitudeAt (rendered, 3000.0f));
    CAPTURE (fundamentalDb);
    CAPTURE (thirdHarmonicDb);
    REQUIRE (thirdHarmonicDb < fundamentalDb - 90.0f);
}

TEST_CASE ("loop interpolation preserves the sample head before its first wrap", "[dsp][voice][quality]")
{
    aod::Sample sample;
    sample.data.resize (256, 1.0f);
    std::fill (sample.data.begin(), sample.data.begin() + 64, 0.0f);
    sample.loopStart = 64;
    sample.loopEnd = 192;
    sample.loopEnabled = true;

    const auto rendered = renderNote (sample, 60, 16);
    const float headPeak = *std::max_element (rendered.begin(), rendered.end(), [] (float a, float b)
    {
        return std::abs (a) < std::abs (b);
    });

    CAPTURE (headPeak);
    REQUIRE (std::abs (headPeak) < 1.0e-5f);
}

TEST_CASE ("band-limited rendering allocates nothing on the audio thread", "[dsp][rt][voice][quality]")
{
    // VoicePool constructs the shared table before render(), so the measured
    // audio path only reads immutable coefficient memory.
    const aod::Sample sample = makeSine (2000.0f, 8192);
    static_cast<void> (renderNote (sample, 96, 512));

    aod::VoicePool pool;
    for (int note = 0; note < aod::VoicePool::maxVoices; ++note)
        pool.start (&sample, note, 0.8f);

    std::vector<float> block (512, 0.0f);

    const x10::instrument::test::AllocationScope scope;
    for (int pass = 0; pass < 32; ++pass)
        pool.render (block.data(), 512, kSampleRate,
                     0.0f, 0.0f, 0, 0, 0.0f,
                     0.0f, 1000.0f, 1.0f, 1000.0f,
                     0.0f, 0.0f);

    REQUIRE (scope.allocationsSoFar() == 0);
}

TEST_CASE ("loop readers remain invariant across render block boundaries", "[quality]")
{
    for (int loopLength : { 7, 48, 193 })
        for (float vibrato : { 0.0f, 12.0f })
        {
            auto sample = makeSine (3000.0f, 512);
            sample.loopEnabled = true;
            sample.loopStart = 31;
            sample.loopEnd = 31 + loopLength;
            aod::VoicePool whole (1), sliced (1);
            whole.start (&sample, 67, 0.8f);
            sliced.start (&sample, 67, 0.8f);
            std::array<float, 2048> expected {}, actual {};
            const auto render = [&](aod::VoicePool& pool, float* out, int size) {
                pool.render (out, size, kSampleRate, 0.0f, 0.0f, 0, 0, 0.0f,
                    0.0f, 1000.0f, 1.0f, 1000.0f, 0.37f, vibrato);
            };
            render (whole, expected.data(), static_cast<int> (expected.size()));
            for (int offset = 0; offset < static_cast<int> (actual.size()); offset += 16)
                render (sliced, actual.data() + offset, 16);
            for (std::size_t i = 0; i < actual.size(); ++i)
            {
                REQUIRE (std::isfinite (actual[i]));
                REQUIRE (std::abs (actual[i] - expected[i]) < 1.0e-6f);
            }
        }
}

TEST_CASE ("unity interpolation preserves original high frequency samples", "[quality]")
{
    const auto& kernel = aod::BandLimitedInterpolator::shared();
    for (float frequency : { 16000.0f, 20000.0f })
        for (int frame = 0; frame < 48; ++frame)
        {
            const auto read = [&](int offset) { return std::cos (2.0f * juce::MathConstants<float>::pi
                * frequency * static_cast<float> (frame + offset) / 48000.0f); };
            REQUIRE (std::abs (kernel.interpolateRate (kernel.positionForRate (1.0), 0.0f, read) - read (0)) < 1.0e-6f);
        }
}

TEST_CASE ("interpolation is continuous across every pitch bracket and phase seam", "[quality]")
{
    const auto& kernel = aod::BandLimitedInterpolator::shared();
    for (int boundary = 0; boundary < aod::BandLimitedInterpolator::kNumRateBrackets; ++boundary)
        for (float phase : { 0.0f, 0.37f, 0.999f })
            for (float frequency : { 8000.0f, 16000.0f, 20000.0f })
            {
                const double rate = std::pow (16.0, static_cast<double> (boundary) / 32.0);
                const auto read = [&](int offset) { return std::cos (2.0f * juce::MathConstants<float>::pi
                    * frequency * static_cast<float> (offset) / 48000.0f); };
                const float before = kernel.interpolateRate (kernel.positionForRate (rate * (1.0 - 1.0e-7)), phase, read);
                const float after = kernel.interpolateRate (kernel.positionForRate (rate * (1.0 + 1.0e-7)), phase, read);
                REQUIRE (std::abs (after - before) < 1.0e-4f);
            }
    const auto wave = [](int offset) { return std::sin (1.7f * static_cast<float> (offset)); };
    const auto shifted = [&](int offset) { return wave (offset + 1); };
    REQUIRE (std::abs (kernel.interpolateRate (0.0f, 0.999999f, wave)
        - kernel.interpolateRate (0.0f, 0.0f, shifted)) < 1.0e-4f);
}

TEST_CASE ("drive automation crosses zero continuously in both directions", "[quality]")
{
    aod::Sample sample;
    sample.data.assign (2048, 1.0f);
    sample.loopEnabled = true;
    sample.loopEnd = 2048;
    for (int curve = 0; curve < 3; ++curve)
    {
        aod::VoicePool pool (1);
        pool.start (&sample, 60, 1.0f);
        std::array<float, 512> block {};
        const auto render = [&](float drive) {
            pool.render (block.data(), 512, kSampleRate, drive, 0.0f, curve, 0, 0.0f,
                0.0f, 1000.0f, 1.0f, 1000.0f, 0.0f, 0.0f);
        };
        render (0.0f);
        render (0.0f);
        float previous = block.back();
        for (float drive : { 0.01f, 0.0f, -0.01f, 0.0f, 12.0f, 0.0f })
        {
            render (drive);
            float maxStep = 0.0f;
            for (float value : block)
            {
                maxStep = std::max (maxStep, std::abs (value - previous));
                previous = value;
            }
            CAPTURE (curve, drive, maxStep);
            REQUIRE (maxStep < 0.05f);
            if (std::abs (drive) < 0.02f)
                REQUIRE (std::abs (block.back() - 1.0f) < 0.001f);
        }
    }
}

// Hidden diagnostic. Prints the peak of one full-scale 997 Hz note against the
// DRIVE setting for each curve. Run with '[diagnostic]'.
//
// It currently shows the DRIVE knob acting as a fader rather than a saturator:
// the wet path is f(gain * x) / gain, and dividing a saturating curve's output
// by the pre-curve gain collapses the level by exactly the drive in dB (-12 dB
// at 12, -50 dB at 50, -100 dB at 100). Recorded in the SQ-3 notes of
// docs/research/aoi-yume-sq-ui-upgrade-plan-2026-09-29.md; changing it changes
// the sound of every patch with DRIVE above zero, so it needs its own decision.
TEST_CASE ("voice drive transfer diagnostic", "[.][diagnostic]")
{
    const auto sample = makeSine (997.0f, 48000);
    for (const float drive : { 0.0f, 0.5f, 1.0f, 2.0f, 6.0f, 12.0f, 25.0f, 50.0f, 75.0f, 100.0f })
        for (const int curve : { 0, 1, 2 })
        {
            aod::VoicePool pool (1);
            pool.start (&sample, 60, 1.0f);
            std::vector<float> rendered (4800, 0.0f);
            pool.render (rendered.data(), 4800, kSampleRate,
                         drive, 0.0f, curve, 0, 0.0f,
                         0.0f, 1000.0f, 1.0f, 1000.0f,
                         0.0f, 0.0f);
            float peak = 0.0f;
            for (const float value : rendered)
                peak = std::max (peak, std::abs (value));
            WARN ("drive=" << drive << " curve=" << curve << " peak=" << peak
                           << " peakDb=" << toDb (peak)
                           << " h997=" << toDb (magnitudeAt (rendered, 997.0f)));
        }
}
