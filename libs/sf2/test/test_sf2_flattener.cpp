#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <span>
#include <vector>

#include "support/Sf2Builder.h"
#include "x10/sf2/Generators.h"
#include "x10/sf2/Sf2Flattener.h"
#include "x10/sf2/Sf2Reader.h"

using namespace x10::sf2;
using namespace x10::sf2::test;
using x10::instrument::LoopMode;

namespace
{

/**
    Owns the byte buffer for the lifetime of the test, because RawBank keeps
    views into it rather than copying the sample data.
*/
class Fixture
{
public:
    explicit Fixture (const Sf2Builder& builder) : bytes_ (builder.build())
    {
        error_ = read (std::span<const std::byte> { bytes_ }, bank_);
        if (error_ == Sf2Error::ok)
            presets_ = flatten (bank_, diagnostics_);
    }

    [[nodiscard]] Sf2Error error() const noexcept { return error_; }
    [[nodiscard]] const std::vector<FlatPreset>& presets() const noexcept { return presets_; }
    [[nodiscard]] const FlattenDiagnostics& diagnostics() const noexcept { return diagnostics_; }

    [[nodiscard]] const x10::instrument::Region& onlyRegion() const
    {
        return presets_.at (0).regions.at (0);
    }

private:
    std::vector<std::byte>  bytes_;
    RawBank                 bank_;
    Sf2Error                error_ = Sf2Error::ok;
    std::vector<FlatPreset> presets_;
    FlattenDiagnostics      diagnostics_;
};

/** One preset with one zone pointing at instrument 0. */
[[nodiscard]] BuilderPreset simplePreset (std::vector<GeneratorPair> extra = {})
{
    BuilderPreset preset;
    preset.name = "P";
    extra.push_back ({ genInstrument, 0 });
    preset.zones.push_back (BuilderZone { std::move (extra) });
    return preset;
}

/** One instrument with one zone pointing at sample 0. */
[[nodiscard]] BuilderInstrument simpleInstrument (std::vector<GeneratorPair> extra = {})
{
    BuilderInstrument instrument;
    instrument.name = "I";
    extra.push_back ({ genSampleID, 0 });
    instrument.zones.push_back (BuilderZone { std::move (extra) });
    return instrument;
}

constexpr float kTolerance = 1.0e-3f;

} // namespace

TEST_CASE ("a default bank flattens to one region with spec defaults", "[sf2][flatten]")
{
    const Fixture fixture { Sf2Builder{} };
    REQUIRE (fixture.error() == Sf2Error::ok);
    REQUIRE (fixture.presets().size() == 1);
    REQUIRE (fixture.presets()[0].regions.size() == 1);
    REQUIRE (fixture.diagnostics().total() == 0);

    const auto& region = fixture.onlyRegion();

    REQUIRE (region.keyLow == 0);
    REQUIRE (region.keyHigh == 127);
    REQUIRE (region.velocityLow == 0);
    REQUIRE (region.velocityHigh == 127);
    REQUIRE (std::abs (region.rootKey - 60.0f) < kTolerance);
    REQUIRE (std::abs (region.sampleRateHz - 44100.0f) < kTolerance);
    REQUIRE (region.loopMode == LoopMode::none);
    REQUIRE (region.exclusiveClass == 0);

    // 13500 absolute cents is the spec default and means "effectively open".
    REQUIRE (std::abs (region.filterCutoffHz - 19912.13f) < 1.0f);

    // -12000 timecents is one millisecond, not one second. Getting this default
    // wrong turns every note into a slow pad.
    REQUIRE (std::abs (region.volumeEnvelope.attackSeconds - 0.0009765f) < 1.0e-5f);
    REQUIRE (std::abs (region.volumeEnvelope.sustainLevel - 1.0f) < kTolerance);
}

