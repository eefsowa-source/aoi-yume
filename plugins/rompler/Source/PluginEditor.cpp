#include "PluginEditor.h"

namespace aod
{

// ============================================================================
// SectionBox
// ============================================================================

SectionBox::SectionBox (const juce::String& title)
{
    addAndMakeVisible (title_);
    title_.setText (title, juce::dontSendNotification);
    title_.setJustificationType (juce::Justification::centred);
    title_.setColour (juce::Label::textColourId, juce::Colour (0xff06231b));
    title_.setFont (makeFont (10.0f, true));
    title_.setBounds (10, -8, 80, 16);
}

SectionBox::~SectionBox() = default;

void SectionBox::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
    g.setColour (theme::mintDeep);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 8.0f, 1.5f);

    // The tab: a mint rectangle sitting on the top edge behind the title.
    const auto tab = juce::Rectangle<int> (10, -8, 80, 16);
    g.setColour (theme::mintDeep);
    g.fillRoundedRectangle (tab.toFloat(), 3.0f);
}

// ============================================================================
// Knob
// ============================================================================

Knob::Knob (juce::RangedAudioParameter& param, bool hot)
    : param_ (param), hot_ (hot)
{
    slider_.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    slider_.setDoubleClickReturnValue (false, 0.5, true);
    slider_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    attachment_ = std::make_unique<juce::SliderParameterAttachment> (param, slider_);

    // The slider is not a visible child (we draw the knob ourselves), so value
    // changes that arrive through the attachment never repaint us. Registering
    // as a parameter listener makes the pointer and value print track the
    // parameter from any source: drag, wheel, host automation, preset load.
    param_.addListener (this);

    addAndMakeVisible (name_);
    name_.setText (param.getName (32), juce::dontSendNotification);
    name_.setJustificationType (juce::Justification::centred);
    name_.setColour (juce::Label::textColourId, theme::ink);
    name_.setFont (makeFont (9.5f, true));

    addAndMakeVisible (value_);
    value_.setJustificationType (juce::Justification::centred);
    value_.setColour (juce::Label::textColourId, theme::inkSoft);
    value_.setFont (makeFont (9.0f, false));

    setSize (70, 96);
}

Knob::~Knob()
{
    // The parameter (owned by the processor) outlives this widget, so we must
    // unregister before destruction or a later host change would call back into
    // freed memory.
    param_.removeListener (this);
}

void Knob::parameterValueChanged (int, float)
{
    // JUCE calls this synchronously and may do so from the audio thread.
    // repaint() must run on the message thread, so just request an async
    // update and let handleAsyncUpdate() do the drawing.
    triggerAsyncUpdate();
}

void Knob::handleAsyncUpdate()
{
    repaint();
}

void Knob::paint (juce::Graphics& g)
{
    const float w = (float) getWidth();
    const float h = (float) getHeight();
    const float knobSize = std::min (std::min (w, h - 30.0f), 56.0f);
    auto circle = juce::Rectangle<float> (0.0f, 0.0f, w, h)
                      .withSizeKeepingCentre (knobSize, knobSize);

    // Knob cap: cream (or hot orange for the drive-shaping controls).
    juce::ColourGradient grad (hot_ ? juce::Colour (0xffffd9b8) : juce::Colour (0xffffffff),
                               circle.getTopLeft(),
                               hot_ ? theme::hotDeep : theme::knobShadow,
                               circle.getBottomLeft(), false);
    g.setGradientFill (grad);
    g.fillEllipse (circle);
    g.setColour (juce::Colour (0x33000000));
    g.drawEllipse (circle, 1.0f);

    // Pointer (mint on cream knobs, dark on hot knobs).
    // slider_.getValue() is the denormalised parameter value (e.g. Drive is
    // 0..100); the pointer angle is computed from the 0..1 normalised value.
    const float unit = static_cast<float> (param_.convertTo0to1 (static_cast<float> (slider_.getValue())));
    const float angleRad = (juce::MathConstants<float>::pi / 180.0f) * ((unit * 264.0f) - 132.0f);
    const auto centre = circle.getCentre();
    const float r = circle.getWidth() * 0.5f - 5.0f;
    const juce::Point<float> tip (centre.x + r * std::cos (angleRad),
                                  centre.y + r * std::sin (angleRad));
    g.setColour (hot_ ? juce::Colour (0xff4a2408) : theme::mintDeep);
    g.drawLine (centre.x, centre.y, tip.x, tip.y, 2.5f);

    value_.setText (slider_.getTextFromValue (slider_.getValue()), juce::dontSendNotification);
}

void Knob::resized()
{
    const auto w = getWidth();
    const auto h = getHeight();
    name_.setBounds (getLocalBounds().withSizeKeepingCentre (w, 14).withY (0));
    value_.setBounds (getLocalBounds().withSizeKeepingCentre (w, 12).withY (h - 12));
}

void Knob::mouseDown (const juce::MouseEvent& e)
{
    lastDragY_ = e.y;
    setMouseCursor (juce::MouseCursor::IBeamCursor);
}

