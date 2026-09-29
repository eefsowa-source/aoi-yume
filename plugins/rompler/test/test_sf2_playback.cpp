#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "PluginProcessor.h"
#include "SF2Loader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlockSize  = 512;

juce::File testSf2File()
{
    return juce::File (X10_SF2_CROSSCHECK_TESTDATA "/Dr._Mario_64_Soundfont.sf2");
}

/**
    The pinned crosscheck fixture resolves one zone per note, so layering needs a
    bank that actually stacks zones. These small corpus banks do; the first one
    present is used.
*/
juce::File layeredTestBank()
{
    const juce::StringArray candidates {
        "Voice_Erhu.sf2",
        "Super_Nintendo_Unofficial_update.sf2",
        "Hot_Breather_HQ.sf2",
        "Korg_X5DR_PCM__PCM98__Soundfont_V2.0.sf2"
    };

    const juce::File corpus (X10_SF2_CROSSCHECK_TESTDATA);
    for (const auto& name : candidates)
    {
        const juce::File candidate (corpus.getChildFile (name));
        if (candidate.existsAsFile())
            return candidate;
    }

    return {};
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
    const aod::Sample* sample = loader.getSample (bank, program, 60, 100);
    REQUIRE (sample != nullptr);
    REQUIRE (! sample->data.empty());
}

TEST_CASE ("overlapping zones of a real bank are all exposed for one note", "[sf2][m1][layers]")
{
    const juce::File layeredFile = layeredTestBank();
    if (! layeredFile.existsAsFile())
        SKIP ("no layered test bank present on this machine");

    aod::SF2Loader loader (static_cast<int> (kSampleRate));
    REQUIRE (loader.loadFile (layeredFile));

    // Velocity splits and stereo pairs are ordinary SoundFont structure, and the
    // engine used to sound only the first match. Scan the whole bank once for
    // the widest stack and for a note whose zones span both sides of the image.
    std::array<const aod::Sample*, aod::SF2Loader::maxMatchingSamples> layers {};
    std::size_t widestStack = 0;
    bool foundStereoPair = false;
    int pairBank = 0;
    int pairProgram = 0;
    int pairKey = 0;
    int pairVelocity = 0;

    for (int preset = 0; preset < loader.presetCount(); ++preset)
    {
        const auto [bank, program] = loader.presetBankProgram (preset);
        for (const int velocity : { 64, 100, 127 })
            for (int key = 0; key < 128; ++key)
            {
                const auto count = loader.getSamples (bank, program, key, velocity, layers);
                widestStack = std::max (widestStack, count);

                float lowestPan = 0.0f;
                float highestPan = 0.0f;
                for (std::size_t index = 0; index < count; ++index)
                {
                    lowestPan = std::min (lowestPan, layers[index]->pan);
                    highestPan = std::max (highestPan, layers[index]->pan);
                }

                if (! foundStereoPair && count >= 2 && lowestPan < 0.0f && highestPan > 0.0f)
                {
                    foundStereoPair = true;
                    pairBank = bank;
                    pairProgram = program;
                    pairKey = key;
                    pairVelocity = velocity;
                }
            }
    }

    REQUIRE (widestStack >= 2);
    REQUIRE (foundStereoPair);

    // Asking again for the stereo note must be stable, and the pool must never
    // be handed the same decoded sample twice.
    const auto resolved = loader.getSamples (pairBank, pairProgram, pairKey, pairVelocity, layers);
    REQUIRE (resolved >= 2);
    REQUIRE (resolved <= layers.size());

    float lowestPan = 0.0f;
    float highestPan = 0.0f;
    for (std::size_t index = 0; index < resolved; ++index)
    {
        REQUIRE (layers[index] != nullptr);
        // The SoundFont pan and attenuation generators must survive the loader.
        REQUIRE (layers[index]->pan >= -1.0f);
        REQUIRE (layers[index]->pan <= 1.0f);
        REQUIRE (layers[index]->attenuationDb >= 0.0f);
        REQUIRE (layers[index]->attenuationDb <= 144.0f);
        lowestPan = std::min (lowestPan, layers[index]->pan);
        highestPan = std::max (highestPan, layers[index]->pan);

        for (std::size_t other = index + 1; other < resolved; ++other)
            REQUIRE (layers[index] != layers[other]);
    }

    REQUIRE (lowestPan < 0.0f);
    REQUIRE (highestPan > 0.0f);
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

TEST_CASE ("the processor publishes its sounding notes for the keybed", "[sf2][m1][ui]")
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

    int soundingKey = -1;
    for (int key = 0; key < 128 && soundingKey < 0; ++key)
        if (loader.getSample (bank, program, key, 100) != nullptr)
            soundingKey = key;
    REQUIRE (soundingKey >= 0);
    processor.selectPreset (bank, program);

    const auto isActive = [&processor] (int note)
    {
        std::uint64_t lo = 0;
        std::uint64_t hi = 0;
        processor.getActiveNotes (lo, hi);
        const auto bit = std::uint64_t { 1 } << static_cast<unsigned> (note % 64);
        return note < 64 ? (lo & bit) != 0 : (hi & bit) != 0;
    };

    juce::AudioBuffer<float> buffer (2, kBlockSize);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, soundingKey, static_cast<juce::uint8> (100)), 0);
    processor.processBlock (buffer, midi);

    REQUIRE (isActive (soundingKey));

    // The editor keeps a note lit for the whole release tail, so it must stay
    // set while the voice is still fading and clear once the tail has run out.
    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOff (1, soundingKey), 0);
    buffer.clear();
    processor.processBlock (buffer, midi);
    REQUIRE (isActive (soundingKey));

    for (int block = 0; block < 64; ++block)
    {
        buffer.clear();
        juce::MidiBuffer empty;
        processor.processBlock (buffer, empty);
    }

    REQUIRE_FALSE (isActive (soundingKey));
}

