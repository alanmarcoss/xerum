// xerum::plugin — the composition's strip, in code. See SectionStrip.h.

#include "SectionStrip.h"

#include "InterfaceLook.h"

#include <algorithm>

namespace xerum::plugin::ui
{

namespace
{
constexpr int kTabGap = 4;
constexpr int kTabMaxWidth = 200;
constexpr int kStripHeight = 30;
constexpr int kDotSize = 6;
constexpr int kPillHeight = 24;
} // namespace

//==============================================================================
SectionTab::SectionTab()
    : juce::Button ({})
{
    setClickingTogglesState (false);
}

void SectionTab::setSection (juce::String title)
{
    title_ = std::move (title);
    repaint();
}

void SectionTab::setSeverity (const bool has, const model::Severity severity)
{
    hasSeverity_ = has;
    severity_ = severity;
    repaint();
}

void SectionTab::setSelected (const bool selected)
{
    selected_ = selected;
    repaint();
}

void SectionTab::paintButton (juce::Graphics& graphics, const bool shouldDrawButtonAsHighlighted,
                              const bool shouldDrawButtonAsDown)
{
    // The design's tab: a pill, `fg-dim` when inactive and the accent with an `accent-bg` fill when
    // active.
    const auto pill = getLocalBounds().toFloat()
                          .withSizeKeepingCentre (getWidth(), static_cast<float> (kPillHeight))
                          .reduced (1.0f, 0.0f);
    const auto radius = pill.getHeight() * 0.5f;
    const juce::Graphics::ScopedSaveState save (graphics);

    if (selected_)
    {
        graphics.setColour (look::accentBg);
        graphics.fillRoundedRectangle (pill, radius);
    }
    else if (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
    {
        graphics.setColour (look::surface2);
        graphics.fillRoundedRectangle (pill, radius);
    }

    auto content = pill.reduced (10.0f, 0.0f);

    if (hasSeverity_)
    {
        // The dot says *this section's modules* reported something, which is what makes the strip a map of
        // the import rather than a second copy of it. Its colour is the severity ramp, one function.
        auto dotArea = content.removeFromRight (kDotSize + 5.0f);
        graphics.setColour (look::severityColour (severity_));
        graphics.fillEllipse (
            juce::Rectangle<float> (kDotSize, kDotSize).withCentre (dotArea.getCentre()));
        content.removeFromRight (4.0f);
    }

    graphics.setColour (selected_ ? look::accent : (shouldDrawButtonAsHighlighted ? look::fg : look::fgDim));
    graphics.setFont (look::bodyFont (look::type::section, true));
    graphics.drawText (title_.toUpperCase(), content.toNearestInt(), juce::Justification::centredLeft, true);

    if (hasKeyboardFocus (true))
    {
        graphics.setColour (look::accent);
        graphics.drawRoundedRectangle (pill.reduced (0.5f), radius, look::kFocusStroke);
    }
}

//==============================================================================
SectionStrip::SectionStrip()
{
    setInterceptsMouseClicks (false, true);
}

void SectionStrip::setSections (std::vector<juce::String> titles)
{
    tabs_.clear();

    for (std::size_t i = 0; i < titles.size(); ++i)
    {
        auto* tab = tabs_.add (new SectionTab());
        tab->setSection (titles[i]);
        tab->onClick = [this, index = static_cast<int> (i)]
        {
            if (onSelect)
                onSelect (index);
        };

        addAndMakeVisible (*tab);
    }

    selected_ = titles.empty() ? 0 : std::min (selected_, static_cast<int> (titles.size()) - 1);
    resized();
}

juce::String SectionStrip::sectionTitle (const int index) const
{
    return index >= 0 && index < sectionCount() ? tabs_[index]->sectionTitle() : juce::String();
}

void SectionStrip::setSelected (const int index)
{
    if (index < 0 || index >= sectionCount())
        return;

    selected_ = index;

    for (int i = 0; i < sectionCount(); ++i)
        tabs_[i]->setSelected (i == index);

    repaint();
}

void SectionStrip::setSeverity (const int index, const bool has, const model::Severity severity)
{
    if (index >= 0 && index < sectionCount())
        tabs_[index]->setSeverity (has, severity);
}

void SectionStrip::clearSeverity()
{
    for (auto* tab : tabs_)
        tab->setSeverity (false, model::Severity::Info);
}

bool SectionStrip::hasSectionSeverity (const int index) const
{
    return index >= 0 && index < sectionCount() && tabs_[index]->hasSeverity();
}

model::Severity SectionStrip::sectionSeverity (const int index) const
{
    return index >= 0 && index < sectionCount() ? tabs_[index]->severity() : model::Severity::Info;
}

juce::Rectangle<int> SectionStrip::tabBounds (const int index) const
{
    return index >= 0 && index < sectionCount() ? tabs_[index]->getBounds() : juce::Rectangle<int>();
}

void SectionStrip::resized()
{
    const auto count = sectionCount();

    if (count == 0)
        return;

    const auto available = getWidth() - kTabGap * (count - 1);
    const auto width = std::min (kTabMaxWidth, available / count);

    int x = 0;

    for (auto* tab : tabs_)
    {
        tab->setBounds (x, 0, width, kStripHeight);
        x += width + kTabGap;
    }
}

void SectionStrip::paint (juce::Graphics& graphics)
{
    // A tab bar, not a second panel: the surface colour, closed by the border underneath.
    auto area = getLocalBounds();

    graphics.setColour (look::surface);
    graphics.fillRect (area);
    graphics.setColour (look::border);
    graphics.fillRect (area.removeFromBottom (1));
}

} // namespace xerum::plugin::ui
