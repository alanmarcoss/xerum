// xerum::plugin — the interface's building blocks, in code. See ParameterPanels.h.

#include "ParameterPanels.h"

#include "InterfaceLook.h"
#include "ModulationMatrixView.h"

#include <algorithm>
#include <cmath>

namespace xerum::plugin::ui
{

namespace
{
constexpr int kReadoutHeight = 13;
constexpr int kLabelHeight = 11;
constexpr int kToggleMaxWidth = 150;
constexpr int kPillHeight = 18;
constexpr int kPillGap = 2;
constexpr int kCompactTitleHeight = 12;
constexpr int kArrowSize = 16;
constexpr int kArrowGap = 1;

/// The diameters the design system names, in one place.
float diameterFor (const KnobSize size)
{
    switch (size)
    {
        case KnobSize::Small: return static_cast<float> (look::knobSmallDiameter);
        case KnobSize::Macro: return static_cast<float> (look::macroDiameter);
        case KnobSize::Default: break;
    }

    return static_cast<float> (look::knobDiameter);
}

/// The face a slider's label is drawn in — the same one `ParameterControl::paint` uses, so a cell's width
/// and the text drawn in it cannot disagree.
juce::Font labelFontFor (const KnobSize size)
{
    return look::bodyFont (size == KnobSize::Default ? look::type::label : look::type::tag);
}

/// A knob's parts at a given diameter: the design's 44px spec, as fractions.
struct KnobGeometry
{
    juce::Point<float> centre;
    float bodyRadius { 0.0f };
    float trackRadius { 0.0f };
    float stroke { 1.0f };
    float modStroke { 1.0f };
    float pointerRadius { 1.0f };
};

KnobGeometry geometryFor (const juce::Rectangle<float> area, const float diameter)
{
    KnobGeometry geometry;
    geometry.centre = area.getCentre();
    geometry.bodyRadius = diameter * look::knobBodyFraction;
    geometry.trackRadius = diameter * look::knobTrackFraction;
    geometry.stroke = juce::jmax (1.0f, diameter * look::knobStrokeFraction);
    geometry.modStroke = juce::jmax (1.0f, diameter * look::knobModStrokeFraction);
    geometry.pointerRadius = juce::jmax (0.8f, diameter * look::knobPointerFraction);
    return geometry;
}

/// The design's arc, start to end, in JUCE's angle convention (radians, clockwise from 12 o'clock).
float angleAtProportion (const double proportion)
{
    const auto clamped = juce::jlimit (0.0, 1.0, proportion);

    return static_cast<float> (look::knobStartRadians
                               + clamped * (look::knobEndRadians - look::knobStartRadians));
}

void strokeArc (juce::Graphics& graphics, const KnobGeometry& geometry, const float from, const float to,
                const juce::Colour colour, const float thickness)
{
    if (std::abs (to - from) < 1.0e-4f)
        return;

    juce::Path arc;
    arc.addCentredArc (geometry.centre.x, geometry.centre.y, geometry.trackRadius, geometry.trackRadius,
                       0.0f, juce::jmin (from, to), juce::jmax (from, to), true);
    graphics.setColour (colour);
    graphics.strokePath (arc, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
}

/// The value's own sign colouring, for a matrix amount cell.
juce::Colour signColour (const double value)
{
    if (value > 0.0)
        return look::green;

    if (value < 0.0)
        return look::red;

    return look::fgMuted;
}

/// How wide a choice's option row wants to be: the design's `btn-opt` pill, 2px 7px of padding around
/// text, in a row with its own 2px gaps.
int pillsWidth (const ChoiceList& choices)
{
    const auto font = look::bodyFont (look::type::label, true);
    int width = 0;

    for (int i = 0; i < choices.size(); ++i)
        width += static_cast<int> (juce::GlyphArrangement::getStringWidth (font, choices.labels[i])) + 14
                 + (i > 0 ? kPillGap : 0);

    return width;
}

/// A toggle's pill, as `PillToggle` draws it: the dot, its gap and the label, inside 6px of padding.
int togglePillWidth (const juce::String& text)
{
    const auto font = look::bodyFont (look::type::label);

    return juce::jmin (kToggleMaxWidth,
                       static_cast<int> (juce::GlyphArrangement::getStringWidth (font, text)) + 5 + 3 + 12);
}
} // namespace

//==============================================================================
Knob::Knob (const KnobSize size, const Mode mode)
    : size_ (size),
      mode_ (mode)
{
    setSliderStyle (mode_ == Mode::Dial ? juce::Slider::Rotary : juce::Slider::LinearHorizontal);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setWantsKeyboardFocus (true);
    setVelocityBasedMode (false);
}

void Knob::setModulationDepth (const double depth)
{
    // The arc is a fact about the preset's matrix rows, so a value outside the range is a bug upstream
    // rather than something to draw: clamp where it is read, not where it is used.
    modulation_ = juce::jlimit (-1.0, 1.0, depth);
    repaint();
}

juce::Rectangle<float> Knob::bodyBounds() const
{
    const auto area = getLocalBounds().toFloat();

    if (mode_ != Mode::Dial)
        return area;

    const auto diameter = juce::jmin (area.getWidth(), area.getHeight());
    const auto geometry = geometryFor (area, diameter);

    return juce::Rectangle<float> (geometry.bodyRadius * 2.0f, geometry.bodyRadius * 2.0f)
        .withCentre (geometry.centre);
}

juce::Point<float> Knob::pointAtProportion (const double proportion) const
{
    const auto area = getLocalBounds().toFloat();
    const auto diameter = juce::jmin (area.getWidth(), area.getHeight());
    const auto geometry = geometryFor (area, diameter);
    const auto angle = angleAtProportion (proportion);

    return { geometry.centre.x + std::sin (angle) * geometry.trackRadius,
             geometry.centre.y - std::cos (angle) * geometry.trackRadius };
}

void Knob::paint (juce::Graphics& graphics)
{
    const auto area = getLocalBounds().toFloat();

    if (mode_ == Mode::Cell)
    {
        const auto value = getValue();
        const auto text = getTextFromValue (value);

        if (hasKeyboardFocus (true) || isMouseOverOrDragging())
        {
            graphics.setColour (look::surface2);
            graphics.fillRoundedRectangle (area.reduced (0.5f), 3.0f);
        }

        juce::Colour colour = signColoured_ ? signColour (value) : (value != 0.0 ? look::fg : look::fgMuted);

        if (! isEnabled())
            colour = colour.withAlpha (0.4f);

        graphics.setColour (colour);
        graphics.setFont (look::monoFont (look::type::readoutSm));
        graphics.drawText (text, area.reduced (6.0f, 0.0f).toNearestInt(),
                           signColoured_ ? juce::Justification::centredRight
                                         : juce::Justification::centredLeft,
                           true);

        if (hasKeyboardFocus (true))
        {
            graphics.setColour (look::accent);
            graphics.drawRoundedRectangle (area.reduced (0.5f), 3.0f, look::kFocusStroke);
        }

        return;
    }

    const auto diameter = juce::jmin (area.getWidth(), area.getHeight());

    if (diameter < 10.0f)
        return;

    const auto geometry = geometryFor (area, diameter);
    const juce::Graphics::ScopedSaveState save (graphics);

    graphics.setOpacity (isEnabled() ? 1.0f : 0.4f);

    // The body: surface2, with the design's own hover / drag / focus strokes.
    graphics.setColour (look::surface2);
    graphics.fillEllipse (bodyBounds());

    auto bodyStroke = look::border;

    if (isMouseOverOrDragging())
        bodyStroke = look::borderHi;

    if (isMouseButtonDown())
        bodyStroke = look::accentDim;

    const auto focused = hasKeyboardFocus (true);

    graphics.setColour (focused ? look::accent : bodyStroke);
    graphics.drawEllipse (bodyBounds().reduced (0.5f), focused ? look::kFocusStroke : 1.0f);

    // The track: the whole sweep, in the border colour.
    strokeArc (graphics, geometry, look::knobStartRadians,
               look::knobEndRadians, look::border, geometry.stroke);

    const auto proportion = valueToProportionOfLength (getValue());
    const auto valueAngle = angleAtProportion (proportion);

    // The value arc, start to the value.
    strokeArc (graphics, geometry, look::knobStartRadians, valueAngle, look::accent, geometry.stroke);

    // **The modulation arc** — the design system's second arc. It is drawn only when a matrix row
    // actually reaches this parameter, in `accentDim` at half opacity, and it runs from the value to the
    // modulated value so the two read as one gesture.
    if (isModulated())
    {
        const auto sweep = static_cast<float> (modulation_) * (look::knobEndRadians - look::knobStartRadians);

        strokeArc (graphics, geometry, valueAngle, valueAngle + sweep, look::accentDim, geometry.modStroke);
    }

    // The pointer, at the value's own angle on the track.
    const auto pointer = pointAtProportion (proportion);
    graphics.setColour (isMouseButtonDown() ? look::accentDim : look::fg);
    graphics.fillEllipse (juce::Rectangle<float> (geometry.pointerRadius * 2.0f, geometry.pointerRadius * 2.0f)
                              .withCentre (pointer));
}

//==============================================================================
PillToggle::PillToggle()
{
    setClickingTogglesState (true);
    setWantsKeyboardFocus (true);
}

void PillToggle::setCompact (const bool compact)
{
    compact_ = compact;
    repaint();
}

void PillToggle::setDotInverted (const bool inverted)
{
    inverted_ = inverted;
    repaint();
}

void PillToggle::paintButton (juce::Graphics& graphics, const bool shouldDrawButtonAsHighlighted,
                              const bool shouldDrawButtonAsDown)
{
    auto area = getLocalBounds().toFloat().reduced (0.5f);
    const auto on = getToggleState();
    const juce::Graphics::ScopedSaveState save (graphics);

    if (! isEnabled())
        graphics.setOpacity (0.4f);

    if (compact_)
    {
        // A matrix cell: the state dot and nothing else — the column header is the label.
        const auto dot = juce::jmin (6.0f, area.getHeight());

        if (shouldDrawButtonAsDown)
            area = area.withSizeKeepingCentre (dot * 1.3f, dot * 1.3f);

        // The design's bypass dot is green when the row is **doing something**. This engine's
        // `kParamBypass` is 1 when it is not, so a bypass cell inverts the dot rather than the parameter.
        const auto live = inverted_ ? ! on : on;
        graphics.setColour (live ? (inverted_ ? look::green : look::accent) : look::fgMuted);
        graphics.fillEllipse (juce::Rectangle<float> (dot, dot).withCentre (area.getCentre()));

        if (hasKeyboardFocus (true))
        {
            graphics.setColour (look::accent);
            graphics.drawEllipse (juce::Rectangle<float> (dot + 4.0f, dot + 4.0f).withCentre (area.getCentre()),
                                  look::kFocusStroke);
        }

        return;
    }

    const auto radius = area.getHeight() * 0.5f;

    if (on)
    {
        graphics.setColour (look::accentBg);
        graphics.fillRoundedRectangle (area, radius);
        graphics.setColour (look::accentDim);
        graphics.drawRoundedRectangle (area, radius, 1.0f);
    }
    else if (shouldDrawButtonAsHighlighted)
    {
        graphics.setColour (look::surface2);
        graphics.fillRoundedRectangle (area, radius);
    }

    // The dot and the label, laid out from the left with the design's 2px 6px padding.
    const auto dot = juce::jmin (5.0f, area.getHeight() - 4.0f);
    auto content = area.reduced (6.0f, 0.0f);
    const auto dotArea = content.removeFromLeft (dot);

    graphics.setColour (on ? look::accent : look::fgMuted);
    graphics.fillEllipse (juce::Rectangle<float> (dot, dot).withCentre (dotArea.getCentre()));

    content.removeFromLeft (3.0f);

    graphics.setColour (on ? look::accent : (shouldDrawButtonAsHighlighted ? look::fg : look::fgDim));
    graphics.setFont (look::bodyFont (look::type::label));
    graphics.drawText (getButtonText(), content.toNearestInt(), juce::Justification::centredLeft, true);

    if (hasKeyboardFocus (true))
    {
        graphics.setColour (look::accent);
        graphics.drawRoundedRectangle (area.reduced (0.5f), radius, look::kFocusStroke);
    }
}

//==============================================================================
ChoiceButtons::ChoiceButtons (juce::RangedAudioParameter& parameter, const ChoiceList& choices)
    : juce::ParameterAttachment (parameter,
                                 [this] (const float value) { parameterChanged (indexFor (value)); }),
      choices_ (choices)
{
    setWantsKeyboardFocus (true);

    // The parameter may already be somewhere else than the default: read it, do not assume it.
    sendInitialUpdate();
}

int ChoiceButtons::choiceCount() const
{
    return choices_.size();
}

juce::String ChoiceButtons::choiceText (const int index) const
{
    return index >= 0 && index < choiceCount() ? choices_.labels[index] : juce::String();
}

int ChoiceButtons::indexFor (const float denormalised) const
{
    // JUCE's own `ParameterAttachment` hands its callback the parameter's value in **its own units**, and
    // an `AudioParameterChoice`'s own unit *is* the choice index (its normalisable range is `0..items-1`
    // with a step of 1). So this is a rounding, not a scale — the scale is the host's business.
    const auto count = choiceCount();

    return count <= 0 ? 0 : juce::jlimit (0, count - 1, static_cast<int> (std::lround (denormalised)));
}

void ChoiceButtons::parameterChanged (const int index)
{
    if (index == selected_)
        return;

    selected_ = index;
    repaint();
}

juce::Rectangle<int> ChoiceButtons::buttonBounds (const int index) const
{
    if (index < 0 || index >= choiceCount())
        return {};

    const auto font = look::bodyFont (look::type::label, true);
    const auto top = (getHeight() - kPillHeight) / 2;
    auto x = 0;

    for (int i = 0; i <= index; ++i)
    {
        const auto width = static_cast<int> (juce::GlyphArrangement::getStringWidth (font, choices_.labels[i])) + 14;

        if (i == index)
            return { x, top, width, kPillHeight };

        x += width + kPillGap;
    }

    return {};
}

int ChoiceButtons::preferredWidth() const
{
    return pillsWidth (choices_);
}

void ChoiceButtons::resized()
{
    repaint();
}

void ChoiceButtons::paint (juce::Graphics& graphics)
{
    const auto font = look::bodyFont (look::type::label, true);

    for (int i = 0; i < choiceCount(); ++i)
    {
        const auto pill = buttonBounds (i).toFloat();
        const auto active = i == selected_;
        const auto hovered = i == hovered_;

        if (active)
        {
            graphics.setColour (look::accentBg);
            graphics.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
            graphics.setColour (look::accentDim);
            graphics.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);
        }
        else if (hovered)
        {
            graphics.setColour (look::borderHi);
            graphics.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);
        }
        else
        {
            graphics.setColour (look::border);
            graphics.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);
        }

