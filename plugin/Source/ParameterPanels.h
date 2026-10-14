// xerum::plugin — the interface's two building blocks: one parameter's control, and one group's panel.
//
// Ticket `31`. Neither class knows a parameter by name. A `ParameterControl` is constructed from a
// `ParamSpec` and the bridge's own parameter object, picks the widget its model type maps to
// (`ui::controlKindFor`), labels it with the spec's label — xerum's own, never the reference's model
// names (`kParamPitch`) and never the reference *plugin's* VST3 strings — and binds the two with JUCE's
// own attachment. A `ParameterPanel` is a titled box holding the controls of one spec group.
//
// That is what makes the binding model-driven: the editor builds one control per spec and one panel per
// group, so adding a parameter to the bridge adds a control here, and the interface cannot be missing a
// parameter the engine has. `plugin/tests/interface_test.cpp` asserts exactly that.

#pragma once

#include "InterfacePlan.h"
#include "ParameterBridge.h"

#include <memory>
#include <string>
#include <string_view>

namespace xerum::plugin::ui
{

class ParameterControl final : public juce::Component
{
public:
    ParameterControl (const ParamSpec& spec, juce::RangedAudioParameter& parameter, const ChoiceList& choices);

    const ParamSpec& spec() const { return spec_; }
    juce::RangedAudioParameter& parameter() const { return parameter_; }

    /// The widget the attachment binds: the toggle, the combo box or the slider.
    juce::Component& widget() const { return *widget_; }

    /// The label the interface shows — the spec's, which is xerum's.
    juce::String label() const;

    /// True once a JUCE attachment connects the widget to the parameter. The coverage test asserts a
    /// binding and not merely a widget, because a widget nobody bound is exactly the drift ticket `13`
    /// (D6) forbids.
    bool isBound() const;

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    const ParamSpec& spec_;
    juce::RangedAudioParameter& parameter_;

    juce::Label label_;
    juce::ToggleButton toggle_;
    juce::ComboBox combo_;
    juce::Slider slider_;
    juce::Component* widget_ { nullptr };

    // Exactly one of these is created, for the kind the spec's type maps to: the three JUCE attachment
    // classes share their job and no base class.
    std::unique_ptr<juce::ButtonParameterAttachment> toggleAttachment_;
    std::unique_ptr<juce::ComboBoxParameterAttachment> choiceAttachment_;
    std::unique_ptr<juce::SliderParameterAttachment> sliderAttachment_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterControl)
};

/// A titled box holding one spec group's controls, in table order.
class ParameterPanel final : public juce::Component
{
public:
    explicit ParameterPanel (std::string title);

    void addControl (std::unique_ptr<ParameterControl> control);

    int rows() const { return controls_.size(); }
    const std::string& title() const { return title_; }

    /// The control bound to `id`, or nullptr. Used by the editor's own coverage check and by the tests.
    ParameterControl* controlFor (std::string_view id) const;

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    std::string title_;
    juce::OwnedArray<ParameterControl> controls_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterPanel)
};

} // namespace xerum::plugin::ui
