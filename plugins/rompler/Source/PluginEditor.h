#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <array>
#include <vector>
#include <functional>

#include "PluginProcessor.h"
#include "Parameters.h"
#include "PresetBrowserOverlay.h"

#if defined(MELATONIN_INSPECTOR_ENABLED)
#include <melatonin_inspector/melatonin_inspector.h>
#endif

namespace aod
{

// === EON analogue hardware theme: warm anodised chassis, ivory legends and
// restrained seafoam status illumination.  The colour hierarchy deliberately
// keeps the panel material neutral, so the signal colours read as hardware
// indicators instead of outlining every software-shaped widget. ===
namespace theme
{
    // Primary chassis and panel surfaces
    inline juce::Colour body         { 0xff171e26 };
    inline juce::Colour body2        { 0xff333f4a };
    inline juce::Colour bodyEdge     { 0xff88929e };

    // Control panels - warm graphite / anodised aluminium
    inline juce::Colour panel        { 0xff35414d };
    inline juce::Colour panelEdge    { 0xff6f7d8a };

    // Text - slightly warm paint rather than a blue-white UI font
    inline juce::Colour ink          { 0xfffbf8e9 };
    inline juce::Colour inkSoft      { 0xffcdd6da };

    // Seafoam accents - reserved for interaction and signal flow
    inline juce::Colour mint         { 0xff5ad2e6 };
    inline juce::Colour mintDeep     { 0xff1f7fa8 };
    inline juce::Colour mintGlow     { 0xffc9ecf6 };

    // Hot / drive controls - amber for emphasis
    inline juce::Colour hot          { 0xfff2b25c };
    inline juce::Colour hotDeep      { 0xffbd6835 };

    // Knob materials. The bright cream remains useful for legends and keys;
    // the rotary caps themselves use a low-sheen graphite stack below.
    inline juce::Colour knobCream    { 0xfff7f4e3 };
    inline juce::Colour knobShadow   { 0xffa4adb4 };
    inline juce::Colour knobWell     { 0xff070b0b };
    inline juce::Colour knobSide     { 0xff182129 };
    inline juce::Colour knobCap      { 0xff232e38 };
    inline juce::Colour knobCapHi    { 0xff6c7c88 };
    inline juce::Colour knobRim      { 0xff9aa6b0 };
    inline juce::Colour knobPointer  { 0xfffffdf1 };

    // LED palette - signal-meter colors
    inline juce::Colour ledRed       { 0xfff16d58 };
    inline juce::Colour ledHot       { 0xffffc06a };
    inline juce::Colour ledMint      { 0xff6cd4ee };
    inline juce::Colour ledOff       { 0xff101722 };

    // Display area - near-black LCD window
    inline juce::Colour displayBg    { 0xff071320 };
    inline juce::Colour displayFg    { 0xff1d3c50 };
    inline juce::Colour displayOn    { 0xff9adff2 };
}

// Build the panel typography from installed workstation-style faces rather
// than JUCE's generic fallback.  Avenir Next keeps the small legends open and
// human, while the extra tracking gives the labels the engraved-panel spacing
// seen on hardware synths.  If a host does not provide the face JUCE falls
// back to its normal sans-serif metrics automatically.
inline juce::Font makeFont (float pt, bool bold)
{
    const auto style = bold ? juce::Font::bold : juce::Font::plain;
    return juce::Font (juce::FontOptions ("Avenir Next", pt, style)
                           .withKerningFactor (bold ? 0.012f : 0.018f));
}

// Numeric readouts use DIN Alternate, whose squared forms and open counters
// read like a real synth LCD without resorting to a synthetic seven-segment
// bitmap.  Keep this separate so descriptive labels remain warmer.
inline juce::Font makeDisplayFont (float pt, bool bold = true)
{
    const auto style = bold ? juce::Font::bold : juce::Font::plain;
    return juce::Font (juce::FontOptions ("DIN Alternate", pt, style)
                           .withKerningFactor (0.02f));
}

// Labels are rendered with a restrained two-pass extrusion.  The one-pixel
// lower pass gives small legends a physical, screen-printed edge while the
// foreground remains crisp at plugin scale.  This is intentionally subtle so
// it reads as depth rather than a drop-shadow effect.
class DepthLabel : public juce::Label
{
public:
    using juce::Label::Label;

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().reduced (1);
        if (isOpaque())
        {
            g.setColour (findColour (juce::Label::backgroundColourId));
            g.fillRoundedRectangle (area.toFloat(), 4.0f);
        }
        const auto text = getText();
        if (text.isEmpty())
            return;

