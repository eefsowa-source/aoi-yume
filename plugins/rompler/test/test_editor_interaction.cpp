#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

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

namespace
{
/** Depth-first list of every descendant component of @p root. */
void collectDescendants (juce::Component& root, std::vector<juce::Component*>& out)
{
    for (auto* child : root.getChildren())
    {
        out.push_back (child);
        collectDescendants (*child, out);
    }
}

/** The component whose accessibility handler carries @p title and @p role. */
juce::Component* findAccessibleByTitle (juce::Component& root,
                                        juce::AccessibilityRole role,
                                        const char* title)
{
    std::vector<juce::Component*> all;
    collectDescendants (root, all);
    for (auto* c : all)
        if (auto* handler = c->getAccessibilityHandler())
            if (handler->getRole() == role && handler->getTitle() == title)
                return c;
    return nullptr;
}

/** JUCE only creates accessibility handlers for components under a native
    peer, so tests that walk the accessible tree need the editor on the
    desktop. The component stays invisible, which keeps the peer window
    offscreen. */
struct TemporaryDesktopPeer
{
    explicit TemporaryDesktopPeer (juce::Component& c) : component (c)
    {
        component.addToDesktop (juce::ComponentPeer::windowIsTemporary);
        // grabKeyboardFocus refuses anything that is not showing, so a peer
        // alone is not enough to exercise the keyboard path.
        component.setVisible (true);
    }
    ~TemporaryDesktopPeer() { component.removeFromDesktop(); }
    juce::Component& component;
};
} // namespace

TEST_CASE ("parameter controls expose a named, valued accessibility node", "[ui][accessibility]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::RomplerProcessor processor;
    aod::RomplerEditor editor (processor);
    const TemporaryDesktopPeer peer (editor);

    // The wrappers draw themselves, so the parameter-carrying slider or combo
    // must sit inside them as a transparent child node. Without that wiring a
    // screen reader saw only an unnamed, ignored component per control.
    for (const auto* title : { "DRIVE", "SUSTAIN", "RELEASE", "POLYPHONY",
                               "PING-PONG FEEDBACK", "DELAY MIX" })
    {
        auto* node = findAccessibleByTitle (editor, juce::AccessibilityRole::slider, title);
        CAPTURE (title);
        REQUIRE (node != nullptr);
        auto* handler = node->getAccessibilityHandler();
        REQUIRE (handler->getDescription().isNotEmpty());
        REQUIRE (handler->getValueInterface() != nullptr);
        REQUIRE (handler->getCurrentState().isFocusable());
    }

    for (const auto* title : { "CURVE", "LEGATO" })
    {
        CAPTURE (title);
        REQUIRE (findAccessibleByTitle (editor, juce::AccessibilityRole::comboBox, title) != nullptr);
    }

    // The sustain knob reads back its value with units, not just a name.
    auto* sustain = findAccessibleByTitle (editor, juce::AccessibilityRole::slider, "SUSTAIN");
    REQUIRE (sustain != nullptr);
    auto* value = sustain->getAccessibilityHandler()->getValueInterface();
    REQUIRE (value->getCurrentValueAsString().contains ("%"));
    const auto range = value->getRange();
    REQUIRE (range.isValid());
    REQUIRE (range.getMinimumValue() == Catch::Approx (0.0));
    REQUIRE (range.getMaximumValue() == Catch::Approx (100.0));
}