TEST_CASE ("velocity levels through a real bank match the reference render", "[sf2][m1][velocity]")
{
    // Reference: FluidSynth 2.x rendering Voice_Erhu.sf2 preset 0/0, note 60, 4096
    // frames at 48 kHz with reverb and chorus off and a fresh synth per velocity.
    // Its attenuation follows the format's velocity curve to the 0.1 dB quantum,
    // so the engine must reproduce the same relative levels.
    const juce::File bank = juce::File (X10_SF2_CROSSCHECK_TESTDATA).getChildFile ("Voice_Erhu.sf2");
    if (! bank.existsAsFile())
        SKIP ("the velocity reference bank is not present on this machine");

    aod::SF2Loader loader (static_cast<int> (kSampleRate));
    REQUIRE (loader.loadFile (bank));

    constexpr int bankMsb = 0;
    constexpr int program = 0;
    constexpr int note = 60;
    constexpr int frames = 4096;

    const std::pair<int, double> reference[] = {
        { 96, -4.9 }, { 64, -11.9 }, { 32, -23.9 }, { 16, -36.0 }, { 8, -48.0 }, { 1, -84.2 }
    };

    const auto levelAt = [&] (int velocity)
    {
        std::array<const aod::Sample*, aod::SF2Loader::maxMatchingSamples> layers {};
        const auto resolved = loader.getSamples (bankMsb, program, note, velocity, layers);
        REQUIRE (resolved != 0);

        aod::VoicePool pool (1);
        pool.start (std::span<const aod::Sample* const> { layers.data(), std::min (resolved, layers.size()) },
                    note, static_cast<float> (velocity) / 127.0f);

        std::vector<float> rendered (static_cast<std::size_t> (frames), 0.0f);
        pool.render (rendered.data(), frames, static_cast<int> (kSampleRate),
                     0.0f, 0.0f, 0, 0, 0.0f, 0.0f, 0.0f, 1.0f, 1000.0f, 0.0f, 0.0f);

        double sumSquares = 0.0;
        for (const float value : rendered)
            sumSquares += static_cast<double> (value) * static_cast<double> (value);
        return std::sqrt (sumSquares / frames);
    };

    const double fullScaleDb = 20.0 * std::log10 (levelAt (127));

    for (const auto& [velocity, expectedDb] : reference)
    {
        const double relativeDb = 20.0 * std::log10 (levelAt (velocity)) - fullScaleDb;
        CAPTURE (velocity, expectedDb, relativeDb);
        REQUIRE (relativeDb == Catch::Approx (expectedDb).margin (0.5));
    }
}


