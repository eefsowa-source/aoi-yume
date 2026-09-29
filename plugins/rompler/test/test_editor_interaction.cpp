#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace
{
/** A synthetic mouse event against @p target; JUCE's dispatch is not simulated. */
juce::MouseEvent mouseEvent (juce::Component& target, float y,
                             juce::ModifierKeys mods = juce::ModifierKeys(),
                             bool wasDragged = false, int clicks = 1)
{
    const juce::Point<float> position (6.0f, y);
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
                             position, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                             &target, &target, now, position, now, clicks, wasDragged);
}

juce::MouseWheelDetails wheel (float deltaY)
{
    juce::MouseWheelDetails details;
    details.deltaX = 0.0f;
    details.deltaY = deltaY;
    details.isReversed = false;
    details.isSmooth = false;
    details.isInertial = false;
    return details;
}

juce::ModifierKeys shiftKey() { return juce::ModifierKeys (juce::ModifierKeys::shiftModifier); }
juce::ModifierKeys altKey()   { return juce::ModifierKeys (juce::ModifierKeys::altModifier); }

/** The parameter value in its own units, not the host's normalised 0..1. */
float parameterValue (juce::RangedAudioParameter& param)
{
    return param.getNormalisableRange().convertFrom0to1 (param.getValue());
}

juce::RangedAudioParameter& rangedParameter (aod::RomplerProcessor& processor, const char* id)
{
    auto* param = dynamic_cast<juce::RangedAudioParameter*> (
        processor.getValueTreeState().getParameter (id));
    REQUIRE (param != nullptr);
    return *param;
}
} // namespace

TEST_CASE ("knob double-click restores the parameter default", "[ui][interaction]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::RomplerProcessor processor;
    auto& param = rangedParameter (processor, aod::ParamIDs::envSustain);
    aod::Knob knob (param);
    knob.setSize (72, 72);

    const auto range = param.getNormalisableRange();
    REQUIRE (parameterValue (param) == Catch::Approx (100.0f).margin (0.05f));

    param.setValueNotifyingHost (range.convertTo0to1 (range.start));
    REQUIRE (parameterValue (param) == Catch::Approx (range.start).margin (0.05f));

    knob.mouseDoubleClick (mouseEvent (knob, 36.0f, {}, false, 2));

    REQUIRE (param.getValue() == Catch::Approx (param.getDefaultValue()).margin (1.0e-4f));
    REQUIRE (parameterValue (param) == Catch::Approx (100.0f).margin (0.05f));
}

TEST_CASE ("knob drag and wheel scale to the parameter and Shift refines them", "[ui][interaction]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::RomplerProcessor processor;
    auto& param = rangedParameter (processor, aod::ParamIDs::voiceDrive);
    const auto range = param.getNormalisableRange();
    const float span = range.end - range.start;

    aod::Knob knob (param);
    knob.setSize (72, 72);

    SECTION ("a 40 px drag moves the value, Shift a tenth as far")
    {
        param.setValueNotifyingHost (range.convertTo0to1 (span * 0.5f));
        const float before = parameterValue (param);
        knob.mouseDown (mouseEvent (knob, 60.0f));
        knob.mouseDrag (mouseEvent (knob, 20.0f));
        const float plain = parameterValue (param) - before;

        param.setValueNotifyingHost (range.convertTo0to1 (span * 0.5f));
        knob.mouseDown (mouseEvent (knob, 60.0f));
        knob.mouseDrag (mouseEvent (knob, 20.0f, shiftKey()));
        const float fine = parameterValue (param) - before;

        REQUIRE (plain == Catch::Approx (40.0f * span / 200.0f).margin (0.05f));
        REQUIRE (fine == Catch::Approx (plain * 0.1f).margin (0.02f));
    }

    SECTION ("a wheel notch scales with the wheel amount, not the snap interval")
    {
        // 0.01 is this parameter's snapping resolution; using it as the wheel
        // step would make one notch inaudible.
        REQUIRE (range.interval == Catch::Approx (0.01f));

        param.setValueNotifyingHost (range.convertTo0to1 (span * 0.5f));
        const float before = parameterValue (param);
        knob.mouseWheelMove (mouseEvent (knob, 36.0f), wheel (0.4f));
        const float plain = parameterValue (param) - before;

        param.setValueNotifyingHost (range.convertTo0to1 (span * 0.5f));
        knob.mouseWheelMove (mouseEvent (knob, 36.0f, shiftKey()), wheel (0.4f));
        const float fine = parameterValue (param) - before;

        REQUIRE (plain == Catch::Approx (0.4f * 0.15f * span).margin (0.05f));
        REQUIRE (fine == Catch::Approx (plain * 0.1f).margin (0.02f));
    }
}

TEST_CASE ("knob readout prints the parameter unit", "[ui][interaction]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::RomplerProcessor processor;
    auto& param = rangedParameter (processor, aod::ParamIDs::outTrim);
    aod::Knob knob (param);
    knob.setSize (72, 72);

    REQUIRE (knob.readoutText().containsIgnoreCase ("dB"));
}

