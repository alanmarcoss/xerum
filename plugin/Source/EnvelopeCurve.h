// xerum::plugin — the amplitude envelope, drawn.
//
// Ticket `59`, and the other half of ticket `57`'s finding. `57` gave the amplitude envelope the page's
// largest knobs, its own full-width band early in the composition and the window's slack — and then
// measured that what it had won was **air**: one row of four 44px knobs on a 1060-wide card, with the
// design system carrying no envelope graphic to put in the rest of it. The dev has now asked for the
// picture directly, so this is that display — an invention, because `docs/design/xerum-direction-warm.html`
// has none, drawn **in the design system's own language** rather than as a second visual dialect: the
// tokens, the panel radius, the accent for the curve and the modulation's `accentDim` for the second one,
// the monospaced face for the times.
//
// ## It displays; it does not control
//
// The four knobs still control the four stages and nothing about them changes: the graph is a **child of
// the envelope's panel**, drawn in the panel's `filter-viz` column (the design's own inline display, the
// place the pole/zero plane sits in the filter panels), and it holds no parameter of its own. Dragging the
// curve is out of scope for this ticket — and saying so is the honest way to leave it, because the picture
// *does* look draggable: a player will reach for the corner of an ADSR. It is a read-out, and if it is to
// become an editor that is a ticket with its own gesture rules, not a silent half-interaction here.
//
// ## Derived, not decorative
//
// The graph reads the **knobs** — the same `juce::Slider`s the panel's controls are, built from the same
// `ParamSpec`s and bound to the same parameters, which is what `ui::envelopeStageForModelPath` finds them
// by. So there is no second source of truth to drift: the curve's shape is a function of
// `Slider::valueToProportionOfLength`, the very number the knob's value arc is drawn from, and its second
// curve is drawn from the same number shifted by the row's amount, which is what the knob's modulation arc
// sweeps. A curve unrelated to the values would be worse than no curve; this one cannot be.
//
// ## The time axis, and why it is not the proportion
//
// The three time knobs are **linear in seconds** over a 0..32 s range (`ParameterBridge`), so a musical
// 5 ms attack is 0.016 % of its own range. A picture drawn in proportion would therefore be a box at every
// setting a player actually uses, and would move by a pixel when the attack went from 1 ms to a second.
// The horizontal axis is logarithmic in seconds instead — from 1 ms (a stage at or below it is drawn as
// the vertical edge an instant stage is) up to the largest maximum among the three time knobs — so each
// stage's width is a *monotone* function of its own value, and the legend underneath prints the values in
// full so the shape is read as a shape and the times as times. The hold is a **level**, not a duration, and
// it keeps a fixed share of the width written as the width a 200 ms stage would take; the four shares are
// normalised to the plot, so the display always fills and the picture changes with the attack whatever the
// other three are set to. The one consequence of that normalisation, stated rather than hidden: scaling
// attack, decay and release *together* leaves the picture's proportions alone — the picture is the shape and
// the legend is the time.

#pragma once

#include "InterfaceLook.h"
#include "InterfacePlan.h"

#include <array>

namespace xerum::plugin::ui
{

/// The amplitude envelope, drawn. One component, four stages, no parameters of its own.
class EnvelopeCurve final : public juce::Component,
                            private juce::Slider::Listener
{
public:
    EnvelopeCurve();
    ~EnvelopeCurve() override;

    /// Bind one stage to the control that carries it — the knob, so that the picture and the arc are the
    /// same number. Null unbinds, and a graph whose four stages are not all bound draws its frame and
    /// nothing else rather than a shape no parameter produced.
    void setStage (EnvelopeStage stage, juce::Slider* knob);
    juce::Slider* stageKnob (EnvelopeStage stage) const;

    /// How much the matrix moves this stage, as a signed fraction of the parameter's range — the same
    /// quantity, in the same units, that `Knob::setModulationDepth` is given, so the second curve and the
    /// knob's second arc cannot disagree. Zero draws no second curve.
    void setStageModulation (EnvelopeStage stage, double depth);
    double stageModulation (EnvelopeStage stage) const;

    /// True when every stage is bound: the graph has four parameters to draw rather than a frame.
    bool isBound() const;

    /// What the curve is drawn from, exposed so a test can assert the picture against the values rather
    /// than against the picture: the stage's own value, its text (the knob's readout, the same string the
    /// control prints), and its position along its range — the number the arc is drawn from.
    double stageValue (EnvelopeStage stage) const;
    juce::String stageText (EnvelopeStage stage) const;
    double stageProportion (EnvelopeStage stage) const;

    /// Whether any stage carries modulation — the second curve is drawn only when a row reaches one, so this
    /// is "is the matrix reaching this envelope" as a question the interface can answer.
    bool isModulated() const;

    /// The curve's own geometry, in this component's coordinates: the five corners, so a test reads the
    /// shape rather than counting pixels. `modulated` draws the second curve — each stage's proportion
    /// shifted by its own modulation depth, the way the arc is.
    struct Geometry
    {
        juce::Rectangle<float> plot;   ///< the area the curve is drawn in
        juce::Point<float> start;
        juce::Point<float> attackPeak;
        juce::Point<float> decayEnd;
        juce::Point<float> sustainEnd;
        juce::Point<float> releaseEnd;
    };

    Geometry geometry (bool modulated = false) const;

    void paint (juce::Graphics& graphics) override;

private:
    void sliderValueChanged (juce::Slider* slider) override;

    juce::Slider* knobFor (EnvelopeStage stage) const;
    double modulationFor (EnvelopeStage stage) const;

    /// The area the curve itself is drawn in: the component less the legend line and the display's own
    /// inset.
    juce::Rectangle<float> plotBounds() const;

    /// The four stages' shares of the plot's width, in draw order — attack, decay, hold, release. The hold
    /// is a share of its own (see the header comment) and is never nought, so the sum is never nought.
    std::array<double, 4> stageWeights (bool modulated) const;

    std::array<juce::Slider*, 4> knobs_ {};
    std::array<double, 4> modulation_ {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeCurve)
};

} // namespace xerum::plugin::ui
