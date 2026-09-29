#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "Sampler.h"

#include "support/Signals.h"
#include "support/Spectrum.h"

namespace
{
using namespace x10::test;

/** 12% is the character-drive operating point the CPU matrix and earlier
    benches use, and it puts a full-scale sample at the gate's own amplitude. */
constexpr float kDrivePercent = 12.0f;
const float kDriveGain = std::pow (10.0f, kDrivePercent / 20.0f);

/** The input amplitude that makes the curve see the gate's operating point. */
const float kToneAmplitude = kGateAmplitude / kDriveGain;

/** The real stage, as the audio path drives it. */
struct DriveUnderTest
{
    aod::Voice::Drive drive;
    int curveId = 0;
    float gain = 1.0f;

    [[nodiscard]] float process (float x) noexcept
    {
        return drive.process (x, curveId, 1.0f, gain);
    }

    void reset() noexcept { drive.reset(); }
};

/** The same drive without antialiasing: the shipped behaviour and the baseline. */
template <x10::dsp::Curve C>
struct DirectDrive
{
    float gain = 1.0f;

    [[nodiscard]] float process (float x) const noexcept { return C::f (gain * x) / gain; }
    void reset() noexcept {}
};

/** Two windows: the first is discarded so the ADAA state reaches steady state. */
[[nodiscard]] std::vector<float> steadyState (auto& processor, const std::vector<float>& input)
{
    auto window = input;
    for (float s : window)
        (void) processor.process (s);

    window = input;
    for (float& s : window)
        s = processor.process (s);

    return window;
}

[[nodiscard]] double fundamentalDb (const std::vector<float>& window, std::size_t bin)
{
    return 10.0 * std::log10 (harmonicPower (window.data(), window.size(), bin));
}
} // namespace

TEST_CASE ("the voice drive reproduces the direct curve when antialiasing is off", "[dsp][voice][adaa]")
{
    // The default must be the previous formula exactly, so that adding the
    // antialiased path cannot change any existing patch.
    aod::Voice::Drive drive;
    REQUIRE_FALSE (drive.antialiasing());

    const auto check = [&drive] (int curveId, auto curveFunction)
    {
        drive.reset();
        for (int step = -200; step <= 200; ++step)
        {
            const float x = static_cast<float> (step) * 0.01f;
            const float expected = curveFunction (kDriveGain * x) / kDriveGain;
            const float actual = drive.process (x, curveId, 1.0f, kDriveGain);
            REQUIRE (std::abs (actual - expected) < 1.0e-6f);
        }
    };

    check (0, [] (float v) { return x10::dsp::curves::Tanh::f (v); });
    check (1, [] (float v) { return x10::dsp::curves::Tube::f (v); });
    check (2, [] (float v) { return x10::dsp::curves::Transformer::f (v); });
}

TEST_CASE ("the antialiased voice drive suppresses aliasing at the voice operating point", "[dsp][voice][adaa]")
{
    // Both sides see the same curve input amplitude, which the stock gate cannot
    // express once the drive gain is inside the candidate.
    const auto compare = [] (int curveId) -> double
    {
        DirectDrive<x10::dsp::curves::Tanh> unused {};
        static_cast<void> (unused);

        DriveUnderTest candidate { {}, curveId, kDriveGain };
        candidate.drive.setAntialiasing (true);

        const auto candidateReport = measureAlias (candidate, kGateToneBin, kToneAmplitude);
        return candidateReport.nmrDb();
    };

    const auto reference = [] (int curveId)
    {
        switch (curveId)
        {
            case 1:  { DirectDrive<x10::dsp::curves::Tube> d { kDriveGain };        return measureAlias (d, kGateToneBin, kToneAmplitude); }
            case 2:  { DirectDrive<x10::dsp::curves::Transformer> d { kDriveGain }; return measureAlias (d, kGateToneBin, kToneAmplitude); }
            default: { DirectDrive<x10::dsp::curves::Tanh> d { kDriveGain };        return measureAlias (d, kGateToneBin, kToneAmplitude); }
        }
    };

    SECTION ("Tanh")
    {
        // Measured 6.8 dB at this operating point, against the library's own
        // 6.8-8.6 dB band at the gate amplitude.
        const double improvementDb = reference (0).nmrDb() - compare (0);
        CAPTURE (improvementDb);
        REQUIRE (improvementDb >= 5.0);
    }

    SECTION ("Tube")
    {
        const double improvementDb = reference (1).nmrDb() - compare (1);
        CAPTURE (improvementDb);
        REQUIRE (improvementDb >= 5.0);
    }

    SECTION ("Transformer")
    {
        const double improvementDb = reference (2).nmrDb() - compare (2);
        CAPTURE (improvementDb);
        REQUIRE (improvementDb >= 5.0);
    }
}