TEST_CASE ("switch cycles choices with a click and Alt-click reverses", "[ui][interaction]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::RomplerProcessor processor;
    auto& choice = *dynamic_cast<juce::AudioParameterChoice*> (
        processor.getValueTreeState().getParameter (aod::ParamIDs::voiceCurve));
    aod::Switch sw (choice, "CURVE");
    sw.setSize (110, 64);

    const int count = choice.choices.size();
    REQUIRE (count >= 3);

    choice.setValueNotifyingHost (0.0f);
    REQUIRE (choice.getIndex() == 0);

    sw.mouseDown (mouseEvent (sw, 20.0f));
    sw.mouseUp (mouseEvent (sw, 20.0f));
    REQUIRE (choice.getIndex() == 1);

    sw.mouseDown (mouseEvent (sw, 20.0f));
    sw.mouseUp (mouseEvent (sw, 20.0f, altKey()));
    REQUIRE (choice.getIndex() == 0);

    // The first choice wraps backwards to the last one.
    sw.mouseDown (mouseEvent (sw, 20.0f));
    sw.mouseUp (mouseEvent (sw, 20.0f, altKey()));
    REQUIRE (choice.getIndex() == count - 1);

    // A drag past the threshold steps once, and the release must not step again.
    choice.setValueNotifyingHost (0.0f);
    sw.mouseDown (mouseEvent (sw, 60.0f));
    sw.mouseDrag (mouseEvent (sw, 20.0f));
    sw.mouseUp (mouseEvent (sw, 20.0f, juce::ModifierKeys(), true));
    REQUIRE (choice.getIndex() == 1);

    // A click that jitters a few pixels is still a click, and a held switch
    // still steps on release because the drag flag never got set.
    choice.setValueNotifyingHost (0.0f);
    sw.mouseDown (mouseEvent (sw, 40.0f));
    sw.mouseDrag (mouseEvent (sw, 36.0f));
    sw.mouseUp (mouseEvent (sw, 36.0f));
    REQUIRE (choice.getIndex() == 1);
}

TEST_CASE ("stepper moves exactly one authored step per wheel notch", "[ui][interaction]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::RomplerProcessor processor;
    auto& param = rangedParameter (processor, aod::ParamIDs::polyLimit);
    auto& polyphony = *dynamic_cast<juce::AudioParameterInt*> (
        processor.getValueTreeState().getParameter (aod::ParamIDs::polyLimit));
    aod::Stepper stepper (param, "POLYPHONY");
    stepper.setSize (110, 64);

    REQUIRE (param.getNormalisableRange().interval == Catch::Approx (1.0f));

    polyphony.setValueNotifyingHost (param.convertTo0to1 (16.0f));
    REQUIRE (polyphony.get() == 16);

    // A smooth wheel reports about 0.1 per notch, which a whole-number
    // parameter would round away entirely.
    stepper.mouseWheelMove (mouseEvent (stepper, 32.0f), wheel (0.1f));
    REQUIRE (polyphony.get() == 17);

    stepper.mouseWheelMove (mouseEvent (stepper, 32.0f), wheel (-0.1f));
    REQUIRE (polyphony.get() == 16);
}

TEST_CASE ("peak meter latches a clip until it is clicked", "[ui][interaction]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::PeakMeter meter;
    meter.setSize (60, 30);

    REQUIRE_FALSE (meter.isClipped());

    meter.setLevel (0.5f);
    REQUIRE_FALSE (meter.isClipped());

    meter.setLevel (0.99f);
    REQUIRE (meter.isClipped());

    // The latch survives the peak falling back down.
    meter.setLevel (0.1f);
    REQUIRE (meter.isClipped());

    meter.mouseDown (mouseEvent (meter, 15.0f));
    REQUIRE_FALSE (meter.isClipped());
}

TEST_CASE ("gain reduction meter holds its peak until it is clicked", "[ui][interaction]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::GainReductionMeter meter;
    meter.setSize (90, 24);

    REQUIRE (meter.peakHoldDb() == Catch::Approx (0.0f));

    meter.setReductionDb (12.0f);
    REQUIRE (meter.peakHoldDb() == Catch::Approx (12.0f));

    // The live level drops away; the hold decays a little but stays readable.
    meter.setReductionDb (0.0f);
    REQUIRE (meter.peakHoldDb() > 10.0f);
    REQUIRE (meter.peakHoldDb() < 12.0f);

    for (int update = 0; update < 20; ++update)
        meter.setReductionDb (0.0f);
    REQUIRE (meter.peakHoldDb() < 1.0f);

    // Clicking drops the hold to the live level, which is 0 here.
    meter.setReductionDb (18.0f);
    REQUIRE (meter.peakHoldDb() == Catch::Approx (18.0f));

    meter.setReductionDb (0.0f);
    REQUIRE (meter.peakHoldDb() > 16.0f);

    meter.mouseDown (mouseEvent (meter, 12.0f));
    REQUIRE (meter.peakHoldDb() == Catch::Approx (0.0f));
}

TEST_CASE ("keyboard draws only the notes it is told are sounding", "[ui][interaction]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::Keyboard keyboard;
    keyboard.setSize (600, 80);
    keyboard.setKeyRange (36, 48);

    REQUIRE_FALSE (keyboard.isNoteLit (60));

    keyboard.setNoteOn (60, true);
    REQUIRE (keyboard.isNoteLit (60));
    REQUIRE_FALSE (keyboard.isNoteLit (61));

    keyboard.setNoteOn (60, false);
    REQUIRE_FALSE (keyboard.isNoteLit (60));
}