        graphics.setColour (active ? look::accent : (hovered ? look::fg : look::fgDim));
        graphics.setFont (font);
        graphics.drawText (choices_.labels[i], pill.toNearestInt(), juce::Justification::centred, true);
    }
}

bool ChoiceButtons::keyPressed (const juce::KeyPress& key)
{
    const auto code = key.getKeyCode();
    const auto step = (code == juce::KeyPress::leftKey || code == juce::KeyPress::upKey)
                          ? -1
                          : ((code == juce::KeyPress::rightKey || code == juce::KeyPress::downKey) ? 1 : 0);

    if (step == 0 || choiceCount() <= 1)
        return false;

    const auto next = juce::jlimit (0, choiceCount() - 1, selected_ + step);

    if (next != selected_)
        setValueAsCompleteGesture (static_cast<float> (next));

    return true;
}

void ChoiceButtons::mouseDown (const juce::MouseEvent&)
{
    grabKeyboardFocus();
}

void ChoiceButtons::mouseUp (const juce::MouseEvent& event)
{
    for (int i = 0; i < choiceCount(); ++i)
        if (buttonBounds (i).contains (event.getPosition()))
        {
            // A click is a complete gesture, not a drag: the host sees one value change. The value is the
            // choice **index**, in the parameter's own units (see `indexFor`).
            setValueAsCompleteGesture (static_cast<float> (i));
            return;
        }
}

