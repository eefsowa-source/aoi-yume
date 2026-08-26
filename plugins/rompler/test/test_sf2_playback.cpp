#include <catch2/catch_test_macros.hpp>

#include "PluginProcessor.h"
#include "SF2Loader.h"

#include <cmath>

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlockSize  = 512;

juce::File testSf2File()
{
    return juce::File (X10_SF2_CROSSCHECK_TESTDATA "/Dr._Mario_64_Soundfont.sf2");
}
} // namespace

TEST_CASE ("SF2Loader loads a real bank and resolves a sample for note-on", "[sf2][m1]")
{
    if (! testSf2File().existsAsFile())
        SKIP ("test SF2 corpus not present on this machine");

    aod::SF2Loader loader (static_cast<int> (kSampleRate));
    REQUIRE (loader.loadFile (testSf2File()));
    REQUIRE (loader.presetCount() > 0);

    const auto [bank, program] = loader.firstPresetProgram();
    aod::Sample* sample = loader.getSample (bank, program, 60, 100);
    REQUIRE (sample != nullptr);
    REQUIRE (! sample->data.empty());
}

TEST_CASE ("a note-on through the processor produces non-silent output", "[sf2][m1]")
{
    if (! testSf2File().existsAsFile())
        SKIP ("test SF2 corpus not present on this machine");

    aod::RomplerProcessor processor;
    processor.setPlayConfigDetails (0, 2, kSampleRate, kBlockSize);
    processor.prepareToPlay (kSampleRate, kBlockSize);
    processor.loadSoundFont (testSf2File());

    aod::SF2Loader loader (static_cast<int> (kSampleRate));
    REQUIRE (loader.loadFile (testSf2File()));
    const auto [bank, program] = loader.firstPresetProgram();

    // Find a midi key that the first preset actually voices, so the block is
    // non-silent regardless of which font is bundled or how regions are pinned.
    int soundingKey = -1;
    for (int key = 0; key < 128 && soundingKey < 0; ++key)
        if (loader.getSample (bank, program, key, 100) != nullptr)
            soundingKey = key;
    REQUIRE (soundingKey >= 0);
    processor.selectPreset (bank, program);

    juce::AudioBuffer<float> buffer (2, kBlockSize);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, soundingKey, static_cast<juce::uint8> (100)), 0);

    processor.processBlock (buffer, midi);

    float peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        peak = std::max (peak, buffer.getMagnitude (ch, 0, kBlockSize));

    REQUIRE (peak > 0.0f);
}

TEST_CASE ("pitch tracks the played MIDI note", "[sf2][pitch]")
{
    if (! testSf2File().existsAsFile())
        SKIP ("test SF2 corpus not present on this machine");

    aod::SF2Loader loader (static_cast<int> (kSampleRate));
    REQUIRE (loader.loadFile (testSf2File()));
    const auto [bank, program] = loader.firstPresetProgram();

    // Find a key that resolves a sample so we can measure its playback pitch.
    aod::Sample* sample = nullptr;
    for (int key = 0; key < 128; ++key)
    {
        sample = loader.getSample (bank, program, key, 100);
        if (sample != nullptr)
            break;
    }
    REQUIRE (sample != nullptr);
    REQUIRE (! sample->data.empty());

    // A note 12 semitones above the root must advance exactly twice as fast
    // (2^(12/12) = 2) when the region uses normal chromatic tuning.
    const double expectedRatio = std::pow (2.0, sample->scaleTuningCentsPerKey / 100.0);
    REQUIRE (expectedRatio > 1.5);

    const int noteA = juce::jlimit (0, 115, static_cast<int> (std::lround (sample->rootKey)));
    const int noteB = noteA + 12;

    auto zeroCrossings = [] (aod::Sample* s, int note, int blockSize)
    {
        aod::VoicePool pool (1);
        pool.start (s, note, 100.0f / 127.0f);
        juce::AudioBuffer<float> buf (1, blockSize);
        pool.render (buf.getWritePointer (0), blockSize, static_cast<int> (kSampleRate),
                     0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f);

        int crossings = 0;
        for (int i = 1; i < blockSize; ++i)
        {
            const float a = buf.getSample (0, i - 1);
            const float b = buf.getSample (0, i);
            if ((a < 0.0f && b >= 0.0f) || (a >= 0.0f && b < 0.0f))
                ++crossings;
        }
        return crossings;
    };

    constexpr int kBigBlock = 8192;
    const int zcA = zeroCrossings (sample, noteA, kBigBlock);
    const int zcB = zeroCrossings (sample, noteB, kBigBlock);

    REQUIRE (zcA > 20); // the sample must actually oscillate
    REQUIRE (zcB > 20);
    const double ratio = static_cast<double> (zcB) / static_cast<double> (zcA);
    REQUIRE (std::abs (ratio - expectedRatio) < 0.2);
}