void Knob::mouseUp (const juce::MouseEvent&)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void Knob::mouseDrag (const juce::MouseEvent& e)
{
    // Map a full-height drag to the parameter's whole range so the knob is
    // actually usable. The slider stores denormalised parameter values (e.g.
    // Drive is 0..100), so a raw per-pixel delta of 1/60 is far too small to
    // move it visibly.
    const auto range = param_.getNormalisableRange();
    const float pixelsPerFullRange = 200.0f;
    const float delta = (lastDragY_ - e.y) * (range.end - range.start) / pixelsPerFullRange;
    lastDragY_ = e.y;
    slider_.setValue (slider_.getValue() + delta, juce::sendNotificationSync);
}

void Knob::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const auto range = param_.getNormalisableRange();
    const float step = (range.end - range.start) / 100.0f;
    slider_.setValue (slider_.getValue() + (float) wheel.deltaY * step, juce::sendNotificationSync);
}

// ============================================================================
// Switch (segmented buttons)
// ============================================================================

Switch::Switch (juce::AudioParameterChoice& param, const juce::String& label, bool leds)
    : param_ (param), leds_ (leds)
{
    addAndMakeVisible (label_);
    label_.setText (label.isEmpty() ? param.getName (32).toUpperCase() : label, juce::dontSendNotification);
    label_.setJustificationType (juce::Justification::centred);
    label_.setColour (juce::Label::textColourId, theme::ink);
    label_.setFont (makeFont (9.5f, true));

    for (const auto& choice : param.getAllValueStrings())
        box_.addItem (choice, box_.getNumItems() + 1);
    attachment_ = std::make_unique<juce::ComboBoxParameterAttachment> (param, box_);

    setSize (110, 64);
}

Switch::~Switch() = default;

void Switch::resized()
{
    const auto w = getWidth();
    const int pillW = juce::jlimit (48, w - 8, 96);
    const int top = leds_ ? 2 : 4;
    pill_ = juce::Rectangle<int> (0, top, pillW, leds_ ? 20 : 24).withX ((w - pillW) / 2);
    // In the led variant the five segments are drawn from pill_.getBottom()+4
    // down to +20, so the label must sit below them instead of overlapping.
    label_.setBounds (getLocalBounds().withSizeKeepingCentre (w, 14).withY (pill_.getBottom() + (leds_ ? 22 : 4)));
}

void Switch::advanceChoice()
{
    const int n = box_.getNumItems();
    if (n <= 1)
        return;
    const int cur = box_.getSelectedId();
    box_.setSelectedId (cur >= n ? 1 : cur + 1, juce::sendNotificationSync);
    repaint();
}

void Switch::paint (juce::Graphics& g)
{
    auto b = pill_.toFloat();
    g.setColour (theme::displayBg);
    g.fillRoundedRectangle (b, 5.0f);
    g.setColour (juce::Colour (0x33000000));
    g.drawRoundedRectangle (b, 5.0f, 1.0f);

    if (leds_)
    {
        const int n = 5;
        const float gap = 3.0f;
        const float pad = 8.0f;
        const float ledW = (getWidth() - 2 * pad - gap * (n - 1)) / (float) n;
        const float ledH = 16.0f;
        const float ledY = b.getBottom() + 4.0f;
        for (int i = 0; i < n; ++i)
        {
            auto r = juce::Rectangle<float> (pad + i * (ledW + gap), ledY, ledW, ledH);
            const auto colour = (i == n - 1) ? theme::ledRed
                             : (i == n - 2) ? theme::ledHot
                                            : theme::ledMint;
            g.setColour (colour);
            g.fillRoundedRectangle (r, 2.0f);
        }
    }

    const int idx = box_.getSelectedItemIndex();
    juce::String name = (idx >= 0 && idx + 1 <= box_.getNumItems())
                        ? box_.getItemText (idx + 1).toUpperCase()
                        : juce::String();
    g.setColour (theme::mint);
    g.setFont (makeFont (9.0f, true));
    g.drawText (name, b, juce::Justification::centred);
}

void Switch::mouseDown (const juce::MouseEvent& e)
{
    lastDragY_ = e.y;
    setMouseCursor (juce::MouseCursor::IBeamCursor);
}

void Switch::mouseUp (const juce::MouseEvent& e)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
    if (std::abs (lastDragY_ - e.y) < 5.0f)
        advanceChoice();
}

void Switch::mouseDrag (const juce::MouseEvent& e)
{
    const float dy = lastDragY_ - e.y;
    if (std::abs (dy) > 24.0f)
    {
        lastDragY_ = e.y;
        advanceChoice();
    }
}

void Switch::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY > 0)
    {
        const int n = box_.getNumItems();
        const int cur = box_.getSelectedId();
        box_.setSelectedId (cur <= 1 ? n : cur - 1, juce::sendNotificationSync);
    }
    else if (wheel.deltaY < 0)
        advanceChoice();
    repaint();
}

// ============================================================================
// Toggle (2-way)
// ============================================================================