void ChoiceButtons::mouseMove (const juce::MouseEvent& event)
{
    auto found = -1;

    for (int i = 0; i < choiceCount(); ++i)
        if (buttonBounds (i).contains (event.getPosition()))
            found = i;

    if (found == hovered_)
        return;

    hovered_ = found;
    repaint();
}

void ChoiceButtons::mouseExit (const juce::MouseEvent&)
{
    if (hovered_ < 0)
        return;

    hovered_ = -1;
    repaint();
}

//==============================================================================
WavetableSlot::WavetableSlot()
{
    setWantsKeyboardFocus (false);
}

void WavetableSlot::setTableName (juce::String name)
{
    name_ = std::move (name);
    repaint();
}

void WavetableSlot::setSteppable (const bool steppable)
{
    steppable_ = steppable;
    repaint();
}

juce::Rectangle<int> WavetableSlot::arrowBounds (const int step) const
{
    if (! steppable_)
        return {};

    const auto top = (getHeight() - kArrowSize) / 2;
    const auto right = getWidth() - kArrowSize;

    return step < 0 ? juce::Rectangle<int> (right - kArrowSize - kArrowGap, top, kArrowSize, kArrowSize)
                    : juce::Rectangle<int> (right, top, kArrowSize, kArrowSize);
}

void WavetableSlot::resized()
{
    repaint();
}

