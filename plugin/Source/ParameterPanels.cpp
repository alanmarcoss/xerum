// xerum::plugin — the interface's building blocks, in code. See ParameterPanels.h.

#include "ParameterPanels.h"

#include "InterfaceLook.h"

namespace xerum::plugin::ui
{

ParameterControl::ParameterControl (const ParamSpec& spec, juce::RangedAudioParameter& parameter,
                                    const ChoiceList& choices, const bool showLabel)
    : spec_ (spec),
      parameter_ (parameter)
{
    const auto kind = controlKindFor (spec.type);

    if (kind == ControlKind::Toggle)
    {
        // A boolean row is the toggle itself: the button's text is the label, so the row needs no label
        // column and the panel reads as a list of switches. A matrix cell has the column header instead.
        toggle_.setButtonText (showLabel ? label() : juce::String());
        toggle_.setClickingTogglesState (true);
        toggle_.setColour (juce::ToggleButton::textColourId, look::text);
        toggle_.setColour (juce::ToggleButton::tickColourId, look::accent);
        toggle_.setColour (juce::ToggleButton::tickDisabledColourId, look::track);
        widget_ = &toggle_;
        toggleAttachment_ = std::make_unique<juce::ButtonParameterAttachment> (parameter, toggle_);
    }
    else if (kind == ControlKind::Choice)
    {
        if (showLabel)
            label_.setText (label(), juce::dontSendNotification);

        label_.setJustificationType (juce::Justification::centredLeft);
        label_.setFont (juce::FontOptions (12.5f));
        label_.setColour (juce::Label::textColourId, look::dimText);

        // The items are the bridge's vocabulary, in its order: the attachment maps item index onto the
        // parameter's steps linearly, so the combo must hold exactly the choices the parameter has — no
        // more (a phantom choice) and no fewer (a choice the user cannot reach).
        for (int i = 0; i < choices.size(); ++i)
            combo_.addItem (choices.labels[i], i + 1);

        combo_.setColour (juce::ComboBox::backgroundColourId, look::track);
        combo_.setColour (juce::ComboBox::textColourId, look::text);
        combo_.setColour (juce::ComboBox::outlineColourId, look::panelBorder);
        combo_.setColour (juce::ComboBox::arrowColourId, look::accent);
        widget_ = &combo_;
        choiceAttachment_ = std::make_unique<juce::ComboBoxParameterAttachment> (parameter, combo_);
    }
    else
    {
        if (showLabel)
            label_.setText (label(), juce::dontSendNotification);

        label_.setJustificationType (juce::Justification::centredLeft);
        label_.setFont (juce::FontOptions (12.5f));
        label_.setColour (juce::Label::textColourId, look::dimText);

        slider_.setSliderStyle (juce::Slider::LinearHorizontal);
        slider_.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 18);
        slider_.setColour (juce::Slider::backgroundColourId, look::track);
        slider_.setColour (juce::Slider::trackColourId, look::accent);
        slider_.setColour (juce::Slider::thumbColourId, look::text);
        slider_.setColour (juce::Slider::textBoxTextColourId, look::text);
        slider_.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        slider_.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

        if (spec.type == ParamType::Int)
            slider_.setNumDecimalPlacesToDisplay (0);

        widget_ = &slider_;
        sliderAttachment_ = std::make_unique<juce::SliderParameterAttachment> (parameter, slider_);
    }

    addAndMakeVisible (*widget_);

    if (label_.getText().isNotEmpty())
        addAndMakeVisible (label_);
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
    graphics.setColour (look::rowDivider);
    graphics.fillRect (getLocalBounds().removeFromBottom (1));
}

void ParameterControl::resized()
{
    auto area = getLocalBounds();

    if (controlKindFor (spec_.type) == ControlKind::Toggle)
    {
        toggle_.setBounds (area.reduced (0, 1));
        return;
    }

    // A cell with no label hands the whole width to the widget — the matrix's case.
    if (label_.getText().isEmpty())
    {
        widget_->setBounds (area.reduced (2, 3));
        return;
    }

    // The label column is capped, because the label is a hint and the control is the thing the user
    // aims at; on a narrow column the two share the row rather than the label eating the slider.
    const auto labelWidth = juce::jmin (kRowLabelWidth, (area.getWidth() * 3) / 5);

    label_.setBounds (area.removeFromLeft (labelWidth).reduced (0, 1));
    widget_->setBounds (area.reduced (2, 3));
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

    graphics.setColour (look::panelFill);
    graphics.fillRoundedRectangle (area, 5.0f);

    graphics.setColour (look::panelBorder);
    graphics.drawRoundedRectangle (area.reduced (0.5f), 5.0f, 1.0f);

    graphics.setColour (look::accent);
    graphics.fillRect (juce::Rectangle<float> (area.getX() + 1.0f, area.getY() + 1.0f, 3.0f, area.getHeight() - 2.0f));

    graphics.setColour (look::text);
    graphics.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    graphics.drawText (juce::String (title_), kPanelPadding + 4, 2, getWidth() - kPanelPadding * 2 - 4,
                       kPanelHeaderHeight, juce::Justification::centredLeft, true);
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
            control->setBounds (area.removeFromTop (kRowHeight));
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
                control->setBounds (slot.removeFromTop (kRowHeight));
                slot.removeFromTop (kRowGap);
            }
        }
    }
}

} // namespace xerum::plugin::ui