Toggle::Toggle (juce::AudioParameterChoice& param, const juce::String& label)
    : param_ (param)
{
    addAndMakeVisible (label_);
    label_.setText (label.isEmpty() ? param.getName (32).toUpperCase() : label, juce::dontSendNotification);
    label_.setJustificationType (juce::Justification::centred);
    label_.setColour (juce::Label::textColourId, theme::ink);
    label_.setFont (makeFont (9.5f, true));

    for (const auto& choice : param.getAllValueStrings())
        box_.addItem (choice, box_.getNumItems() + 1);
    attachment_ = std::make_unique<juce::ComboBoxParameterAttachment> (param, box_);

    setSize (110, 64);
}

Toggle::~Toggle() = default;

void Toggle::resized()
{
    const auto w = getWidth();
    label_.setBounds (getLocalBounds().withSizeKeepingCentre (w, 14).withY (32));
}

void Toggle::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const int idx = box_.getSelectedItemIndex();

    const float trackW = 44.0f;
    const float trackH = 20.0f;
    auto track = juce::Rectangle<float> ((b.getWidth() - trackW) * 0.5f, 6.0f, trackW, trackH);

    const bool on = (idx == 1);
    g.setColour (on ? theme::mint : juce::Colour (0xffb3aa8a));
    g.fillRoundedRectangle (track, trackH * 0.5f);
    g.setColour (juce::Colour (0x40000000));
    g.drawRoundedRectangle (track, trackH * 0.5f, 1.0f);

    const float thumbD = 14.0f;
    const float thumbX = on ? track.getX() + track.getWidth() - thumbD - 2.0f : track.getX() + 2.0f;
    g.setColour (juce::Colour (0xfff2ecd8));
    g.fillEllipse (thumbX, track.getY() + 2.0f, thumbD, thumbD);

    // Pre / Post labels either side.
    g.setColour (on ? theme::mintDeep : theme::inkSoft);
    g.setFont (makeFont (8.0f, true));
    g.drawText ("PRE",  juce::roundToInt (track.getX()) - 30, 8, 26, 16, juce::Justification::centred);
    g.drawText ("POST", juce::roundToInt (track.getRight()) + 4, 8, 30, 16, juce::Justification::centred);
}

void Toggle::mouseDown (const juce::MouseEvent&)
{
    box_.setSelectedId ((box_.getSelectedId() == 1) ? 2 : 1, juce::sendNotificationSync);
    repaint();
}

// ============================================================================
// Stepper (polyphony)
// ============================================================================

Stepper::Stepper (juce::RangedAudioParameter& param, const juce::String& label)
    : param_ (param)
{
    addAndMakeVisible (label_);
    label_.setText (label.isEmpty() ? param.getName (32).toUpperCase() : label, juce::dontSendNotification);
    label_.setJustificationType (juce::Justification::centred);
    label_.setColour (juce::Label::textColourId, theme::ink);
    label_.setFont (makeFont (9.5f, true));

    slider_.setRange (param.getNormalisableRange().start, param.getNormalisableRange().end, 1.0);
    slider_.setValue (param.getValue(), juce::dontSendNotification);
    attachment_ = std::make_unique<juce::SliderParameterAttachment> (param, slider_);

    // See Knob: the hidden slider's value changes do not repaint the readout
    // on their own, so listen to the parameter directly.
    param_.addListener (this);

    setSize (110, 64);
}

Stepper::~Stepper()
{
    param_.removeListener (this);
}

void Stepper::parameterValueChanged (int, float)
{
    triggerAsyncUpdate();
}

void Stepper::handleAsyncUpdate()
{
    repaint();
}

void Stepper::resized()
{
    const auto w = getWidth();
    label_.setBounds (getLocalBounds().withSizeKeepingCentre (w, 14).withY (36));
}

void Stepper::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const int value = juce::roundToInt (slider_.getValue());

    auto digits = juce::Rectangle<float> ((b.getWidth() - 48.0f) * 0.5f, 2.0f, 48.0f, 28.0f);
    g.setColour (theme::displayBg);
    g.fillRoundedRectangle (digits, 3.0f);
    g.setColour (theme::mint);
    g.setFont (makeFont (13.0f, true));
    g.drawText (juce::String (value).paddedLeft ('0', 2), digits, juce::Justification::centred);
}

void Stepper::mouseDown (const juce::MouseEvent& e)
{
    lastDragY_ = e.y;
    setMouseCursor (juce::MouseCursor::IBeamCursor);
}

void Stepper::mouseUp (const juce::MouseEvent&)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void Stepper::mouseDrag (const juce::MouseEvent& e)
{
    // Same scaling as Knob: a full-height drag spans the parameter's range, so
    // integer parameters (polyphony 1..128) advance visibly instead of 1 per
    // 60 pixels.
    const auto range = param_.getNormalisableRange();
    const float pixelsPerFullRange = 200.0f;
    const float delta = (lastDragY_ - e.y) * (range.end - range.start) / pixelsPerFullRange;
    lastDragY_ = e.y;
    slider_.setValue (slider_.getValue() + delta, juce::sendNotificationSync);
}