void WavetableSlot::paint (juce::Graphics& graphics)
{
    // The design's `wavetable-name`: a monospaced field on `bg`, 1px border, 3px radius, taking the width
    // the arrows do not.
    const auto arrows = steppable_ ? kArrowSize * 2 + kArrowGap : 0;
    auto field = getLocalBounds().withTrimmedRight (arrows > 0 ? arrows + 4 : 0)
                     .withSizeKeepingCentre (getWidth() - (arrows > 0 ? arrows + 4 : 0), 18);

    graphics.setColour (look::bg);
    graphics.fillRoundedRectangle (field.toFloat(), 3.0f);
    graphics.setColour (look::border);
    graphics.drawRoundedRectangle (field.toFloat().reduced (0.5f), 3.0f, 1.0f);

    graphics.setColour (look::fg);
    graphics.setFont (look::monoFont (9.0f));
    graphics.drawText (name_.isEmpty() ? juce::String ("\xe2\x80\x94 off \xe2\x80\x94") : name_,
                       field.reduced (7, 0), juce::Justification::centredLeft, true);

    if (! steppable_)
        return;

    for (const auto step : { -1, 1 })
    {
        const auto box = arrowBounds (step).toFloat();

        graphics.setColour (look::bg);
        graphics.fillRoundedRectangle (box, 3.0f);
        graphics.setColour (hovered_ == step ? look::borderHi : look::border);
        graphics.drawRoundedRectangle (box.reduced (0.5f), 3.0f, 1.0f);

        // The design's arrows are glyphs; drawn as paths so the direction is a shape rather than a
        // character that a fallback font might render as a box.
        const auto centre = box.getCentre();
        juce::Path triangle;
        const auto half = 3.0f;

        if (step < 0)
            triangle.addTriangle (centre.x + half, centre.y - half, centre.x + half, centre.y + half,
                                  centre.x - half, centre.y);
        else
            triangle.addTriangle (centre.x - half, centre.y - half, centre.x - half, centre.y + half,
                                  centre.x + half, centre.y);

        graphics.setColour (hovered_ == step ? look::fg : look::fgDim);
        graphics.fillPath (triangle);
    }
}

void WavetableSlot::mouseDown (const juce::MouseEvent& event)
{
    if (! steppable_ || ! onStep)
        return;

    for (const auto step : { -1, 1 })
        if (arrowBounds (step).contains (event.getPosition()))
        {
            onStep (step);
            return;
        }
}

void WavetableSlot::mouseMove (const juce::MouseEvent& event)
{
    auto found = 0;

    if (steppable_)
        for (const auto step : { -1, 1 })
            if (arrowBounds (step).contains (event.getPosition()))
                found = step;

    if (found == hovered_)
        return;

    hovered_ = found;
    repaint();
}

void WavetableSlot::mouseExit (const juce::MouseEvent&)
{
    if (hovered_ == 0)
        return;

    hovered_ = 0;
    repaint();
}

//==============================================================================
ParameterControl::ParameterControl (const ParamSpec& spec, juce::RangedAudioParameter& parameter,
                                    const ChoiceList& choices, const bool showLabel)
    : spec_ (spec),
      parameter_ (parameter),
      // A cell has no label of its own, and that is exactly a matrix cell: the compact numeric field
      // rather than a dial. The decision is made here, before the attachment exists, because the widget
      // cannot be swapped after it is bound.
      knob_ (knobSizeFor (spec), showLabel ? Knob::Mode::Dial : Knob::Mode::Cell),
      showLabel_ (showLabel)
{
    const auto kind = controlKindFor (spec.type);

    if (kind == ControlKind::Toggle)
    {
        // A boolean row is the toggle itself: the button's text is the label, so the row needs no label
        // column and the panel reads as a list of switches. A matrix cell has the column header instead.
        toggle_.setButtonText (showLabel ? label() : juce::String());
        widget_ = &toggle_;
        toggleAttachment_ = std::make_unique<juce::ButtonParameterAttachment> (parameter, toggle_);
    }
    else if (kind == ControlKind::Choice)
    {
        // The design draws a short vocabulary as a row of option buttons and a long one as a picker, and
        // the choice is the vocabulary's length rather than the group's name.
        if (choiceStyleFor (choices.size()) == ChoiceStyle::Buttons)
        {
            choiceButtons_ = std::make_unique<ChoiceButtons> (parameter, choices);
            widget_ = choiceButtons_.get();
        }
        else
        {
            // The items are the bridge's vocabulary, in its order: the attachment maps item index onto the
            // parameter's steps linearly, so the combo must hold exactly the choices the parameter has — no
            // more (a phantom choice) and no fewer (a choice the user cannot reach).
            for (int i = 0; i < choices.size(); ++i)
                combo_.addItem (choices.labels[i], i + 1);

            combo_.setColour (juce::ComboBox::backgroundColourId, look::bg);
            combo_.setColour (juce::ComboBox::textColourId, look::fg);
            combo_.setColour (juce::ComboBox::outlineColourId, look::border);
            combo_.setColour (juce::ComboBox::arrowColourId, look::accent);
            combo_.setWantsKeyboardFocus (true);
            widget_ = &combo_;
            choiceAttachment_ = std::make_unique<juce::ComboBoxParameterAttachment> (parameter, combo_);
        }
    }
    else
    {
        knob_.setColour (juce::Slider::backgroundColourId, look::surface2);
        knob_.setColour (juce::Slider::textBoxTextColourId, look::fg);
        widget_ = &knob_;
        sliderAttachment_ = std::make_unique<juce::SliderParameterAttachment> (parameter, knob_);
    }

    addAndMakeVisible (*widget_);
}

void ParameterControl::setSignColouring (const bool on)
{
    knob_.setSignColouring (on);
}

void ParameterControl::setCompactToggle (const bool compact, const bool inverted)
{
    toggle_.setCompact (compact);
    toggle_.setDotInverted (inverted);
}

juce::String ParameterControl::label() const
{
    return juce::String (spec_.label);
}

ChoiceStyle ParameterControl::choiceStyle() const
{
    if (controlKindFor (spec_.type) != ControlKind::Choice)
        return ChoiceStyle::Menu;

    return choiceButtons_ != nullptr ? ChoiceStyle::Buttons : ChoiceStyle::Menu;
}

