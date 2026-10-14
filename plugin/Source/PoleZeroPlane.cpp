// xerum::plugin — the drawn filter, in code. See PoleZeroPlane.h.

#include "PoleZeroPlane.h"

#include "InterfaceLook.h"
#include "InterfacePlan.h"

#include <algorithm>

namespace xerum::plugin::ui
{

namespace
{
using Section = dsp::PoleZeroDescription;

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

dsp::PoleZeroDescription poleZeroFromDocument (const model::PresetDocument& document, const int filterSlot)
{
    dsp::PoleZeroDescription description;

    const auto slot = "VoiceFilter" + std::to_string (filterSlot);

    for (std::size_t object = 0; object < Section::kMaxObjects; ++object)
    {
        const auto prefix = slot + "/PZs[" + std::to_string (object) + "]";

        // `fp[0]` is the object's presence: the reference writes all eight sections of an object or none
        // of it, so a partial object is not a case this has to read.
        if (document.parameters.find (prefix + "/fp[0]") == document.parameters.end())
            break;

        double poleFrequency[Section::kSectionsPerObject] {};
        double zeroFrequency[Section::kSectionsPerObject] {};
        double poleQ[Section::kSectionsPerObject] {};
        double zeroQ[Section::kSectionsPerObject] {};
        double gain[Section::kSectionsPerObject] {};
        int type[Section::kSectionsPerObject] {};

        const std::pair<std::string_view, double*> numericFields[] = {
            { "fp", poleFrequency }, { "fz", zeroFrequency }, { "qp", poleQ }, { "qz", zeroQ },
            { "gain", gain },
        };

        for (const auto& [field, out] : numericFields)
            for (std::size_t i = 0; i < Section::kSectionsPerObject; ++i)
                if (const auto found = document.parameters.find (prefix + "/" + std::string (field) + "["
                                                                 + std::to_string (i) + "]");
                    found != document.parameters.end())
                    found->second.numberAsDouble (out[i]);

        for (std::size_t i = 0; i < Section::kSectionsPerObject; ++i)
            if (const auto found = document.parameters.find (prefix + "/types[" + std::to_string (i) + "]");
                found != document.parameters.end() && found->second.type == model::Value::Type::Int)
                type[i] = static_cast<int> (found->second.integer);

        dsp::appendPoleZeroObject (description, poleFrequency, zeroFrequency, poleQ, zeroQ, gain, type);
    }

    return description;
}

//==============================================================================
PoleZeroPlane::PoleZeroPlane()
{
    setInterceptsMouseClicks (false, false);
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
    repaint();
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

    graphics.setColour (look::panelFill);
    graphics.fillRoundedRectangle (bounds.toFloat(), 5.0f);
    graphics.setColour (look::panelBorder);
    graphics.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), 5.0f, 1.0f);

    auto title = bounds.removeFromTop (kPanelHeaderHeight);
    graphics.setColour (look::text);
    graphics.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    graphics.drawText ("Pole / zero plane", title.reduced (kPanelPadding + 4, 0),
                       juce::Justification::centredLeft, true);

    graphics.setColour (look::dimText);
    graphics.setFont (juce::FontOptions (11.0f));
    graphics.drawText (type_.isEmpty() ? juce::String ("filter type not set") : type_,
                       title.reduced (kPanelPadding + 4, 0), juce::Justification::centredRight, true);

    const auto area = planeAreaFor (getLocalBounds());
    const auto axis = area.getCentreY();

    // The frequency axis and its own grid: the preset's values snap to a 1/16 grid often enough that a
    // grid is what makes a section's position readable rather than merely plotted.
    for (int i = 1; i < 8; ++i)
    {
        const auto line = area.getX() + area.getWidth() * static_cast<float> (i) / 8.0f;
        graphics.setColour (look::rowDivider);
        graphics.fillRect (juce::Rectangle<float> (line, area.getY(), 1.0f, area.getHeight()));
    }

    graphics.setColour (look::panelBorder);
    graphics.fillRect (juce::Rectangle<float> (area.getX(), axis, area.getWidth(), 1.0f));

    graphics.setColour (look::dimText);
    graphics.setFont (juce::FontOptions (9.5f));
    graphics.drawText ("poles", juce::Rectangle<int> (static_cast<int> (area.getX()),
                                                      static_cast<int> (area.getY()), 60, 12),
                       juce::Justification::centredLeft, false);
    graphics.drawText ("zeros", juce::Rectangle<int> (static_cast<int> (area.getX()),
                                                      static_cast<int> (area.getBottom()) - 12, 60, 12),
                       juce::Justification::centredLeft, false);
    graphics.drawText ("freq: 0 … 1 of Nyquist", area.toNearestInt(), juce::Justification::bottomRight, false);

    for (int section = 0; section < sectionCount(); ++section)
    {
        const auto& s = sections_.sections[static_cast<std::size_t> (section)];
        const auto pole = polePoint (section);
        const auto zero = zeroPoint (section);
        const auto poleRadius = radiusForQ (s.poleQ);
        const auto zeroRadius = radiusForQ (s.zeroQ);

        // The two frequencies of one section are tied together, so a section reads as one object rather
        // than as two unrelated marks.
        graphics.setColour (look::rowDivider);
        graphics.drawLine (pole.x, pole.y, zero.x, zero.y, 1.0f);

        // A pole is a cross and a zero is a circle: the convention every pole/zero plot uses, and the
        // reason the two are not both drawn as the prototype's identical crossed circles.
        graphics.setColour (look::accent);
        graphics.drawEllipse (pole.x - poleRadius, pole.y - poleRadius, poleRadius * 2.0f, poleRadius * 2.0f,
                              1.4f);
        graphics.drawLine (pole.x - poleRadius, pole.y, pole.x + poleRadius, pole.y, 1.4f);
        graphics.drawLine (pole.x, pole.y - poleRadius, pole.x, pole.y + poleRadius, 1.4f);

        graphics.setColour (look::severityInfo);
        graphics.drawEllipse (zero.x - zeroRadius, zero.y - zeroRadius, zeroRadius * 2.0f, zeroRadius * 2.0f,
                              1.4f);
    }

    auto footer = bounds.removeFromBottom (kPlaneCaptionHeight).reduced (kPanelPadding, 0);
    graphics.setColour (look::text);
    graphics.setFont (juce::FontOptions (11.5f));
    graphics.drawText (caption(), footer.removeFromTop (16), juce::Justification::centredLeft, true);

    graphics.setColour (look::dimText);
    graphics.setFont (juce::FontOptions (10.5f));
    graphics.drawFittedText (
        "the preset draws this response; xerum's engine takes a filter model and no pole/zero table, so "
        "the response is shown and the filter plays its type",
        footer, juce::Justification::centredLeft, 2);
}

} // namespace xerum::plugin::ui