void Stepper::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    slider_.setValue (slider_.getValue() + (float) wheel.deltaY, juce::sendNotificationSync);
}

// ============================================================================
// PeakMeter
// ============================================================================

PeakMeter::PeakMeter() = default;
PeakMeter::~PeakMeter() = default;

void PeakMeter::setLevel (float level)
{
    level_ = juce::jlimit (0.0f, 1.0f, level);
    repaint();
}

void PeakMeter::paint (juce::Graphics& g)
{
    const float w = (float) getWidth();
    const float h = (float) getHeight();
    const float gap = 3.0f;
    const float ledW = (w - gap * (numSegments - 1)) / (float) numSegments;
    const int lit = juce::roundToInt (level_ * (float) numSegments);

    for (int i = 0; i < numSegments; ++i)
    {
        auto r = juce::Rectangle<float> (i * (ledW + gap), 0.0f, ledW, h);
        const bool on = i < lit;
        const auto colour = (i >= numSegments - 1) ? theme::ledRed
                          : (i >= numSegments - 3) ? theme::ledHot
                                                   : theme::ledMint;
        g.setColour (on ? colour : theme::ledOff);
        g.fillRoundedRectangle (r, 2.0f);
    }
}

// ============================================================================
// Keyboard
// ============================================================================

Keyboard::Keyboard()
{
    setSize (800, 120);
}

Keyboard::~Keyboard() = default;