        g.setFont (getFont());
        const auto justification = getJustificationType();
        const auto colour = findColour (juce::Label::textColourId);
        g.setColour (juce::Colour (0x52000000));
        g.drawFittedText (text, area.translated (0, 1), justification, 4,
                          getMinimumHorizontalScale());
        g.setColour (colour);
        g.drawFittedText (text, area, justification, 4,
                          getMinimumHorizontalScale());
    }
};

/** A local, paint-only hardware key used for loading a SoundFont.  Keeping it
    isolated avoids changing JUCE's global button look in a host window. */
class HardwareButton final : public juce::TextButton
{
public:
    explicit HardwareButton (const juce::String& text) : juce::TextButton (text) {}

    /** In skin mode the faceplate already paints the button artwork; the
        control only draws a hover/press state glow and stays transparent. */
    void setSkinMode (bool on) { skinMode_ = on; repaint(); }

    void paintButton (juce::Graphics&, bool isMouseOverButton, bool isButtonDown) override;

private:
    bool skinMode_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardwareButton)
};

/**
    A mint-outlined cream panel with a tab label in its top edge, like the
    microKORG-style function boxes in the design mockup. Child controls are
    laid out by the owning editor; this class just draws the box.
*/
class SectionBox final : public juce::Component
{
public:
    explicit SectionBox (const juce::String& title);
    ~SectionBox() override;
    void paint (juce::Graphics&) override;

private:
    DepthLabel title_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionBox)
};

/**
    Rotary control with an original dark workstation-hardware treatment: a
    recessed well, sidewall, knurled perimeter, bright indicator strip and a
    264-degree scale centred at 12 o'clock. Drives a single
    RangedAudioParameter through a SliderParameterAttachment. Vertical drag
    and the mouse wheel change the value; its name and live value appear only
    while it is being adjusted.
*/
class Knob final : public juce::Component,
                   public juce::SettableTooltipClient,
                   private juce::AudioProcessorParameter::Listener,
                   private juce::AsyncUpdater,
                   private juce::Timer
{
public:
    explicit Knob (juce::RangedAudioParameter& param, bool hot = false);
    ~Knob() override;

    /** Optionally override the printed name with an explicit UI label. */
    void setNameOverride (const juce::String& label) { name_.setText (label, juce::dontSendNotification); }

    /** In skin mode the knob body is part of the faceplate image; only the
        live pointer, fill arc and transient readout are drawn on top. */
    void setSkinMode (bool on);

    /** Pull the current parameter value into the custom-drawn slider immediately. */
    void syncFromParameter();

    /** True while the parameter sits at its APVTS default value. */
    [[nodiscard]] bool isAtInitState() const noexcept { return atInit_; }

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** The transient readout string, including the parameter's unit label. */
    [[nodiscard]] juce::String readoutText();

    /** Called (synchronously, possibly from the audio thread) when the bound
        parameter's value changes from any source - drag, wheel, host
        automation, preset restore. We only flag an async update here; the
        actual repaint happens on the message thread in handleAsyncUpdate. */
    void parameterValueChanged (int, float) override;
    void parameterGestureChanged (int, bool) override {}

private:
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void setReadoutVisible (bool shouldBeVisible);
    void refreshInitState();
    void paintSkin (juce::Graphics&);

    [[maybe_unused]] juce::RangedAudioParameter& param_;
    juce::Slider slider_;
    DepthLabel name_;
    DepthLabel value_;
    std::unique_ptr<juce::SliderParameterAttachment> attachment_;
    float lastDragY_ = 0.0f;
    bool hot_ = false;
    bool skinMode_ = false;
    bool readoutVisible_ = false;
    bool pressed_ = false;
    bool atInit_ = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Knob)
};