TEST_CASE ("a focused knob steps its parameter with the arrow keys", "[ui][accessibility]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::RomplerProcessor processor;
    aod::RomplerEditor editor (processor);
    const TemporaryDesktopPeer peer (editor);

    auto* sustainNode = findAccessibleByTitle (editor, juce::AccessibilityRole::slider, "SUSTAIN");
    REQUIRE (sustainNode != nullptr);
    auto* knob = sustainNode->getParentComponent();
    REQUIRE (knob != nullptr);

    auto& param = rangedParameter (processor, aod::ParamIDs::envSustain);
    param.setValueNotifyingHost (param.convertTo0to1 (50.0f));

    // A full arrow press steps 2% of the 0..100 range; the interval floor is
    // only 0.01, which would be an inaudible nudge on its own.
    knob->keyPressed (juce::KeyPress (juce::KeyPress::leftKey));
    REQUIRE (parameterValue (param) == Catch::Approx (48.0).margin (0.25));

    // Shift narrows the step to a tenth, matching the fine mouse gestures.
    knob->keyPressed (juce::KeyPress (juce::KeyPress::leftKey,
                                      juce::ModifierKeys (juce::ModifierKeys::shiftModifier), 0));
    REQUIRE (parameterValue (param) == Catch::Approx (47.8).margin (0.1));

    knob->keyPressed (juce::KeyPress (juce::KeyPress::endKey));
    REQUIRE (parameterValue (param) == Catch::Approx (100.0).margin (0.25));
}

TEST_CASE ("a focused switch cycles its choice with the arrow keys", "[ui][accessibility]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::RomplerProcessor processor;
    aod::RomplerEditor editor (processor);
    const TemporaryDesktopPeer peer (editor);

    auto* node = findAccessibleByTitle (editor, juce::AccessibilityRole::comboBox, "CURVE");
    REQUIRE (node != nullptr);
    auto* control = node->getParentComponent();
    REQUIRE (control != nullptr);

    auto* param = dynamic_cast<juce::AudioParameterChoice*> (
        processor.getValueTreeState().getParameter (aod::ParamIDs::voiceCurve));
    REQUIRE (param != nullptr);

    const int before = param->getIndex();
    control->keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
    REQUIRE (param->getIndex() == (before + 1) % param->choices.size());
    control->keyPressed (juce::KeyPress (juce::KeyPress::leftKey));
    REQUIRE (param->getIndex() == before);
}

TEST_CASE ("a focused integer stepper moves by a whole step", "[ui][accessibility]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::RomplerProcessor processor;
    aod::RomplerEditor editor (processor);
    const TemporaryDesktopPeer peer (editor);

    auto* node = findAccessibleByTitle (editor, juce::AccessibilityRole::slider, "POLYPHONY");
    REQUIRE (node != nullptr);
    auto* stepper = node->getParentComponent();
    REQUIRE (stepper != nullptr);

    auto& param = rangedParameter (processor, aod::ParamIDs::polyLimit);
    param.setValueNotifyingHost (param.convertTo0to1 (32.0f));
    REQUIRE (parameterValue (param) == Catch::Approx (32.0f).margin (0.01f));

    // An AudioParameterInt reports an interval of 0 because its fourth
    // constructor argument is the default, not a step. The control falls back
    // to one whole unit, which is what keeps POLYPHONY alive on the keyboard.
    stepper->keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
    REQUIRE (parameterValue (param) > 32.0f);

    stepper->keyPressed (juce::KeyPress (juce::KeyPress::leftKey));
    REQUIRE (parameterValue (param) == Catch::Approx (32.0f).margin (0.01f));

    stepper->keyPressed (juce::KeyPress (juce::KeyPress::endKey));
    REQUIRE (parameterValue (param) == Catch::Approx (128.0f).margin (0.01f));
}