void Keyboard::setKeyRange (int lowNote, int numKeys)
{
    keys_.clear();
    lit_.clear();
    numWhite_ = 0;

    // Chromatic black-key pattern: C#=1, D#=3, F#=6, G#=8, A#=10
    static constexpr int isBlack[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
    for (int i = 0; i < numKeys; ++i)
    {
        const int note = lowNote + i;
        const int pc = note % 12;
        if (isBlack[pc])
            keys_.push_back ({ note, true, numWhite_ - 1 });
        else
        {
            keys_.push_back ({ note, false, numWhite_ });
            ++numWhite_;
        }
    }
    repaint();
}

juce::Rectangle<int> Keyboard::boundsFor (const PianoKey& k) const
{
    const auto b = getLocalBounds();
    const float whiteW = (numWhite_ > 0) ? (float) b.getWidth() / (float) numWhite_ : 0.0f;

    if (!k.black)
    {
        const int x = static_cast<int> (k.whiteIndex * whiteW);
        return { x, b.getY(), static_cast<int> (std::lround (whiteW)), b.getHeight() };
    }

    const int blackW = static_cast<int> (whiteW * 0.62f);
    const int x = static_cast<int> ((k.whiteIndex + 1) * whiteW) - blackW / 2;
    const int blackH = static_cast<int> (b.getHeight() * 0.62f);
    return { x, b.getY(), blackW, blackH };
}

int Keyboard::findNote (juce::Point<float> p) const
{
    for (const auto& k : keys_)
        if (k.black && boundsFor (k).toFloat().contains (p))
            return k.note;
    for (const auto& k : keys_)
        if (!k.black && boundsFor (k).toFloat().contains (p))
            return k.note;
    return -1;
}

void Keyboard::setNoteOn (int note, bool on)
{
    lit_.set (note, on);
    repaint();
}

void Keyboard::paint (juce::Graphics& g)
{
    g.fillAll (theme::body2);

    for (const auto& k : keys_)
        if (!k.black)
        {
            auto r = boundsFor (k).toFloat();
            const bool lit = lit_[k.note];
            juce::Colour top = lit ? theme::mint : theme::knobCream;
            juce::Colour bot = lit ? theme::mintDeep : theme::knobShadow;
            g.setGradientFill (juce::ColourGradient (top, r.getTopLeft(), bot, r.getBottomLeft(), false));
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (juce::Colour (0xff7d786d));
            g.drawRoundedRectangle (r, 2.0f, 1.0f);
        }

    for (const auto& k : keys_)
        if (k.black)
        {
            auto r = boundsFor (k).toFloat();
            const bool lit = lit_[k.note];
            juce::Colour top = lit ? theme::mint : juce::Colour (0xff4a4a4a);
            juce::Colour bot = lit ? theme::mintDeep : juce::Colour (0xff0d0d0d);
            g.setGradientFill (juce::ColourGradient (top, r.getTopLeft(), bot, r.getBottomLeft(), false));
            g.fillRoundedRectangle (r, 2.0f);
        }
}

void Keyboard::resized()
{
    repaint();
}

void Keyboard::mouseDown (const juce::MouseEvent& e)
{
    if (const int note = findNote (e.position))
    {
        setNoteOn (note, true);
        if (noteCallback_)
            noteCallback_ (note, true);
    }
}

void Keyboard::mouseUp (const juce::MouseEvent& e)
{
    if (const int note = findNote (e.position))
    {
        setNoteOn (note, false);
        if (noteCallback_)
            noteCallback_ (note, false);
    }
}

// ============================================================================
// BankBrowser
// ============================================================================

BankBrowser::BankBrowser()
{
    addAndMakeVisible (bankCombo_);
    bankCombo_.setColour (juce::ComboBox::backgroundColourId, theme::displayBg);
    bankCombo_.setColour (juce::ComboBox::textColourId, theme::mint);
    bankCombo_.setColour (juce::ComboBox::arrowColourId, theme::mintDeep);
    bankCombo_.setColour (juce::PopupMenu::backgroundColourId, theme::body2);
    bankCombo_.setColour (juce::PopupMenu::textColourId, theme::knobCream);
    bankCombo_.setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::mintDeep);
    bankCombo_.setColour (juce::PopupMenu::highlightedTextColourId, juce::Colour (0xff06231b));
    bankCombo_.setJustificationType (juce::Justification::centred);
    bankCombo_.setColour (juce::ComboBox::outlineColourId, theme::mintDeep);
    bankCombo_.setColour (juce::ComboBox::buttonColourId, theme::mintDeep);
    bankCombo_.onChange = [this]
    {
        const int sel = bankCombo_.getSelectedId() - 1;
        if (sel >= 0 && sel < (int) banks_.size())
            filterFor (banks_[static_cast<std::size_t> (sel)]);
    };

    addAndMakeVisible (list_);
    list_.setOutlineThickness (0);
    list_.setColour (juce::ListBox::backgroundColourId, theme::displayBg);
    list_.setColour (juce::ListBox::outlineColourId, theme::mintDeep);
    list_.getViewport()->setScrollBarsShown (true, false);
    list_.getViewport()->setColour (juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
    list_.getViewport()->setColour (juce::ScrollBar::trackColourId, theme::bodyEdge);
    list_.getViewport()->setColour (juce::ScrollBar::thumbColourId, theme::mintDeep);
    list_.setRowHeight (24);

    addAndMakeVisible (emptyHint_);
    emptyHint_.setJustificationType (juce::Justification::centred);
    emptyHint_.setColour (juce::Label::textColourId, theme::mint);
    emptyHint_.setFont (makeFont (11.0f, false));
    emptyHint_.setText ("NO SOUNDFONT LOADED - PRESS LOAD TO BROWSE PRESETS",
                        juce::dontSendNotification);
}

BankBrowser::~BankBrowser() = default;

void BankBrowser::buildBanks()
{
    juce::SortedSet<int> seen;
    for (const auto& p : presets_)
        seen.add (p.bank);

    banks_.clear();
    bankCombo_.clear (juce::dontSendNotification);
    for (int b : seen)
    {
        banks_.push_back (b);
        bankCombo_.addItem (juce::String (b).paddedLeft ('0', 2), (int) banks_.size());
    }

    selectedBank_ = banks_.empty() ? -1 : banks_.front();
    if (!banks_.empty())
        bankCombo_.setSelectedId (1, juce::dontSendNotification);
    filterFor (selectedBank_);
}

void BankBrowser::filterFor (int bank)
{
    selectedBank_ = bank;
    filtered_.clear();
    for (int i = 0; i < (int) presets_.size(); ++i)
        if (presets_[static_cast<std::size_t> (i)].bank == bank)
            filtered_.push_back (i);
    list_.updateContent();
    list_.deselectAllRows();
    list_.scrollToEnsureRowIsOnscreen (0);
    repaint();
}

void BankBrowser::setPresets (std::vector<PresetEntry> presets)
{
    presets_ = std::move (presets);
    buildBanks();
    list_.setVisible (!filtered_.empty());
    emptyHint_.setVisible (filtered_.empty());
}

void BankBrowser::setCurrent (int bank, int program)
{
    // Move the bank strip to the requested bank, then highlight its program.
    if (!banks_.empty())
    {
        for (std::size_t i = 0; i < banks_.size(); ++i)
        {
            if (banks_[i] == bank && bankCombo_.getSelectedId() != (int) (i + 1))
            {
                bankCombo_.setSelectedId ((int) (i + 1), juce::dontSendNotification);
                filterFor (bank);
                break;
            }
        }
    }

    for (std::size_t row = 0; row < filtered_.size(); ++row)
    {
        const auto& p = presets_[static_cast<std::size_t> (filtered_[row])];
        if (p.bank == bank && p.program == program)
        {
            if (list_.getSelectedRow() != (int) row)
            {
                juce::SparseSet<int> selection;
                selection.addRange ({ (int) row, (int) row + 1 });
                list_.setSelectedRows (selection, juce::dontSendNotification);
                list_.scrollToEnsureRowIsOnscreen ((int) row);
            }
            return;
        }
    }
}

int BankBrowser::getNumRows()
{
    return (int) filtered_.size();
}

void BankBrowser::paintListBoxItem (int rowNumber, juce::Graphics& g, int width,
                                    int height, bool rowIsSelected)
{
    auto r = juce::Rectangle<int> (0, 0, width, height).reduced (2, 1).toFloat();
    if (rowNumber < 0 || rowNumber >= (int) filtered_.size())
        return;
    const auto& p = presets_[static_cast<std::size_t> (filtered_[static_cast<std::size_t> (rowNumber)])];

    if (rowIsSelected)
    {
        g.setGradientFill (juce::ColourGradient (theme::mint, r.getTopLeft(),
                                                 theme::mintDeep, r.getBottomLeft(), false));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (juce::Colour (0xff06231b));
    }
    else
    {
        g.setColour ((rowNumber % 2) ? juce::Colour (0xff1a1d19) : theme::body2);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (theme::knobCream);
    }

    const auto num = juce::String (p.bank).paddedLeft ('0', 2) + "-"
                     + juce::String (p.program).paddedLeft ('0', 3);
    g.setFont (makeFont (10.0f, true));
    g.drawText (num, 6, 0, 56, height, juce::Justification::centredLeft);
    g.setFont (makeFont (10.0f, false));
    g.drawText (p.name, 64, 0, width - 70, height, juce::Justification::centredLeft);
}

void BankBrowser::selectedRowsChanged (int lastRowChanged)
{
    if (lastRowChanged < 0 || lastRowChanged >= (int) filtered_.size())
        return;
    const auto& p = presets_[static_cast<std::size_t> (filtered_[static_cast<std::size_t> (lastRowChanged)])];
    if (onSelect_)
        onSelect_ (p.bank, p.program);
}

void BankBrowser::resized()
{
    auto b = getLocalBounds();
    bankCombo_.setBounds (b.removeFromTop (26).withTrimmedLeft (2).withTrimmedRight (2));
    b.removeFromTop (4);
    list_.setBounds (b.reduced (2, 0));
    emptyHint_.setBounds (b.reduced (2, 0));
}

// ============================================================================
// RomplerEditor
// ============================================================================

RomplerEditor::RomplerEditor (RomplerProcessor& processorRef)
    : juce::AudioProcessorEditor (processorRef),
      processor_ (processorRef),
      voiceBox_ ("VOICE"),
      busBox_ ("BUS"),
      fxBox_ ("FX")
{
    addAndMakeVisible (brandTitle_);
    brandTitle_.setText ("EON-DS50", juce::dontSendNotification);
    brandTitle_.setJustificationType (juce::Justification::centredLeft);
    brandTitle_.setColour (juce::Label::textColourId, theme::mint);
    brandTitle_.setFont (makeFont (26.0f, true));

    addAndMakeVisible (brandSub_);
    brandSub_.setText ("SF2 ROMPLER  /  NONLINEAR DRIVE", juce::dontSendNotification);
    brandSub_.setJustificationType (juce::Justification::centredLeft);
    brandSub_.setColour (juce::Label::textColourId, theme::mint);
    brandSub_.setFont (makeFont (10.0f, false));

    addAndMakeVisible (badges_);
    badges_.setJustificationType (juce::Justification::centredRight);
    badges_.setColour (juce::Label::textColourId, juce::Colour (0xff8b9186));
    badges_.setFont (makeFont (9.5f, false));
    badges_.setText ("VST3 / AU / STANDALONE\nSisyphus Audio - M1", juce::dontSendNotification);

    addAndMakeVisible (voiceBox_);
    addAndMakeVisible (busBox_);
    addAndMakeVisible (fxBox_);

    auto& apvts = processor_.getValueTreeState();
    controls_[0]  = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::voiceDrive)));
    controls_[1]  = std::make_unique<Switch> (*dynamic_cast<juce::AudioParameterChoice*>  (apvts.getParameter (ParamIDs::voiceCurve)));
    controls_[2]  = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::voiceVelToDrive)));
    controls_[3]  = std::make_unique<Toggle> (*dynamic_cast<juce::AudioParameterChoice*>  (apvts.getParameter (ParamIDs::voiceFilterRouting)));
    controls_[4]  = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::voiceFilterOffset)));
    controls_[5]  = std::make_unique<Stepper> (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::polyLimit)));
    controls_[6]  = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::busTapeDrive)));
    controls_[7]  = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::busFold)));
    controls_[8]  = std::make_unique<Switch> (*dynamic_cast<juce::AudioParameterChoice*>  (apvts.getParameter (ParamIDs::busOsFactor)), juce::String(), true);
    controls_[9]  = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::outTrim)));
    controls_[10] = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::outMix)));
    controls_[11] = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::fxChorusRate)));
    controls_[12] = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::fxChorusDepth)));
    controls_[13] = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::fxChorusMix)));
    controls_[14] = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::fxReverbRoom)));
    controls_[15] = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::fxReverbDamp)));
    controls_[16] = std::make_unique<Knob>   (*dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (ParamIDs::fxReverbMix)));
    for (auto& c : controls_)
    {
        if (c)
            addAndMakeVisible (*c);
    }

    // UI labels follow the design mockup (shorter than the APVTS names).
    const auto setKnobLabel = [] (std::unique_ptr<juce::Component>& c, const juce::String& s)
    {
        if (auto* k = dynamic_cast<Knob*> (c.get()))
            k->setNameOverride (s);
    };
    const auto setChoiceLabel = [] (std::unique_ptr<juce::Component>& c, const juce::String& s)
    {
        if (auto* sw = dynamic_cast<Switch*> (c.get()))
            sw->setLabel (s);
        if (auto* t = dynamic_cast<Toggle*> (c.get()))
            t->setLabel (s);
        if (auto* st = dynamic_cast<Stepper*> (c.get()))
            st->setLabel (s);
    };
    setKnobLabel (controls_[0], "DRIVE");
    setChoiceLabel (controls_[1], "CURVE");
    setKnobLabel (controls_[2], "VEL > DRIVE");
    setChoiceLabel (controls_[3], "FILTER ROUTE");
    setKnobLabel (controls_[4], "FILTER OFFSET");
    setChoiceLabel (controls_[5], "POLYPHONY");
    setKnobLabel (controls_[6], "TAPE DRIVE");
    setKnobLabel (controls_[7], "FOLD");
    setChoiceLabel (controls_[8], "OVERSAMPLE");
    setKnobLabel (controls_[9], "OUT TRIM");
    setKnobLabel (controls_[10], "MIX");
    setKnobLabel (controls_[11], "CH RATE");
    setKnobLabel (controls_[12], "CH DEPTH");
    setKnobLabel (controls_[13], "CH MIX");
    setKnobLabel (controls_[14], "REV ROOM");
    setKnobLabel (controls_[15], "REV DAMP");
    setKnobLabel (controls_[16], "REV MIX");

    // Dock: soundfont display + bank/program + load + peak meter.
    addAndMakeVisible (sfLabel_);
    sfLabel_.setText ("SOUNDFONT", juce::dontSendNotification);
    sfLabel_.setJustificationType (juce::Justification::centredLeft);
    sfLabel_.setColour (juce::Label::textColourId, juce::Colour (0xff9aa39b));
    sfLabel_.setFont (makeFont (9.5f, true));

    addAndMakeVisible (sfDisplay_);
    sfDisplay_.setJustificationType (juce::Justification::centredLeft);
    sfDisplay_.setColour (juce::Label::textColourId, theme::mint);
    sfDisplay_.setColour (juce::Label::backgroundColourId, theme::displayBg);
    sfDisplay_.setFont (makeFont (10.5f, true));
    refreshDisplay();

    addAndMakeVisible (bankDigits_);
    bankDigits_.setJustificationType (juce::Justification::centred);
    bankDigits_.setColour (juce::Label::textColourId, theme::mint);
    bankDigits_.setColour (juce::Label::backgroundColourId, theme::displayBg);
    bankDigits_.setFont (makeFont (11.0f, true));

    addAndMakeVisible (loadButton_);
    loadButton_.setColour (juce::TextButton::buttonColourId, theme::mintDeep);
    loadButton_.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff06231b));
    loadButton_.onClick = [this] { onLoadButtonClicked(); };

    addAndMakeVisible (peakMeter_);

    // Bank/program browser: fills from the loaded SoundFont's preset table.
    bankBrowser_ = std::make_unique<BankBrowser>();
    bankBrowser_->setOnSelect ([this] (int bank, int program) {
        processor_.selectPreset (bank, program);
    });
    addAndMakeVisible (*bankBrowser_);
    refreshPresetList();

    addAndMakeVisible (keyboard_);
    keyboard_.setKeyRange (36, 36);   // C2..B4
    keyboard_.setNoteCallback ([this] (int note, bool on) {
        processor_.postNote (note, on, 100);
    });

    setSize (820, 820);

    startTimerHz (20);
}