TEST_CASE ("the processor emits a spread stereo chord", "[sf2][m1][stereo]")
{
    if (! testSf2File().existsAsFile())
        SKIP ("test SF2 corpus not present on this machine");

    aod::SF2Loader loader (static_cast<int> (kSampleRate));
    REQUIRE (loader.loadFile (testSf2File()));
    const auto [bank, program] = loader.firstPresetProgram();
    std::vector<int> soundingKeys;
    for (int key = 0; key < 128 && soundingKeys.size() < 3; ++key)
        if (loader.getSample (bank, program, key, 100) != nullptr)
            soundingKeys.push_back (key);
    REQUIRE (soundingKeys.size() == 3);

    aod::RomplerProcessor processor;
    processor.loadSoundFont (testSf2File());
    processor.selectPreset (bank, program);
    auto& state = processor.getValueTreeState();
    for (const auto* id : { aod::ParamIDs::fxChorusMix,
                            aod::ParamIDs::fxReverbMix,
                            aod::ParamIDs::fxDelayMix })
        REQUIRE (state.getParameter (id) != nullptr);
    state.getParameter (aod::ParamIDs::fxChorusMix)->setValueNotifyingHost (0.0f);
    state.getParameter (aod::ParamIDs::fxReverbMix)->setValueNotifyingHost (0.0f);
    state.getParameter (aod::ParamIDs::fxDelayMix)->setValueNotifyingHost (0.0f);
    processor.setPlayConfigDetails (0, 2, kSampleRate, kBlockSize);
    processor.prepareToPlay (kSampleRate, kBlockSize);

    juce::AudioBuffer<float> buffer (2, kBlockSize);
    juce::MidiBuffer midi;
    for (const int key : soundingKeys)
        midi.addEvent (juce::MidiMessage::noteOn (1, key, static_cast<juce::uint8> (100)), 0);
    processor.processBlock (buffer, midi);

    REQUIRE (processor.getActiveVoiceCountForTesting() >= 3);
    const float leftPeak = buffer.getMagnitude (0, 0, kBlockSize);
    const float rightPeak = buffer.getMagnitude (1, 0, kBlockSize);
    REQUIRE (leftPeak > 0.0f);
    REQUIRE (rightPeak > 0.0f);
    float maximumChannelDifference = 0.0f;
    for (int sample = 0; sample < kBlockSize; ++sample)
        maximumChannelDifference = std::max (maximumChannelDifference,
            std::abs (buffer.getSample (0, sample) - buffer.getSample (1, sample)));
    REQUIRE (maximumChannelDifference > 1.0e-5f);

    processor.releaseResources();
}

TEST_CASE ("pitch tracks the played MIDI note", "[sf2][pitch]")
{
    if (! testSf2File().existsAsFile())
        SKIP ("test SF2 corpus not present on this machine");

    aod::SF2Loader loader (static_cast<int> (kSampleRate));
    REQUIRE (loader.loadFile (testSf2File()));
    const auto [bank, program] = loader.firstPresetProgram();

    // Find a key that resolves a sample so we can measure its playback pitch.
    const aod::Sample* sample = nullptr;
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

    // Preserve the loaded region's pitch metadata but use a known fundamental.
    // Counting crossings of an arbitrary SF2 waveform also counts harmonics;
    // band-limiting can change that count without changing playback pitch.
    aod::Sample pitchProbe = *sample;
    pitchProbe.loopEnabled = false;
    pitchProbe.data.resize (96000);
    for (std::size_t frame = 0; frame < pitchProbe.data.size(); ++frame)
        pitchProbe.data[frame] = static_cast<float> (0.5 * std::sin (
            2.0 * juce::MathConstants<double>::pi * 1000.0 * static_cast<double> (frame) / kSampleRate));

    auto zeroCrossings = [] (aod::Sample* s, int note, int blockSize)
    {
        aod::VoicePool pool (1);
        pool.start (s, note, 100.0f / 127.0f);
        juce::AudioBuffer<float> buf (1, blockSize);
        pool.render (buf.getWritePointer (0), blockSize, static_cast<int> (kSampleRate),
                     0.0f, 0.0f, 0, 0, 0.0f, 5.0f, 300.0f, 0.7f, 80.0f, 0.0f, 0.0f);

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
    const int zcA = zeroCrossings (&pitchProbe, noteA, kBigBlock);
    const int zcB = zeroCrossings (&pitchProbe, noteB, kBigBlock);

    REQUIRE (zcA > 20); // the sample must actually oscillate
    REQUIRE (zcB > 20);
    const double ratio = static_cast<double> (zcB) / static_cast<double> (zcA);
    CAPTURE (zcA, zcB, ratio, expectedRatio);
    REQUIRE (std::abs (ratio - expectedRatio) < 0.02);
}
