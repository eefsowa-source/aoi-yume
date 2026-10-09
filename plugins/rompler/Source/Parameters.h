#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace aod
{

/**
    Parameter identifiers and the APVTS layout.

    The set follows the specification table in the planning document. Parameter
    identifiers stay stable so saved plugin state remains compatible as the
    engine grows.
*/
namespace ParamIDs
{
inline constexpr auto voiceDrive         = "voice.drive";
inline constexpr auto voiceCurve         = "voice.curve";
inline constexpr auto voiceVelToDrive    = "voice.velToDrive";
inline constexpr auto voiceFilterRouting = "voice.filterRouting";
inline constexpr auto voiceFilterOffset  = "voice.filterOffset";
inline constexpr auto polyLimit          = "poly.limit";
inline constexpr auto busTapeDrive       = "bus.tapeDrive";
inline constexpr auto busFold            = "bus.fold";
inline constexpr auto busOsFactor        = "bus.osFactor";
inline constexpr auto outTrim            = "out.trim";
inline constexpr auto outMix             = "out.mix";
inline constexpr auto fxChorusRate       = "fx.chorusRate";
inline constexpr auto fxChorusDepth      = "fx.chorusDepth";
inline constexpr auto fxChorusMix        = "fx.chorusMix";
inline constexpr auto fxReverbRoom       = "fx.reverbRoom";
inline constexpr auto fxReverbDamp       = "fx.reverbDamp";
inline constexpr auto fxReverbMix        = "fx.reverbMix";
inline constexpr auto envAttack          = "env.attack";
inline constexpr auto envDecay           = "env.decay";
inline constexpr auto envSustain         = "env.sustain";
inline constexpr auto envRelease         = "env.release";
} // namespace ParamIDs

/** Choice orderings, kept here so the DSP and the UI cannot disagree on them. */
namespace Choices
{
inline const juce::StringArray curve       { "Tanh", "Tube", "Transformer" };
inline const juce::StringArray filterRouting { "Pre", "Post" };
    // The FIR entries reuse the same indices as a new choice rather than a new
    // parameter so saved sessions keep their stored index.
    inline const juce::StringArray osFactor    { "1x", "2x", "4x", "8x", "2x FIR", "4x FIR", "8x FIR" };
} // namespace Choices

[[nodiscard]] inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;

    AudioProcessorValueTreeState::ParameterLayout layout;

    const auto percent = String ("%");
    const auto cents   = String (" cents");
    const auto decibel = String (" dB");
    const auto hertz   = String (" Hz");
    const auto millis  = String (" ms");

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::voiceDrive, 1 }, "Drive",
        NormalisableRange<float> { 0.0f, 100.0f, 0.01f }, 20.0f,
        AudioParameterFloatAttributes{}.withLabel (percent)));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ParamIDs::voiceCurve, 1 }, "Curve", Choices::curve, 0));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::voiceVelToDrive, 1 }, "Velocity to Drive",
        NormalisableRange<float> { -100.0f, 100.0f, 0.01f }, 50.0f,
        AudioParameterFloatAttributes{}.withLabel (percent)));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ParamIDs::voiceFilterRouting, 1 }, "Filter Routing",
        Choices::filterRouting, 0));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::voiceFilterOffset, 1 }, "Filter Offset",
        NormalisableRange<float> { -4800.0f, 4800.0f, 1.0f }, 0.0f,
        AudioParameterFloatAttributes{}.withLabel (cents)));

    layout.add (std::make_unique<AudioParameterInt> (
        ParameterID { ParamIDs::polyLimit, 1 }, "Polyphony", 1, 128, 32));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::busTapeDrive, 1 }, "Tape Drive",
        NormalisableRange<float> { 0.0f, 100.0f, 0.01f }, 0.0f,
        AudioParameterFloatAttributes{}.withLabel (percent)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::busFold, 1 }, "Fold",
        NormalisableRange<float> { 0.0f, 100.0f, 0.01f }, 0.0f,
        AudioParameterFloatAttributes{}.withLabel (percent)));

    // Changing this will change the halfband filter delay once oversampling
    // exists, so it must drive setLatencySamples() and a host notification.
    // Latency is reported as zero for now because no oversampling is present.
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ParamIDs::busOsFactor, 1 }, "Oversampling",
        Choices::osFactor, 2));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::outTrim, 1 }, "Output Trim",
        NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
        AudioParameterFloatAttributes{}.withLabel (decibel)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::outMix, 1 }, "Mix",
        NormalisableRange<float> { 0.0f, 100.0f, 0.01f }, 100.0f,
        AudioParameterFloatAttributes{}.withLabel (percent)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::fxChorusRate, 1 }, "Chorus Rate",
        NormalisableRange<float> { 0.05f, 5.0f, 0.01f }, 1.0f,
        AudioParameterFloatAttributes{}.withLabel (hertz)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::fxChorusDepth, 1 }, "Chorus Depth",
        NormalisableRange<float> { 0.0f, 100.0f, 0.01f }, 30.0f,
        AudioParameterFloatAttributes{}.withLabel (percent)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::fxChorusMix, 1 }, "Chorus Mix",
        NormalisableRange<float> { 0.0f, 100.0f, 0.01f }, 25.0f,
        AudioParameterFloatAttributes{}.withLabel (percent)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::fxReverbRoom, 1 }, "Reverb Room",
        NormalisableRange<float> { 0.0f, 100.0f, 0.01f }, 40.0f,
        AudioParameterFloatAttributes{}.withLabel (percent)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::fxReverbDamp, 1 }, "Reverb Damp",
        NormalisableRange<float> { 0.0f, 100.0f, 0.01f }, 50.0f,
        AudioParameterFloatAttributes{}.withLabel (percent)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::fxReverbMix, 1 }, "Reverb Mix",
        NormalisableRange<float> { 0.0f, 100.0f, 0.01f }, 20.0f,
        AudioParameterFloatAttributes{}.withLabel (percent)));

    // Defaults reproduce Adsr's built-in values (10ms/300ms/0.7/80ms) so a
    // session saved before these existed keeps its sound. The time knobs are
    // skewed toward short values because that is where musically useful
    // resolution lives.
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::envAttack, 1 }, "Envelope Attack",
        NormalisableRange<float> { 0.0f, 5000.0f, 0.01f, 0.3f }, 10.0f,
        AudioParameterFloatAttributes{}.withLabel (millis)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::envDecay, 1 }, "Envelope Decay",
        NormalisableRange<float> { 0.0f, 5000.0f, 0.01f, 0.3f }, 300.0f,
        AudioParameterFloatAttributes{}.withLabel (millis)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::envSustain, 1 }, "Envelope Sustain",
        NormalisableRange<float> { 0.0f, 100.0f, 0.01f }, 70.0f,
        AudioParameterFloatAttributes{}.withLabel (percent)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::envRelease, 1 }, "Envelope Release",
        NormalisableRange<float> { 0.0f, 5000.0f, 0.01f, 0.3f }, 80.0f,
        AudioParameterFloatAttributes{}.withLabel (millis)));

    return layout;
}

} // namespace aod