RomplerEditor::~RomplerEditor() = default;

void RomplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::body2);
    auto face = getLocalBounds().reduced (8);
    g.setColour (theme::bodyEdge);
    g.drawRoundedRectangle (face.toFloat(), 14.0f, 2.0f);

    // Mint pinstripe under the top edge.
    g.setColour (theme::mint);
    g.fillRoundedRectangle (juce::Rectangle<float> ((float) face.getX() + 14, (float) face.getY() + 10,
                                                    (float) face.getWidth() - 28, 2.0f), 1.0f);
}

void RomplerEditor::resized()
{
    auto b = getLocalBounds().reduced (14);

    auto top = b.removeFromTop (52);
    brandTitle_.setBounds (top.removeFromLeft (190).withY (4));
    brandSub_.setBounds (top.removeFromLeft (190).withY (30));
    badges_.setBounds (top.removeFromRight (180).reduced (4, 8));

    b.removeFromTop (8);

    // Two-column deck: VOICE and BUS boxes side by side.
    auto deck = b.removeFromTop (250);
    const int colW = (deck.getWidth() - 12) / 2;
    voiceBox_.setBounds (deck.withWidth (colW));
    busBox_.setBounds (deck.withX (colW + 12).withWidth (colW));

    layoutVoiceControls (voiceBox_.getBounds().reduced (10, 12).withTop (voiceBox_.getY() + 18));
    layoutBusControls (busBox_.getBounds().reduced (10, 12).withTop (busBox_.getY() + 18));

    b.removeFromTop (8);

    // FX box spans the full width.
    auto fx = b.removeFromTop (120);
    fxBox_.setBounds (fx);
    layoutFxControls (fxBox_.getBounds().reduced (10, 12).withTop (fxBox_.getY() + 18));

    b.removeFromTop (8);

    // Bank/program browser between the FX section and the dock.
    bankBrowser_->setBounds (b.removeFromTop (200));

    b.removeFromTop (8);

    // Dock.
    auto dock = b.removeFromTop (56);
    const int dx = dock.getX();
    const int dy = dock.getY();
    const int loadX  = dock.getRight() - 96;
    const int bankX  = loadX - 8 - 76;
    const int meterX = bankX - 12 - 88;
    sfLabel_.setBounds   (dx,       dy + 1,  92,                  14);
    sfDisplay_.setBounds (dx + 97,  dy + 18, meterX - dx - 97 - 12, 24);
    peakMeter_.setBounds (meterX,   dy + 20, 88,                  20);
    bankDigits_.setBounds (bankX,   dy + 18, 76,                  24);
    loadButton_.setBounds (loadX,   dy + 10, 96,                  36);

    b.removeFromTop (8);

    keyboard_.setBounds (b.reduced (0, 2));
}

