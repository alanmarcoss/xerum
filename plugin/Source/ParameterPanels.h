// xerum::plugin — the interface's building blocks: one parameter's control, one group's panel, and the
// composition that arranges the panels.
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
// A slider is a **rotary knob** (`ui::Knob`) sized from the plan (`ui::knobSizeFor`), with the track arc,
// the value arc, the design's second **modulation arc**, a pointer and a keyboard focus ring. A boolean
// is a **pill toggle** with a state dot rather than a stock checkbox. The remaining stock widget is a
// themed picker, and it is a subclass-free use of JUCE's own: `SliderParameterAttachment`,
// `ButtonParameterAttachment` and `ComboBoxParameterAttachment` are what make the binding model-driven,
// and the tests assert the widget *is* a `juce::Slider`/`ToggleButton`/`ComboBox`. `ui::Knob` derives
// from `juce::Slider`, `ui::PillToggle` from `juce::ToggleButton`, so every attachment and every
// dynamic_cast in the harness still holds while the paint routine is xerum's own.
//
// ## Ticket `52`: the panel arranges itself, the composition arranges the panels
//
// `ui::ParameterPanel` now lays its own controls out — a row of knobs, a row of pickers, a row of pills,
// wrapped to the width it is given — because that is the design's own grammar (`osc-knobs` then
// `osc-controls`) and it is derived from the model types rather than written down. NOISE and SUB, the
// design's compact band, put their knobs and their option buttons on one line (`--gap-inline`).
//
// Above a panel sits `ui::CompositionView`: the one component that arranges whole sections — three
// oscillators in a row, noise and sub beneath them, the filters with their response displays beside their
// knobs, the matrix, the effects and macros side by side — from `ui::SectionPlan` data. A choice whose
// vocabulary is small is drawn the way the design draws one (`ui::ChoiceButtons`, the `btn-opt` row);
// a longer one is the picker.

#pragma once

#include "InterfacePlan.h"
#include "ParameterBridge.h"
#include "document.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

/// A choice whose vocabulary is short enough to be the design's **row of option buttons** (`btn-opt`):
/// NOISE's four colours and SUB's six waveforms. One pill per choice, the selected one in the accent on
/// `accent-bg`, and the parameter bound through JUCE's own `ParameterAttachment` — the same mechanism the
/// picker uses, so a click here is a host gesture and the coverage count still counts one control.
class ChoiceButtons final : public juce::Component,
                            private juce::ParameterAttachment
{
public:
    ChoiceButtons (juce::RangedAudioParameter& parameter, const ChoiceList& choices);

    int choiceCount() const;
    int selectedIndex() const { return selected_; }

    /// The word on a pill — the bridge's own label for the choice, so the row and the parameter cannot
    /// call a choice two things.
    juce::String choiceText (int index) const;

    /// Where a pill is, so a click and a test aim at the same rectangle.
    juce::Rectangle<int> buttonBounds (int index) const;

    /// The width the whole row wants — the panel's row layout uses it, so pills are never clipped.
    int preferredWidth() const;

    /// The arrows step the selection — the option row is a control like the picker beside it, and a
    /// keyboard user gets the same choices rather than a row only a mouse can reach.
    bool keyPressed (const juce::KeyPress& key) override;

    void paint (juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;

private:
    /// The choice a normalised parameter value selects — the same linear mapping JUCE's own
    /// `AudioParameterChoice` uses, so a click here and a click on the picker agree.
    int indexFor (float normalised) const;

    void parameterChanged (int index);

    const ChoiceList& choices_;
    int selected_ { 0 };
    int hovered_ { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoiceButtons)
};

/// The design's wavetable slot: the monospaced name field and the two 16px arrow buttons, in an
/// oscillator panel's head. What it shows is the loaded table's own name — the reference's asset name, or
/// `inline` for a preset carrying its table in its own data, or `off` for a slot that is not
/// synthesising a wavetable.
///
/// **What the arrows do, and what they do not.** The design's arrows choose a different wavetable; xerum
/// has no wavetable chooser — a recorded decision (`plugin/README.md`: a table chooser is a UI feature
/// with a dynamic choice list), and the loaded table is a preset asset rather than a parameter, so there
/// is no list for the arrows to step. They step the slot's own **frame position** instead: the one
/// per-slot table control the model has, the same gesture on the same asset, and `onStep` is wired to
/// that parameter by the editor. A slot with no frame parameter gets the field and **no arrows**, rather
/// than arrows that write nothing.
class WavetableSlot final : public juce::Component
{
public:
    WavetableSlot();

    void setTableName (juce::String name);
    const juce::String& tableName() const { return name_; }

    /// Draw the arrows at all. False when the slot's table has no frame parameter to step.
    void setSteppable (bool steppable);
    bool isSteppable() const { return steppable_; }

    /// One step of the arrows: -1 for ◀, +1 for ▶.
    std::function<void (int)> onStep;

    juce::Rectangle<int> arrowBounds (int step) const;

    void paint (juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;

private:
    juce::String name_;
    bool steppable_ { true };
    int hovered_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WavetableSlot)
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

    /// The widget the attachment binds: the toggle, the picker, the button row or the slider.
    juce::Component& widget() const { return *widget_; }

    /// How a choice of this control is drawn — the design's option row for a short vocabulary, the picker
    /// otherwise. A non-choice answers `Menu`, which is the picker it has.
    ChoiceStyle choiceStyle() const;

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

    /// The width this control's cell wants at most — a knob's diameter and its label, a choice's option
    /// row or its label column. The panel's row layout asks before it packs a row.
    int preferredWidth() const;

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    const ParamSpec& spec_;
    juce::RangedAudioParameter& parameter_;

    PillToggle toggle_;
    juce::ComboBox combo_;
    Knob knob_;
    std::unique_ptr<ChoiceButtons> choiceButtons_;   ///< present only for the design's option row
    juce::Component* widget_ { nullptr };

    bool showLabel_ { true };

    // Exactly one of these is created, for the kind the spec's type maps to: the three JUCE attachment
    // classes share their job and no base class.
    std::unique_ptr<juce::ButtonParameterAttachment> toggleAttachment_;
    std::unique_ptr<juce::ComboBoxParameterAttachment> choiceAttachment_;
    std::unique_ptr<juce::SliderParameterAttachment> sliderAttachment_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterControl)
};

