// xerum::plugin — the drawn amplitude envelope. See EnvelopeCurve.h.

#include "EnvelopeCurve.h"

#include <algorithm>
#include <cmath>

namespace xerum::plugin::ui
{

namespace
{
/// The display's own numbers, from the design system rather than from nowhere: the inset is its `--gap-inline`
/// (6), the surface's corner is the panel radius, and the legend is one 8px monospaced line.
constexpr float kCurveInset = static_cast<float> (look::space::inlineGap);
constexpr float kCurveStroke = 1.5f;
constexpr float kLegendHeight = 11.0f;

/// The time axis' floor: a stage at or below one millisecond is drawn as the vertical edge an instant stage
/// is, because that is what it is. The axis' ceiling is the knobs' own maximum (see `stageWeights`), so
/// nothing about the range is written down a second time.
constexpr double kTimeFloorSeconds = 0.001;

/// The hold's share of the axis, written as the width **a 200 ms stage would take**: sustain is a level and
/// not a duration, so it cannot have a time of its own, and this is the honest form of "a plateau the
/// picture can show". It is never nought, which is also what keeps the four shares a real distribution.
constexpr double kHoldSeconds = 0.2;

/// The four stages in draw order, so a loop over them is the picture's own order rather than an aside.
constexpr std::array<EnvelopeStage, 4> kStages { EnvelopeStage::Attack, EnvelopeStage::Decay,
                                                 EnvelopeStage::Sustain, EnvelopeStage::Release };

/// The letter the legend prints for a stage — the letters every envelope's four stages are known by.
const char* stageLetter (const EnvelopeStage stage)
{
    switch (stage)
    {
        case EnvelopeStage::Attack:  return "A";
        case EnvelopeStage::Decay:   return "D";
        case EnvelopeStage::Sustain: return "S";
        case EnvelopeStage::Release: return "R";
    }

    return "?";
}

constexpr int stageIndex (const EnvelopeStage stage)
{
    return static_cast<int> (stage);
}
} // namespace

//==============================================================================
EnvelopeCurve::EnvelopeCurve()
{
    // The graph is a read-out: it paints and it never takes a click or a key. `setInterceptsMouseClicks
    // (false, false)` is what says so at the component level, so a click on the curve reaches the panel
    // behind it rather than dying in a component that does nothing with it.
    setInterceptsMouseClicks (false, false);
}

EnvelopeCurve::~EnvelopeCurve()
{
    for (auto* knob : knobs_)
        if (knob != nullptr)
            knob->removeListener (this);
}

void EnvelopeCurve::setStage (const EnvelopeStage stage, juce::Slider* knob)
{
    auto& bound = knobs_[static_cast<std::size_t> (stageIndex (stage))];

    if (bound == knob)
        return;

    if (bound != nullptr)
        bound->removeListener (this);

    bound = knob;

    if (bound != nullptr)
        bound->addListener (this);

    repaint();
}

juce::Slider* EnvelopeCurve::stageKnob (const EnvelopeStage stage) const
{
    return knobs_[static_cast<std::size_t> (stageIndex (stage))];
}

void EnvelopeCurve::setStageModulation (const EnvelopeStage stage, const double depth)
{
    // Clamped where the knob clamps its own (`Knob::setModulationDepth`), so the two curves and the two
    // arcs are built from one number rather than two roundings of it.
    modulation_[static_cast<std::size_t> (stageIndex (stage))] = juce::jlimit (-1.0, 1.0, depth);
    repaint();
}

double EnvelopeCurve::stageModulation (const EnvelopeStage stage) const
{
    return modulation_[static_cast<std::size_t> (stageIndex (stage))];
}

bool EnvelopeCurve::isBound() const
{
    return std::all_of (knobs_.begin(), knobs_.end(), [] (const juce::Slider* knob) { return knob != nullptr; });
}

double EnvelopeCurve::stageValue (const EnvelopeStage stage) const
{
    auto* knob = stageKnob (stage);

    return knob != nullptr ? knob->getValue() : 0.0;
}

juce::String EnvelopeCurve::stageText (const EnvelopeStage stage) const
{
    auto* knob = stageKnob (stage);

    // `getTextFromValue` is the knob's own readout, not a second formatting of the parameter, so the
    // legend and the four controls print the same string.
    return knob != nullptr ? knob->getTextFromValue (knob->getValue()) : juce::String();
}

double EnvelopeCurve::stageProportion (const EnvelopeStage stage) const
{
    auto* knob = stageKnob (stage);

    return knob != nullptr ? knob->valueToProportionOfLength (knob->getValue()) : 0.0;
}

juce::Slider* EnvelopeCurve::knobFor (const EnvelopeStage stage) const
{
    return stageKnob (stage);
}

double EnvelopeCurve::modulationFor (const EnvelopeStage stage) const
{
    return stageModulation (stage);
}

bool EnvelopeCurve::isModulated() const
{
    return std::any_of (modulation_.begin(), modulation_.end(), [] (const double depth) { return depth != 0.0; });
}

juce::Rectangle<float> EnvelopeCurve::plotBounds() const
{
    auto area = getLocalBounds().toFloat();

    area.removeFromBottom (juce::jmin (kLegendHeight, area.getHeight() / 4.0f));

    return area.reduced (kCurveInset);
}

std::array<double, 4> EnvelopeCurve::stageWeights (const bool modulated) const
{
    // The axis' ceiling: the largest maximum among the three **time** knobs. Read from the knobs rather than
    // written down, so a table with a different range moves the axis with it; 1 s is a floor for a range
    // whose top is nought or below, which would otherwise divide by nought.
    auto ceiling = 1.0;

    for (const auto stage : { EnvelopeStage::Attack, EnvelopeStage::Decay, EnvelopeStage::Release })
        if (const auto* knob = knobFor (stage))
            ceiling = juce::jmax (ceiling, knob->getMaximum());

    const auto axis = std::log (1.0 + ceiling / kTimeFloorSeconds);

    const auto weightOfSeconds = [axis] (const double seconds)
    {
        return std::log (1.0 + juce::jmax (0.0, seconds) / kTimeFloorSeconds) / axis;
    };

    // A stage's **seconds**: the knob's own value for the first curve, and for the second the value at the
    // proportion the knob's modulation arc sweeps — the same `proportion + depth` the arc's angle comes
    // from, mapped back through the knob's own range so a skewed range is handled by the range and not here.
    const auto secondsOf = [this, modulated] (const EnvelopeStage stage)
    {
        auto* knob = knobFor (stage);

        if (knob == nullptr)
            return 0.0;

        const auto proportion = knob->valueToProportionOfLength (knob->getValue())
                                + (modulated ? modulationFor (stage) : 0.0);

        return knob->proportionOfLengthToValue (juce::jlimit (0.0, 1.0, proportion));
    };

    return { weightOfSeconds (secondsOf (EnvelopeStage::Attack)),
             weightOfSeconds (secondsOf (EnvelopeStage::Decay)),
             weightOfSeconds (kHoldSeconds),
             weightOfSeconds (secondsOf (EnvelopeStage::Release)) };
}

EnvelopeCurve::Geometry EnvelopeCurve::geometry (const bool modulated) const
{
    Geometry drawn;
    drawn.plot = plotBounds();

    if (drawn.plot.getWidth() < 2.0f || drawn.plot.getHeight() < 2.0f)
        return drawn;

    const auto weights = stageWeights (modulated);
    const auto total = weights[0] + weights[1] + weights[2] + weights[3];

    // The hold's weight is positive by construction, so this is a distribution and never a division by
    // nought — which is why the picture survives the knobs being at their minimum, where a proportion-based
    // axis would collapse.
    const auto unit = static_cast<float> (drawn.plot.getWidth() / total);

    const auto sustain = juce::jlimit (0.0, 1.0, stageProportion (EnvelopeStage::Sustain)
                                                    + (modulated ? modulationFor (EnvelopeStage::Sustain) : 0.0));
    const auto sustainY = drawn.plot.getBottom() - static_cast<float> (sustain) * drawn.plot.getHeight();

    drawn.start = { drawn.plot.getX(), drawn.plot.getBottom() };
    drawn.attackPeak = { drawn.start.x + unit * static_cast<float> (weights[0]), drawn.plot.getY() };
    drawn.decayEnd = { drawn.attackPeak.x + unit * static_cast<float> (weights[1]), sustainY };
    drawn.sustainEnd = { drawn.decayEnd.x + unit * static_cast<float> (weights[2]), sustainY };
    drawn.releaseEnd = { drawn.sustainEnd.x + unit * static_cast<float> (weights[3]), drawn.plot.getBottom() };

    return drawn;
}

void EnvelopeCurve::sliderValueChanged (juce::Slider*)
{
    // The knobs are the graph's only input, so a knob moving is the graph changing.
    repaint();
}

void EnvelopeCurve::paint (juce::Graphics& graphics)
{
    auto area = getLocalBounds().toFloat();

    if (area.getWidth() < 24.0f || area.getHeight() < 24.0f)
        return;

    // The display's own surface — the design's panel-in-a-panel, which is how a drawn surface inside a
    // panel is drawn everywhere in this interface: the window's `bg`, a `border` stroke, the panel radius.
    graphics.setColour (look::bg);
    graphics.fillRoundedRectangle (area, static_cast<float> (look::radiusPanel));
    graphics.setColour (look::border);
    graphics.drawRoundedRectangle (area.reduced (0.5f), static_cast<float> (look::radiusPanel), 1.0f);

    auto legend = area.removeFromBottom (juce::jmin (kLegendHeight, area.getHeight() / 4.0f)).reduced (1.0f, 0.0f);

    if (! isBound())
    {
        // Not a curve: four stages the graph cannot read are four parameters this display must not invent.
        // The frame above is the whole of what it can honestly say.
        return;
    }

    const auto curve = geometry (false);

    // The area under the envelope, in the accent's own 10 % wash (`accent-bg`, the design's active fill).
    {
        juce::Path filled;
        filled.startNewSubPath (curve.start);
        filled.lineTo (curve.attackPeak);
        filled.lineTo (curve.decayEnd);
        filled.lineTo (curve.sustainEnd);
        filled.lineTo (curve.releaseEnd);
        filled.lineTo (curve.releaseEnd.x, curve.plot.getBottom());
        filled.closeSubPath();

        graphics.setColour (look::accentBg);
        graphics.fillPath (filled);
    }

    // The stage boundaries, as the design's hairline grid: where attack ends, where decay ends and where the
    // hold ends. They are what makes the four stages countable in the picture.
    graphics.setColour (look::border);

    for (const auto x : { curve.attackPeak.x, curve.decayEnd.x, curve.sustainEnd.x })
        graphics.fillRect (juce::Rectangle<float> (x, curve.plot.getY(), 1.0f, curve.plot.getHeight()));

    const auto strokeOf = [&graphics] (const Geometry& drawn, const juce::Colour colour)
    {
        juce::Path path;
        path.startNewSubPath (drawn.start);
        path.lineTo (drawn.attackPeak);
        path.lineTo (drawn.decayEnd);
        path.lineTo (drawn.sustainEnd);
        path.lineTo (drawn.releaseEnd);

        graphics.setColour (colour);
        graphics.strokePath (path, juce::PathStrokeType (kCurveStroke, juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));
    };

    // **The second curve, when the matrix reaches a stage** — the same construction from the same number
    // shifted by the row's amount, drawn in `accentDim`, which is the colour the knob's modulation arc is.
    // This is "the graph shows what the arcs show": a row that moves the attack moves the first segment.
    if (isModulated())
        strokeOf (geometry (true), look::accentDim);

    strokeOf (curve, look::accent);

    // The corners, so the four stages read as four stages rather than as one polyline.
    graphics.setColour (look::accent);

    for (const auto& point : { curve.attackPeak, curve.decayEnd, curve.sustainEnd, curve.releaseEnd })
        graphics.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (point));

    // The legend: the four stages' own values, in the design's monospaced face — the same strings the four
    // knobs print, read from the same knobs, so the picture and the controls cannot disagree about a time
    // either. One cell per stage, so the numbers stay put as they change.
    auto cell = legend.getWidth() / static_cast<float> (kStages.size());

    for (const auto stage : kStages)
    {
        auto item = legend.removeFromLeft (cell);
        const auto letter = juce::String (stageLetter (stage));

        graphics.setFont (look::monoFont (look::type::readoutSm));
        graphics.setColour (look::fgMuted);
        graphics.drawText (letter, item.removeFromLeft (10.0f).toNearestInt(),
                           juce::Justification::centredLeft, false);

        graphics.setColour (look::fg);
        graphics.drawText (stageText (stage), item.toNearestInt(), juce::Justification::centredLeft, true);
    }
}

} // namespace xerum::plugin::ui