bool ParameterControl::isBound() const
{
    return toggleAttachment_ != nullptr || choiceAttachment_ != nullptr || sliderAttachment_ != nullptr
           || choiceButtons_ != nullptr;
}

int ParameterControl::preferredWidth() const
{
    const auto kind = controlKindFor (spec_.type);

    if (kind == ControlKind::Toggle)
        return togglePillWidth (showLabel_ ? label() : juce::String());

    if (kind == ControlKind::Choice)
    {
        const auto labelWidth = static_cast<int> (juce::GlyphArrangement::getStringWidth (
                                     look::bodyFont (look::type::label), label())) + 8;

        if (choiceButtons_ != nullptr)
            return labelWidth + juce::jmax (80, choiceButtons_->preferredWidth()) + 6;

        // The picker takes the row it is on: the label column and the control share the width.
        return juce::jmax (kRowLabelWidth, 240);
    }

    const auto diameter = static_cast<int> (diameterFor (knob_.size()));
    const auto labelWidth = static_cast<int> (juce::GlyphArrangement::getStringWidth (
                                labelFontFor (knob_.size()), label()));

    return juce::jmax (diameter + 4, labelWidth + 4);
}

void ParameterControl::paint (juce::Graphics& graphics)
{
    const auto kind = controlKindFor (spec_.type);

    if (! showLabel_)
        return;

    if (kind == ControlKind::Slider)
    {
        const auto area = getLocalBounds();
        const auto footprint = juce::jmin (area.getWidth(),
                                           static_cast<int> (diameterFor (knob_.size())));

        auto text = area.withTrimmedTop (static_cast<int> (diameterFor (knob_.size())));
        auto readout = text.removeFromTop (kReadoutHeight);
        auto labelArea = text.removeFromTop (kLabelHeight);
        const auto centre = juce::Rectangle<int> (0, 0, footprint, readout.getHeight())
                                .withCentre ({ area.getCentreX(), readout.getCentreY() });

        graphics.setColour (look::fg);
        graphics.setFont (look::monoFont (knob_.size() == KnobSize::Small ? look::type::readoutSm
                                                                          : look::type::readout));
        graphics.drawText (knob_.getTextFromValue (knob_.getValue()), centre, juce::Justification::centred, true);

        const auto labelCentre = juce::Rectangle<int> (0, 0, juce::jmax (footprint, 60), labelArea.getHeight())
                                     .withCentre ({ area.getCentreX(), labelArea.getCentreY() });
        graphics.setColour (look::fgDim);
        graphics.setFont (labelFontFor (knob_.size()));
        graphics.drawText (label(), labelCentre, juce::Justification::centred, true);
    }
    else if (kind == ControlKind::Choice)
    {
        auto area = getLocalBounds();
        const auto labelWidth = juce::jmin (kRowLabelWidth, (area.getWidth() * 2) / 5);

        graphics.setColour (look::fgDim);
        graphics.setFont (choiceButtons_ != nullptr ? look::bodyFont (look::type::label)
                                                    : look::bodyFont (11.0f));
        graphics.drawText (label(), area.removeFromLeft (labelWidth).reduced (2, 0),
                           juce::Justification::centredLeft, true);
    }
}

void ParameterControl::resized()
{
    auto area = getLocalBounds();
    const auto kind = controlKindFor (spec_.type);

    if (kind == ControlKind::Toggle)
    {
        auto pill = area.removeFromLeft (juce::jmin (area.getWidth(), kToggleMaxWidth));

        // A pill is one line tall wherever it sits. The design's `.toggle` is an 8px label in `2px 6px` of
        // padding, and the design's **compact band** puts a pill on the same line as a `knob-sm` — so a pill
        // given its whole row would be nearly three times the design's height beside a 28px knob (ticket
        // `53`: the noise and sub rows are one line each now, and this is what that line's height would have
        // done to the pill).
        pill = pill.withSizeKeepingCentre (pill.getWidth(), juce::jmin (pill.getHeight(), kRowHeight));

        toggle_.setBounds (pill.reduced (0, 2));
        return;
    }

    // A cell with no label hands the whole width to the widget — the matrix's case.
    if (! showLabel_)
    {
        widget_->setBounds (area.reduced (1, 2));
        return;
    }

    if (kind == ControlKind::Choice)
    {
        // The label column is capped, because the label is a hint and the control is the thing the user
        // aims at; on a narrow column the two share the row rather than the label eating the picker.
        const auto labelWidth = juce::jmin (kRowLabelWidth, (area.getWidth() * 2) / 5);

        area.removeFromLeft (labelWidth);

        if (choiceButtons_ != nullptr)
            choiceButtons_->setBounds (area.removeFromLeft (
                juce::jmin (area.getWidth(), choiceButtons_->preferredWidth())));
        else
            combo_.setBounds (area.reduced (2, 3));

        return;
    }

    // A dial: the knob at the top, its readout and its label under it.
    auto diameter = static_cast<int> (diameterFor (knob_.size()));
    diameter = juce::jmin (diameter, juce::jmax (16, area.getHeight() - kReadoutHeight - kLabelHeight));

    knob_.setBounds (area.removeFromTop (diameter).withSizeKeepingCentre (diameter, diameter));
}

//==============================================================================
ParameterPanel::ParameterPanel (std::string title)
    : title_ (std::move (title))
{
    setInterceptsMouseClicks (false, true);
}

void ParameterPanel::addControl (std::unique_ptr<ParameterControl> control)
{
    addAndMakeVisible (*control);
    controls_.add (control.release());
}

void ParameterPanel::setWavetableSlot (std::unique_ptr<WavetableSlot> slot)
{
    wavetable_ = std::move (slot);

    if (wavetable_ != nullptr)
        addAndMakeVisible (*wavetable_);

    resized();
}

void ParameterPanel::setInlineDisplay (juce::Component* display, const int minimumHeight)
{
    display_ = display;
    displayMinHeight_ = juce::jmax (0, minimumHeight);

    if (display_ != nullptr)
        addAndMakeVisible (*display_);

    resized();
}

