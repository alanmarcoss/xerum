// xerum::plugin — the device strip, in code. See DeviceStrip.h.

#include "DeviceStrip.h"

#include "InterfaceLook.h"

#include <algorithm>

namespace xerum::plugin::ui
{

namespace
{
constexpr int kTabGap = 6;
constexpr int kTabMaxWidth = 200;
constexpr int kStripHeight = 42;
constexpr int kDotSize = 7;
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
    auto area = getLocalBounds().toFloat();

    if (selected_)
    {
        graphics.setColour (look::selectedTab);
        graphics.fillRoundedRectangle (area.withTrimmedBottom (2.0f), 4.0f);
        graphics.setColour (look::accent);
        graphics.fillRect (area.withTop (area.getBottom() - 2.0f));
    }
    else if (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
    {
        graphics.setColour (look::panelFill);
        graphics.fillRoundedRectangle (area.withTrimmedBottom (2.0f), 4.0f);
    }

    auto text = area.toNearestInt().reduced (7, 4);
    text.removeFromTop (2);

    graphics.setColour (selected_ ? look::text : look::dimText);
    graphics.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    graphics.drawText (title_, text.removeFromTop (15), juce::Justification::centredLeft, true);

    graphics.setColour (look::dimText);
    graphics.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.5f,
                                                      juce::Font::plain)));
    graphics.drawText (module_, text.removeFromTop (11), juce::Justification::centredLeft, true);

    if (hasSeverity_)
    {
        // The dot says *this device's module* reported something, which is what makes the strip a map of
        // the import rather than a second copy of it. Its colour is the severity ramp, one function.
        const auto dot = juce::Rectangle<float> (area.getRight() - kDotSize - 5.0f, area.getY() + 5.0f,
                                                 kDotSize, kDotSize);
        graphics.setColour (look::severityColour (severity_));
        graphics.fillEllipse (dot);
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
    auto area = getLocalBounds();

    graphics.setColour (look::barFill);
    graphics.fillRect (area);
    graphics.setColour (look::panelBorder);
    graphics.fillRect (area.removeFromBottom (1));
}

} // namespace xerum::plugin::ui