/// A panel: one spec group's controls, laid out the way the design lays a panel out — a head, optionally
/// the design's wavetable slot, then rows of controls derived from their kinds.
///
/// The panel lays out **all** of its controls every time it is resized, and it never decides *which* panel
/// or section it is in: `CompositionView` decides where the panel is and how wide, and this decides how its
/// own rows fall inside that. `preferredHeight` answers the same question for the composition's layout, so
/// a height and a rectangle cannot disagree.
class ParameterPanel final : public juce::Component
{
public:
    explicit ParameterPanel (std::string title);

    void addControl (std::unique_ptr<ParameterControl> control);

    /// The design's wavetable slot for this panel's head, or null. The panel takes ownership.
    void setWavetableSlot (std::unique_ptr<WavetableSlot> slot);
    WavetableSlot* wavetableSlot() const { return wavetable_.get(); }

    /// A drawn surface the panel carries **inside itself**, beside its controls — the design's
    /// `filter-viz`, which is the filter's response display. Not owned: the editor owns the plane.
    void setInlineDisplay (juce::Component* display);
    juce::Component* inlineDisplay() const { return display_; }

    /// A strip item (a macro in the bottom row): no panel chrome, and the title under the control, which
    /// is how the design draws the macro strip.
    void setCompact (bool compact);
    bool isCompact() const { return compact_; }

    int rows() const { return controls_.size(); }
    const std::string& title() const { return title_; }

    /// The control at a row index, or nullptr. The panel's own order is the plan's row order.
    ParameterControl* controlAtRow (int row) const;

    /// The control bound to `id`, or nullptr. Used by the editor's own coverage check and by the tests.
    ParameterControl* controlFor (std::string_view id) const;

    /// The height this panel needs at this width, from the same row derivation `resized` uses.
    int preferredHeight (int width) const;

    /// The width a **strip item** wants: the design's `macro-item` is as wide as the wider of its knob and
    /// its label, and a strip of them is their own widths plus the composition's panel gap. A panel that
    /// shares a full-width row instead of standing in a strip has no preferred width — the row gives it one.
    int preferredWidth() const;

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    /// One row of controls, by index into `controls_`, and how tall it is.
    struct Row
    {
        std::vector<int> controls;
        int height { 0 };
        int gap { kKnobCellGap };   ///< the design's `--gap-knob`, or `--gap-inline` in the compact band
    };

    /// The width the inline display takes out of this panel at this width.
    int displayReserve (int width) const;

    /// The width a control's cell wants inside a row of this panel.
    int cellWidthFor (const ParameterControl& control, int inner) const;

    /// The panel's controls, grouped into rows the way the design groups them.
    std::vector<Row> rowsFor (int width) const;

    std::string title_;
    juce::OwnedArray<ParameterControl> controls_;
    std::unique_ptr<WavetableSlot> wavetable_;
    juce::Component* display_ { nullptr };
    bool compact_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterPanel)
};