/**
    A labelled segmented button group for choice parameters, like the CURVE
    and OVERSAMPLE switches in the mockup. Drives an AudioParameterChoice via
    a ComboBoxParameterAttachment backed by a hidden combo box.
*/
class Switch final : public juce::Component,
                     public juce::SettableTooltipClient
{
public:
    explicit Switch (juce::AudioParameterChoice& param, const juce::String& label = {}, bool leds = false);
    ~Switch() override;

    void setLabel (const juce::String& s) { label_.setText (s, juce::dontSendNotification); }

    /** In skin mode only the live choice is drawn over the faceplate
        artwork: a capsule readout for CURVE, an LED strip for OVERSAMPLE. */
    void setSkinMode (bool on) { skinMode_ = on; label_.setVisible (! on); repaint(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    /** Moves the choice by @p direction, wrapping; negative steps backwards. */
    void advanceChoice (int direction = 1);
    void paintSkin (juce::Graphics&);

    [[maybe_unused]] juce::AudioParameterChoice& param_;
    DepthLabel label_;
    juce::ComboBox box_;
    std::unique_ptr<juce::ComboBoxParameterAttachment> attachment_;
    juce::Rectangle<int> pill_;
    float lastDragY_ = 0.0f;
    /// Set when a drag already stepped the choice, so the release is not
    /// counted as a second click.
    bool steppedByDrag_ = false;
    bool leds_ = false;
    bool skinMode_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Switch)
};

/**
    A 2-way toggle switch, like the FILTER ROUTE Pre/Post switch in the
    mockup. Drives an AudioParameterChoice with exactly two choices.
*/
class Toggle final : public juce::Component,
                     public juce::SettableTooltipClient
{
public:
    explicit Toggle (juce::AudioParameterChoice& param, const juce::String& label = {});
    ~Toggle() override;

    void setLabel (const juce::String& s) { label_.setText (s, juce::dontSendNotification); }

    /** In skin mode the painted PRE/POST pill stays visible; the control
        only highlights the active half. */
    void setSkinMode (bool on) { skinMode_ = on; label_.setVisible (! on); repaint(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    [[maybe_unused]] juce::AudioParameterChoice& param_;
    DepthLabel label_;
    juce::ComboBox box_;
    std::unique_ptr<juce::ComboBoxParameterAttachment> attachment_;
    bool skinMode_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Toggle)
};

/**
    A vertical, spring-loaded pitch-bend wheel like the one beside a synth's
    keybed. Dragging bends pitch across [-1, 1]; releasing snaps it back to
    centre, matching physical pitch wheels. setValue() lets the editor
    reflect live MIDI pitch-bend or host automation while the wheel is not
    being dragged, so the widget tracks real playing input, not just the UI.
*/
class PitchWheel final : public juce::Component
{
public:
    explicit PitchWheel (std::function<void (float)> onChange);
    ~PitchWheel() override;

    /** In skin mode only the carriage position marker is drawn over the
        painted wheel slot. */
    void setSkinMode (bool on) { skinMode_ = on; label_.setVisible (! on); repaint(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    /** Reflects an externally-driven value (live MIDI or host automation)
        while the wheel is not being dragged. Value in [-1, 1]. */
    void setValue (float normalizedValue);

private:
    std::function<void (float)> onChange_;
    DepthLabel label_;
    float value_ = 0.0f;
    bool dragging_ = false;
    float dragStartY_ = 0.0f;
    float dragStartValue_ = 0.0f;
    bool skinMode_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchWheel)
};

/**
    A vertical modulation wheel. Dragging changes the value across [0, 1] and,
    unlike the pitch wheel, holds its position on release. setValue() lets the
    editor reflect live MIDI CC1 or host automation while it is not being
    dragged, so the wheel visually tracks actual vibrato depth in real time.
*/
class ModWheel final : public juce::Component
{
public:
    explicit ModWheel (std::function<void (float)> onChange);
    ~ModWheel() override;

    /** In skin mode only the carriage position marker is drawn over the
        painted wheel slot. */
    void setSkinMode (bool on) { skinMode_ = on; label_.setVisible (! on); repaint(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    /** Reflects an externally-driven value (live MIDI or host automation)
        while the wheel is not being dragged. Value in [0, 1]. */
    void setValue (float normalizedValue);

private:
    std::function<void (float)> onChange_;
    DepthLabel label_;
    float value_ = 0.0f;
    bool dragging_ = false;
    float dragStartY_ = 0.0f;
    float dragStartValue_ = 0.0f;
    bool skinMode_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModWheel)
};

/**
    A numeric stepper with up/down arrows and an LED digit readout, like the
    POLYPHONY control in the mockup. Drives an AudioParameterInt via a
    SliderParameterAttachment backed by a hidden slider.
*/
class Stepper final : public juce::Component,
                      public juce::SettableTooltipClient,
                      private juce::AudioProcessorParameter::Listener,
                      private juce::AsyncUpdater
{
public:
    explicit Stepper (juce::RangedAudioParameter& param, const juce::String& label = {});
    ~Stepper() override;

    void setLabel (const juce::String& s) { label_.setText (s, juce::dontSendNotification); }

    /** In skin mode the digit window replaces the painted readout with a
        live LCD; the legend stays printed on the faceplate. */
    void setSkinMode (bool on) { skinMode_ = on; label_.setVisible (! on); repaint(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** Called (synchronously, possibly from the audio thread) when the bound
        parameter's value changes from any source. Only flags an async update;
        the digit readout repaints on the message thread in handleAsyncUpdate. */
    void parameterValueChanged (int, float) override;
    void parameterGestureChanged (int, bool) override {}

private:
    void handleAsyncUpdate() override;

    [[maybe_unused]] juce::RangedAudioParameter& param_;
    DepthLabel label_;
    juce::Slider slider_;
    std::unique_ptr<juce::SliderParameterAttachment> attachment_;
    float lastDragY_ = 0.0f;
    bool skinMode_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Stepper)
};

/** A column of LED segments that light up with the output peak level. */
class PeakMeter final : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    PeakMeter();
    ~PeakMeter() override;

    void setLevel (float level);   // 0..1
    /** True while the clip segment is latched. Exposed for the UI tests. */
    [[nodiscard]] bool isClipped() const noexcept { return clipped_; }

    /** In skin mode only the lit segments draw over the painted meter strip. */
    void setSkinMode (bool on) { skinMode_ = on; repaint(); }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static constexpr int numSegments = 8;
    /// Segments at or above this level count as the safety ceiling engaging
    /// (the ceiling shaper saturates at 0.985, so 0.98 is just under it).
    static constexpr float clipThreshold = 0.98f;
    float level_ = 0.0f;
    bool clipped_ = false;
    bool skinMode_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PeakMeter)
};

/** Read-only horizontal LED meter for compressor gain reduction in dB, with a
    slow-falling peak hold that a click clears. */
class GainReductionMeter final : public juce::Component,
                                 public juce::SettableTooltipClient
{
public:
    GainReductionMeter();
    ~GainReductionMeter() override;

    void setReductionDb (float reductionDb);
    /** Largest reduction on the hold marker, in dB. Exposed for the UI tests. */
    [[nodiscard]] float peakHoldDb() const noexcept { return peakHoldDb_; }

    /** In skin mode only the lit segments draw over the painted GR strip. */
    void setSkinMode (bool on) { skinMode_ = on; repaint(); }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static constexpr int numSegments = 6;
    /** Segment index the hold marker sits on, or -1 when the live level is
        already at least as high. @p lit is the live lit-segment count. */
    [[nodiscard]] int holdSegmentFor (int lit) const noexcept;
    /// Per-update fall of the hold marker. The meter is pushed once per UI
    /// tick, so this is a fixed rate rather than a wall-clock decay.
    static constexpr float holdFallDb = 0.9f;
    float reductionDb_ = 0.0f;
    float peakHoldDb_ = 0.0f;
    bool skinMode_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GainReductionMeter)
};

struct PianoKey
{
    int note = 0;
    bool black = false;
    int whiteIndex = 0;   // white key this black key sits immediately to the right of
};

/** A clickable, playable piano. Sends note-on/off to the processor. */
class Keyboard final : public juce::Component
{
public:
    Keyboard();
    ~Keyboard() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    void setKeyRange (int lowNote, int numKeys);
    void setNoteOn (int note, bool on);
    /** True while @p note is drawn as sounding (keybed press or MIDI mirror). */
    [[nodiscard]] bool isNoteLit (int note) const;
    void setNoteCallback (std::function<void (int, bool)> cb) { noteCallback_ = std::move (cb); }
    int findNote (juce::Point<float>) const;

    /** In skin mode the painted keybed stays visible; only pressed-key
        highlights are drawn on top. */
    void setSkinMode (bool on) { skinMode_ = on; repaint(); }

private:
    juce::Rectangle<int> boundsFor (const PianoKey&) const;

    std::vector<PianoKey> keys_;
    int numWhite_ = 0;
    juce::HashMap<int, bool> lit_;
    std::function<void (int, bool)> noteCallback_;   // set by the editor -> postNote
    bool skinMode_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Keyboard)
};

/** A single SoundFont program as listed in the browser. */
struct PresetEntry
{
    int bank = 0;
    int program = 0;
    juce::String name;
};

/**
    The bank/program browser: a bank selector strip on top and a scrolling list
    of the programs in the active bank below. Clicking a row selects that
    program through the callback wired to RomplerProcessor::selectPreset.
*/
class BankBrowser final : public juce::Component,
                          private juce::ListBoxModel
{
public:
    BankBrowser();
    ~BankBrowser() override;

    void setPresets (std::vector<PresetEntry> presets);
    void setCurrent (int bank, int program);
    void setOnSelect (std::function<void (int, int)> cb) { onSelect_ = std::move (cb); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // juce::ListBoxModel
    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics&, int width,
                           int height, bool rowIsSelected) override;
    void selectedRowsChanged (int lastRowChanged) override;

    void buildBanks();
    void filterFor (int bank);

    juce::ComboBox bankCombo_;
    juce::ListBox list_ { "preset list", this };
    DepthLabel emptyHint_;
    std::vector<PresetEntry> presets_;
    std::vector<int> banks_;
    std::vector<int> filtered_;          // indices into presets_ for the active bank
    int selectedBank_ = -1;
    std::function<void (int, int)> onSelect_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BankBrowser)
};

