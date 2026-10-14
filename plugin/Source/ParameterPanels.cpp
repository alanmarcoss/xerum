// xerum::plugin — the interface's building blocks, in code. See ParameterPanels.h.

#include "ParameterPanels.h"

#include "InterfaceLook.h"

#include <algorithm>

namespace xerum::plugin::ui
{

namespace
{
constexpr int kReadoutHeight = 13;
constexpr int kLabelHeight = 11;
constexpr int kToggleMaxWidth = 150;

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

bool ParameterControl::isBound() const
{
    return toggleAttachment_ != nullptr || choiceAttachment_ != nullptr || sliderAttachment_ != nullptr;
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
        graphics.setFont (look::bodyFont (knob_.size() == KnobSize::Default ? look::type::label
                                                                           : look::type::tag));
        graphics.drawText (label(), labelCentre, juce::Justification::centred, true);
    }
    else if (kind == ControlKind::Choice)
    {
        auto area = getLocalBounds();
        const auto labelWidth = juce::jmin (kRowLabelWidth, (area.getWidth() * 2) / 5);

        graphics.setColour (look::fgDim);
        graphics.setFont (look::bodyFont (11.0f));
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
        toggle_.setBounds (area.removeFromLeft (juce::jmin (area.getWidth(), kToggleMaxWidth)).reduced (0, 2));
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

void ParameterPanel::setColumns (std::vector<std::vector<int>> columns)
{
    columns_ = std::move (columns);
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

void ParameterPanel::paint (juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat();

    graphics.setColour (look::surface);
    graphics.fillRoundedRectangle (area, static_cast<float> (look::radiusPanel));

    graphics.setColour (look::border);
    graphics.drawRoundedRectangle (area.reduced (0.5f), static_cast<float> (look::radiusPanel), 1.0f);

    // The design's panel title: 9px bold, uppercase, the primary foreground.
    graphics.setColour (look::fg);
    graphics.setFont (look::bodyFont (look::type::section, true));
    graphics.drawText (juce::String (title_).toUpperCase(),
                       kPanelPadding + 3, 0, getWidth() - kPanelPadding * 2 - 6, kPanelHeaderHeight,
                       juce::Justification::centredLeft, true);
}

void ParameterPanel::resized()
{
    auto area = getLocalBounds().reduced (kPanelPadding);

    area.removeFromTop (kPanelHeaderHeight);

    // No columns set means one column: the plan always sets them, but a panel built by hand (a test) must
    // still be laid out rather than left with every control at zero size.
    if (columns_.empty())
    {
        for (auto* control : controls_)
        {
            control->setBounds (area.removeFromTop (rowHeightFor (control->spec())));
            area.removeFromTop (kRowGap);
        }

        return;
    }

    const auto columns = static_cast<int> (columns_.size());
    const auto columnWidth = (area.getWidth() - kColumnGap * (columns - 1)) / columns;

    auto remaining = area;

    for (const auto& column : columns_)
    {
        auto slot = remaining.removeFromLeft (columnWidth);
        remaining.removeFromLeft (kColumnGap);

        for (const auto row : column)
        {
            if (auto* control = controlAtRow (row))
            {
                // The control takes the whole column: a knob centres itself in it, and a label/control row
                // uses the width it is given rather than being clipped to a written-down number.
                control->setBounds (slot.removeFromTop (rowHeightFor (control->spec())));
                slot.removeFromTop (kRowGap);
            }
        }
    }
}

} // namespace xerum::plugin::ui