void RomplerEditor::layoutVoiceControls (juce::Rectangle<int> area)
{
    // 3x2 grid: Drive | Vel->Drive | Filter Offset / Curve | Filter Route | Polyphony
    const auto rows = 2;
    const auto cols = 3;
    const auto cellW = area.getWidth() / cols;
    const auto cellH = area.getHeight() / rows;

    auto place = [&] (int idx, int r, int c) {
        if (idx >= 0 && controls_[static_cast<size_t> (idx)])
            controls_[static_cast<size_t> (idx)]->setBounds (
                area.withX (area.getX() + c * cellW).withY (area.getY() + r * cellH)
                    .withSize (cellW, cellH).reduced (4, 2));
    };
    place (0, 0, 0);   // Drive
    place (2, 0, 1);   // Vel -> Drive
    place (4, 0, 2);   // Filter Offset
    place (1, 1, 0);   // Curve
    place (3, 1, 1);   // Filter Route
    place (5, 1, 2);   // Polyphony
}

void RomplerEditor::layoutBusControls (juce::Rectangle<int> area)
{
    const auto rows = 2;
    const auto cols = 3;
    const auto cellW = area.getWidth() / cols;
    const auto cellH = area.getHeight() / rows;

    auto place = [&] (int idx, int r, int c) {
        if (idx >= 0 && controls_[static_cast<size_t> (idx)])
            controls_[static_cast<size_t> (idx)]->setBounds (
                area.withX (area.getX() + c * cellW).withY (area.getY() + r * cellH)
                    .withSize (cellW, cellH).reduced (4, 2));
    };
    place (6, 0, 0);   // Tape Drive
    place (7, 0, 1);   // Fold
    place (8, 0, 2);   // Oversample
    place (9, 1, 0);   // Out Trim
    place (10, 1, 1);  // Mix
}