/// The composed page: the design's arrangement of whole sections, in the design's order — three
/// oscillators in a row, noise and sub beneath them, the amplitude envelope's own band, the two filters
/// with their response displays beside their knobs, the macros beside the fall-through section in the
/// design's bottom row, and the compact matrix last. Ticket `52`'s answer to "the design specifies a
/// composition, the interface derived a grid", with ticket `57`'s editorial order — the envelope's section
/// and the matrix's place — on top of it.
///
/// It arranges and paints; it binds nothing and owns nothing. The panels, the matrix and the planes belong
/// to the editor, and this is the one place that knows where they sit — which is why the layout is a
/// single function that both sets the rectangles and returns the height, rather than a `resized` and a
/// separate estimate that could disagree.
///
/// ## Ticket `53`: the page is packed to fit, and one section takes the slack
///
/// The design's own content box is a grid whose fourth row is `1fr` and it fits a 900×600 window with no
/// scroll at all. The composition here is packed the same way: the sections' own heights are measured, and
/// the difference between what they need and what the window has is given to the composition's **flexible
/// section** — `SectionSpec::flexible`, the design's `1fr` — which shows as much of itself as fits and says
/// how much it is showing. Below its natural height the page is taller than the window and scrolls; it
/// never shrinks a section to hide it.
///
/// ## Ticket `57`: the flexible section is the amplitude envelope's, and the matrix is compact
///
/// Ticket `53` handed the slack to the **matrix**, on the argument that a grid of 64 rows is the one
/// section a taller window can usefully give more of. The dev's editorial call is the other way round: the
/// amplitude envelope is the control a player touches on every patch, and the matrix is the one that was
/// occupying the page. So the matrix gives the flexible row up (it is the envelope's in the data now), the
/// compact matrix shows `kMatrixVisibleRows` rows and moves to the **end** of the composition, and the
/// envelope's own band — early, at full width, with the design's largest knobs — is the section a taller
/// window grows.
///
/// What the envelope's slack *is* is air: the row of four knobs is a fixed 68px and the design system has
/// no envelope content that uses height. That is stated in `plugin/tests/interface_test.cpp` as a
/// measurement rather than hidden, because it is the one part of this ticket the geometry cannot settle.
class CompositionView final : public juce::Component
{
public:
    CompositionView();

    /// The resolved composition and the panels it holds, indexed as `SectionPlan::panels` indexes them.
    void setComposition (std::vector<SectionPlan> sections, std::vector<ParameterPanel*> panels);

    /// The matrix, when the composition has a matrix section. Not owned; null is allowed.
    void setMatrix (juce::Component* matrixViewport);

    /// How many rows of the matrix the matrix section shows.
    ///
    /// Ticket `53` set this to the design's own **eight** — the tag on the design's panel reads `8 / 64`
    /// — and the section grew from there into whatever slack the window had. Ticket `57` makes the matrix
    /// a **compact panel** that keeps its tag but gives its slack up: it shows four of the 64 rows, which
    /// is half the design's matrix and few enough that the last band of the page is no taller than the
    /// bottom row above it. `matrixVisibleRows` still reports what the geometry actually shows, so the tag
    /// in the heading and the grid on screen cannot disagree.
    static constexpr int kMatrixVisibleRows = 4;

    /// Arrange the whole page for this width and this much window height, and return the height it needs.
    /// The height is what the sections themselves need: when the sections fit inside `windowHeight`, the
    /// **flexible** section (the data's `1fr`) grows into the difference and the page is exactly
    /// `windowHeight`; when they do not, the page is taller than the window and the editor scrolls it —
    /// never a clipped section.
    int layout (int width, int windowHeight);

    /// The resolved composition, in the design's order, and where each section landed in the page's own
    /// coordinates.
    const std::vector<SectionPlan>& sections() const { return sections_; }
    juce::Rectangle<int> sectionBounds (int index) const;

    /// Which section covers a y in the composition's coordinates, or -1.
    int sectionAt (int y) const;

    /// How many rows of the 64 the matrix section is showing right now — the tag in its heading.
    int matrixVisibleRows() const;

    int totalHeight() const { return totalHeight_; }

    void paint (juce::Graphics& graphics) override;

private:
    void paintSectionHeading (juce::Graphics& graphics, const SectionPlan& section,
                              juce::Rectangle<int> bounds) const;


    std::vector<SectionPlan> sections_;
    std::vector<std::vector<ParameterPanel*>> panels_;   ///< per section
    std::vector<juce::Rectangle<int>> bounds_;           ///< per section
    juce::Component* matrix_ { nullptr };
    int matrixRows_ { 0 };
    int totalHeight_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompositionView)
};

} // namespace xerum::plugin::ui