void ParameterPanel::setCompact (const bool compact)
{
    compact_ = compact;
    resized();
}

ParameterControl* ParameterPanel::controlAtRow (const int row) const
{
    return row >= 0 && row < controls_.size() ? controls_[row] : nullptr;
}

ParameterControl* ParameterPanel::controlFor (const std::string_view id) const
{
    for (auto* control : controls_)
        if (control->spec().id == id)
            return control;

    return nullptr;
}

int ParameterPanel::displayReserve (const int width) const
{
    if (display_ == nullptr)
        return 0;

    const auto available = width - kPanelPaddingX * 2;

    return juce::jlimit (0, juce::jmax (0, available / 2),
                         juce::jmax (kInlineDisplayMinWidth, available * 2 / 5));
}

int ParameterPanel::cellWidthFor (const ParameterControl& control, const int inner) const
{
    const auto kind = controlKindFor (control.spec().type);

    if (kind == ControlKind::Choice && control.choiceStyle() == ChoiceStyle::Menu)
        return inner;   // a picker takes its whole row — the design's label column plus the control

    return juce::jmin (inner, control.preferredWidth());
}

std::vector<ParameterPanel::Row> ParameterPanel::rowsFor (const int width) const
{
    const auto reserve = displayReserve (width);
    const auto inner = juce::jmax (60, width - kPanelPaddingX * 2 - reserve
                                             - (display_ != nullptr ? kInlineGap : 0));

    // How the design groups a panel's controls. Its **compact band** — NOISE and SUB, the two panels whose
    // knobs are `knob-sm` — puts the knobs *and* their option row on one line (`--gap-inline`: "between
    // inline elements (knobs + buttons in noise/sub)"), so that panel is one group packed with the inline
    // gap. Every other panel groups by kind in the design's order: the knobs are the body, then the
    // pickers, then the pills at the bottom (`osc-knobs` then `osc-controls`).
    //
    // **The band is a fact about the panel's group, not about its first control.** Asking the first control
    // for a knob size read `Noise`'s own `Enable` pill, answered `Default`, and quietly opted the panel out
    // of the design's compact band — which is why the noise row spent a line of its own on its pills.
    auto compactBand = false;

    for (auto* control : controls_)
        compactBand = compactBand || knobSizeFor (control->spec()) == KnobSize::Small;

    std::vector<std::vector<int>> groups;

    if (compactBand)
    {
        std::vector<int> band;

        for (int i = 0; i < controls_.size(); ++i)
            band.push_back (i);

        if (! band.empty())
            groups.push_back (std::move (band));
    }
    else
    {
        for (const auto kind : { ControlKind::Slider, ControlKind::Choice, ControlKind::Toggle })
        {
            std::vector<int> group;

            for (int i = 0; i < controls_.size(); ++i)
                if (controlKindFor (controls_[i]->spec().type) == kind)
                    group.push_back (i);

            if (! group.empty())
                groups.push_back (std::move (group));
        }
    }

    const auto gap = compactBand ? kInlineGap : kKnobCellGap;

    std::vector<Row> rows;

    for (const auto& group : groups)
    {
        Row row;
        row.gap = gap;
        int used = 0;

        for (const auto index : group)
        {
            const auto cell = cellWidthFor (*controls_[index], inner);

            if (! row.controls.empty() && used + gap + cell > inner)
            {
                rows.push_back (row);
                row = Row {};
                row.gap = gap;
                used = 0;
            }

            used += (row.controls.empty() ? 0 : gap) + cell;
            row.height = juce::jmax (row.height, rowHeightFor (controls_[index]->spec()));
            row.controls.push_back (index);
        }

        if (! row.controls.empty())
            rows.push_back (std::move (row));
    }

    return rows;
}

int ParameterPanel::preferredHeight (const int width) const
{
    int content = 0;

    for (const auto& row : rowsFor (width))
        content += row.height + kRowGap;

    if (content > 0)
        content -= kRowGap;

    const auto chrome = compact_ ? kPanelPaddingY / 2 + kCompactTitleHeight : kPanelPaddingY * 2;
    const auto table = (! compact_ && wavetable_ != nullptr) ? kWavetableHeadHeight : 0;
    auto height = (compact_ ? 0 : kPanelHeaderHeight) + table + chrome + content;

    if (display_ != nullptr)
        height = juce::jmax (height, chrome + displayMinHeight_);

    return height;
}

int ParameterPanel::preferredWidth() const
{
    // The design's `macro-item` is as wide as the wider of the knob it draws and the label under it; this
    // item's label is the panel's own title, because that is the caption the design draws under a macro.
    const auto title = juce::String (title_).toUpperCase();
    auto widest = static_cast<int> (juce::GlyphArrangement::getStringWidth (
        look::bodyFont (look::type::tag, true), title));

    for (const auto* control : controls_)
        widest = juce::jmax (widest, control->preferredWidth());

    return widest + 4;
}

void ParameterPanel::paint (juce::Graphics& graphics)
{
    if (compact_)
    {
        // A macro item: the design draws the knob bare, with its label under it and no panel around it.
        graphics.setColour (look::fgMuted);
        graphics.setFont (look::bodyFont (look::type::tag, true));
        graphics.drawText (juce::String (title_).toUpperCase(), getLocalBounds().removeFromBottom (kCompactTitleHeight),
                           juce::Justification::centred, true);
        return;
    }

    auto area = getLocalBounds().toFloat();

    graphics.setColour (look::surface);
    graphics.fillRoundedRectangle (area, static_cast<float> (look::radiusPanel));

    graphics.setColour (look::border);
    graphics.drawRoundedRectangle (area.reduced (0.5f), static_cast<float> (look::radiusPanel), 1.0f);

    // The design's panel title: 9px bold, uppercase, the primary foreground.
    graphics.setColour (look::fg);
    graphics.setFont (look::bodyFont (look::type::section, true));
    graphics.drawText (juce::String (title_).toUpperCase(),
                       kPanelPaddingX + 3, 0, getWidth() - kPanelPaddingX * 2 - 6, kPanelHeaderHeight,
                       juce::Justification::centredLeft, true);
}