TEST_CASE ("an instrument global zone supplies defaults its local zones override",
           "[sf2][flatten]")
{
    // The first zone is global precisely because it names no sample. This is the
    // rule that decides whether a bank's shared settings apply at all.
    Sf2Builder builder;
    builder.presets = { simplePreset() };

    BuilderInstrument instrument;
    instrument.zones.push_back (BuilderZone { { { genInitialFilterFc, 6000 } } });  // global
    instrument.zones.push_back (BuilderZone { { { genSampleID, 0 } } });            // local
    builder.instruments = { instrument };

    const Fixture fixture { builder };
    REQUIRE (fixture.presets().at (0).regions.size() == 1);

    // 6000 absolute cents = 8.176 * 2^5.
    REQUIRE (std::abs (fixture.onlyRegion().filterCutoffHz - 261.6f) < 1.0f);
}

TEST_CASE ("a local zone beats the global zone it inherits from", "[sf2][flatten]")
{
    Sf2Builder builder;
    builder.presets = { simplePreset() };

    BuilderInstrument instrument;
    instrument.zones.push_back (BuilderZone { { { genInitialFilterFc, 6000 } } });
    instrument.zones.push_back (BuilderZone { { { genInitialFilterFc, 7200 },
                                                { genSampleID, 0 } } });
    builder.instruments = { instrument };

    const Fixture fixture { builder };
    REQUIRE (std::abs (fixture.onlyRegion().filterCutoffHz - 523.2f) < 1.0f);
}

TEST_CASE ("preset generators are offsets, not replacements", "[sf2][flatten]")
{
    // The instrument says +12 semitones; the preset says -2 more. A reader that
    // treats the preset value as absolute would tune this region to -2 semitones
    // instead of +10, which sounds plausible enough to ship unnoticed.
    Sf2Builder builder;
    builder.presets     = { simplePreset ({ { genCoarseTune, signedAmount (-2) } }) };
    builder.instruments = { simpleInstrument ({ { genCoarseTune, 12 } }) };

    const Fixture fixture { builder };
    REQUIRE (std::abs (fixture.onlyRegion().tuneCents - 1000.0f) < kTolerance);
}

TEST_CASE ("generators forbidden at preset level are dropped", "[sf2][flatten]")
{
    // Sample offsets are absolute addresses. Adding a preset's copy on top would
    // point the region outside its own sample, which then looks like a corrupt
    // bank rather than like a flattening bug.
    Sf2Builder builder;
    builder.presets     = { simplePreset ({ { genStartAddrsOffset, 1000 },
                                            { genExclusiveClass, 7 } }) };
    builder.instruments = { simpleInstrument() };

    const Fixture fixture { builder };
    const auto& region = fixture.onlyRegion();

    REQUIRE (region.start == 0);
    REQUIRE (region.exclusiveClass == 0);
}

TEST_CASE ("key and velocity ranges intersect across levels", "[sf2][flatten]")
{
    SECTION ("the narrower preset range wins")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset ({ { genKeyRange, rangeAmount (60, 72) } }) };
        builder.instruments = { simpleInstrument ({ { genKeyRange, rangeAmount (0, 127) } }) };

        const Fixture fixture { builder };
        REQUIRE (fixture.onlyRegion().keyLow == 60);
        REQUIRE (fixture.onlyRegion().keyHigh == 72);
    }

    SECTION ("velocity behaves the same way")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset ({ { genVelRange, rangeAmount (0, 100) } }) };
        builder.instruments = { simpleInstrument ({ { genVelRange, rangeAmount (64, 127) } }) };

        const Fixture fixture { builder };
        REQUIRE (fixture.onlyRegion().velocityLow == 64);
        REQUIRE (fixture.onlyRegion().velocityHigh == 100);
    }

    SECTION ("a disjoint pair produces no region at all")
    {
        // Ranges must not be added or clamped into overlapping — a preset that
        // selects keys the instrument does not cover simply plays nothing.
        Sf2Builder builder;
        builder.presets     = { simplePreset ({ { genKeyRange, rangeAmount (60, 72) } }) };
        builder.instruments = { simpleInstrument ({ { genKeyRange, rangeAmount (0, 50) } }) };

        const Fixture fixture { builder };
        REQUIRE (fixture.presets().at (0).regions.empty());
        REQUIRE (fixture.diagnostics().emptyKeyRange == 1);
    }
}