void RomplerEditor::layoutFxControls (juce::Rectangle<int> area)
{
    const auto count = 6;
    const auto cellW = area.getWidth() / count;
    for (int i = 0; i < count; ++i)
    {
        const int idx = 11 + i;
        if (controls_[static_cast<size_t> (idx)])
            controls_[static_cast<size_t> (idx)]->setBounds (
                area.withX (area.getX() + i * cellW).withY (area.getY())
                    .withSize (cellW, area.getHeight()).reduced (6, 2));
    }
}

void RomplerEditor::refreshDisplay()
{
    const auto file = processor_.getLoadedFileName();
    juce::String text = file.isEmpty() ? "NO FILE LOADED" : file;
    sfDisplay_.setText (text, juce::dontSendNotification);
}

void RomplerEditor::refreshPresetList()
{
    std::vector<PresetEntry> presets;
    const int count = processor_.getPresetCount();
    presets.reserve (static_cast<std::size_t> (count));
    for (int i = 0; i < count; ++i)
    {
        const auto [bank, program] = processor_.getPresetBankProgram (i);
        presets.push_back ({ bank, program, processor_.getPresetName (i) });
    }
    bankBrowser_->setPresets (std::move (presets));
    const auto [bank, program] = processor_.getCurrentBankProgram();
    bankBrowser_->setCurrent (bank, program);
}

void RomplerEditor::onLoadButtonClicked()
{
    fileChooser_ = std::make_unique<juce::FileChooser> (
        "Load a SoundFont...", juce::File (), "*.sf2;*.sf3");
    fileChooser_->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file.existsAsFile())
            {
                processor_.loadSoundFont (file);
                refreshPresetList();
            }
            fileChooser_.reset();
        });
}

void RomplerEditor::comboBoxChanged (juce::ComboBox*)
{
    // No preset combo in this layout; nothing to do.
}

void RomplerEditor::timerCallback()
{
    peakMeter_.setLevel (processor_.getLastPeak());

    const auto [bank, program] = processor_.getCurrentBankProgram();
    bankDigits_.setText (juce::String (bank).paddedLeft ('0', 2), juce::dontSendNotification);

    if (bankBrowser_)
        bankBrowser_->setCurrent (bank, program);
}

} // namespace aod
