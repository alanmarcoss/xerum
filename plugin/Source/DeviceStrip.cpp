// xerum::plugin — the device strip, in code. See DeviceStrip.h.

#include "DeviceStrip.h"

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
DeviceTab::DeviceTab()
    : juce::Button ({})
{
    setClickingTogglesState (false);
}

void DeviceTab::setDevice (juce::String title, juce::String module)
{
    title_ = std::move (title);
    module_ = std::move (module);
    repaint();
}

void DeviceTab::setSeverity (const bool has, const model::Severity severity)
{
    hasSeverity_ = has;
    severity_ = severity;
    repaint();
}

void DeviceTab::setSelected (const bool selected)
{
    selected_ = selected;
    repaint();
}

void DeviceTab::paintButton (juce::Graphics& graphics, const bool shouldDrawButtonAsHighlighted,
                             const bool shouldDrawButtonAsDown)
{
    // The design's tab: a pill, `fg-dim` when inactive and the accent with an `accent-bg` fill when
    // active. The module the device is stays on the pill in the muted colour — a device strip that did not
    // say which model module a page is would make the report's own module paths unreadable.
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
        // The dot says *this device's module* reported something, which is what makes the strip a map of
        // the import rather than a second copy of it. Its colour is the severity ramp, one function.
        auto dotArea = content.removeFromRight (kDotSize + 5.0f);
        graphics.setColour (look::severityColour (severity_));
        graphics.fillEllipse (
            juce::Rectangle<float> (kDotSize, kDotSize).withCentre (dotArea.getCentre()));
        content.removeFromRight (4.0f);
    }

    const auto moduleFont = look::monoFont (look::type::tag);
    const auto moduleWidth = juce::jmin (content.getWidth() * 0.5f,
                                         juce::GlyphArrangement::getStringWidth (moduleFont, module_));
    auto moduleArea = content.removeFromRight (moduleWidth);

    graphics.setColour (look::fgMuted);
    graphics.setFont (moduleFont);
    graphics.drawText (module_, moduleArea.toNearestInt(), juce::Justification::centredRight, true);

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
DeviceStrip::DeviceStrip()
{
    setInterceptsMouseClicks (false, true);
}

void DeviceStrip::setDevices (std::vector<juce::String> titles, std::vector<juce::String> modules)
{
    tabs_.clear();

    const auto count = std::min (titles.size(), modules.size());

    for (std::size_t i = 0; i < count; ++i)
    {
        auto* tab = tabs_.add (new DeviceTab());
        tab->setDevice (titles[i], modules[i]);
        tab->onClick = [this, index = static_cast<int> (i)]
        {
            if (onSelect)
                onSelect (index);
        };

        addAndMakeVisible (*tab);
    }

    selected_ = count == 0 ? 0 : std::min (selected_, static_cast<int> (count) - 1);
    resized();
}

juce::String DeviceStrip::deviceTitle (const int index) const
{
    return index >= 0 && index < deviceCount() ? tabs_[index]->deviceTitle() : juce::String();
}

juce::String DeviceStrip::deviceModule (const int index) const
{
    return index >= 0 && index < deviceCount() ? tabs_[index]->moduleName() : juce::String();
}

void DeviceStrip::setSelected (const int index)
{
    if (index < 0 || index >= deviceCount())
        return;

    selected_ = index;

    for (int i = 0; i < deviceCount(); ++i)
        tabs_[i]->setSelected (i == index);

    repaint();
}

void DeviceStrip::setSeverity (const int index, const bool has, const model::Severity severity)
{
    if (index >= 0 && index < deviceCount())
        tabs_[index]->setSeverity (has, severity);
}

void DeviceStrip::clearSeverity()
{
    for (auto* tab : tabs_)
        tab->setSeverity (false, model::Severity::Info);
}

bool DeviceStrip::hasDeviceSeverity (const int index) const
{
    return index >= 0 && index < deviceCount() && tabs_[index]->hasSeverity();
}

model::Severity DeviceStrip::deviceSeverity (const int index) const
{
    return index >= 0 && index < deviceCount() ? tabs_[index]->severity() : model::Severity::Info;
}

juce::Rectangle<int> DeviceStrip::tabBounds (const int index) const
{
    return index >= 0 && index < deviceCount() ? tabs_[index]->getBounds() : juce::Rectangle<int>();
}

void DeviceStrip::resized()
{
    const auto count = deviceCount();

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

void DeviceStrip::paint (juce::Graphics& graphics)
{
    // A tab bar, not a second panel: the surface colour, closed by the border underneath.
    auto area = getLocalBounds();

    graphics.setColour (look::surface);
    graphics.fillRect (area);
    graphics.setColour (look::border);
    graphics.fillRect (area.removeFromBottom (1));
}

} // namespace xerum::plugin::ui
