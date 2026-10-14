// xerum::plugin — the drawn filter, as the interface shows it **and edits it**.
//
// Ticket `44`, the second of the two surfaces ticket `43` called hard. `PZ_SVF` is not a filter model
// xerum builds: it is a **response drawn in the preset**, as up to four objects of eight second-order
// pole/zero sections each — 32 sections in series (tickets `02`/`04`, and `dsp::PoleZeroDescription` is
// the engine's own shape for them). The prototype drew a plane for it; this is that plane, and it plots
// what the preset actually carries:
//
//   * **x is frequency**, as the preset stores it — a fraction of Nyquist, which is why many factory
//     values snap to a 1/16 grid;
//   * **a pole is drawn above the axis and a zero below**, at its own frequency, because the two are
//     *separate* frequencies in this model rather than one magnitude response. A pole above and a zero
//     below is the reading that keeps both visible; putting them on one line would hide whichever is
//     second;
//   * **marker size is the section's Q**, which is the third number a section carries (`qp`/`qz`) and the
//     only one left to show.
//
// ## Ticket 45: the plane is now the editor
//
// Ticket `44` left these sections **shown and not applied**, and the plugin reported a drawn response at
// `high`, because `dsp::VoiceFilter::setPoleZero` existed and nothing in `plugin/` reached it. That is
// no longer true: the response is read from the document into `FilterSettings::poleZero`, the voice binds
// it with the model, and this plane edits it. So the `high` report is **retracted** in the same change,
// which is the rule ticket `41` applied to `interpolateAfterLoad` — a report that was true before and
// false after is worse than no report.
//
// The edit gesture is stated here because it is the whole of the interaction:
//
//   * dragging a **pole** marker horizontally sets that section's pole frequency; dragging a **zero**
//     marker sets its zero frequency;
//   * dragging either marker vertically sets that marker's Q (up increases it), because the marker's
//     vertical *position* is fixed by whether it is a pole or a zero, leaving its size the thing to shape;
//   * the nearest marker within a small radius is the one grabbed, so a section is edited by pointing at
//     the shape it draws.
//
// The plane carries no document of its own: an edit is reported through `onSectionChanged` with the
// whole section, and the owner writes one `PZs[n]` object and republishes. That keeps the plane a view
// whose *input* is the document — the same relationship ticket `44` gave the matrix.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "document.h"
#include "filter.h"

namespace xerum::plugin::ui
{

/// The pole/zero plane: every section the preset drew, up to the engine's own cap of 32.
class PoleZeroPlane final : public juce::Component
{
public:
    PoleZeroPlane();

    /// Which voice filter this plane belongs to, and the type the preset selected for it — the type is
    /// shown because it is what the engine builds.
    void setFilter (int filterSlot, juce::String typeName);
    void setSections (dsp::PoleZeroDescription description);

    /// Whether a marker can be grabbed. False keeps the plane the pure view ticket `44` built.
    void setEditable (bool editable);

    /// One section after an edit: the index into the current description and the whole section. The owner
    /// writes it into the document and hands the engine the new response.
    std::function<void (int section, const dsp::PoleZeroSection&)> onSectionChanged;

    int filterSlot() const { return filterSlot_; }
    int sectionCount() const { return static_cast<int> (sections_.count); }
    const dsp::PoleZeroDescription& sections() const { return sections_; }

    /// The section a drag would move, or -1. What a drag last selected.
    int selectedSection() const { return selected_; }

    /// The engine's cap, stated where the plane is: `PoleZeroDescription::kMaxSections`.
    static int sectionCapacity() { return static_cast<int> (dsp::PoleZeroDescription::kMaxSections); }

    /// The line under the plane: how many of the 32 sections are drawn.
    juce::String caption() const;

    /// Where a section's **pole** is drawn, in this component's coordinates. The test reads it back so
    /// that "the plane shows its sections" is measured rather than asserted about.
    juce::Point<float> polePoint (int section) const;

    /// Where a section's **zero** is drawn.
    juce::Point<float> zeroPoint (int section) const;

    /// Which marker a point is nearest to, or -1. Public so a test can ask the same question the mouse
    /// does rather than re-deriving the geometry.
    int markerAt (juce::Point<float> point) const;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;

    void paint (juce::Graphics& graphics) override;

private:
    /// `marker < 0` means no marker; otherwise bit 0 of the pair says pole (0) or zero (1).
    juce::Point<float> pointFor (int section, bool pole) const;

    int filterSlot_ { 0 };
    juce::String type_;
    dsp::PoleZeroDescription sections_;
    bool editable_ { false };
    int selected_ { -1 };

    /// The marker a drag is moving: the section and whether it is the pole side.
    int dragSection_ { -1 };
    bool dragPole_ { false };
    float dragStartY_ { 0.0f };
    double dragStartQ_ { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PoleZeroPlane)
};

} // namespace xerum::plugin::ui
