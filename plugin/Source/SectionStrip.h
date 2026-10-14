// xerum::plugin — the composition's strip.
//
// Ticket `44` built this as a **device strip**: one tab per device page, every device present, with the
// severity dot of the model module the tab is. Ticket `52` made the editor one **composed page** in the
// design's arrangement — the composition's sections are no longer pages, they are bands of the one page —
// so the strip now names the **sections**, and a click scrolls the created page to that section. The class
// is renamed with the thing it names: a tab names a section of the composed page.
//
// A tab carries two things: the section's title (xerum's own words for the band — `Oscillators`,
// `Filter`, `Mod Matrix`) and a severity dot when the import report named something inside one of the
// section's model modules, so a half-loaded preset is located where it happened instead of only collected
// in the drawer. The module names themselves left the tab in ticket `52` — a section holds *several*
// modules (`Filter` is `VoiceFilter0` and `VoiceFilter1`) and the design's tabs are labels — and the plan
// still exposes them, which is where the evidence reads them.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "document.h"

#include <functional>
#include <string>
#include <vector>

namespace xerum::plugin::ui
{

/// One section tab. Drawn rather than themed: a `TextButton` with a look-and-feel tweak cannot carry a
/// severity dot, and the dot is the reason the strip exists rather than a menu.
class SectionTab final : public juce::Button
{
public:
    SectionTab();

    void setSection (juce::String title);
    const juce::String& sectionTitle() const { return title_; }

    void setSeverity (bool has, model::Severity severity);
    void setSelected (bool selected);

    /// The dot's state, so that "the strip says where it went wrong" is a measurement rather than a
    /// claim about a paint routine.
    bool hasSeverity() const { return hasSeverity_; }
    model::Severity severity() const { return severity_; }

    void paintButton (juce::Graphics& graphics, bool shouldDrawButtonAsHighlighted,
                      bool shouldDrawButtonAsDown) override;

private:
    juce::String title_;
    bool hasSeverity_ { false };
    model::Severity severity_ { model::Severity::Info };
    bool selected_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionTab)
};

/// The strip: one tab per section of the composition, all of them present, always.
class SectionStrip final : public juce::Component
{
public:
    SectionStrip();

    void setSections (std::vector<juce::String> titles);

    int sectionCount() const { return tabs_.size(); }
    juce::String sectionTitle (int index) const;

    void setSelected (int index);
    int selected() const { return selected_; }

    void setSeverity (int index, bool has, model::Severity severity);
    void clearSeverity();

    /// What a tab's dot says, per section.
    bool hasSectionSeverity (int index) const;
    model::Severity sectionSeverity (int index) const;

    /// What a click on a tab does. The owner scrolls to the section; the strip only reports which tab.
    std::function<void (int)> onSelect;

    /// A tab's bounds — what a click aims at, and what a test reads to prove the strip is laid out.
    juce::Rectangle<int> tabBounds (int index) const;

    void resized() override;
    void paint (juce::Graphics& graphics) override;

private:
    juce::OwnedArray<SectionTab> tabs_;
    int selected_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionStrip)
};

} // namespace xerum::plugin::ui