/** Interactive ADSR envelope preview. Drag a stage point to edit its bound
    parameter; the existing knobs and host automation stay in sync. */
class EnvelopeGraph final : public juce::Component,
                           private juce::AudioProcessorParameter::Listener,
                           private juce::AsyncUpdater
{
public:
    EnvelopeGraph (juce::RangedAudioParameter& attack,
                   juce::RangedAudioParameter& decay,
                   juce::RangedAudioParameter& sustain,
                   juce::RangedAudioParameter& release);
    ~EnvelopeGraph() override;

    /** In skin mode the curve and drag points draw directly over the
        painted ENV display; no bezel or screen is painted. */
    void setSkinMode (bool on) { skinMode_ = on; repaint(); }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void parameterValueChanged (int, float) override;
    void parameterGestureChanged (int, bool) override {}
    void handleAsyncUpdate() override { repaint(); }
    juce::Point<float> pointFor (int stage, juce::Rectangle<float> area) const;

    std::array<juce::RangedAudioParameter*, 4> params_;
    std::array<juce::Slider, 4> sliders_;
    std::array<std::unique_ptr<juce::SliderParameterAttachment>, 4> attachments_;
    int activeStage_ = -1;
    bool skinMode_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeGraph)
};

class RomplerEditor final : public juce::AudioProcessorEditor,
                            private juce::ComboBox::Listener,
                            private juce::Timer
{
public:
    explicit RomplerEditor (RomplerProcessor& processorRef);
    ~RomplerEditor() override;

    /** Updates the document identity shown in the always-visible header. */
    void setPresetHeaderDocument (const PresetDocument& document);
    /** Marks the current document clean after a successful save. */
    void markPresetHeaderSaved();
    [[nodiscard]] bool isPresetHeaderDirty() const noexcept { return presetDirty_; }

    /** Opens the non-modal preset library without taking host keyboard focus. */
    void openPresetBrowser();
    void closePresetBrowser();

    void paint (juce::Graphics&) override;
    void resized() override;

    /** The image-space canvas used by the skin and its interactive overlays. */
    [[nodiscard]] juce::Rectangle<int> getSkinCanvasBoundsForTesting() const noexcept;

private:
    RomplerProcessor& processor_;
    PresetLibrary presetLibrary_;

    DepthLabel brandTitle_;
    DepthLabel brandSub_;
    DepthLabel brandSub2_;
    DepthLabel presetName_;
    DepthLabel presetDirtyIndicator_;

    SectionBox voiceBox_;
    SectionBox busBox_;
    SectionBox compBox_;
    SectionBox envBox_;
    SectionBox fxBox_;
    std::unique_ptr<EnvelopeGraph> envGraph_;

    // Controls, in a flat list. Order: VOICE (0-5), BUS (6-10, 29-30), FX
    // (11-16, 27-28), ENV (17-20), COMP (21-26), VOICE legato toggle (31).
    std::array<std::unique_ptr<juce::Component>, 32> controls_;

    // Performance controls beside the keyboard: pitch bend (springs to
    // centre) and mod wheel (holds position, drives vibrato depth). Both
    // post through the processor so dragging behaves identically to live
    // MIDI, and both repaint from a 20 Hz timer poll of the processor's
    // current value so real MIDI/host input is reflected too.
    std::unique_ptr<PitchWheel> pitchWheel_;
    std::unique_ptr<ModWheel> modWheel_;

    DepthLabel compPathLabel_;
    GainReductionMeter gainReductionMeter_;

    DepthLabel sfLabel_;
    DepthLabel sfDisplay_;
    DepthLabel bankDigits_;
    HardwareButton loadButton_ { "LOAD" };
    PeakMeter peakMeter_;

    std::unique_ptr<BankBrowser> bankBrowser_;
    std::unique_ptr<PresetBrowserOverlay> presetOverlay_;
    HardwareButton presetButton_ { "PRESETS" };
    Keyboard keyboard_;

    // The approved Blue Dream product render is embedded in the plugin and
    // used as the exact faceplate surface.  Child controls remain alive as
    // transparent hit targets so parameter/MIDI behaviour is preserved.
    juce::Image skinImage_;
    bool skinMode_ = false;

    std::unique_ptr<juce::FileChooser> fileChooser_;
    PresetDocument activePreset_;
    bool hasActivePreset_ = false;
    bool presetDirty_ = false;

    // Last published sounding-note mask; the timer diffs it against the
    // processor's snapshot so only changed keys repaint.
    std::uint64_t uiNotesLo_ = 0;
    std::uint64_t uiNotesHi_ = 0;

    // Hosts every child control's setTooltip() text. Owned by the editor so
    // the window dies before the components it observes.
    std::unique_ptr<juce::TooltipWindow> tooltipWindow_;

    void refreshDisplay();
    void refreshPresetHeader();
    void refreshPresetList();
    void loadPresetUuid (const juce::String& uuid);
    void handlePresetDirtyChoice (const juce::String& uuid, PresetBrowserOverlay::DirtyChoice choice);
    void onLoadButtonClicked();
    void layoutSkin();
    void layoutVoiceControls (juce::Rectangle<int> area);
    void layoutBusControls (juce::Rectangle<int> area);
    void layoutCompControls (juce::Rectangle<int> area);
    void layoutEnvControls (juce::Rectangle<int> area);
    void layoutFxControls (juce::Rectangle<int> area);
    [[nodiscard]] juce::Rectangle<float> skinCanvasBounds() const noexcept;

    void comboBoxChanged (juce::ComboBox*) override;
    void timerCallback() override;

#if defined(MELATONIN_INSPECTOR_ENABLED)
    melatonin::Inspector inspector { *this, false };
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RomplerEditor)
};

} // namespace aod
