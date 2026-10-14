// xerum::plugin — the device strip.
//
// Ticket `44`, and the half of variant 3 that makes one-device-per-page navigable rather than a maze:
// **the strip always shows every device** — the one you are looking at and all the others. The
// alternative the ticket rejected is a page that hides its siblings behind a menu.
//
// A tab carries three things: the device's name (xerum's own group name for a parameter page, "Matrix"
// for the matrix), the model module it is (`Oscillator0`, `VoiceFilter1`, `ModSlot`), and a severity dot
// when the import report named something inside that module — so a half-loaded preset is located where it
// happened instead of only collected in the drawer.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "document.h"

#include <functional>
#include <string>
#include <vector>

namespace xerum::plugin::ui
{

/// One device tab. Drawn rather than themed: a `TextButton` with a look-and-feel tweak cannot carry a
/// severity dot, and the dot is the reason the strip exists rather than a menu.
class DeviceTab final : public juce::Button
{
public:
    DeviceTab();

    void setDevice (juce::String title, juce::String module);
    const juce::String& moduleName() const { return module_; }
    const juce::String& deviceTitle() const { return title_; }

    void setSeverity (bool has, model::Severity severity);
    void setSelected (bool selected);

    /// The dot's state, so that "the strip says where it went wrong" is a measurement rather than a
    /// claim about a paint routine.
    bool hasSeverity() const { return hasSeverity_; }
    model::Severity severity() const { return severity_; }

    void paintButton (juce::Graphics& graphics, bool shouldDrawButtonAsHighlighted,
                      bool shouldDrawButtonAsDown) override;

private:
    juce::String title_, module_;
    bool hasSeverity_ { false };
    model::Severity severity_ { model::Severity::Info };
    bool selected_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceTab)
};

/// The strip: one tab per device, all of them present, always.
class DeviceStrip final : public juce::Component
{
public:
    DeviceStrip();

    void setDevices (std::vector<juce::String> titles, std::vector<juce::String> modules);

    int deviceCount() const { return tabs_.size(); }
    juce::String deviceTitle (int index) const;
    juce::String deviceModule (int index) const;

    void setSelected (int index);
    int selected() const { return selected_; }

    void setSeverity (int index, bool has, model::Severity severity);
    void clearSeverity();

    /// What a tab's dot says, per device.
    bool hasDeviceSeverity (int index) const;
    model::Severity deviceSeverity (int index) const;

    /// What a click on a tab does. The owner switches the page; the strip only reports which tab.
    std::function<void (int)> onSelect;

    /// A tab's bounds — what a click aims at, and what a test reads to prove the strip is laid out.
    juce::Rectangle<int> tabBounds (int index) const;

    void resized() override;
    void paint (juce::Graphics& graphics) override;

private:
    juce::OwnedArray<DeviceTab> tabs_;
    int selected_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceStrip)
};

} // namespace xerum::plugin::ui