TEST_CASE ("the rendered voice drive matches the direct curve", "[dsp][voice][adaa]")
{
    // End to end through the voice: the drive stage is the only nonlinearity, so
    // its alias floor should read the same as the same curve applied directly.
    aod::Sample sample;
    sample.data = binCentredSine (kFftSize, kGateToneBin, 1.0f);
    sample.sampleRate = static_cast<int> (kSampleRate);
    sample.rootKey = 60.0f;
    // Fully immediate stages keep the window exactly periodic; a zone attack ramp
    // would smear energy across bins and read as alias.
    sample.volumeEnvelope = { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };

    const auto renderAt = [&sample] (float drive)
    {
        aod::VoicePool pool (1);
        pool.start (&sample, 60, 1.0f);
        std::vector<float> out (kFftSize, 0.0f);
        pool.render (out.data(), static_cast<int> (out.size()), static_cast<int> (kSampleRate),
                     drive, 0.0f, 0, 0, 0.0f, 0.0f, 0.0f, 1.0f, 1000.0f, 0.0f, 0.0f);
        return out;
    };

    // The voice scales the sample on its way to the drive (the dry render is
    // 0.70 of the sample level), so the reference is driven by the voice's own
    // dry output instead of the raw sample. Comparing against the raw sample
    // would drive the reference harder and flatter the direct curve's aliasing.
    const auto voiceInput = renderAt (0.0f);
    const auto voice = analyseHarmonics (renderAt (kDrivePercent), kGateToneBin);

    DirectDrive<x10::dsp::curves::Tanh> direct { kDriveGain };
    const auto directReport = analyseHarmonics (steadyState (direct, voiceInput), kGateToneBin);

    const double differenceDb = std::abs (voice.nmrDb() - directReport.nmrDb());
    CAPTURE (voice.nmrDb(), directReport.nmrDb(), differenceDb);
    REQUIRE (differenceDb < 2.0);
}

TEST_CASE ("the antialiased voice drive trades top-end response for that suppression", "[dsp][voice][adaa]")
{
    // First-order ADAA evaluates the two-point average of the curve in the
    // small-signal limit, so it costs top-end response where the curve is
    // linear. Measured at 11.7 dB for 20 kHz, which is why antialiasing is off
    // by default and why this number is pinned: a cheaper substitute must not
    // change it silently.
    const std::vector<float> tone = binCentredSine (kFftSize, 6827, 0.02f); // 20000.0 Hz

    DriveUnderTest antialiased { {}, 0, kDriveGain };
    antialiased.drive.setAntialiasing (true);
    const auto adaaOut = steadyState (antialiased, tone);

    DirectDrive<x10::dsp::curves::Tanh> direct { kDriveGain };
    const auto directOut = steadyState (direct, tone);

    const double lossDb = fundamentalDb (directOut, 6827) - fundamentalDb (adaaOut, 6827);
    CAPTURE (lossDb);
    REQUIRE (lossDb > 10.0);
    REQUIRE (lossDb < 13.0);
}

TEST_CASE ("voice drive cost per sample", "[.][benchmark][adaa]")
{
    constexpr int numSamples = 1 << 16;
    std::vector<float> buffer (numSamples);
    auto rng = Xorshift32 (0xA01u);
    for (auto& s : buffer)
        s = rng.bipolar();

    const auto time = [] (aod::Voice::Drive& drive, bool antialias, std::vector<float>& data)
    {
        drive.reset();
        drive.setAntialiasing (antialias);
        const auto start = std::chrono::steady_clock::now();
        for (int repeat = 0; repeat < 40; ++repeat)
            for (float& s : data)
                s = drive.process (s, 0, 1.0f, 4.0f);
        const auto elapsed = std::chrono::duration<double, std::nano> (
            std::chrono::steady_clock::now() - start).count();
        return elapsed / static_cast<double> (numSamples * 40);
    };

    aod::Voice::Drive drive;
    const auto directNs = time (drive, false, buffer);
    const auto antialiasedNs = time (drive, true, buffer);
    std::printf ("drive cost: direct=%.2f ns/sample antialiased=%.2f ns/sample ratio=%.2fx\n",
                 directNs, antialiasedNs, antialiasedNs / directNs);
    REQUIRE (antialiasedNs > 0.0);
}
