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
//
// ## Ticket `51`: the controls draw
//
// A slider is now a **rotary knob** (`ui::Knob`) sized from the plan (`ui::knobSizeFor`), with the track
// arc, the value arc, the design's second **modulation arc**, a pointer and a keyboard focus ring — the
// drawing half of a decision `InterfacePlan` already made. A boolean is a **pill toggle** with a state dot
// rather than a stock checkbox, and a choice is a themed picker.
//
// The two stock widgets that remain are subclasses, not replacements, and that is deliberate: JUCE's
// `SliderParameterAttachment`/`ButtonParameterAttachment`/`ComboBoxParameterAttachment` are what make the
// binding model-driven, and the tests assert the widget *is* a `juce::Slider`/`ToggleButton`/`ComboBox`.
// `ui::Knob` derives from `juce::Slider`, `ui::PillToggle` from `juce::ToggleButton`, so every attachment
// and every dynamic_cast in the harness still holds while the paint routine is xerum's own.

#pragma once

#include "InterfacePlan.h"
#include "ParameterBridge.h"

#include <memory>
#include <string>
#include <string_view>

namespace xerum::plugin::ui
{

/// A rotary knob: JUCE's slider — so the attachment binds it and the harness can see a `juce::Slider` —
/// with xerum's own paint. `Dial` draws the design's knob; `Cell` draws one compact numeric field, which
/// is what a matrix cell is: 512 dials down a table would be a grid of dials, not a matrix.
class Knob final : public juce::Slider
{
public:
    enum class Mode { Dial, Cell };

    explicit Knob (KnobSize size = KnobSize::Default, Mode mode = Mode::Dial);

    KnobSize size() const { return size_; }
    Mode mode() const { return mode_; }

    /// How much this parameter is modulated, as a signed fraction of its full range: `+0.5` is a row at
    /// `+50`. Zero means "no row reaches this control", and zero draws **no** modulation arc — the second
    /// arc is a fact about the loaded preset, never decoration.
    void setModulationDepth (double depth);
    double modulationDepth() const { return modulation_; }
    bool isModulated() const { return modulation_ != 0.0; }

    /// A matrix amount cell: colour the value's sign (positive green, negative red) the way the design's
    /// matrix does, because for an amount the sign *is* the polarity.
    void setSignColouring (bool on) { signColoured_ = on; repaint(); }

    void paint (juce::Graphics& graphics) override;

    /// The knob's own geometry, so a test can read back where the arcs and the pointer are drawn instead
    /// of counting pixels to find them. `valueProportion` is 0..1 along the design's sweep.
    juce::Point<float> pointAtProportion (double proportion) const;
    juce::Rectangle<float> bodyBounds() const;

private:
    float angleForProportion (double proportion) const;

    KnobSize size_;
    Mode mode_;
    double modulation_ { 0.0 };
    bool signColoured_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Knob)
};

/// A two-state control as the design draws one: a pill with a state dot, the accent when on and the muted
/// foreground when off. In `compact` mode — a matrix cell — the pill is the dot alone.
class PillToggle final : public juce::ToggleButton
{
public:
    PillToggle();

    /// A matrix cell: the dot and nothing else, because the column header is the label.
    void setCompact (bool compact);

    /// Draw the dot as *live* when the value is off. The design's bypass dot is green when the row is
    /// doing something, and this engine's `kParamBypass` is `1` when it is **not** — so the cell inverts
    /// the dot rather than the parameter, which is the only honest reading of a parameter named "bypass".
    void setDotInverted (bool inverted);

    bool isCompact() const { return compact_; }
    bool dotInverted() const { return inverted_; }

    void paintButton (juce::Graphics& graphics, bool shouldDrawButtonAsHighlighted,
                      bool shouldDrawButtonAsDown) override;

private:
    bool compact_ { false };
    bool inverted_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PillToggle)
};

class ParameterControl final : public juce::Component
{
public:
    /// `showLabel` is false for a **matrix cell**, where the column header is the label and a per-cell one
    /// would be repeated 64 times down the grid.
    ParameterControl (const ParamSpec& spec, juce::RangedAudioParameter& parameter, const ChoiceList& choices,
                      bool showLabel = true);

    /// A matrix amount cell: colour the value's sign, the way the design's matrix does — for an amount the
    /// sign *is* the polarity, and a zero must read as the centre rather than as a value.
    void setSignColouring (bool on);

    /// A matrix boolean cell: the dot alone, and — for the bypass — green while the row is live.
    void setCompactToggle (bool compact, bool inverted);

    const ParamSpec& spec() const { return spec_; }
    juce::RangedAudioParameter& parameter() const { return parameter_; }

    /// The widget the attachment binds: the toggle, the combo box or the slider.
    juce::Component& widget() const { return *widget_; }

    /// The knob, when this control is a slider — otherwise null. The editor sets its modulation arc.
    Knob* knob() { return controlKindFor (spec_.type) == ControlKind::Slider ? &knob_ : nullptr; }

    /// The label the interface shows — the spec's, which is xerum's.
    juce::String label() const;

    /// True once a JUCE attachment connects the widget to the parameter. The coverage test asserts a
    /// binding and not merely a widget, because a widget nobody bound is exactly the drift ticket `13`
    /// (D6) forbids.
    bool isBound() const;

    /// True once the control draws its own label / readout, which a matrix cell does not.
    bool showsLabel() const { return showLabel_; }

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    const ParamSpec& spec_;
    juce::RangedAudioParameter& parameter_;

    PillToggle toggle_;
    juce::ComboBox combo_;
    Knob knob_;
    juce::Component* widget_ { nullptr };

    bool showLabel_ { true };

    // Exactly one of these is created, for the kind the spec's type maps to: the three JUCE attachment
    // classes share their job and no base class.
    std::unique_ptr<juce::ButtonParameterAttachment> toggleAttachment_;
    std::unique_ptr<juce::ComboBoxParameterAttachment> choiceAttachment_;
    std::unique_ptr<juce::SliderParameterAttachment> sliderAttachment_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterControl)
};

/// A page: one spec group's controls, in table order, laid out in the columns the plan balanced its rows
/// into.
///
/// Ticket `44` made a panel a *page* — one device behind the strip — so the panel is now the thing the
/// user sees filling the window, and `setColumns` is how the plan's balance reaches the screen: the
/// panel places rows, it does not decide which column a row lands in. A panel with no columns set lays
/// its rows out in one column, which is what a test that only wants the controls does.
class ParameterPanel final : public juce::Component
{
public:
    explicit ParameterPanel (std::string title);

    void addControl (std::unique_ptr<ParameterControl> control);

    /// The plan's row columns: `columns[c]` lists the row indices column `c` holds, in reading order.
    /// Every row appears exactly once — the plan asserts it rather than this class repairing it.
    void setColumns (std::vector<std::vector<int>> columns);

    int rows() const { return controls_.size(); }
    int columnCount() const { return columns_.empty() ? 1 : static_cast<int> (columns_.size()); }
    const std::string& title() const { return title_; }

    /// The control at a row index, or nullptr. The panel's own order is the plan's row order.
    ParameterControl* controlAtRow (int row) const;

    /// The control bound to `id`, or nullptr. Used by the editor's own coverage check and by the tests.
    ParameterControl* controlFor (std::string_view id) const;

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    std::string title_;
    juce::OwnedArray<ParameterControl> controls_;
    std::vector<std::vector<int>> columns_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterPanel)
};

} // namespace xerum::plugin::ui
