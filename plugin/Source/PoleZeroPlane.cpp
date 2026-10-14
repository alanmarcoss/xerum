// xerum::plugin — the drawn filter, in code. See PoleZeroPlane.h.

#include "PoleZeroPlane.h"

#include "InterfaceLook.h"
#include "InterfacePlan.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace xerum::plugin::ui
{

namespace
{
constexpr int kPlaneCaptionHeight = 34;   ///< the caption and the note under the plane
constexpr float kMarkerHalfHeight = 0.30f; ///< how far a marker sits from the frequency axis

/// The plane's drawing area inside a component of this size: below the title, above the caption, with a
/// margin so a marker at frequency 0 or 1 is still fully visible. `paint` and `polePoint`/`zeroPoint` both
/// come through here, so what a test reads back is where the marker is drawn.
juce::Rectangle<float> planeAreaFor (const juce::Rectangle<int> bounds)
{
    return bounds.withTrimmedTop (kPanelHeaderHeight)
        .withTrimmedBottom (kPlaneCaptionHeight)
        .toFloat()
        .reduced (16.0f, 14.0f);
}

float xForFrequency (const juce::Rectangle<float> area, const double fraction)
{
    const auto clamped = std::clamp (fraction, 0.0, 1.0);
    return area.getX() + static_cast<float> (clamped) * area.getWidth();
}

float radiusForQ (const double q)
{
    return 2.0f + static_cast<float> (std::clamp (q, 0.0, 1.0)) * 6.0f;
}
} // namespace

//==============================================================================
PoleZeroPlane::PoleZeroPlane()
{
    // The plane is the editor now, so it takes the mouse — but only acts when `editable_`, which keeps a
    // test's plane a pure view.
    setInterceptsMouseClicks (true, false);
}

void PoleZeroPlane::setFilter (const int filterSlot, juce::String typeName)
{
    filterSlot_ = filterSlot;
    type_ = std::move (typeName);
    repaint();
}

void PoleZeroPlane::setSections (dsp::PoleZeroDescription description)
{
    sections_ = description;
    selected_ = -1;
    repaint();
}

void PoleZeroPlane::setEditable (const bool editable)
{
    editable_ = editable;
}

juce::Point<float> PoleZeroPlane::pointFor (const int section, const bool pole) const
{
    return pole ? polePoint (section) : zeroPoint (section);
}

int PoleZeroPlane::markerAt (const juce::Point<float> point) const
{
    // A pole marker is section * 2, a zero is section * 2 + 1, so the result is stable and a caller can
    // recover both from it. The grab radius is the marker's own size plus a margin, so a small marker can
    // still be caught.
    auto best = -1;
    auto bestDistance = std::numeric_limits<float>::max();

    for (int section = 0; section < sectionCount(); ++section)
    {
        const auto& s = sections_.sections[static_cast<std::size_t> (section)];

        for (int side = 0; side < 2; ++side)
        {
            const auto pole = side == 0;
            const auto centre = pointFor (section, pole);
            const auto radius = radiusForQ (pole ? s.poleQ : s.zeroQ) + 6.0f;
            const auto distance = centre.getDistanceFrom (point);

            if (distance <= radius && distance < bestDistance)
            {
                bestDistance = distance;
                best = section * 2 + side;
            }
        }
    }

    return best;
}

void PoleZeroPlane::mouseDown (const juce::MouseEvent& event)
{
    if (! editable_)
        return;

    const auto marker = markerAt (event.position);

    if (marker < 0)
    {
        selected_ = -1;
        dragSection_ = -1;
        repaint();
        return;
    }

    dragSection_ = marker / 2;
    dragPole_ = marker % 2 == 0;
    dragStartY_ = event.position.y;
    dragStartQ_ = dragPole_ ? sections_.sections[static_cast<std::size_t> (dragSection_)].poleQ
                            : sections_.sections[static_cast<std::size_t> (dragSection_)].zeroQ;
    selected_ = dragSection_;
    repaint();
}

void PoleZeroPlane::mouseDrag (const juce::MouseEvent& event)
{
    if (! editable_ || dragSection_ < 0 || dragSection_ >= sectionCount())
        return;

    const auto area = planeAreaFor (getLocalBounds());
    auto& s = sections_.sections[static_cast<std::size_t> (dragSection_)];

    // Horizontal: the frequency, as a fraction of Nyquist, clamped to the axis the plane draws.
    const auto frequency = juce::jlimit (0.0, 1.0, static_cast<double> ((event.position.x - area.getX())
                                                                       / area.getWidth()));

    // Vertical: the Q. The marker's own y is fixed by whether it is a pole or a zero, so the drag's
    // vertical distance is free to shape the marker's size instead — up increases it.
    const auto q = juce::jlimit (0.0, 1.0, dragStartQ_ + (static_cast<double> (dragStartY_) - event.position.y)
                                                     / 120.0);

    if (dragPole_)
    {
        s.poleFrequency = frequency;
        s.poleQ = q;
    }
    else
    {
        s.zeroFrequency = frequency;
        s.zeroQ = q;
    }

    repaint();

    if (onSectionChanged)
        onSectionChanged (dragSection_, s);
}

juce::String PoleZeroPlane::caption() const
{
    if (sectionCount() == 0)
        return "no pole/zero sections in this preset";

    return juce::String (sectionCount()) + " of " + juce::String (sectionCapacity()) + " sections drawn";
}

juce::Point<float> PoleZeroPlane::polePoint (const int section) const
{
    if (section < 0 || section >= sectionCount())
        return {};

    const auto area = planeAreaFor (getLocalBounds());
    const auto& s = sections_.sections[static_cast<std::size_t> (section)];

    return { xForFrequency (area, s.poleFrequency), area.getCentreY() - area.getHeight() * kMarkerHalfHeight };
}

juce::Point<float> PoleZeroPlane::zeroPoint (const int section) const
{
    if (section < 0 || section >= sectionCount())
        return {};

    const auto area = planeAreaFor (getLocalBounds());
    const auto& s = sections_.sections[static_cast<std::size_t> (section)];

    return { xForFrequency (area, s.zeroFrequency), area.getCentreY() + area.getHeight() * kMarkerHalfHeight };
}

void PoleZeroPlane::paint (juce::Graphics& graphics)
{
    auto bounds = getLocalBounds();

    // The design's "Filter Display": the plane is a display, so its background is the window's own `bg`
    // closed by a 3px-cornered `border`, not a second panel.
    graphics.setColour (look::bg);
    graphics.fillRoundedRectangle (bounds.toFloat(), 3.0f);
    graphics.setColour (look::border);
    graphics.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), 3.0f, 1.0f);

    auto title = bounds.removeFromTop (kPanelHeaderHeight);
    graphics.setColour (look::fg);
    graphics.setFont (look::bodyFont (look::type::section, true));
    graphics.drawText ("POLE / ZERO PLANE", title.reduced (kPanelPadding + 2, 0),
                       juce::Justification::centredLeft, true);

    graphics.setColour (look::accent);
    graphics.setFont (look::monoFont (look::type::tag));
    graphics.drawText (type_.isEmpty() ? juce::String ("filter type not set") : type_,
                       title.reduced (kPanelPadding + 2, 0), juce::Justification::centredRight, true);

    const auto area = planeAreaFor (getLocalBounds());
    const auto axis = area.getCentreY();

    // The frequency axis and its own grid: the preset's values snap to a 1/16 grid often enough that a
    // grid is what makes a section's position readable rather than merely plotted.
    for (int i = 1; i < 8; ++i)
    {
        const auto line = area.getX() + area.getWidth() * static_cast<float> (i) / 8.0f;
        graphics.setColour (look::border);
        graphics.fillRect (juce::Rectangle<float> (line, area.getY(), 1.0f, area.getHeight()));
    }

    // The quarter points are labelled, because the axis is a fraction of Nyquist and "where is 0.5" should
    // not need a ruler. The design's own display labels its axis the same way.
    graphics.setColour (look::fgMuted);
    graphics.setFont (look::monoFont (6.0f));

    for (int i = 0; i <= 4; ++i)
    {
        const auto fraction = i / 4.0f;
        const auto x = area.getX() + area.getWidth() * fraction;
        const auto text = i == 0 ? juce::String ("0") : juce::String (fraction, 2);

        graphics.drawText (text,
                           juce::Rectangle<int> (static_cast<int> (x) - 12,
                                                 static_cast<int> (area.getBottom()) - 11, 40, 10),
                           juce::Justification::centredLeft, false);
    }

    graphics.setColour (look::border);
    graphics.fillRect (juce::Rectangle<float> (area.getX(), axis, area.getWidth(), 1.0f));

    graphics.setColour (look::fgDim);
    graphics.setFont (look::monoFont (look::type::readoutSm));
    graphics.drawText ("poles", juce::Rectangle<int> (static_cast<int> (area.getX()),
                                                      static_cast<int> (area.getY()), 60, 12),
                       juce::Justification::centredLeft, false);
    graphics.drawText ("zeros", juce::Rectangle<int> (static_cast<int> (area.getX()),
                                                      static_cast<int> (area.getBottom()) - 25, 60, 12),
                       juce::Justification::centredLeft, false);

    for (int section = 0; section < sectionCount(); ++section)
    {
        const auto& s = sections_.sections[static_cast<std::size_t> (section)];
        const auto pole = polePoint (section);
        const auto zero = zeroPoint (section);
        const auto poleRadius = radiusForQ (s.poleQ);
        const auto zeroRadius = radiusForQ (s.zeroQ);

        // The two frequencies of one section are tied together, so a section reads as one object rather
        // than as two unrelated marks.
        graphics.setColour (look::border);
        graphics.drawLine (pole.x, pole.y, zero.x, zero.y, 1.0f);

        // A pole is a cross and a zero is a circle: the convention every pole/zero plot uses, and the
        // reason the two are not both drawn as the prototype's identical crossed circles. The pole takes
        // the accent, the zero the accent's dimmed partner — the same "primary / secondary" pair the
        // design system uses for a value and its modulation, because that is what the two are.
        graphics.setColour (look::accent);
        graphics.drawEllipse (pole.x - poleRadius, pole.y - poleRadius, poleRadius * 2.0f, poleRadius * 2.0f,
                              1.4f);
        graphics.drawLine (pole.x - poleRadius, pole.y, pole.x + poleRadius, pole.y, 1.4f);
        graphics.drawLine (pole.x, pole.y - poleRadius, pole.x, pole.y + poleRadius, 1.4f);

        graphics.setColour (look::accentDim);
        graphics.drawEllipse (zero.x - zeroRadius, zero.y - zeroRadius, zeroRadius * 2.0f, zeroRadius * 2.0f,
                              1.4f);

        // The section a drag would move is ringed, so "which one am I editing" is on screen rather than
        // inferred from the pointer.
        if (section == selected_)
        {
            graphics.setColour (look::fg);
            graphics.drawEllipse (pole.x - poleRadius - 3.0f, pole.y - poleRadius - 3.0f,
                                  (poleRadius + 3.0f) * 2.0f, (poleRadius + 3.0f) * 2.0f, 1.0f);
            graphics.drawEllipse (zero.x - zeroRadius - 3.0f, zero.y - zeroRadius - 3.0f,
                                  (zeroRadius + 3.0f) * 2.0f, (zeroRadius + 3.0f) * 2.0f, 1.0f);
        }
    }

    auto footer = bounds.removeFromBottom (kPlaneCaptionHeight).reduced (kPanelPadding, 0);
    graphics.setColour (look::fg);
    graphics.setFont (look::bodyFont (11.5f));
    graphics.drawText (caption(), footer.removeFromTop (16), juce::Justification::centredLeft, true);

    graphics.setColour (look::fgDim);
    graphics.setFont (look::bodyFont (10.5f));
    graphics.drawFittedText (
        editable_ ? "drag a pole or zero to move its frequency, and up or down to change its Q"
                  : "the preset draws this response; the engine plays it through its pole/zero model",
        footer, juce::Justification::centredLeft, 2);
}

} // namespace xerum::plugin::ui