TEST_CASE ("a velocity split produces one region per layer", "[sf2][flatten]")
{
    Sf2Builder builder;
    builder.presets = { simplePreset() };

    BuilderInstrument instrument;
    instrument.zones.push_back (BuilderZone { { { genVelRange, rangeAmount (0, 63) },
                                                { genSampleID, 0 } } });
    instrument.zones.push_back (BuilderZone { { { genVelRange, rangeAmount (64, 127) },
                                                { genSampleID, 0 } } });
    builder.instruments = { instrument };

    const Fixture fixture { builder };
    const auto& regions = fixture.presets().at (0).regions;

    REQUIRE (regions.size() == 2);
    REQUIRE (regions[0].velocityHigh == 63);
    REQUIRE (regions[1].velocityLow == 64);
}

TEST_CASE ("unit conversions land in engine units", "[sf2][flatten]")
{
    SECTION ("attenuation is centibels")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({ { genInitialAttenuation, 100 } }) };

        const Fixture fixture { builder };
        REQUIRE (std::abs (fixture.onlyRegion().attenuationDb - 10.0f) < kTolerance);
    }

    SECTION ("pan is tenths of a percent")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({ { genPan, signedAmount (-500) } }) };

        const Fixture fixture { builder };
        REQUIRE (std::abs (fixture.onlyRegion().pan + 1.0f) < kTolerance);
    }

    SECTION ("envelope stages are timecents")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({ { genAttackVolEnv, 0 } }) };

        const Fixture fixture { builder };
        REQUIRE (std::abs (fixture.onlyRegion().volumeEnvelope.attackSeconds - 1.0f) < kTolerance);
    }

    SECTION ("volume sustain is an attenuation, expressed as a level")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({ { genSustainVolEnv, 200 } }) };

        // 200 centibels of attenuation is -20 dB, a level of 0.1.
        const Fixture fixture { builder };
        REQUIRE (std::abs (fixture.onlyRegion().volumeEnvelope.sustainLevel - 0.1f) < 1.0e-3f);
    }

    SECTION ("modulation sustain is a percentage decrease, expressed as a level")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({ { genSustainModEnv, 250 } }) };

        const Fixture fixture { builder };
        REQUIRE (std::abs (fixture.onlyRegion().modulationEnvelope.sustainLevel - 0.75f) < kTolerance);
    }

    SECTION ("modulation depth to pitch and filter is carried in cents")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({ { genModEnvToPitch, 1200 },
                                                    { genModEnvToFilterFc, -2400 } }) };

        const Fixture fixture { builder };
        const auto& region = fixture.onlyRegion();

        // Cents, not a normalised 0..1 depth: the voice turns these into a
        // semitone offset and a filter cutoff ratio respectively, so a 0.1
        // normalised reading here would be a 100 cent error downstream.
        REQUIRE (std::abs (region.modEnvToPitchCents - 1200.0f) < kTolerance);
        REQUIRE (std::abs (region.modEnvToFilterCents + 2400.0f) < kTolerance);
    }

    SECTION ("an instrument with no modulation generators reads as no modulation")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({}) };

        const Fixture fixture { builder };
        const auto& region = fixture.onlyRegion();

        // The default has to be a true no-op. The voice uses these two values
        // to decide whether to tick a second envelope at all, so a non-zero
        // default would add work to every voice and shift every preset.
        REQUIRE (std::abs (region.modEnvToPitchCents) < kTolerance);
        REQUIRE (std::abs (region.modEnvToFilterCents) < kTolerance);
    }
}