void ParameterPanel::resized()
{
    auto area = getLocalBounds();

    if (! compact_)
        area = area.reduced (kPanelPaddingX, kPanelPaddingY);

    if (! compact_)
        area.removeFromTop (kPanelHeaderHeight);

    if (! compact_ && wavetable_ != nullptr)
        wavetable_->setBounds (area.removeFromTop (kWavetableHeadHeight));

    if (compact_)
        area.removeFromBottom (kCompactTitleHeight);

    if (display_ != nullptr)
    {
        auto displayArea = area.removeFromRight (displayReserve (getWidth()));
        display_->setBounds (displayArea);
        area.removeFromRight (kInlineGap);
    }

    for (const auto& row : rowsFor (getWidth()))
    {
        auto line = area.removeFromTop (row.height);
        int x = 0;

        for (const auto index : row.controls)
        {
            auto* control = controls_[index];
            const auto cell = juce::jmin (cellWidthFor (*control, line.getWidth()),
                                          juce::jmax (0, line.getWidth() - x));

            if (cell <= 0)
                break;

            control->setBounds (line.getX() + x, line.getY(), cell, row.height);
            x += cell + row.gap;
        }

        area.removeFromTop (kRowGap);
    }
}

//==============================================================================
CompositionView::CompositionView()
{
    setInterceptsMouseClicks (false, true);
}

void CompositionView::setComposition (std::vector<SectionPlan> sections,
                                      std::vector<ParameterPanel*> panels)
{
    sections_ = std::move (sections);
    panels_.assign (sections_.size(), {});
    bounds_.assign (sections_.size(), {});

    for (std::size_t index = 0; index < sections_.size(); ++index)
        for (const auto panel : sections_[index].panels)
            if (panel >= 0 && panel < static_cast<int> (panels.size()) && panels[static_cast<std::size_t> (panel)] != nullptr)
            {
                panels_[index].push_back (panels[static_cast<std::size_t> (panel)]);
                addAndMakeVisible (*panels_[index].back());
            }
}

void CompositionView::setMatrix (juce::Component* matrixViewport)
{
    matrix_ = matrixViewport;

    if (matrix_ != nullptr)
        addAndMakeVisible (*matrix_);
}

namespace
{
/// One band of the composition: the run of sections that share a `band`, the width each of them got, and
/// the height each one needs. Measuring and placing are two steps because a section's height is its tallest
/// panel's at the width the band gave it, which is a number the placement below needs and the measurement
/// above produces.
struct Band
{
    std::size_t begin { 0 };          ///< the first section of the band
    std::size_t end { 0 };            ///< one past the last
    std::vector<int> widths;          ///< per section
    std::vector<int> heights;         ///< per section, heading included
    int height { 0 };                 ///< the band's own height: its tallest section
};
} // namespace

juce::Rectangle<int> CompositionView::sectionBounds (const int index) const
{
    return index >= 0 && index < static_cast<int> (bounds_.size()) ? bounds_[static_cast<std::size_t> (index)]
                                                                   : juce::Rectangle<int>();
}

int CompositionView::sectionAt (const int y) const
{
    for (std::size_t index = 0; index < bounds_.size(); ++index)
        if (y >= bounds_[index].getY() && y < bounds_[index].getBottom())
            return static_cast<int> (index);

    return -1;
}

int CompositionView::matrixVisibleRows() const
{
    return matrixRows_;
}