TEST_CASE ("the tab walk reaches every parameter control in signal order", "[ui][accessibility]")
{
    const juce::ScopedJuceInitialiser_GUI gui;
    aod::RomplerProcessor processor;
    aod::RomplerEditor editor (processor);
    const TemporaryDesktopPeer peer (editor);

    // The walk is the only thing that decides where a keyboard user lands, so
    // pin the order the panel actually reads in: voice, then bus and output,
    // then the FX rail, then the envelope and the compressor, then legato.
    const auto& traversable = editor.createFocusTraverser();
    REQUIRE (traversable != nullptr);

    std::vector<juce::Component*> walked;
    for (auto* c = traversable->getDefaultComponent (&editor);
         c != nullptr && walked.size() < 64;
         c = traversable->getNextComponent (c))
        walked.push_back (c);

    // setWantsKeyboardFocus on a child is only walked by the Tab key when an
    // ancestor is a keyboard focus container; otherwise the key never reaches
    // keyPressed and the whole keyboard layer is inert.
    REQUIRE (editor.isKeyboardFocusContainer());

    // The walk has to visit the controls themselves, not just the editor: a
    // container that nobody can focus is as broken as no container at all.
    int focusableStops = 0;
    for (auto* c : walked)
        if (c->getWantsKeyboardFocus())
            ++focusableStops;
    REQUIRE (focusableStops > 20);

    // And the key has to arrive, not just the focus ring: dispatch a real
    // key event at the walked focus owner and watch the parameter move. This
    // is the whole chain - container, traverser, keyPressed, attachment.
    auto* sustainNode = findAccessibleByTitle (editor, juce::AccessibilityRole::slider, "SUSTAIN");
    REQUIRE (sustainNode != nullptr);
    sustainNode->getParentComponent()->grabKeyboardFocus();
    REQUIRE (sustainNode->getParentComponent()->hasKeyboardFocus (true));

    auto& param = rangedParameter (processor, aod::ParamIDs::envSustain);
    param.setValueNotifyingHost (param.convertTo0to1 (50.0f));

    // ComponentPeer::handleKeyPress is the real dispatch path: it walks up
    // from the focus owner asking each target, so this proves the container,
    // the focus owner and keyPressed are all wired together.
    auto* componentPeer = editor.getPeer();
    REQUIRE (componentPeer != nullptr);
    const juce::KeyPress right (juce::KeyPress::rightKey);
    REQUIRE (componentPeer->handleKeyPress (right));
    REQUIRE (parameterValue (param) > 50.0f);

    const auto indexOf = [&walked] (const juce::Component* target)
    {
        for (std::size_t i = 0; i < walked.size(); ++i)
            if (walked[i] == target)
                return static_cast<int> (i);
        return -1;
    };

    auto* drive = findAccessibleByTitle (editor, juce::AccessibilityRole::slider, "DRIVE");
    auto* sustain = findAccessibleByTitle (editor, juce::AccessibilityRole::slider, "SUSTAIN");
    auto* trim = findAccessibleByTitle (editor, juce::AccessibilityRole::slider, "OUTPUT TRIM");
    REQUIRE (drive != nullptr);
    REQUIRE (sustain != nullptr);
    REQUIRE (trim != nullptr);

    // Voice and bus controls sit before the envelope, matching VOICE -> BUS.
    REQUIRE (indexOf (drive->getParentComponent()) >= 0);
    REQUIRE (indexOf (sustain->getParentComponent()) > indexOf (drive->getParentComponent()));
    REQUIRE (indexOf (trim->getParentComponent()) < indexOf (sustain->getParentComponent()));

    // The envelope follows the FX rail on the panel, so ATTACK has to come
    // after CHORUS RATE. controls_ allocates the two delay knobs at 27/28,
    // ahead of the envelope at 17..20, so array order and panel order are not
    // the same thing and the walk is what keeps them honest.
    auto* attack = findAccessibleByTitle (editor, juce::AccessibilityRole::slider, "ATTACK");
    auto* chorusRate = findAccessibleByTitle (editor, juce::AccessibilityRole::slider, "CHORUS RATE");
    REQUIRE (attack != nullptr);
    REQUIRE (chorusRate != nullptr);

    const auto firstAttack = indexOf (attack->getParentComponent());
    REQUIRE (firstAttack >= 0);
    for (auto* c : walked)
    {
        auto* handler = c->getAccessibilityHandler();
        if (handler == nullptr || handler->getTitle() != "CHORUS RATE")
            continue;
        REQUIRE (indexOf (c) < firstAttack);
    }
}