TEST_CASE ("root key, tuning and loop mode come through", "[sf2][flatten]")
{
    SECTION ("overridingRootKey replaces the sample's own pitch")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({ { genOverridingRootKey, 69 } }) };

        const Fixture fixture { builder };
        REQUIRE (std::abs (fixture.onlyRegion().rootKey - 69.0f) < kTolerance);
    }

    SECTION ("the sample's pitch correction is folded into the tuning")
    {
        BuilderSample sample;
        sample.pitchCorrection = -35;

        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({ { genFineTune, signedAmount (10) } }) };
        builder.samples     = { sample };

        const Fixture fixture { builder };
        REQUIRE (std::abs (fixture.onlyRegion().tuneCents - (-25.0f)) < kTolerance);
    }

    SECTION ("scaleTuning of zero pins every key to the root")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({ { genScaleTuning, 0 } }) };

        const Fixture fixture { builder };
        REQUIRE (std::abs (fixture.onlyRegion().scaleTuningCentsPerKey) < kTolerance);
    }

    SECTION ("sampleModes selects the loop behaviour")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({ { genSampleModes, 3 } }) };

        const Fixture fixture { builder };
        REQUIRE (fixture.onlyRegion().loopMode == LoopMode::sustainThenRelease);
    }

    SECTION ("exclusiveClass survives for hi-hat style choking")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({ { genExclusiveClass, 3 } }) };

        const Fixture fixture { builder };
        REQUIRE (fixture.onlyRegion().exclusiveClass == 3);
    }
}

TEST_CASE ("structurally broken references are skipped and counted", "[sf2][flatten]")
{
    SECTION ("a sample index past the end of the table")
    {
        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument ({ { genSampleID, 99 } }) };

        // simpleInstrument appends its own sampleID, so the later one wins; make
        // the intent explicit instead.
        BuilderInstrument instrument;
        instrument.zones.push_back (BuilderZone { { { genSampleID, 99 } } });
        builder.instruments = { instrument };

        const Fixture fixture { builder };
        REQUIRE (fixture.presets().at (0).regions.empty());
        REQUIRE (fixture.diagnostics().badSampleIndex == 1);
    }

    SECTION ("an instrument index past the end of the table")
    {
        Sf2Builder builder;
        BuilderPreset preset;
        preset.zones.push_back (BuilderZone { { { genInstrument, 42 } } });
        builder.presets     = { preset };
        builder.instruments = { simpleInstrument() };

        const Fixture fixture { builder };
        REQUIRE (fixture.presets().at (0).regions.empty());
        REQUIRE (fixture.diagnostics().badInstrumentIndex == 1);
    }

    SECTION ("ROM samples are declined rather than played from nothing")
    {
        BuilderSample sample;
        sample.sampleType = sampleTypeRomMono;

        Sf2Builder builder;
        builder.presets     = { simplePreset() };
        builder.instruments = { simpleInstrument() };
        builder.samples     = { sample };

        const Fixture fixture { builder };
        REQUIRE (fixture.presets().at (0).regions.empty());
        REQUIRE (fixture.diagnostics().romSamplesSkipped == 1);
    }
}

TEST_CASE ("a preset global zone reaches every zone under it", "[sf2][flatten]")
{
    Sf2Builder builder;

    BuilderPreset preset;
    preset.zones.push_back (BuilderZone { { { genCoarseTune, 5 } } });          // global
    preset.zones.push_back (BuilderZone { { { genInstrument, 0 } } });          // local
    builder.presets     = { preset };
    builder.instruments = { simpleInstrument() };

    const Fixture fixture { builder };
    REQUIRE (fixture.presets().at (0).regions.size() == 1);
    REQUIRE (std::abs (fixture.onlyRegion().tuneCents - 500.0f) < kTolerance);
}

TEST_CASE ("multiple presets keep their own bank and program numbers", "[sf2][flatten]")
{
    Sf2Builder builder;

    BuilderPreset first  = simplePreset();
    first.name    = "Lead";
    first.bank    = 0;
    first.program = 7;

    BuilderPreset second = simplePreset();
    second.name    = "Drums";
    second.bank    = 128;
    second.program = 0;

    builder.presets     = { first, second };
    builder.instruments = { simpleInstrument() };

    const Fixture fixture { builder };
    REQUIRE (fixture.presets().size() == 2);

    REQUIRE (fixture.presets()[0].name == "Lead");
    REQUIRE (fixture.presets()[0].program == 7);
    REQUIRE (fixture.presets()[1].bank == 128);
    REQUIRE (fixture.presets()[1].regions.size() == 1);
}