int CompositionView::layout (const int width)
{
    // The matrix's own height: the compact panel of ticket `57` — the heading, the column header and
    // `kMatrixVisibleRows` rows — and a *natural* height rather than a floor, because nothing takes slack
    // any more (ticket `59`).
    const auto matrixNatural = kSectionTitleHeight + ModulationMatrixView::kHeaderHeight
                               + kMatrixVisibleRows * ModulationMatrixView::kRowHeight + 2;

    // --- measure ------------------------------------------------------------------------------------
    //
    // The sections are laid out in **bands**: consecutive entries sharing a `band` sit side by side in one
    // row. That is the design's `bottom-row` — the effects cards beside the macro strip — expressed as
    // data, and the reason a section carries its row rather than its own y.
    std::vector<Band> bands;
    std::size_t index = 0;

    while (index < sections_.size())
    {
        std::size_t end = index + 1;

        while (end < sections_.size() && sections_[end].band == sections_[index].band)
            ++end;

        const auto count = static_cast<int> (end - index);
        Band band;
        band.begin = index;
        band.end = end;
        band.widths.assign (static_cast<std::size_t> (count), 0);
        band.heights.assign (static_cast<std::size_t> (count), 0);

        // A strip is as wide as its own items — each item's own preferred width, so the macro strip's width
        // comes from the label it draws; the panels sections of the band share what is left — the design's
        // `flex: 1` beside a natural-width strip.
        int fixed = 0;
        int flexibleCount = 0;

        for (int i = 0; i < count; ++i)
        {
            const auto slot = static_cast<std::size_t> (index + static_cast<std::size_t> (i));
            const auto& section = sections_[slot];

            if (section.kind == SectionKind::Strip)
            {
                int items = 0;

                for (const auto* panel : panels_[slot])
                {
                    if (items > 0)
                        items += kPanelGap;

                    items += panel->preferredWidth();
                }

                band.widths[static_cast<std::size_t> (i)] = items;
                fixed += items;
            }
            else
            {
                ++flexibleCount;
            }
        }

        const auto gaps = (count - 1) * kPanelGap;
        auto remaining = juce::jmax (flexibleCount * 80, width - fixed - gaps);

        for (int i = 0; i < count; ++i)
        {
            if (sections_[static_cast<std::size_t> (index + static_cast<std::size_t> (i))].kind == SectionKind::Strip)
                continue;

            band.widths[static_cast<std::size_t> (i)] = remaining / flexibleCount;
            remaining -= band.widths[static_cast<std::size_t> (i)];
            --flexibleCount;
        }

        // A section's height is its heading plus the tallest panel it holds at the width it was given; the
        // matrix is its heading plus the grid it shows.
        for (int i = 0; i < count; ++i)
        {
            const auto slot = static_cast<std::size_t> (index + static_cast<std::size_t> (i));
            const auto& section = sections_[slot];
            auto& sectionPanels = panels_[slot];

            int tallest = 0;

            if (section.kind == SectionKind::Matrix)
            {
                tallest = matrixNatural - kSectionTitleHeight;
            }
            else if (! sectionPanels.empty())
            {
                const auto panelWidth = (band.widths[static_cast<std::size_t> (i)]
                                         - (static_cast<int> (sectionPanels.size()) - 1) * kPanelGap)
                                        / static_cast<int> (sectionPanels.size());

                for (auto* panel : sectionPanels)
                    tallest = juce::jmax (tallest, panel->preferredHeight (panelWidth));
            }

            band.heights[static_cast<std::size_t> (i)] = tallest == 0 ? 0 : kSectionTitleHeight + tallest;
            band.height = juce::jmax (band.height, band.heights[static_cast<std::size_t> (i)]);
        }

        bands.push_back (std::move (band));
        index = end;
    }

    // --- place --------------------------------------------------------------------------------------
    int y = 0;

    for (const auto& band : bands)
    {
        const auto count = static_cast<int> (band.end - band.begin);

        // A single strip alone in its band is centred, which is where the design's `macro-section` puts it
        // (`align-items: center`) when nothing shares its row. A strip the fall-through section shares the
        // row with is not alone, and starts at the row's left edge with the section beside it.
        int fixed = 0;
        auto flexibles = 0;

        for (int i = 0; i < count; ++i)
            if (sections_[band.begin + static_cast<std::size_t> (i)].kind == SectionKind::Strip)
                fixed += band.widths[static_cast<std::size_t> (i)] + kPanelGap;
            else
                ++flexibles;

        auto x = flexibles == 0 && fixed > 0 && fixed - kPanelGap < width ? (width - fixed + kPanelGap) / 2 : 0;

        for (int i = 0; i < count; ++i)
        {
            const auto slot = band.begin + static_cast<std::size_t> (i);
            const auto& section = sections_[slot];
            auto& sectionPanels = panels_[slot];

            const auto sectionRect = juce::Rectangle<int> (x, y, band.widths[static_cast<std::size_t> (i)],
                                                           band.height);
            bounds_[slot] = sectionRect;

            auto content = sectionRect.withTrimmedTop (kSectionTitleHeight);

            if (section.kind == SectionKind::Matrix)
            {
                // The rows the section has room for — `kMatrixVisibleRows` of them, since ticket `57` gives
                // the matrix no slack to grow into. The tag in the heading is this number, so the panel says
                // `4 / 64` rather than promising a count it does not show.
                const auto rows = juce::jmax (0, (content.getHeight() - ModulationMatrixView::kHeaderHeight)
                                                     / ModulationMatrixView::kRowHeight);

                matrixRows_ = rows;

                if (matrix_ != nullptr)
                    matrix_->setBounds (content.removeFromTop (
                        juce::jmin (content.getHeight(),
                                    ModulationMatrixView::kHeaderHeight + rows * ModulationMatrixView::kRowHeight + 2)));
            }
            else if (! sectionPanels.empty())
            {
                const auto panelWidth = (content.getWidth()
                                         - (static_cast<int> (sectionPanels.size()) - 1) * kPanelGap)
                                        / static_cast<int> (sectionPanels.size());
                auto px = content.getX();

                for (auto* panel : sectionPanels)
                {
                    panel->setBounds (px, content.getY(), panelWidth, content.getHeight());
                    px += panelWidth + kPanelGap;
                }
            }

            x += band.widths[static_cast<std::size_t> (i)] + kPanelGap;
        }

        y += band.height + kSectionGap;
    }

    totalHeight_ = juce::jmax (0, y - kSectionGap);
    setSize (width, totalHeight_);
    return totalHeight_;
}

void CompositionView::paintSectionHeading (juce::Graphics& graphics, const SectionPlan& section,
                                           juce::Rectangle<int> bounds) const
{
    auto heading = bounds.removeFromTop (kSectionTitleHeight);

    graphics.setColour (look::fgMuted);
    graphics.setFont (look::bodyFont (look::type::section, true).withExtraKerningFactor (0.18f));
    graphics.drawText (juce::String (section.title).toUpperCase(), heading,
                       juce::Justification::centredLeft, false);

    if (section.kind != SectionKind::Matrix)
        return;

    // The design's tag on the matrix's panel title: how many of the rows are on screen. The number is the
    // geometry's own — the section's height decided it in `layout` — rather than a written-down "8 / 64",
    // so a window with room to spare is described as showing more rows, and the caption never promises a
    // count the panel does not show.
    const auto font = look::monoFont (look::type::tag, true);
    const auto label = juce::String (matrixRows_) + " / " + juce::String (kModulationSlots);
    const auto width = static_cast<int> (juce::GlyphArrangement::getStringWidth (font, label)) + 12;
    const auto tag = juce::Rectangle<int> (heading.getX()
                                               + static_cast<int> (juce::GlyphArrangement::getStringWidth (
                                                     look::bodyFont (look::type::section, true), section.title)) + 22,
                                           heading.getY(), width, heading.getHeight())
                         .withSizeKeepingCentre (width, 14)
                         .toFloat();

    graphics.setColour (look::accentBg);
    graphics.fillRoundedRectangle (tag, 7.0f);
    graphics.setColour (look::accent);
    graphics.setFont (font);
    graphics.drawText (label, tag.toNearestInt(), juce::Justification::centred, true);
}

void CompositionView::paint (juce::Graphics& graphics)
{
    for (std::size_t index = 0; index < sections_.size(); ++index)
        if (index < bounds_.size() && ! bounds_[index].isEmpty())
            paintSectionHeading (graphics, sections_[index], bounds_[index]);
}

} // namespace xerum::plugin::ui
